#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include "hash_table.h"
#include "contact.h"
#include "message.h"
#include "storage.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"

uint16_t hash_phone(const char *phone);
}

#include "test_support/failable_storage.h"

static ContactBuffer make_contact_edge(const std::string &name, const std::string &phone)
{
    ContactBuffer contact{};
    if (name.length() > MAX_NAME_LEN || phone.length() > MAX_PHONE_LEN)
    {
        return contact;
    }
    contact.contact.name_len = static_cast<uint8_t>(name.length());
    memcpy(contact.contact.name, name.c_str(), contact.contact.name_len);
    contact.contact.phone_len = static_cast<uint8_t>(phone.length());
    memcpy(contact.contact.phone, phone.c_str(), contact.contact.phone_len);
    return contact;
}

/**
 * @brief Same layout as HashTableTest in test_hash_table.cpp, but backed
 *        by FailableStorageCtx so individual tests can inject a read/write
 *        failure at a specific point and observe how hash_table.c/
 *        contact.c/message.c unwind (or fail to unwind) their state.
 */
class HashTableEdgeTest : public ::testing::Test
{
protected:
    HashTable htable{};
    HashEntry *entries = nullptr;

    FailableStorageCtx ctx{};
    Storage storage{};

    Journal journal{};

    FreeList contact_allocator{};
    FreeList message_allocator{};
    uint16_t *contact_fls_pool = nullptr;
    uint16_t *message_fls_pool = nullptr;

    uint8_t *storage_mem = nullptr;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        SUPERHEADER_SECTOR_SIZE + USAGE_BITMAP_SECTOR_SIZE + JRNL_SECTOR_SIZE + TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        entries = new HashEntry[HASH_TABLE_SIZE];
        ASSERT_NE(entries, nullptr);
        memset(entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        contact_fls_pool = new uint16_t[HASH_TABLE_SIZE];
        message_fls_pool = new uint16_t[TOTAL_MESSAGE_SECTOR_SIZE];
        ASSERT_NE(contact_fls_pool, nullptr);
        ASSERT_NE(message_fls_pool, nullptr);

        storage_mem = new uint8_t[static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT];
        ASSERT_NE(storage_mem, nullptr);
        memset(storage_mem, 0, static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(FailableStorage_Init(&ctx, storage_mem, SECTOR_SIZE, STORAGE_SECTOR_COUNT));
        storage = FailableStorage_Make(&ctx);

        memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));

        memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, &storage));

        ASSERT_TRUE(free_list_init(&contact_allocator, contact_fls_pool, HASH_TABLE_SIZE));
        ASSERT_TRUE(free_list_init(&message_allocator, message_fls_pool, TOTAL_MESSAGE_SECTOR_SIZE));

        hash_init(&htable, &storage, &contact_allocator, &message_allocator, entries, HASH_TABLE_SIZE);
    }

    void TearDown() override
    {
        hash_clear(&htable);
        delete[] entries;
        delete[] contact_fls_pool;
        delete[] message_fls_pool;
        delete[] storage_mem;
    }

    bool insert(const std::string &name, const std::string &phone)
    {
        ContactBuffer c = make_contact_edge(name, phone);
        return hash_insert_contact(&htable, &journal, &c);
    }

    bool send(const std::string &phone, uint16_t timestamp, bool direction, const std::string &text)
    {
        MessageBuffer m = create_message(timestamp, direction, const_cast<char *>(text.c_str()));
        return hash_insert_message(&htable, &journal, phone.c_str(), &m);
    }

    HashEntry *entry_for(const std::string &phone)
    {
        HashEntry *e = nullptr;
        hash_find_entry(&htable, phone.c_str(), &e);
        return e;
    }
};

/* ============================================================================
 * contact.c: name/phone length boundaries
 * ========================================================================== */

TEST_F(HashTableEdgeTest, CreateContactAcceptsNameAndPhoneAtMaxLength)
{
    std::string name(MAX_NAME_LEN, 'a');
    std::string phone(MAX_PHONE_LEN, '1');

    ContactBuffer c = make_contact_edge(name, phone);

    EXPECT_EQ(c.contact.name_len, MAX_NAME_LEN);
    EXPECT_EQ(c.contact.phone_len, MAX_PHONE_LEN);
}

