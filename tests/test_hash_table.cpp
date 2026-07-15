#include <gtest/gtest.h>
#include <climits>

extern "C" {
#include "hash_table.h"
}

/**
 * @brief Test fixture for HashTable unit tests
 *
 * Provides a fresh hash table, freelist allocator, and backing storage
 * for each test case. Ensures deterministic behaviour across all tests.
 */
class HashTableTest : public ::testing::Test
{
protected:
    HashTable table;        /**< Hash table instance under test */
    HashEntry *entries;     /**< Backing storage for hash entries */
    FreeList freelist;      /**< Free list allocator for sector management */

    /**
     * @brief Initialise test environment before each test
     *
     * Allocates software-backed memory, initialises the free list allocator,
     * and configures the hash table for deterministic testing.
     */
    void SetUp() override
    {
        entries = hash_create_software();
        ASSERT_NE(entries, nullptr);

        ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

        hash_create(&table, &freelist, entries, HASH_TABLE_SIZE);
    }

    /**
     * @brief Clean up after each test
     *
     * Clears the hash table and releases any allocated test resources.
     */
    void TearDown() override
    {
        hash_clear(&table);

        hash_destroy_software(&table, nullptr);
    }
};

/**
 * @brief Verify that basic insertion works correctly
 *
 * Inserts two unique keys and ensures the size is updated accordingly.
 */
TEST_F(HashTableTest, InsertBasic)
{
    EXPECT_TRUE(hash_insert(&table, 100));
    EXPECT_TRUE(hash_insert(&table, 200));

    EXPECT_EQ(hash_size(&table), 2u);
}

/**
 * @brief Verify that duplicate inserts do not increase table size
 *
 * Ensures idempotent behaviour when inserting an existing key.
 */
TEST_F(HashTableTest, DuplicateInsert)
{
    hash_insert(&table, 55);

    size_t before = hash_size(&table);

    EXPECT_TRUE(hash_insert(&table, 55));

    EXPECT_EQ(hash_size(&table), before);
}

/**
 * @brief Verify that existing entries can be found
 *
 * Confirms that inserted keys can be retrieved and mapped to a sector.
 */
TEST_F(HashTableTest, FindExisting)
{
    hash_insert(&table, 1234);

    uint32_t sector = hash_find(&table, 1234);

    EXPECT_NE(sector, UINT32_MAX);
}

/**
 * @brief Verify that missing entries return UINT32_MAX
 *
 * Ensures correct failure behaviour when searching for non-existent keys.
 */
TEST_F(HashTableTest, FindMissing)
{
    EXPECT_EQ(hash_find(&table, 99999), UINT32_MAX);
}

/**
 * @brief Verify that removal of entries works correctly
 *
 * Ensures removed entries are no longer accessible via hash_find().
 */
TEST_F(HashTableTest, RemoveEntry)
{
    hash_insert(&table, 777);

    EXPECT_TRUE(hash_remove(&table, 777));

    EXPECT_EQ(hash_find(&table, 777), UINT32_MAX);
}

/**
 * @brief Verify that removing a non-existent entry fails safely
 *
 * Ensures hash_remove() does not incorrectly modify table state.
 */
TEST_F(HashTableTest, RemoveMissing)
{
    EXPECT_FALSE(hash_remove(&table, 404));
}

/**
 * @brief Verify that size tracking is accurate
 *
 * Inserts multiple entries and checks that size reflects correct count.
 */
TEST_F(HashTableTest, SizeTracking)
{
    for (uint32_t i = 0; i < 50; i++)
    {
        hash_insert(&table, i);
    }

    EXPECT_EQ(hash_size(&table), 50u);
}

/**
 * @brief Verify that clearing the table resets its state
 *
 * Ensures all entries are removed and size is reset to zero.
 */
TEST_F(HashTableTest, ClearResetsTable)
{
    hash_insert(&table, 1);
    hash_insert(&table, 2);
    hash_insert(&table, 3);

    hash_clear(&table);

    EXPECT_EQ(hash_size(&table), 0u);
}

/**
 * @brief Verify deterministic collision handling using forced hash overlap
 *
 * Inserts two keys that map to the same primary hash index and ensures
 * both entries are stored and retrievable.
 */
TEST_F(HashTableTest, CollisionHandling)
{
    uint32_t base = 10;

    uint32_t id1 = base;
    uint32_t id2 = base + HASH_TABLE_SIZE;

    EXPECT_TRUE(hash_insert(&table, id1));
    EXPECT_TRUE(hash_insert(&table, id2));

    EXPECT_NE(hash_find(&table, id1), UINT32_MAX);
    EXPECT_NE(hash_find(&table, id2), UINT32_MAX);

    EXPECT_GT(table.collision_count, 0u);
}

/**
 * @brief Verify multiple collision resolution using double hashing
 *
 * This test forces a probing sequence by pre-filling slots that will be
 * visited during insertion, ensuring multiple collisions occur.
 */
TEST_F(HashTableTest, MultiCollisionResolution)
{
    // Choose a small, controlled set of IDs
    // These are designed to land in predictable clustered positions
    uint32_t base = 10;

    uint32_t id1 = base;
    uint32_t id2 = base + HASH_TABLE_SIZE;   // same h1 as id1
    uint32_t id3 = base + 2 * HASH_TABLE_SIZE;

    // First insert goes directly to h1 slot
    EXPECT_TRUE(hash_insert(&table, id1));

    // Second insert must collide at h1 and resolve via probing
    EXPECT_TRUE(hash_insert(&table, id2));

    // Third insert increases pressure further on same probe sequence
    EXPECT_TRUE(hash_insert(&table, id3));

    // All must be retrievable
    EXPECT_NE(hash_find(&table, id1), UINT32_MAX);
    EXPECT_NE(hash_find(&table, id2), UINT32_MAX);
    EXPECT_NE(hash_find(&table, id3), UINT32_MAX);

    EXPECT_GT(table.collision_count, 0u);
}

/**
 * @brief Stress test for hash table insertion under moderate load
 *
 * Inserts multiple sequential keys to verify stability under load.
 */
TEST_F(HashTableTest, SmallFillStress)
{
    for (uint32_t i = 1; i <= 200; i++)
    {
        EXPECT_TRUE(hash_insert(&table, i));
    }

    EXPECT_EQ(hash_size(&table), 200u);
}

/**
 * @brief Stress test for hash table insertion under moderate load
 *
 * Inserts multiple sequential keys to verify stability under load.
 */
TEST_F(HashTableTest, FullFillStress)
{
    for (uint32_t i = 1; i <= 10000; i++)
    {
        EXPECT_TRUE(hash_insert(&table, i));
    }

    EXPECT_EQ(hash_size(&table), 10000u);
}
