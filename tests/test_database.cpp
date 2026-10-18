#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "test_support/message_ts.h"

extern "C"
{
#include "database.h"
#include "heap_storage.h"
#include "superheader.h"
#include "usage_bitmap.h"
#include "mem_layout.h"

// database.c backing storage for the host build (not declared in a header)
extern uint8_t heap_mem[];
}

// Raw sectors spanned by the database (matches DATABASE_RAW_SECTOR_SIZE in database.c)
static constexpr size_t DB_RAW_SECTORS = DATA_REGION_START_SECTOR + TOTAL_DATABASE_SECTOR_SIZE;

/*
 * Write wrapper that forwards to heap_storage but can simulate a power cut:
 * once g_writes_left reaches zero every further write fails.
 */
static int g_writes_left = -1; // -1 = never fail

static bool PowerCut_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    if (g_writes_left == 0)
    {
        return false;
    }
    if (g_writes_left > 0)
    {
        g_writes_left--;
    }
    return heap_storage.write_block(context, index, in);
}

static bool PowerCut_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in)
{
    for (size_t i = 0; i < writeNum; i++)
    {
        if (!PowerCut_WriteBlock(context, index + i, in + (i * SECTOR_SIZE)))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Fixture for the high level database API (database.c).
 *
 * Every test starts from a blank card: heap_mem is wiped so database_init()
 * sees an uninitialised superheader and formats. reboot() re-runs
 * database_init() over the same heap_mem to exercise the mount path.
 */
class DatabaseTest : public ::testing::Test
{
protected:
    Database db{};

    void SetUp() override
    {
        g_writes_left = -1;
        std::memset(heap_mem, 0, DB_RAW_SECTORS * SECTOR_SIZE);
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        ASSERT_EQ(database_init(&db), STRG_OK);
    }

    void TearDown() override
    {
        g_writes_left = -1;
    }

    /** @brief Simulate a reboot: RAM is wiped and the database is mounted from heap_mem. */
    STRG_RET reboot()
    {
        g_writes_left = -1;
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        return database_init(&db);
    }

    /** @brief Make the nth write from now (1 based) and every write after it fail. */
    void cut_power_after(int writes)
    {
        db.storage.write_block = PowerCut_WriteBlock;
        db.storage.write_multiblock = PowerCut_WriteMultiBlock;
        g_writes_left = writes;
    }

    /** @brief Full contact list as "name|phone" strings, in name index order. */
    std::vector<std::string> list_all()
    {
        std::vector<ContactBuffer> out(database_contact_count(&db) + 1);
        size_t count = 0;
        std::vector<std::string> names;

        STRG_RET ret = database_contact_list(&db, 0, out.size(), &count, out.data());
        if (ret != STRG_OK)
        {
            return names;
        }

        for (size_t i = 0; i < count; i++)
        {
            const Contact &c = out[i].contact;
            names.push_back(std::string(c.name, c.name_len) + "|" + std::string(c.phone, c.phone_len));
        }
        return names;
    }

    /** @brief Newest first message strings of a chat. */
    std::vector<std::string> chat(const char *phone, size_t n = 64)
    {
        std::vector<Message> out(n);
        size_t count = 0;
        std::vector<std::string> strs;

        if (database_message_get_chat(&db, phone, 0, n, &count, out.data()) != STRG_OK)
        {
            return strs;
        }

        for (size_t i = 0; i < count; i++)
        {
            strs.push_back(std::string(out[i].str, strnlen(out[i].str, SMS_MAX_MESSAGE_LENGTH)));
        }
        return strs;
    }

    /** @brief Read the superheader, let fn modify it, then write it back with a fresh CRC. */
    template <typename Fn>
    void patch_superheader(Fn fn)
    {
        SuperHeaderBuffer sh;
        std::memcpy(sh.buffer, &heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE], SECTOR_SIZE);
        fn(sh);
        sector_crc_stamp(sh.buffer);
        std::memcpy(&heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE], sh.buffer, SECTOR_SIZE);
    }
};

static std::string phone_for(int i)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "04%08d", i);
    return buf;
}

static std::string name_for(int i)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Contact%05d", i);
    return buf;
}

/* ============================================================================
 * Init / format / mount
 * ========================================================================== */

/**
 * @brief A blank card is formatted into an empty database.
 */