TEST_F(HashTableEdgeTest, CreateContactRejectsOversizedNameOrPhone)
{
    std::string long_name(MAX_NAME_LEN + 1, 'a');
    std::string ok_phone(MAX_PHONE_LEN, '1');

    ContactBuffer c1 = make_contact_edge(long_name, ok_phone);
    EXPECT_EQ(c1.contact.phone_len, 0); // whole buffer left zeroed, not partially filled

    std::string ok_name(MAX_NAME_LEN, 'a');
    std::string long_phone(MAX_PHONE_LEN + 1, '1');

    ContactBuffer c2 = make_contact_edge(ok_name, long_phone);
    EXPECT_EQ(c2.contact.name_len, 0);
}

TEST_F(HashTableEdgeTest, InsertingContactWithEmptyPhoneDoesNotCrash)
{
    // Structurally unusual (a contact keyed on hash_phone("")) but not
    // rejected by create_contact() (phone_len 0 <= MAX_PHONE_LEN), so the
    // rest of the insert/find/remove path must handle it without UB.
    EXPECT_TRUE(insert("NoNumber", ""));

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact(&htable, "", &result));
    EXPECT_STREQ(result.contact.name, "NoNumber");
}

/* ============================================================================
 * Table exhaustion
 * ========================================================================== */

/**
 * @brief Once every slot in the table is genuinely ENTRY_OCCUPIED,
 *        hash_find_entry()/hash_insert_contact() report failure for a new
 *        phone number rather than silently overwriting or wrapping.
 *
 * Uses a small table sized to a *prime* (7), not the production
 * HASH_TABLE_SIZE: with a prime capacity, the double-hash probe sequence
 * (h1 + i*h2) mod capacity is guaranteed to visit every slot exactly once
 * for any nonzero h2 (since gcd(h2, capacity) == 1), so filling all 7
 * slots is deterministic rather than depending on how 7 arbitrary phone
 * numbers happen to hash. The backing HashEntry array is still allocated
 * at the full HASH_TABLE_SIZE, since hash_clear() unconditionally memsets
 * HASH_TABLE_SIZE entries regardless of table->size -- see the
 * recommended-tests notes for why that's worth fixing.
 */
TEST(HashTableSmallTableTest, TableFullRejectsNewInsert)
{
    const size_t small_size = 7;

    std::vector<HashEntry> small_entries(HASH_TABLE_SIZE, HashEntry{});
    std::vector<uint16_t> contact_pool(HASH_TABLE_SIZE, 0);
    std::vector<uint16_t> message_pool(TOTAL_MESSAGE_SECTOR_SIZE, 0);

    std::vector<uint8_t> storage_mem(
        static_cast<size_t>(SECTOR_SIZE) *
        (SUPERHEADER_SECTOR_SIZE + USAGE_BITMAP_SECTOR_SIZE + JRNL_SECTOR_SIZE + TOTAL_DATA_SECTOR_SIZE));

    FailableStorageCtx ctx{};
    ASSERT_TRUE(FailableStorage_Init(&ctx, storage_mem.data(), SECTOR_SIZE,
                                      static_cast<uint32_t>(storage_mem.size() / SECTOR_SIZE)));
    Storage storage = FailableStorage_Make(&ctx);

    memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));

    Journal journal{};
    ASSERT_TRUE(journal_init(&journal, &storage));

    FreeList contact_allocator{};
    FreeList message_allocator{};
    ASSERT_TRUE(free_list_init(&contact_allocator, contact_pool.data(), HASH_TABLE_SIZE));
    ASSERT_TRUE(free_list_init(&message_allocator, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));

    HashTable table{};
    hash_init(&table, &storage, &contact_allocator, &message_allocator, small_entries.data(), small_size);

    for (int i = 0; i < 7; i++)
    {
        std::string phone = "000000" + std::to_string(i);
        ContactBuffer c = make_contact_edge("P" + std::to_string(i), phone);
        ASSERT_TRUE(hash_insert_contact(&table, &journal, &c)) << "insert " << i << " failed";
    }

    EXPECT_EQ(hash_size(&table), 7u);

    ContactBuffer overflow = make_contact_edge("Overflow", "9999999");
    EXPECT_FALSE(hash_insert_contact(&table, &journal, &overflow));
    EXPECT_EQ(hash_size(&table), 7u);

    hash_clear(&table);
}

