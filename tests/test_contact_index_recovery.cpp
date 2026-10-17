#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

extern "C"
{
#include "storage.h"
#include "heap_storage.h"
#include "contact.h"
#include "message.h"
#include "hash_table.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
}

/*
 * Storage wrapper that forwards to heap_storage but can fail every read of one
 * raw sector, to simulate an SD read error on a single sector.
 */
static int64_t g_fail_read_raw = -1; // -1 = never fail

static bool FaultyRead_ReadBlock(void *context, uint32_t index, uint8_t *out)
{
    if (g_fail_read_raw >= 0 && index == (uint32_t)g_fail_read_raw)
    {
        return false;
    }
    return heap_storage.read_block(context, index, out);
}

/**
 * @brief Fixture for the insert/update split, the contact_index message header
 *        and orphan chat cleanup during message reconstruction.
 */
class ContactIndexRecoveryTest : public ::testing::Test
{
protected:
    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE;

    std::vector<uint8_t> storage_mem;
    HeapStorageContext storage_ctx{};
    Storage storage_obj{};
    Storage *storage = &storage_obj;
    Journal journal{};

    std::vector<HashEntry> entries;
    std::vector<uint16_t> contact_pool;
    std::vector<uint16_t> message_pool;
    FreeList contact_fls{};
    FreeList message_fls{};
    HashTable table{};

    void SetUp() override
    {
        storage_mem.assign(static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT, 0);
        ASSERT_TRUE(HeapStorage_Init(&storage_ctx, storage_mem.data(), SECTOR_SIZE, STORAGE_SECTOR_COUNT));

        storage_obj = heap_storage;
        storage_obj.context = &storage_ctx;
        storage_obj.read_block = FaultyRead_ReadBlock;
        g_fail_read_raw = -1;

        ASSERT_TRUE(init_usage_bitmap(storage));
        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));

        init_table(false);
    }

    void TearDown() override
    {
        g_fail_read_raw = -1;
    }

    /** @brief (Re)initialise the hash table. empty=true for a reconstruction boot. */
    void init_table(bool empty)
    {
        entries.assign(HASH_TABLE_SIZE, HashEntry{});
        contact_pool.assign(HASH_TABLE_ENTRIES, 0);
        message_pool.assign(TOTAL_MESSAGE_SECTOR_SIZE, 0);
        contact_fls = FreeList{};
        message_fls = FreeList{};
        table = HashTable{};

        if (empty)
        {
            ASSERT_TRUE(free_list_empty_init(&contact_fls, contact_pool.data(), HASH_TABLE_ENTRIES));
            ASSERT_TRUE(free_list_empty_init(&message_fls, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));
        }
        else
        {
            ASSERT_TRUE(free_list_init(&contact_fls, contact_pool.data(), HASH_TABLE_ENTRIES));
            ASSERT_TRUE(free_list_init(&message_fls, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));
        }
        hash_init(&table, storage, &contact_fls, &message_fls, entries.data(), HASH_TABLE_SIZE);
    }

    /** @brief Simulate a reboot: reload bitmap + journal, start with an empty table. */
    void reboot()
    {
        g_fail_read_raw = -1;
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        ASSERT_TRUE(read_usage_bitmap(storage));
        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));
        init_table(true);
    }

    /** @brief Full boot reconstruction. */
    bool reconstruct()
    {
        return hash_reconstruct_contact(&table) && hash_reconstruct_message(&table, &journal);
    }

    /** @brief Number of distinct sectors on the message free stack. */
    size_t distinct_free_message_sectors()
    {
        std::set<uint16_t> s(message_fls.free_stack, message_fls.free_stack + message_fls.stack_top);
        return s.size();
    }

    /** @brief true if no message sector is marked used in the bitmap. */
    bool no_message_bits_set()
    {
        for (uint16_t i = 0; i < TOTAL_MESSAGE_SECTOR_SIZE; i++)
        {
            if (check_usage_bit(MESSAGE_DATA_SECTOR(i)))
            {
                return false;
            }
        }
        return true;
    }

    /** @brief Expect every message sector free exactly once and none marked used. */
    void expect_message_region_fully_free()
    {
        EXPECT_TRUE(no_message_bits_set());
        EXPECT_EQ(free_list_available(&message_fls), (size_t)TOTAL_MESSAGE_SECTOR_SIZE);
        EXPECT_EQ(distinct_free_message_sectors(), (size_t)TOTAL_MESSAGE_SECTOR_SIZE)
            << "a message sector was freed more than once";
    }

    /**
     * @brief Write a chat directly to storage at chosen sector indices.
     *        sectors[0] is the oldest, sectors.back() the head (latest).
     */
    void write_chat(uint16_t contact_index, const std::vector<uint16_t> &sectors)
    {
        MessageBuffer m = create_message(1, true, (char *)"msg");
        ASSERT_EQ(write_new_message_sector(storage, &journal, contact_index, sectors[0], &m), STRG_OK);
        for (size_t i = 1; i < sectors.size(); i++)
        {
            ASSERT_EQ(write_next_message_sector(storage, &journal, contact_index, sectors[i - 1], sectors[i], &m),
                      STRG_OK);
        }
    }

    /** @brief Insert a contact and return the contact sector its hash slot points at. */
    uint16_t insert(const char *name, const char *phone)
    {
        ContactBuffer c = create_contact(name, phone);
        uint16_t slot = UINT16_MAX;
        EXPECT_EQ(hash_insert_contact(&table, &journal, &c, &slot), STRG_OK);
        if (slot >= table.size)
        {
            return UINT16_MAX;
        }
        return table.htable[slot].sector;
    }

    HashEntry *find(const char *phone)
    {
        HashEntry *e = nullptr;
        if (!hash_find_entry(&table, phone, &e) || e == nullptr || e->state != ENTRY_OCCUPIED)
        {
            return nullptr;
        }
        return e;
    }
};