TEST_F(DatabaseTest, BlankCardFormatsEmpty)
{
    size_t count = 99;
    ContactBuffer c[1];
    MessageHistoryEntry mh[1];
    CallHistoryEntry ch[1];

    EXPECT_EQ(database_contact_count(&db), 0u);
    EXPECT_EQ(database_contact_list(&db, 0, 1, &count, c), STRG_EMPTY);
    EXPECT_EQ(count, 0u);

    EXPECT_EQ(database_message_recent(&db, 0, 1, &count, mh), STRG_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(database_call_recent(&db, 0, 1, &count, ch), STRG_OK);
    EXPECT_EQ(count, 0u);
}

/**
 * @brief After format, the superheader is valid so the next boot mounts instead of formatting.
 */
TEST_F(DatabaseTest, FormatWritesValidSuperheader)
{
    SuperHeaderBuffer sh;
    EXPECT_EQ(superheader_check(&db.storage, &sh), SUPR_GOOD);
}

/**
 * @brief Mounting an empty database succeeds and stays empty.
 */
TEST_F(DatabaseTest, RebootEmptyDatabase)
{
    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(database_contact_count(&db), 0u);
}

/**
 * @brief Contacts, chats and history all survive a reboot.
 */
TEST_F(DatabaseTest, RebootPreservesEverything)
{
    ASSERT_EQ(database_contact_add(&db, "Zed", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "alice", "0400000002"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 1, true, (char *)"one"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, false, (char *)"two"), STRG_OK);
    ASSERT_EQ(database_call_add(&db, "0400000001"), STRG_OK);

    std::vector<std::string> before = list_all();

    ASSERT_EQ(reboot(), STRG_OK);

    EXPECT_EQ(database_contact_count(&db), 2u);
    EXPECT_EQ(list_all(), before);
    EXPECT_EQ(chat("0400000002"), (std::vector<std::string>{"two", "one"}));

    size_t count = 0;
    MessageHistoryEntry mh[4];
    ASSERT_EQ(database_message_recent(&db, 0, 4, &count, mh), STRG_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(msg_ts(mh[0].message), 2u);
    EXPECT_EQ(msg_ts(mh[1].message), 1u);

    CallHistoryEntry ch[4];
    ASSERT_EQ(database_call_recent(&db, 0, 4, &count, ch), STRG_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(std::string(ch[0].contact.name, ch[0].contact.name_len), "Zed");
}

/**
 * @brief Writes after a reboot continue on from the rebuilt state.
 */
TEST_F(DatabaseTest, WritesAfterRebootAppend)
{
    ASSERT_EQ(database_message_add(&db, "0400000002", 1, true, (char *)"before"), STRG_OK);
    ASSERT_EQ(reboot(), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"after"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "New", "0400000003"), STRG_OK);

    EXPECT_EQ(chat("0400000002"), (std::vector<std::string>{"after", "before"}));
    EXPECT_EQ(database_contact_count(&db), 2u);

    size_t count = 0;
    MessageHistoryEntry mh[4];
    ASSERT_EQ(database_message_recent(&db, 0, 4, &count, mh), STRG_OK);
    EXPECT_EQ(count, 2u);
}

/**
 * @brief A corrupted superheader is reported and the card is not wiped.
 */
TEST_F(DatabaseTest, CorruptSuperheaderNotWiped)
{
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);

    // flip a data byte without restamping the CRC
    heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE + 8] ^= 0xFF;

    EXPECT_EQ(reboot(), STRG_CORRUPT);

    // repair the byte, the contact must still be there
    heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE + 8] ^= 0xFF;
    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(database_contact_count(&db), 1u);
}

/**
 * @brief An outdated superheader version is reported rather than mounted.
 */
TEST_F(DatabaseTest, OutdatedSuperheaderReported)
{
    patch_superheader([](SuperHeaderBuffer &sh) { sh.var.data.var.version += 1; });
    EXPECT_EQ(reboot(), STRG_CORRUPT);
}

/**
 * @brief database_format after a bad superheader leaves a usable, empty database.
 */
TEST_F(DatabaseTest, FormatAfterCorruptSuperheader)
{
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);
    heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE + 8] ^= 0xFF;
    ASSERT_EQ(reboot(), STRG_CORRUPT);

    ASSERT_EQ(database_format(&db), STRG_OK);
    EXPECT_EQ(database_contact_count(&db), 0u);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Bob|0400000002"}));
}

/**
 * @brief A NULL database is rejected.
 */
TEST_F(DatabaseTest, InitNullFails)
{
    EXPECT_EQ(database_init(nullptr), STRG_FAIL);
    EXPECT_EQ(database_format(nullptr), STRG_FAIL);
}

/* ============================================================================
 * Contacts
 * ========================================================================== */

/**
 * @brief An added contact can be found by phone number.
 */
TEST_F(DatabaseTest, AddThenGet)
{
    ContactBuffer out{};
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_get(&db, "0400000001", &out), STRG_OK);
    EXPECT_EQ(std::string(out.contact.name, out.contact.name_len), "Ada");
    EXPECT_EQ(database_contact_count(&db), 1u);
}

/**
 * @brief Looking up an unknown phone number returns STRG_EMPTY.
 */
TEST_F(DatabaseTest, GetUnknownIsEmpty)
{
    ContactBuffer out{};
    EXPECT_EQ(database_contact_get(&db, "0400000001", &out), STRG_EMPTY);
}

/**
 * @brief A second contact with the same phone number is rejected and not indexed twice.
 */
TEST_F(DatabaseTest, DuplicatePhoneRejected)
{
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);
    EXPECT_EQ(database_contact_add(&db, "Other", "0400000001"), STRG_EXISTS);
    EXPECT_EQ(database_contact_count(&db), 1u);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Ada|0400000001"}));
}

/**
 * @brief Invalid arguments are rejected without changing the database.
 */
TEST_F(DatabaseTest, AddRejectsInvalidArgs)
{
    std::string long_name(MAX_NAME_LEN + 1, 'a');
    std::string long_phone(MAX_PHONE_LEN, '1'); // must leave room for the terminator

    EXPECT_EQ(database_contact_add(nullptr, "Ada", "0400000001"), STRG_FAIL);
    EXPECT_EQ(database_contact_add(&db, nullptr, "0400000001"), STRG_FAIL);
    EXPECT_EQ(database_contact_add(&db, "Ada", nullptr), STRG_FAIL);
    EXPECT_EQ(database_contact_add(&db, "Ada", ""), STRG_FAIL);
    EXPECT_EQ(database_contact_add(&db, long_name.c_str(), "0400000001"), STRG_FAIL);
    EXPECT_EQ(database_contact_add(&db, "Ada", long_phone.c_str()), STRG_FAIL);
    EXPECT_EQ(database_contact_count(&db), 0u);
}