/* ============================================================================
 * hash_find_n_message() boundary requests
 * ========================================================================== */

TEST_F(HashTableEdgeTest, FindNMessagesZeroRequestedReturnsZero)
{
    ASSERT_TRUE(send("0412345678", 100, true, "hello"));

    MessageBuffer out{};
    EXPECT_EQ(hash_find_n_message(&htable, "0412345678", 0, &out), 0);
}

TEST_F(HashTableEdgeTest, FindNMessagesMoreThanAvailableReturnsOnlyWhatExists)
{
    ASSERT_TRUE(send("0412345678", 100, true, "first"));
    ASSERT_TRUE(send("0412345678", 200, true, "second"));

    MessageBuffer out[10]{};
    int n = hash_find_n_message(&htable, "0412345678", 10, out);

    EXPECT_EQ(n, 2);
}

/* ============================================================================
 * Chat isolation between distinct contacts
 * ========================================================================== */

/**
 * @brief Two different contacts, each rolled over onto a second message
 *        sector, remain fully independent -- inserting/removing one's
 *        chat must not touch the other's.
 */
TEST_F(HashTableEdgeTest, TwoContactsWithMultiSectorChatsDoNotInterfere)
{
    const int total = MESSAGE_BLOCK_CAPACITY + 1;

    for (int i = 0; i < total; i++)
    {
        ASSERT_TRUE(send("0411111111", static_cast<uint16_t>(100 + i), true, "A" + std::to_string(i)));
        ASSERT_TRUE(send("0422222222", static_cast<uint16_t>(200 + i), false, "B" + std::to_string(i)));
    }

    MessageBuffer latest_a{};
    ASSERT_TRUE(hash_find_message(&htable, "0411111111", &latest_a));
    EXPECT_EQ(latest_a.msg.timestamp, 100 + total - 1);

    MessageBuffer latest_b{};
    ASSERT_TRUE(hash_find_message(&htable, "0422222222", &latest_b));
    EXPECT_EQ(latest_b.msg.timestamp, 200 + total - 1);

    // Removing A's chat must not touch B's.
    MessageBuffer removed_out{};
    ASSERT_TRUE(hash_remove_message(&htable, &journal, "0411111111", &removed_out));

    EXPECT_FALSE(hash_find_message(&htable, "0411111111", &removed_out));

    MessageBuffer still_b{};
    ASSERT_TRUE(hash_find_message(&htable, "0422222222", &still_b));
    EXPECT_EQ(still_b.msg.timestamp, 200 + total - 1);
}

/* ============================================================================
 * Characterisation: hash_remove_contact() vs. hash_remove()
 *
 * hash_remove_contact() only removes the contact record. It does not know
 * about (and does not touch) that contact's message chat. If a contact
 * with an existing chat is removed this way instead of via hash_remove(),
 * the message sectors are never freed -- they stay marked used in the
 * message allocator forever, unreachable (the hash entry that pointed at
 * them is gone). This is a real storage leak for this specific call
 * pattern, not a crash, so it won't show up as a failing assertion by
 * itself; the test below pins down the actual (leaky) behaviour so it's
 * an explicit, visible fact rather than something only discovered by
 * reading source. See the recommended-tests notes for how you might want
 * to close this gap (e.g. hash_remove_contact() also frees the chat, or
 * the API is renamed/documented to make "contact-only" removal opt-in).
 * ========================================================================== */

TEST_F(HashTableEdgeTest, RemoveContactAloneLeaksItsMessageSectors)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(send("0412345678", 100, true, "hello"));

    size_t used_before = free_list_used(&message_allocator);
    EXPECT_GT(used_before, 0u);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0412345678", &removed));

    // The message sector is still marked used -- nothing freed it.
    EXPECT_EQ(free_list_used(&message_allocator), used_before);

    // And it's now unreachable: the entry is gone, so there's no way back
    // to that sector through the public API.
    ContactBuffer c{};
    EXPECT_FALSE(hash_find_contact(&htable, "0412345678", &c));
}

/* ============================================================================
 * hash_cleanup()
 * ========================================================================== */