/* ============================================================================
 * hash_insert_contact: insert only
 * ========================================================================== */

TEST_F(ContactIndexRecoveryTest, InsertReturnsHashSlot)
{
    ContactBuffer c = create_contact("Ada", "0411111111");
    uint16_t slot = UINT16_MAX;

    ASSERT_EQ(hash_insert_contact(&table, &journal, &c, &slot), STRG_OK);
    ASSERT_LT(slot, table.size);

    HashEntry *e = find("0411111111");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e, &table.htable[slot]);
    EXPECT_EQ(e->state, ENTRY_OCCUPIED);
    EXPECT_EQ(e->latest_msg_extent, UINT16_MAX);
}

TEST_F(ContactIndexRecoveryTest, InsertDuplicatePhoneReturnsExistsAndKeepsContact)
{
    insert("Ada", "0411111111");

    ContactBuffer dup = create_contact("Eve", "0411111111");
    uint16_t idx = UINT16_MAX;
    EXPECT_EQ(hash_insert_contact(&table, &journal, &dup, &idx), STRG_EXISTS);
    EXPECT_EQ(table.num_elems, 1u);

    ContactBuffer out{};
    ASSERT_TRUE(hash_find_contact(&table, "0411111111", &out));
    EXPECT_EQ(std::string(out.contact.name, out.contact.name_len), "Ada");
}

TEST_F(ContactIndexRecoveryTest, InsertNullAllocOutputFails)
{
    ContactBuffer c = create_contact("Ada", "0411111111");
    EXPECT_EQ(hash_insert_contact(&table, &journal, &c, nullptr), STRG_FAIL);
    EXPECT_EQ(table.num_elems, 0u);
}

/* ============================================================================
 * hash_update_contact
 * ========================================================================== */

TEST_F(ContactIndexRecoveryTest, UpdateNameKeepsSector)
{
    uint16_t idx = insert("Ada", "0411111111");

    ContactBuffer old_c = create_contact("Ada", "0411111111");
    ContactBuffer new_c = create_contact("Ada Lovelace", "0411111111");
    ASSERT_EQ(hash_update_contact(&table, &journal, &old_c.contact, &new_c.contact), STRG_OK);

    HashEntry *e = find("0411111111");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->sector, idx);

    ContactBuffer out{};
    ASSERT_TRUE(hash_find_contact(&table, "0411111111", &out));
    EXPECT_EQ(std::string(out.contact.name, out.contact.name_len), "Ada Lovelace");
}

TEST_F(ContactIndexRecoveryTest, UpdateMissingContactReturnsEmpty)
{
    ContactBuffer old_c = create_contact("Ada", "0411111111");
    ContactBuffer new_c = create_contact("Ada", "0411111111");
    EXPECT_EQ(hash_update_contact(&table, &journal, &old_c.contact, &new_c.contact), STRG_EMPTY);
}

TEST_F(ContactIndexRecoveryTest, UpdatePhoneMovesEntryAndKeepsChat)
{
    uint16_t idx = insert("Ada", "0411111111");
    MessageBuffer m = create_message(70000, true, (char *)"hello");
    ASSERT_TRUE(hash_insert_message(&table, &journal, "0411111111", &m));
    const uint16_t chat = find("0411111111")->latest_msg_extent;

    ContactBuffer old_c = create_contact("Ada", "0411111111");
    ContactBuffer new_c = create_contact("Ada", "0499999999");
    ASSERT_EQ(hash_update_contact(&table, &journal, &old_c.contact, &new_c.contact), STRG_OK);

    EXPECT_EQ(find("0411111111"), nullptr);
    HashEntry *e = find("0499999999");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->sector, idx);
    EXPECT_EQ(e->latest_msg_extent, chat);
    EXPECT_EQ(table.num_elems, 1u);

    MessageBuffer out{};
    ASSERT_TRUE(hash_find_message(&table, "0499999999", &out));
    EXPECT_EQ(std::memcmp(out.buffer, m.buffer, sizeof(Message)), 0);
}