/**
 * @brief The longest allowed name and phone number are accepted and round trip.
 */
TEST_F(DatabaseTest, AddMaxLengthFields)
{
    std::string name(MAX_NAME_LEN, 'n');
    std::string phone(MAX_PHONE_LEN - 1, '9');
    ContactBuffer out{};

    ASSERT_EQ(database_contact_add(&db, name.c_str(), phone.c_str()), STRG_OK);
    ASSERT_EQ(database_contact_get(&db, phone.c_str(), &out), STRG_OK);
    EXPECT_EQ(std::string(out.contact.name, out.contact.name_len), name);
}

/**
 * @brief The list is alphabetical (case insensitive) regardless of insert order.
 */
TEST_F(DatabaseTest, ListIsAlphabetical)
{
    ASSERT_EQ(database_contact_add(&db, "Zed", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "alice", "0400000002"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000003"), STRG_OK);

    EXPECT_EQ(list_all(), (std::vector<std::string>{"alice|0400000002", "Bob|0400000003", "Zed|0400000001"}));
}

/**
 * @brief Contacts with the same name are both kept, ordered by phone number.
 */
TEST_F(DatabaseTest, SameNameDifferentPhones)
{
    ASSERT_EQ(database_contact_add(&db, "Sam", "0400000009"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Sam", "0400000001"), STRG_OK);

    EXPECT_EQ(list_all(), (std::vector<std::string>{"Sam|0400000001", "Sam|0400000009"}));
}

/**
 * @brief Names sharing the first 8 characters (the key) are ordered by tiebreak.
 */
TEST_F(DatabaseTest, LongSharedPrefixOrdered)
{
    ASSERT_EQ(database_contact_add(&db, "Alexandra Zed", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Alexandra Amy", "0400000002"), STRG_OK);

    EXPECT_EQ(list_all(), (std::vector<std::string>{"Alexandra Amy|0400000002", "Alexandra Zed|0400000001"}));
}

/**
 * @brief Paging through the list returns the right window and clips at the end.
 */
TEST_F(DatabaseTest, ListPaging)
{
    for (int i = 0; i < 10; i++)
    {
        ASSERT_EQ(database_contact_add(&db, name_for(i).c_str(), phone_for(i).c_str()), STRG_OK);
    }

    ContactBuffer out[4];
    size_t count = 0;

    ASSERT_EQ(database_contact_list(&db, 4, 4, &count, out), STRG_OK);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(std::string(out[0].contact.name, out[0].contact.name_len), name_for(4));

    ASSERT_EQ(database_contact_list(&db, 8, 4, &count, out), STRG_OK);
    EXPECT_EQ(count, 2u);

    EXPECT_EQ(database_contact_list(&db, 10, 4, &count, out), STRG_EMPTY);
    EXPECT_EQ(count, 0u);
}

/**
 * @brief Seek finds the rank of the first name >= the prefix, ignoring case.
 */
TEST_F(DatabaseTest, SeekPrefix)
{
    ASSERT_EQ(database_contact_add(&db, "Adam", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "bella", "0400000002"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000003"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Carl", "0400000004"), STRG_OK);

    size_t rank = 99;
    ASSERT_EQ(database_contact_seek(&db, "B", &rank), STRG_OK);
    EXPECT_EQ(rank, 1u);

    ContactBuffer out[1];
    size_t count = 0;
    ASSERT_EQ(database_contact_list(&db, rank, 1, &count, out), STRG_OK);
    EXPECT_EQ(std::string(out[0].contact.name, out[0].contact.name_len), "bella");

    ASSERT_EQ(database_contact_seek(&db, "bo", &rank), STRG_OK);
    EXPECT_EQ(rank, 2u);

    ASSERT_EQ(database_contact_seek(&db, "z", &rank), STRG_OK);
    EXPECT_EQ(rank, 4u);
}

/**
 * @brief Renaming a contact moves it in the list.
 */
TEST_F(DatabaseTest, UpdateNameReorders)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);

    ASSERT_EQ(database_contact_update(&db, "0400000001", "Zara", "0400000001"), STRG_OK);

    EXPECT_EQ(list_all(), (std::vector<std::string>{"Bob|0400000002", "Zara|0400000001"}));

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Bob|0400000002", "Zara|0400000001"}));
}

/**
 * @brief Changing the phone number moves the contact and keeps its chat.
 */
TEST_F(DatabaseTest, UpdatePhoneKeepsChat)
{
    ContactBuffer out{};
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"hello"), STRG_OK);

    ASSERT_EQ(database_contact_update(&db, "0400000001", "Amy", "0400000009"), STRG_OK);

    EXPECT_EQ(database_contact_get(&db, "0400000001", &out), STRG_EMPTY);
    ASSERT_EQ(database_contact_get(&db, "0400000009", &out), STRG_OK);
    EXPECT_EQ(chat("0400000009"), (std::vector<std::string>{"hello"}));
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Amy|0400000009"}));
    EXPECT_EQ(database_contact_count(&db), 1u);

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(database_contact_get(&db, "0400000001", &out), STRG_EMPTY);
    EXPECT_EQ(chat("0400000009"), (std::vector<std::string>{"hello"}));
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Amy|0400000009"}));
}

/**
 * @brief Changing to a phone number already in use is rejected and nothing changes.
 */
TEST_F(DatabaseTest, UpdateToExistingPhoneRejected)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);

    EXPECT_EQ(database_contact_update(&db, "0400000001", "Amy", "0400000002"), STRG_EXISTS);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Amy|0400000001", "Bob|0400000002"}));
}