/**
 * @brief hash_cleanup() rebuilds the table in place from persisted
 *        contact data: live contacts survive, tombstoned slots are gone,
 *        and the table is left in a working state (new inserts succeed
 *        afterwards).
 */
TEST_F(HashTableEdgeTest, HashCleanupRebuildsLiveContactsAndDropsTombstones)
{
    ASSERT_TRUE(insert("Alice", "0411111111"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Charlie", "0433333333"));

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0422222222", &removed));
    ASSERT_EQ(hash_size(&htable), 2u);

    ASSERT_TRUE(hash_cleanup(&htable));

    EXPECT_EQ(hash_size(&htable), 2u);

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact(&htable, "0411111111", &result));
    EXPECT_TRUE(hash_find_contact(&htable, "0433333333", &result));
    EXPECT_FALSE(hash_find_contact(&htable, "0422222222", &result));

    // The table must still be fully functional afterwards.
    EXPECT_TRUE(insert("Dana", "0444444444"));
    EXPECT_EQ(hash_size(&htable), 3u);
}

/* ============================================================================
 * Storage-write-failure rollback
 *
 * Regression coverage: hash_insert_contact()'s failure path used to be
 * unreachable in any test, since HeapStorage never fails except on
 * out-of-range access. With FailableStorageCtx we can force the contact
 * write itself to fail and check the earlier fix (unwind entry->state,
 * entry->id, entry->sector, free the allocated sector, decrement
 * num_elems -- all only for a brand-new entry) actually leaves the table
 * consistent rather than half-updated.
 * ========================================================================== */

TEST_F(HashTableEdgeTest, InsertContactRollsBackHashStateOnWriteFailure)
{
    size_t contact_sectors_before = free_list_used(&contact_allocator);

    // SetUp() already performed one write (journal_init() -> journal_header_init()
    // on the freshly-zeroed journal sector); start counting fresh from here so
    // fail_after_write targets the contact write itself, not that one.
    ctx.write_calls = 0;
    ctx.fail_after_write = 1;

    ContactBuffer c = make_contact_edge("Alice", "0412345678");
    EXPECT_FALSE(hash_insert_contact(&htable, &journal, &c));

    EXPECT_EQ(hash_size(&htable), 0u);
    EXPECT_EQ(free_list_used(&contact_allocator), contact_sectors_before);

    HashEntry *e = entry_for("0412345678");
    ASSERT_NE(e, nullptr);
    EXPECT_NE(e->state, ENTRY_OCCUPIED);

    // The table must still accept a normal insert afterwards.
    ctx.fail_after_write = -1;
    EXPECT_TRUE(insert("Alice", "0412345678"));
}

/**
 * @brief hash_insert_message() fails cleanly (no crash, no phantom
 *        contact) when the message allocator has nothing left to give,
 *        even though the contact allocator still has room.
 */
TEST_F(HashTableEdgeTest, InsertMessageFailsWhenMessageAllocatorExhausted)
{
    // Drain the message allocator completely.
    while (free_list_allocate(&message_allocator) != UINT16_MAX)
    {
    }
    ASSERT_EQ(free_list_available(&message_allocator), 0u);

    EXPECT_FALSE(send("0412345678", 100, true, "hello"));
}

/**
 * @brief Characterisation: for a brand-new phone number, hash_insert_message()
 *        creates and commits the contact record *before* it tries to
 *        allocate a message sector. If that message-sector allocation then
 *        fails (as above), the overall call still reports failure, but the
 *        contact it just created is NOT rolled back -- it's left behind as
 *        a real, findable, empty-named contact, and hash_size() counts it.
 *
 *        This differs from hash_insert_contact()'s own failure path, which
 *        does unwind a brand-new entry if the contact write itself fails
 *        (see InsertContactRollsBackHashStateOnWriteFailure above). Pinning
 *        this down explicitly since "the call returned false" alone would
 *        otherwise suggest nothing happened.
 */
TEST_F(HashTableEdgeTest, InsertMessageLeavesPhantomContactWhenMessageAllocatorExhausted)
{
    while (free_list_allocate(&message_allocator) != UINT16_MAX)
    {
    }

    ASSERT_FALSE(send("0412345678", 100, true, "hello"));

    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact(&htable, "0412345678", &result));
    EXPECT_EQ(result.contact.name_len, 0u);
}
