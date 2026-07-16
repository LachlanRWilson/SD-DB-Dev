#include <gtest/gtest.h>
#include <climits>
#include <cstring>

extern "C" {

#include "hash_table.h"
#include "free_list_stack.h"
#include "heap_storage.h"

}


/**
 * @brief Edge-case fixture for HashTable tests.
 *
 * Provides:
 * - Heap-backed storage
 * - Hash entry memory
 * - Free list allocator
 *
 * Each test receives an isolated database environment.
 */
class HashTableEdgeTest : public ::testing::Test
{
protected:
    HashTable table;

    HashEntry *entries = nullptr;
    FreeList freelist;

    uint16_t *pool = nullptr;

    void SetUp() override
    {
        entries = new HashEntry[HASH_TABLE_SIZE];

        ASSERT_NE(entries, nullptr);

        std::memset(entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        pool = new uint16_t[HASH_TABLE_SIZE];

        ASSERT_NE(pool, nullptr);

        ASSERT_TRUE( free_list_init( &freelist, pool, HASH_TABLE_SIZE));

        hash_init( &table, &freelist, entries, HASH_TABLE_SIZE);
    }

    void TearDown() override
    {
        hash_clear(&table);

        delete[] entries;
        delete[] freelist.free_stack;
    }
};


/**
 * @brief Verify hash_find_entry returns the correct entry pointer.
 */
TEST_F(HashTableEdgeTest, FindEntry)
{
    EXPECT_TRUE(hash_insert(&table, 123));

    HashEntry *entry = nullptr;

    EXPECT_TRUE(hash_find_entry(&table, 123, &entry));

    ASSERT_NE(entry, nullptr);

    EXPECT_EQ(entry->id, 123);
}

/**
 * @brief Verify hash_find_entry fails for missing IDs.
 */
TEST_F(HashTableEdgeTest, FindEntryMissing)
{
    HashEntry *entry = nullptr;

    EXPECT_FALSE(hash_find_entry(&table, 9999, &entry));

    EXPECT_EQ(entry, nullptr);
}

/**
 * @brief Verify hash_find_message returns the stored message extent.
 */
TEST_F(HashTableEdgeTest, FindMessage)
{
    EXPECT_TRUE(hash_insert(&table, 50));

    HashEntry *entry = nullptr;
    ASSERT_TRUE(hash_find_entry(&table, 50, &entry));

    entry->latest_msg_extent = 1234;

    EXPECT_EQ(hash_find_message(&table, 50), 1234);
}

/**
 * @brief Verify removing an entry returns the removed hash entry.
 */
TEST_F(HashTableEdgeTest, RemoveReturnsEntry)
{
    EXPECT_TRUE(hash_insert(&table, 10));

    HashEntry *removed = nullptr;

    EXPECT_TRUE(hash_remove(&table, 10, &removed));

    ASSERT_NE(removed, nullptr);

    EXPECT_EQ(removed->id, 10);
}

/**
 * @brief Verify removing a missing entry sets removed to nullptr.
 */
TEST_F(HashTableEdgeTest, RemoveMissingReturnsNull)
{
    HashEntry *removed = reinterpret_cast<HashEntry *>(1);

    EXPECT_FALSE(hash_remove(&table, 1000, &removed));

}

TEST_F(HashTableEdgeTest, IndependentMessageExtents)
{
    EXPECT_TRUE(hash_insert(&table, 1));
    EXPECT_TRUE(hash_insert(&table, 2));

    HashEntry *a = nullptr;
    HashEntry *b = nullptr;

    ASSERT_TRUE(hash_find_entry(&table, 1, &a));
    ASSERT_TRUE(hash_find_entry(&table, 2, &b));

    a->latest_msg_extent = 5;
    b->latest_msg_extent = 42;

    EXPECT_EQ(hash_find_message(&table, 1), 5);
    EXPECT_EQ(hash_find_message(&table, 2), 42);
}