/**
 * @brief Updating an unknown contact returns STRG_EMPTY.
 */
TEST_F(DatabaseTest, UpdateUnknownIsEmpty)
{
    EXPECT_EQ(database_contact_update(&db, "0400000001", "Amy", "0400000001"), STRG_EMPTY);
}

/**
 * @brief Removing a contact drops it from lookup, the list and the count.
 */
TEST_F(DatabaseTest, RemoveContact)
{
    ContactBuffer out{};
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);

    ASSERT_EQ(database_contact_remove(&db, "0400000001"), STRG_OK);

    EXPECT_EQ(database_contact_get(&db, "0400000001", &out), STRG_EMPTY);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Bob|0400000002"}));
    EXPECT_EQ(database_contact_count(&db), 1u);

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Bob|0400000002"}));
}

/**
 * @brief Removing one of two same-named contacts removes the right one.
 */
TEST_F(DatabaseTest, RemoveSameNameRemovesRightOne)
{
    ASSERT_EQ(database_contact_add(&db, "Sam", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Sam", "0400000002"), STRG_OK);

    ASSERT_EQ(database_contact_remove(&db, "0400000002"), STRG_OK);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"Sam|0400000001"}));
}

/**
 * @brief Removing a contact deletes its chat, so re-adding the number starts empty.
 */
TEST_F(DatabaseTest, RemoveContactDeletesChat)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"gone"), STRG_OK);

    ASSERT_EQ(database_contact_remove(&db, "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);

    size_t count = 99;
    Message out[4];
    EXPECT_EQ(database_message_get_chat(&db, "0400000001", 0, 4, &count, out), STRG_EMPTY);
    EXPECT_EQ(count, 0u);
}

/**
 * @brief Removing an unknown contact returns STRG_EMPTY.
 */
TEST_F(DatabaseTest, RemoveUnknownIsEmpty)
{
    EXPECT_EQ(database_contact_remove(&db, "0400000001"), STRG_EMPTY);
}

/**
 * @brief A removed contact's slot is reused, and the count stays correct.
 */
TEST_F(DatabaseTest, AddRemoveChurn)
{
    for (int round = 0; round < 3; round++)
    {
        for (int i = 0; i < 50; i++)
        {
            ASSERT_EQ(database_contact_add(&db, name_for(i).c_str(), phone_for(i).c_str()), STRG_OK);
        }
        for (int i = 0; i < 50; i += 2)
        {
            ASSERT_EQ(database_contact_remove(&db, phone_for(i).c_str()), STRG_OK);
        }
        EXPECT_EQ(database_contact_count(&db), 25u);
        for (int i = 1; i < 50; i += 2)
        {
            ASSERT_EQ(database_contact_remove(&db, phone_for(i).c_str()), STRG_OK);
        }
        EXPECT_EQ(database_contact_count(&db), 0u);
    }

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(database_contact_count(&db), 0u);
}

/* ============================================================================
 * Messages
 * ========================================================================== */

/**
 * @brief A chat spanning several message sectors reads back newest first.
 */
TEST_F(DatabaseTest, ChatSpansSectors)
{
    std::vector<std::string> expect;
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);

    for (int i = 1; i <= (int)(MESSAGE_SECTOR_CAPACITY * 4 + 1); i++)
    {
        std::string s = "msg" + std::to_string(i);
        ASSERT_EQ(database_message_add(&db, "0400000001", i, (i % 2) == 0, (char *)s.c_str()), STRG_OK);
        expect.insert(expect.begin(), s);
    }

    EXPECT_EQ(chat("0400000001"), expect);

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(chat("0400000001"), expect);
}

/**
 * @brief Reading a chat from an offset skips the newest messages.
 */
TEST_F(DatabaseTest, ChatOffset)
{
    for (int i = 1; i <= 7; i++)
    {
        std::string s = "m" + std::to_string(i);
        ASSERT_EQ(database_message_add(&db, "0400000001", i, true, (char *)s.c_str()), STRG_OK);
    }

    Message out[3];
    size_t count = 0;
    ASSERT_EQ(database_message_get_chat(&db, "0400000001", 2, 3, &count, out), STRG_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(msg_ts(out[0]), 5u);
    EXPECT_EQ(msg_ts(out[2]), 3u);
}

/**
 * @brief Chats are kept separate per phone number.
 */
TEST_F(DatabaseTest, ChatsAreSeparate)
{
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"to amy"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"to bob"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000001", 3, false, (char *)"from amy"), STRG_OK);

    EXPECT_EQ(chat("0400000001"), (std::vector<std::string>{"from amy", "to amy"}));
    EXPECT_EQ(chat("0400000002"), (std::vector<std::string>{"to bob"}));
}

/**
 * @brief A message from an unknown number creates an unnamed contact in the list.
 */
TEST_F(DatabaseTest, MessageFromUnknownCreatesContact)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0499999999", 1, false, (char *)"who?"), STRG_OK);

    EXPECT_EQ(database_contact_count(&db), 2u);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"|0499999999", "Amy|0400000001"}));

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(list_all(), (std::vector<std::string>{"|0499999999", "Amy|0400000001"}));
}