TEST_F(ContactIndexRecoveryTest, UpdatePhoneToExistingPhoneReturnsExists)
{
    uint16_t ada = insert("Ada", "0411111111");
    uint16_t bob = insert("Bob", "0422222222");

    ContactBuffer old_c = create_contact("Ada", "0411111111");
    ContactBuffer new_c = create_contact("Ada", "0422222222");
    EXPECT_EQ(hash_update_contact(&table, &journal, &old_c.contact, &new_c.contact), STRG_EXISTS);

    ASSERT_NE(find("0411111111"), nullptr);
    ASSERT_NE(find("0422222222"), nullptr);
    EXPECT_EQ(find("0411111111")->sector, ada);
    EXPECT_EQ(find("0422222222")->sector, bob);

    ContactBuffer out{};
    ASSERT_TRUE(hash_find_contact(&table, "0422222222", &out));
    EXPECT_EQ(std::string(out.contact.name, out.contact.name_len), "Bob");
}

TEST_F(ContactIndexRecoveryTest, UpdatePhoneSurvivesReboot)
{
    uint16_t idx = insert("Ada", "0411111111");
    for (uint32_t i = 0; i < MESSAGE_SECTOR_CAPACITY + 1; i++) // two-sector chat
    {
        MessageBuffer m = create_message(i + 1, true, (char *)"msg");
        ASSERT_TRUE(hash_insert_message(&table, &journal, "0411111111", &m));
    }
    const uint16_t chat = find("0411111111")->latest_msg_extent;

    ContactBuffer old_c = create_contact("Ada", "0411111111");
    ContactBuffer new_c = create_contact("Ada", "0499999999");
    ASSERT_EQ(hash_update_contact(&table, &journal, &old_c.contact, &new_c.contact), STRG_OK);

    reboot();
    ASSERT_TRUE(reconstruct());

    EXPECT_EQ(find("0411111111"), nullptr) << "phantom contact for the old phone";
    HashEntry *e = find("0499999999");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->sector, idx);
    EXPECT_EQ(e->latest_msg_extent, chat);
    EXPECT_EQ(table.num_elems, 1u);
    EXPECT_EQ(free_list_used(&message_fls), 2u);
}

/* ============================================================================
 * Message layout
 * ========================================================================== */

TEST_F(ContactIndexRecoveryTest, TimestampAbove16BitsRoundTrips)
{
    const uint32_t ts = 0x12345678u;
    MessageBuffer m = create_message(ts, true, (char *)"t");

    EXPECT_EQ(((uint32_t)m.msg.timestamp_hi << 16) | m.msg.timestamp_lo, ts);
}

TEST_F(ContactIndexRecoveryTest, NewMessageSectorStoresContactIndex)
{
    MessageSectorBuffer s{};
    create_new_message_sector(&s, 1234, UINT16_MAX);

    EXPECT_EQ(s.var.header.contact_index, 1234);
    EXPECT_EQ(s.var.header.next, UINT16_MAX);
    EXPECT_EQ(s.var.header.prev, UINT16_MAX);
    EXPECT_EQ(s.var.header.msg_count, 0);
}

TEST_F(ContactIndexRecoveryTest, HashInsertMessageWritesContactIndexHeader)
{
    uint16_t idx = insert("Ada", "0411111111");
    MessageBuffer m = create_message(1, true, (char *)"hi");
    ASSERT_TRUE(hash_insert_message(&table, &journal, "0411111111", &m));

    MessageSectorBuffer s{};
    ASSERT_EQ(read_message_sector(storage, find("0411111111")->latest_msg_extent, &s), STRG_OK);
    EXPECT_EQ(s.var.header.contact_index, idx);
}

/* ============================================================================
 * Reconstruction: attaching chats and removing orphans
 * ========================================================================== */

TEST_F(ContactIndexRecoveryTest, ReconstructAttachesChatByContactIndex)
{
    uint16_t idx = insert("Ada", "0411111111");
    write_chat(idx, {20, 7, 33}); // head = 33

    reboot();
    ASSERT_TRUE(reconstruct());

    HashEntry *e = find("0411111111");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->latest_msg_extent, 33);
    EXPECT_EQ(free_list_used(&message_fls), 3u);
    EXPECT_EQ(distinct_free_message_sectors(), (size_t)TOTAL_MESSAGE_SECTOR_SIZE - 3);
}

