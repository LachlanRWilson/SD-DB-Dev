#include <gtest/gtest.h>
#include <climits>

extern "C" {
#include "hash_table.h"
#include "free_list_stack.h"
}

/**
 * @brief Duplicate ID handling test
 *
 * Ensures inserting the same ID twice does not increase table size.
 * Expected behaviour: duplicate insert should not create a second entry.
 */
TEST(HashInsertEdge, DuplicateID)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, HASH_TABLE_SIZE);

    uint32_t id = 42;

    EXPECT_TRUE(hash_insert(&table, id));
    size_t before = hash_size(&table);

    EXPECT_TRUE(hash_insert(&table, id));

    EXPECT_EQ(hash_size(&table), before);

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Collision handling test (forced collisions)
 *
 * Uses a small table size so multiple keys collide.
 * Ensures all inserted values remain retrievable.
 */
TEST(HashInsertEdge, CollisionHandling)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    // Small table forces collisions
    hash_create(&table, &freelist, entries, 5);

    uint32_t ids[] = {10, 15, 20, 25};

    for (uint32_t id : ids)
    {
        EXPECT_TRUE(hash_insert(&table, id));
    }

    for (uint32_t id : ids)
    {
        EXPECT_NE(hash_find(&table, id), UINT32_MAX);
    }

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Lookup non-existent key
 *
 * Ensures searching for missing ID returns UINT32_MAX.
 */
TEST(HashFindEdge, NotFound)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 10);

    EXPECT_EQ(hash_find(&table, 99999), UINT32_MAX);

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Remove non-existent key
 *
 * Ensures removing missing ID returns false.
 */
TEST(HashRemoveEdge, NotFound)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 10);

    EXPECT_FALSE(hash_remove(&table, 12345));

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Zero bucket safety test
 *
 * This MUST fail if your implementation does not guard against capacity = 0.
 * Expected safe behaviour: either reject or never crash.
 */
TEST(HashCreateEdge, ZeroBuckets)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, 10));

    hash_create(&table, &freelist, entries, 0);

    // These assertions will fail if implementation is unsafe
    EXPECT_EQ(hash_size(&table), 0u);
    EXPECT_EQ(table.capacity, 0u);
}

/**
 * @brief Clearing empty table
 *
 * Ensures hash_clear does not crash on empty table.
 */
TEST(HashClearEdge, EmptyTable)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 10);

    hash_clear(&table);

    EXPECT_EQ(hash_size(&table), 0u);

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Stress test large insertion
 *
 * Inserts many elements and ensures all are retrievable.
 */
TEST(HashInsertEdge, StressTest)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 2000);

    const int N = 1000;

    for (uint32_t i = 0; i < N; i++)
    {
        EXPECT_TRUE(hash_insert(&table, i));
    }

    EXPECT_EQ(hash_size(&table), (size_t)N);

    for (uint32_t i = 0; i < N; i++)
    {
        EXPECT_NE(hash_find(&table, i), UINT32_MAX);
    }

    hash_destroy_software(&table, nullptr);
}

/**
 * @brief Safe destroy on empty table
 */
TEST(HashDestroyEdge, EmptyTable)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 10);

    hash_destroy(&table);

    SUCCEED();
}

/**
 * @brief Safe destroy on populated table
 */
TEST(HashDestroyEdge, PopulatedTable)
{
    HashTable table;
    HashEntry *entries = hash_create_software();
    FreeList freelist;

    ASSERT_TRUE(free_list_init_software(&freelist, HASH_TABLE_SIZE));

    hash_create(&table, &freelist, entries, 100);

    for (uint32_t i = 0; i < 50; i++)
    {
        hash_insert(&table, i);
    }

    hash_destroy(&table);

    SUCCEED();
}