/**
 * @brief Naming an unnamed contact (created by a message) moves it in the list and keeps the chat.
 */
TEST_F(DatabaseTest, NameUnknownContact)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0499999999", 1, false, (char *)"hi"), STRG_OK);

    ASSERT_EQ(database_contact_update(&db, "0499999999", "Zoe", "0499999999"), STRG_OK);

    EXPECT_EQ(list_all(), (std::vector<std::string>{"Amy|0400000001", "Zoe|0499999999"}));
    EXPECT_EQ(chat("0499999999"), (std::vector<std::string>{"hi"}));
}

/**
 * @brief Removing a chat keeps the contact, and new messages start a new chat.
 */
TEST_F(DatabaseTest, RemoveChatKeepsContact)
{
    ContactBuffer out{};
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    for (int i = 1; i <= 5; i++)
    {
        ASSERT_EQ(database_message_add(&db, "0400000001", i, true, (char *)"x"), STRG_OK);
    }

    ASSERT_EQ(database_message_remove_chat(&db, "0400000001"), STRG_OK);

    EXPECT_EQ(database_contact_get(&db, "0400000001", &out), STRG_OK);
    EXPECT_TRUE(chat("0400000001").empty());
    EXPECT_EQ(database_message_remove_chat(&db, "0400000001"), STRG_EMPTY);

    ASSERT_EQ(database_message_add(&db, "0400000001", 6, true, (char *)"fresh"), STRG_OK);
    EXPECT_EQ(chat("0400000001"), (std::vector<std::string>{"fresh"}));

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(chat("0400000001"), (std::vector<std::string>{"fresh"}));
}

/**
 * @brief Chat reads for unknown numbers or contacts without messages return STRG_EMPTY.
 */
TEST_F(DatabaseTest, ChatEmptyCases)
{
    Message out[1];
    size_t count = 99;

    EXPECT_EQ(database_message_get_chat(&db, "0400000001", 0, 1, &count, out), STRG_EMPTY);
    EXPECT_EQ(count, 0u);

    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    EXPECT_EQ(database_message_get_chat(&db, "0400000001", 0, 1, &count, out), STRG_EMPTY);
    EXPECT_EQ(database_message_remove_chat(&db, "0400000001"), STRG_EMPTY);
}

/**
 * @brief Invalid message arguments are rejected.
 */
TEST_F(DatabaseTest, MessageRejectsInvalidArgs)
{
    EXPECT_EQ(database_message_add(&db, "0400000001", 0, true, (char *)"t0"), STRG_FAIL);
    EXPECT_EQ(database_message_add(&db, "0400000001", 1, true, nullptr), STRG_FAIL);
    EXPECT_EQ(database_message_add(&db, "", 1, true, (char *)"x"), STRG_FAIL);
    EXPECT_EQ(database_message_add(nullptr, "0400000001", 1, true, (char *)"x"), STRG_FAIL);
    EXPECT_EQ(database_contact_count(&db), 0u);
}

/**
 * @brief Message history is newest first across all chats and records the chat sector.
 */
