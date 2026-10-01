#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include "hash_table.h"
#include "iterator.h"
#include "contact.h"
#include "message.h"
#include "storage.h"
#include "free_list_stack.h"
#include "heap_storage.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
}

static ContactBuffer make_contact_iter(const std::string &name, const std::string &phone)
{
    ContactBuffer contact{};
    contact.contact.name_len = static_cast<uint8_t>(name.length());
    memcpy(contact.contact.name, name.c_str(), contact.contact.name_len);
    contact.contact.phone_len = static_cast<uint8_t>(phone.length());
    memcpy(contact.contact.phone, phone.c_str(), contact.contact.phone_len);
    return contact;
}

/**
 * @brief Same on-disk layout as HashTableTest in test_hash_table.cpp; only
 *        used here to exercise hash_table_iterator_init/_next/_prev/_get
 *        over table->htable, so no message-store setup is needed.
 */
class HashTableIteratorTest : public ::testing::Test
{
protected:
    HashTable htable{};
    HashEntry *entries = nullptr;

    Storage *storage = &heap_storage;
    HeapStorageContext storage_ctx{};

    Journal journal{};

    FreeList contact_allocator{};
    FreeList message_allocator{};
    uint16_t *contact_fls_pool = nullptr;
    uint16_t *message_fls_pool = nullptr;

    uint8_t *storage_mem = nullptr;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        SUPERHEADER_SECTOR_SIZE +
        USAGE_BITMAP_SECTOR_SIZE +
        JRNL_SECTOR_SIZE +
        TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        entries = new HashEntry[HASH_TABLE_SIZE];
        ASSERT_NE(entries, nullptr);
        std::memset(entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        contact_fls_pool = new uint16_t[HASH_TABLE_SIZE];
        ASSERT_NE(contact_fls_pool, nullptr);

        message_fls_pool = new uint16_t[TOTAL_MESSAGE_SECTOR_SIZE];
        ASSERT_NE(message_fls_pool, nullptr);

        storage_mem = new uint8_t[static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT];
        ASSERT_NE(storage_mem, nullptr);
        std::memset(storage_mem, 0,
                    static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(HeapStorage_Init(&storage_ctx, storage_mem, SECTOR_SIZE,
                                     STORAGE_SECTOR_COUNT));
        storage->context = &storage_ctx;

        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));

        ASSERT_TRUE(free_list_init(&contact_allocator, contact_fls_pool, HASH_TABLE_SIZE));
        ASSERT_TRUE(free_list_init(&message_allocator, message_fls_pool, TOTAL_MESSAGE_SECTOR_SIZE));

        hash_init(&htable, storage, &contact_allocator, &message_allocator, entries, HASH_TABLE_SIZE);
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
        ContactBuffer c = make_contact_iter(name, phone);
        return hash_insert_contact(&htable, &journal, &c);
    }

    bool remove(const std::string &phone)
    {
        ContactBuffer removed{};
        return hash_remove_contact(&htable, &journal, phone.c_str(), &removed);
    }
};

/**
 * @brief A freshly initialised iterator has no current position until it
 *        has been advanced with next() or prev().
 */
TEST_F(HashTableIteratorTest, GetFailsBeforeFirstAdvance)
{
    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    uint16_t value = 0;
    EXPECT_FALSE(iterator_get_fn(&it, &value));
}

/**
 * @brief An iterator over an empty table is immediately exhausted in both
 *        directions.
 */
TEST_F(HashTableIteratorTest, EmptyTableFindsNothing)
{
    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    EXPECT_FALSE(iterator_next_fn(&it));
    EXPECT_FALSE(iterator_prev_fn(&it));
}

/**
 * @brief next() visits every occupied slot in ascending index order and
 *        reports exhaustion once done; get() reports the slot index, not
 *        the entry contents.
 */
TEST_F(HashTableIteratorTest, NextVisitsOccupiedSlotsInAscendingOrder)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Carol", "0433333333"));

    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    std::vector<uint16_t> visited;
    while (iterator_next_fn(&it))
    {
        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        ASSERT_LT(value, HASH_TABLE_SIZE);
        EXPECT_EQ(entries[value].state, ENTRY_OCCUPIED);
        visited.push_back(value);
    }

    EXPECT_EQ(visited.size(), 3u);
    EXPECT_TRUE(std::is_sorted(visited.begin(), visited.end()));

    uint16_t value = 0;
    EXPECT_FALSE(iterator_get_fn(&it, &value));
}

/**
 * @brief prev() visits every occupied slot in descending index order.
 */
TEST_F(HashTableIteratorTest, PrevVisitsOccupiedSlotsInDescendingOrder)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Carol", "0433333333"));

    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    std::vector<uint16_t> visited;
    while (iterator_prev_fn(&it))
    {
        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        visited.push_back(value);
    }

    EXPECT_EQ(visited.size(), 3u);
    EXPECT_TRUE(std::is_sorted(visited.rbegin(), visited.rend()));
}

/**
 * @brief A tombstoned (ENTRY_DELETED) slot must not be visited by next().
 */
TEST_F(HashTableIteratorTest, NextSkipsTombstonedSlots)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Carol", "0433333333"));
    ASSERT_TRUE(remove("0422222222"));

    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    int visited = 0;
    while (iterator_next_fn(&it))
    {
        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        EXPECT_EQ(entries[value].state, ENTRY_OCCUPIED);
        visited++;
    }

    EXPECT_EQ(visited, 2);
}

/**
 * @brief next() must not repeat the slot it is already positioned on.
 */
TEST_F(HashTableIteratorTest, NextDoesNotRepeatCurrentSlot)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(insert("Bob", "0422222222"));

    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    uint16_t first = 0;
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &first));

    uint16_t second = 0;
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &second));

    EXPECT_NE(first, second);
}

/**
 * @brief prev() called after next() reaches the last slot walks back to the
 *        same slot next() started from, and then exhausts.
 */
TEST_F(HashTableIteratorTest, NextThenPrevReturnsToStart)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(insert("Bob", "0422222222"));

    HashTableIteratorCtx ctx;
    Iterator it = hash_table_iterator_init(&ctx, &htable);

    uint16_t first = 0, second = 0, back = 0;
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &first));
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &second));

    ASSERT_TRUE(iterator_prev_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &back));
    EXPECT_EQ(back, first);

    EXPECT_FALSE(iterator_prev_fn(&it));
}