TEST_F(ContactIndexRecoveryTest, OrphanSingleSectorChatIsFreed)
{
    write_chat(500, {12}); // contact slot 500 was never written

    reboot();
    ASSERT_TRUE(reconstruct());

    expect_message_region_fully_free();
}

/**
 * @brief Older sectors with a higher index than the head are cleared but
 *        must only be freed once (by the reconstruction range free).
 */
TEST_F(ContactIndexRecoveryTest, OrphanChatWithHigherPrevIndexFreedExactlyOnce)
{
    write_chat(500, {40, 25, 9, 5}); // head = 5, all older sectors have higher indices

    reboot();
    ASSERT_TRUE(reconstruct());

    expect_message_region_fully_free();
}

TEST_F(ContactIndexRecoveryTest, OrphanChatWithMixedPrevIndicesFreedExactlyOnce)
{
    write_chat(500, {3, 50, 1, 20}); // head = 20, older sectors both sides of it

    reboot();
    ASSERT_TRUE(reconstruct());

    expect_message_region_fully_free();
}

TEST_F(ContactIndexRecoveryTest, OrphanRemovalLeavesLiveChatsIntact)
{
    uint16_t idx = insert("Ada", "0411111111");
    write_chat(idx, {4, 30});        // live chat, head = 30
    write_chat(500, {31, 2, 10});    // orphan, head = 10

    reboot();
    ASSERT_TRUE(reconstruct());

    HashEntry *e = find("0411111111");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->latest_msg_extent, 30);
    EXPECT_TRUE(check_usage_bit(MESSAGE_DATA_SECTOR(4)));
    EXPECT_TRUE(check_usage_bit(MESSAGE_DATA_SECTOR(30)));
    for (uint16_t s : {31, 2, 10})
    {
        EXPECT_FALSE(check_usage_bit(MESSAGE_DATA_SECTOR(s))) << "orphan sector " << s;
    }
    EXPECT_EQ(free_list_used(&message_fls), 2u);
    EXPECT_EQ(distinct_free_message_sectors(), (size_t)TOTAL_MESSAGE_SECTOR_SIZE - 2);
}

TEST_F(ContactIndexRecoveryTest, OrphanRemovalPersistsAcrossSecondReboot)
{
    write_chat(500, {40, 5});

    reboot();
    ASSERT_TRUE(reconstruct());
    reboot();
    ASSERT_TRUE(reconstruct());

    expect_message_region_fully_free();
}

/**
 * @brief Power cut part way through an orphan removal: the head's bit was
 *        cleared but older sectors remain. A sector whose next is no longer in
 *        use must be treated as a head so the rest is cleaned up.
 */
TEST_F(ContactIndexRecoveryTest, PartiallyRemovedOrphanChatCleanedUpNextBoot)
{
    write_chat(500, {15, 8, 3}); // head = 3
    ASSERT_EQ(update_usage_bit(storage, MESSAGE_DATA_SECTOR(3), false), STRG_OK);

    reboot();
    ASSERT_TRUE(reconstruct());

    expect_message_region_fully_free();
}

TEST_F(ContactIndexRecoveryTest, RemovedContactChatIsFreedOnReboot)
{
    insert("Ada", "0411111111");
    MessageBuffer m = create_message(1, true, (char *)"hi");
    ASSERT_TRUE(hash_insert_message(&table, &journal, "0411111111", &m));
    ASSERT_TRUE(hash_remove(&table, &journal, "0411111111", nullptr));

    reboot();
    ASSERT_TRUE(reconstruct());

    EXPECT_EQ(find("0411111111"), nullptr);
    expect_message_region_fully_free();
}

/**
 * @brief An SD read error on the contact must not delete the chat: the
 *        contact may be fine on the next boot.
 */
TEST_F(ContactIndexRecoveryTest, ContactReadErrorKeepsChat)
{
    uint16_t idx = insert("Ada", "0411111111");
    write_chat(idx, {6, 2});

    reboot();
    ASSERT_TRUE(hash_reconstruct_contact(&table));

    g_fail_read_raw = DATA_SECTOR_TO_RAW(CONTACT_DATA_START_SECTOR + idx / CONTACT_SECTOR_CAPACITY);
    EXPECT_FALSE(hash_reconstruct_message(&table, &journal)) << "read error must abort the rebuild";
    g_fail_read_raw = -1;

    EXPECT_TRUE(check_usage_bit(MESSAGE_DATA_SECTOR(6)));
    EXPECT_TRUE(check_usage_bit(MESSAGE_DATA_SECTOR(2))) << "chat deleted on a transient read error";
}