TEST_F(DatabaseTest, MessageHistoryAcrossChats)
{
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000001", 3, true, (char *)"c"), STRG_OK);

    MessageHistoryEntry mh[8];
    size_t count = 0;
    ASSERT_EQ(database_message_recent(&db, 0, 8, &count, mh), STRG_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(msg_ts(mh[0].message), 3u);
    EXPECT_EQ(msg_ts(mh[1].message), 2u);
    EXPECT_EQ(msg_ts(mh[2].message), 1u);
    EXPECT_EQ(mh[0].msgInd, mh[2].msgInd); // same chat, same (only) sector
    EXPECT_NE(mh[0].msgInd, mh[1].msgInd);
}

/**
 * @brief Message history pages across ring buffer sectors.
 */
TEST_F(DatabaseTest, MessageHistoryPaging)
{
    const int total = (int)(MESSAGE_HISTORY_SECTOR_CAPACITY * 3 + 1);
    for (int i = 1; i <= total; i++)
    {
        ASSERT_EQ(database_message_add(&db, "0400000001", i, true, (char *)"x"), STRG_OK);
    }

    MessageHistoryEntry mh[4];
    size_t count = 0;
    ASSERT_EQ(database_message_recent(&db, 2, 4, &count, mh), STRG_OK);
    ASSERT_EQ(count, 4u);
    for (size_t i = 0; i < count; i++)
    {
        EXPECT_EQ(msg_ts(mh[i].message), (uint32_t)(total - 2 - (int)i));
    }

    ASSERT_EQ(reboot(), STRG_OK);
    ASSERT_EQ(database_message_recent(&db, 0, 4, &count, mh), STRG_OK);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(msg_ts(mh[0].message), (uint32_t)total);
}

/* ============================================================================
 * Call history
 * ========================================================================== */

/**
 * @brief Calls from a known contact store the contact, unknown callers only the phone.
 */
TEST_F(DatabaseTest, CallKnownAndUnknown)
{
    ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
    ASSERT_EQ(database_call_add(&db, "0400000001"), STRG_OK);
    ASSERT_EQ(database_call_add(&db, "0499999999"), STRG_OK);

    CallHistoryEntry ch[4];
    size_t count = 0;
    ASSERT_EQ(database_call_recent(&db, 0, 4, &count, ch), STRG_OK);
    ASSERT_EQ(count, 2u);

    // newest first
    EXPECT_EQ(ch[0].contact_index, UINT16_MAX);
    EXPECT_EQ(ch[0].contact.name_len, 0u);
    EXPECT_EQ(std::string(ch[0].contact.phone, ch[0].contact.phone_len), "0499999999");

    EXPECT_NE(ch[1].contact_index, UINT16_MAX);
    EXPECT_EQ(std::string(ch[1].contact.name, ch[1].contact.name_len), "Amy");

    // an unknown caller does not become a contact
    EXPECT_EQ(database_contact_count(&db), 1u);
}

/**
 * @brief Call history pages across ring buffer sectors and survives a reboot.
 */
TEST_F(DatabaseTest, CallHistoryPaging)
{
    const int total = (int)(CALL_HISTORY_SECTOR_CAPACITY * 2 + 1);
    for (int i = 0; i < total; i++)
    {
        ASSERT_EQ(database_call_add(&db, phone_for(i).c_str()), STRG_OK);
    }

    ASSERT_EQ(reboot(), STRG_OK);

    CallHistoryEntry ch[4];
    size_t count = 0;
    ASSERT_EQ(database_call_recent(&db, 0, 4, &count, ch), STRG_OK);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(std::string(ch[0].contact.phone, ch[0].contact.phone_len), phone_for(total - 1));
    EXPECT_EQ(std::string(ch[3].contact.phone, ch[3].contact.phone_len), phone_for(total - 4));
}

/**
 * @brief Invalid call arguments are rejected.
 */
TEST_F(DatabaseTest, CallRejectsInvalidArgs)
{
    size_t count = 0;
    CallHistoryEntry ch[1];

    EXPECT_EQ(database_call_add(&db, ""), STRG_FAIL);
    EXPECT_EQ(database_call_add(nullptr, "0400000001"), STRG_FAIL);
    EXPECT_EQ(database_call_recent(&db, 0, 1, nullptr, ch), STRG_FAIL);
    EXPECT_EQ(database_call_recent(&db, 0, 1, &count, nullptr), STRG_FAIL);
}

/* ============================================================================
 * Capacity
 * ========================================================================== */

/**
 * @brief The database holds HASH_TABLE_ENTRIES contacts, rejects one more, and
 * rebuilds all of them in order on reboot.
 */
TEST_F(DatabaseTest, CapacityLimit)
{
    for (int i = 0; i < HASH_TABLE_ENTRIES; i++)
    {
        ASSERT_EQ(database_contact_add(&db, name_for(i).c_str(), phone_for(i).c_str()), STRG_OK) << "i=" << i;
    }
    EXPECT_EQ(database_contact_count(&db), (size_t)HASH_TABLE_ENTRIES);

    // one past the limit is rolled back out of the hash table too
    ContactBuffer out{};
    std::string extra = phone_for(HASH_TABLE_ENTRIES);
    EXPECT_EQ(database_contact_add(&db, "Extra", extra.c_str()), STRG_FULL);
    EXPECT_EQ(database_contact_count(&db), (size_t)HASH_TABLE_ENTRIES);
    EXPECT_EQ(database_contact_get(&db, extra.c_str(), &out), STRG_EMPTY);

    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(database_contact_count(&db), (size_t)HASH_TABLE_ENTRIES);

    std::vector<std::string> names = list_all();
    ASSERT_EQ(names.size(), (size_t)HASH_TABLE_ENTRIES);
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    EXPECT_EQ(names.front(), name_for(0) + "|" + phone_for(0));
    EXPECT_EQ(names.back(), name_for(HASH_TABLE_ENTRIES - 1) + "|" + phone_for(HASH_TABLE_ENTRIES - 1));

    // removing one frees space for another
    ASSERT_EQ(database_contact_remove(&db, phone_for(0).c_str()), STRG_OK);
    EXPECT_EQ(database_contact_add(&db, "Extra", extra.c_str()), STRG_OK);
}

/* ============================================================================
 * Power loss
 * ========================================================================== */

/**
 * @brief Check the hash table and name index agree: every listed contact can
 * be found by phone, and the list size matches the count.
 */
static void expect_consistent(DatabaseTest *t, Database *db, const std::vector<std::string> &names)
{
    (void)t;
    EXPECT_EQ(names.size(), database_contact_count(db));
    for (const std::string &n : names)
    {
        ContactBuffer out{};
        std::string phone = n.substr(n.find('|') + 1);
        EXPECT_EQ(database_contact_get(db, phone.c_str(), &out), STRG_OK) << n;
    }
}

/**
 * @brief Cutting power at every write of a contact add leaves either the old or
 * the new state after reboot, never a half added contact.
 */
TEST_F(DatabaseTest, PowerCutDuringContactAdd)
{
    for (int cut = 0; cut < 8; cut++)
    {
        SetUp();
        ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
        std::vector<std::string> before = list_all();

        cut_power_after(cut);
        STRG_RET ret = database_contact_add(&db, "Bob", "0400000002");

        ASSERT_EQ(reboot(), STRG_OK) << "cut=" << cut;
        std::vector<std::string> after = list_all();
        expect_consistent(this, &db, after);

        std::vector<std::string> added = {"Amy|0400000001", "Bob|0400000002"};
        if (ret == STRG_OK)
        {
            EXPECT_EQ(after, added) << "cut=" << cut;
        } else {
            // a cut after the commit (e.g. journal free) reports failure but the add is already durable
            EXPECT_TRUE((after == before) || (after == added)) << "cut=" << cut;
        }
    }
}

/**
 * @brief Cutting power at every write of a contact remove leaves either the
 * contact and chat intact or fully removed after reboot.
 */
TEST_F(DatabaseTest, PowerCutDuringContactRemove)
{
    for (int cut = 0; cut < 12; cut++)
    {
        SetUp();
        ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);
        ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);
        for (int i = 1; i <= 4; i++)
        {
            ASSERT_EQ(database_message_add(&db, "0400000002", i, true, (char *)"x"), STRG_OK);
        }

        cut_power_after(cut);
        STRG_RET ret = database_contact_remove(&db, "0400000002");

        ASSERT_EQ(reboot(), STRG_OK) << "cut=" << cut;
        std::vector<std::string> after = list_all();
        expect_consistent(this, &db, after);

        if (ret == STRG_OK)
        {
            EXPECT_EQ(after, (std::vector<std::string>{"Amy|0400000001"})) << "cut=" << cut;
        } else {
            // contact may survive, but whatever is left must still be readable
            for (const std::string &s : chat("0400000002"))
            {
                EXPECT_EQ(s, "x") << "cut=" << cut;
            }
        }
    }
}

/**
 * @brief Cutting power at every write of a message add never loses earlier
 * messages in the chat.
 */
TEST_F(DatabaseTest, PowerCutDuringMessageAdd)
{
    for (int cut = 0; cut < 8; cut++)
    {
        SetUp();
        // fill the first sector so the next message needs a new one
        for (int i = 1; i <= (int)MESSAGE_SECTOR_CAPACITY; i++)
        {
            std::string s = "m" + std::to_string(i);
            ASSERT_EQ(database_message_add(&db, "0400000001", i, true, (char *)s.c_str()), STRG_OK);
        }
        std::vector<std::string> before = chat("0400000001");

        cut_power_after(cut);
        STRG_RET ret = database_message_add(&db, "0400000001", 100, true, (char *)"new");

        ASSERT_EQ(reboot(), STRG_OK) << "cut=" << cut;
        std::vector<std::string> after = chat("0400000001");

        if (ret == STRG_OK)
        {
            std::vector<std::string> expect = before;
            expect.insert(expect.begin(), "new");
            EXPECT_EQ(after, expect) << "cut=" << cut;
        } else {
            // the new message may or may not have landed, the old ones must be intact
            if (!after.empty() && after.front() == "new")
            {
                after.erase(after.begin());
            }
            EXPECT_EQ(after, before) << "cut=" << cut;
        }
    }
}

/**
 * @brief Cutting power during a rename never leaves the contact missing from
 * the list or listed twice.
 */
TEST_F(DatabaseTest, PowerCutDuringUpdate)
{
    for (int cut = 0; cut < 8; cut++)
    {
        SetUp();
        ASSERT_EQ(database_contact_add(&db, "Amy", "0400000001"), STRG_OK);

        cut_power_after(cut);
        STRG_RET ret = database_contact_update(&db, "0400000001", "Zara", "0400000009");

        ASSERT_EQ(reboot(), STRG_OK) << "cut=" << cut;
        std::vector<std::string> after = list_all();
        expect_consistent(this, &db, after);

        if (ret == STRG_OK)
        {
            EXPECT_EQ(after, (std::vector<std::string>{"Zara|0400000009"})) << "cut=" << cut;
        } else {
            ASSERT_EQ(after.size(), 1u) << "cut=" << cut;
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Chat list (chat_order.c)
 * ------------------------------------------------------------------ */

/** @brief Every chat as "phone|latest message", most recent first. */
static std::vector<std::string> chat_list_all(Database *db)
{
    std::vector<ChatSummary> out(database_chat_count(db) + 1);
    size_t count = 0;
    std::vector<std::string> rows;

    if (database_chat_list(db, 0, out.size(), &count, out.data()) != STRG_OK)
    {
        return rows;
    }

    for (size_t i = 0; i < count; i++)
    {
        const ChatSummary &c = out[i];
        rows.push_back(std::string(c.contact.phone, c.contact.phone_len) + "|" +
                       std::string(c.latest.str, strnlen(c.latest.str, SMS_MAX_MESSAGE_LENGTH)));
    }
    return rows;
}

/**
 * @brief No chats: the list is empty and a start of 0 is past the end.
 */
TEST_F(DatabaseTest, ChatListEmpty)
{
    ChatSummary out[1];
    size_t count = 9;

    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);
    EXPECT_EQ(database_chat_count(&db), 0u);
    EXPECT_EQ(database_chat_list(&db, 0, 1, &count, out), STRG_EMPTY);
    EXPECT_EQ(count, 0u);
}

/**
 * @brief Chats are listed most recent message first with their contact and
 * latest message, and a new message moves its chat to the top.
 */
TEST_F(DatabaseTest, ChatListMostRecentFirst)
{
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Bob", "0400000002"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a1"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b1"), STRG_OK);
    // unknown number: new unnamed contact and chat
    ASSERT_EQ(database_message_add(&db, "0499999999", 3, false, (char *)"x1"), STRG_OK);

    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0499999999|x1", "0400000002|b1", "0400000001|a1"}));

    ASSERT_EQ(database_message_add(&db, "0400000001", 4, false, (char *)"a2"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0400000001|a2", "0499999999|x1", "0400000002|b1"}));
    EXPECT_EQ(database_chat_count(&db), 3u);

    ChatSummary out[3];
    size_t count = 0;
    ASSERT_EQ(database_chat_list(&db, 0, 1, &count, out), STRG_OK);
    EXPECT_EQ(std::string(out[0].contact.name, out[0].contact.name_len), "Ada");
    EXPECT_EQ(msg_ts(out[0].latest), 4u);
}

/**
 * @brief start and n page through the list, a start past the end is STRG_EMPTY.
 */
TEST_F(DatabaseTest, ChatListPaging)
{
    ChatSummary out[2];
    size_t count = 0;
    char phone[16];

    for (int i = 0; i < 5; i++)
    {
        std::snprintf(phone, sizeof(phone), "04%08d", i);
        ASSERT_EQ(database_message_add(&db, phone, i + 1, true, (char *)"hi"), STRG_OK);
    }

    ASSERT_EQ(database_chat_list(&db, 1, 2, &count, out), STRG_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(std::string(out[0].contact.phone, out[0].contact.phone_len), "0400000003");
    EXPECT_EQ(std::string(out[1].contact.phone, out[1].contact.phone_len), "0400000002");

    ASSERT_EQ(database_chat_list(&db, 4, 2, &count, out), STRG_OK);
    EXPECT_EQ(count, 1u);

    EXPECT_EQ(database_chat_list(&db, 5, 2, &count, out), STRG_EMPTY);
}

/**
 * @brief Removing a chat or its contact drops it from the list.
 */
TEST_F(DatabaseTest, ChatListRemove)
{
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000003", 3, true, (char *)"c"), STRG_OK);

    ASSERT_EQ(database_message_remove_chat(&db, "0400000002"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0400000003|c", "0400000001|a"}));

    ASSERT_EQ(database_contact_remove(&db, "0400000003"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0400000001|a"}));

    // removing a contact without a chat leaves the list alone
    ASSERT_EQ(database_contact_add(&db, "Dan", "0400000004"), STRG_OK);
    ASSERT_EQ(database_contact_remove(&db, "0400000004"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0400000001|a"}));
}

/**
 * @brief A phone number change keeps the chat's place and shows the new number.
 */
TEST_F(DatabaseTest, ChatListPhoneChange)
{
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000003", 3, true, (char *)"c"), STRG_OK);

    ASSERT_EQ(database_contact_update(&db, "0400000002", "Bob", "0411111111"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), (std::vector<std::string>{"0400000003|c", "0411111111|b", "0400000001|a"}));

    // the moved chat still moves to the front on a new message
    ASSERT_EQ(database_message_add(&db, "0411111111", 4, true, (char *)"b2"), STRG_OK);
    EXPECT_EQ(chat_list_all(&db).front(), "0411111111|b2");
}

/**
 * @brief The order is rebuilt from the latest message timestamps on reboot,
 * including after a remove, a phone change and a chat spanning sectors.
 */
TEST_F(DatabaseTest, ChatListSurvivesReboot)
{
    ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000003", 3, true, (char *)"c"), STRG_OK);
    ASSERT_EQ(database_message_add(&db, "0400000004", 4, true, (char *)"d"), STRG_OK);
    // more than a sector of messages, so the latest is in a new extent
    for (int i = 0; i <= (int)MESSAGE_SECTOR_CAPACITY; i++)
    {
        std::string s = "a" + std::to_string(i);
        ASSERT_EQ(database_message_add(&db, "0400000001", 10 + i, true, (char *)s.c_str()), STRG_OK);
    }
    ASSERT_EQ(database_message_remove_chat(&db, "0400000003"), STRG_OK);
    ASSERT_EQ(database_contact_update(&db, "0400000002", "Bob", "0411111111"), STRG_OK);

    std::vector<std::string> before = chat_list_all(&db);
    ASSERT_EQ(before.size(), 3u);
    ASSERT_EQ(reboot(), STRG_OK);
    EXPECT_EQ(chat_list_all(&db), before);
}

/**
 * @brief A message add cut short by power loss leaves a consistent list after
 * reboot: every chat is listed once and its latest message matches the chat.
 */
TEST_F(DatabaseTest, PowerCutDuringMessageAddChatList)
{
    for (int cut = 0; cut < 8; cut++)
    {
        SetUp();
        ASSERT_EQ(database_message_add(&db, "0400000001", 1, true, (char *)"a"), STRG_OK);
        ASSERT_EQ(database_message_add(&db, "0400000002", 2, true, (char *)"b"), STRG_OK);

        cut_power_after(cut);
        database_message_add(&db, "0400000001", 3, true, (char *)"a2");

        ASSERT_EQ(reboot(), STRG_OK) << "cut=" << cut;
        std::vector<std::string> rows = chat_list_all(&db);
        ASSERT_EQ(rows.size(), 2u) << "cut=" << cut;
        std::string expect_front = (chat("0400000001").front() == "a2") ? "0400000001|a2" : "0400000002|b";
        EXPECT_EQ(rows.front(), expect_front) << "cut=" << cut;
    }
}
