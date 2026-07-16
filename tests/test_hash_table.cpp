#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "hash_table.h"
}

/**
 * @brief Test fixture for HashTable.
 *
 * Creates a fresh hash table and free-list allocator for every test.
 * Each test is completely independent.
 */
class HashTableTest : public ::testing::Test
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
 * @brief Verify a newly initialised table is empty.
 */
TEST_F(HashTableTest, Initialise)
{
    EXPECT_EQ(hash_size(&table), 0u);
    EXPECT_EQ(table.size, HASH_TABLE_SIZE);
}

/**
 * @brief Verify inserting a single contact succeeds.
 */
TEST_F(HashTableTest, InsertSingle)
{
    EXPECT_TRUE(hash_insert(&table, 100));

    EXPECT_EQ(hash_size(&table), 1u);

    EXPECT_NE(hash_find_sector(&table, 100), UINT16_MAX);
}

/**
 * @brief Verify duplicate insertion does not create another entry.
 */
TEST_F(HashTableTest, DuplicateInsert)
{
    EXPECT_TRUE(hash_insert(&table, 55));

    size_t before = hash_size(&table);

    EXPECT_TRUE(hash_insert(&table, 55));

    EXPECT_EQ(hash_size(&table), before);
}

/**
 * @brief Verify many sequential insertions.
 */
TEST_F(HashTableTest, InsertMany)
{
    for (uint16_t i = 0; i < 1000; i++)
    {
        EXPECT_TRUE(hash_insert(&table, i));
    }

    EXPECT_EQ(hash_size(&table), 1000u);
}

/**
 * @brief Verify lookup of an existing contact.
 */
TEST_F(HashTableTest, FindExisting)
{
    hash_insert(&table, 123);

    EXPECT_NE(hash_find_sector(&table, 123), UINT16_MAX);
}

/**
 * @brief Verify lookup of a missing contact.
 */
TEST_F(HashTableTest, FindMissing)
{
    EXPECT_EQ(hash_find_sector(&table, 123), UINT16_MAX);
}

/**
 * @brief Verify retrieval of a hash table entry.
 */
TEST_F(HashTableTest, FindEntry)
{
    hash_insert(&table, 500);

    HashEntry *entry = nullptr;

    EXPECT_TRUE(
        hash_find_entry(
            &table,
            500,
            &entry));

    ASSERT_NE(entry, nullptr);

    EXPECT_EQ(entry->id, 500);
}

/**
 * @brief Verify lookup of a missing entry returns nullptr.
 */
TEST_F(HashTableTest, FindEntryMissing)
{
    HashEntry *entry = nullptr;

    EXPECT_FALSE(
        hash_find_entry(
            &table,
            999,
            &entry));

    EXPECT_EQ(entry, nullptr);
}

/**
 * @brief Verify the message extent associated with a contact.
 */
TEST_F(HashTableTest, FindMessageExtent)
{
    hash_insert(&table, 77);

    HashEntry *entry = nullptr;

    ASSERT_TRUE(
        hash_find_entry(
            &table,
            77,
            &entry));

    entry->latest_msg_extent = 42;

    EXPECT_EQ(
        hash_find_message(
            &table,
            77),
        42);
}

/**
 * @brief Verify requesting the message extent of a missing
 * contact returns UINT16_MAX.
 */
TEST_F(HashTableTest, FindMessageExtentMissing)
{
    EXPECT_EQ(
        hash_find_message(
            &table,
            555),
        UINT16_MAX);
}

/**
 * @brief Verify removing an existing contact.
 */
TEST_F(HashTableTest, RemoveExisting)
{
    hash_insert(&table, 20);

    HashEntry *removed = nullptr;

    EXPECT_TRUE(
        hash_remove(
            &table,
            20,
            &removed));

    ASSERT_NE(removed, nullptr);

    EXPECT_EQ(removed->id, 20);

    EXPECT_EQ(hash_find_sector(&table, 20), UINT16_MAX);

    EXPECT_EQ(hash_size(&table), 0u);
}

/**
 * @brief Verify removing a non-existent contact fails.
 */
TEST_F(HashTableTest, RemoveMissing)
{
    HashEntry *removed = nullptr;

    EXPECT_FALSE( hash_remove( &table, 200, &removed));

    EXPECT_EQ(removed, nullptr);
}

/**
 * @brief Verify clearing the hash table removes all entries.
 */
TEST_F(HashTableTest, ClearTable)
{
    for (uint16_t i = 0; i < 100; i++)
    {
        hash_insert(&table, i);
    }

    hash_clear(&table);

    EXPECT_EQ(hash_size(&table), 0u);

    for (uint16_t i = 0; i < 100; i++)
    {
        EXPECT_EQ(hash_find_sector(&table, i), UINT16_MAX);
    }
}

/**
 * @brief Verify collision resolution using two keys that hash
 * to the same primary bucket.
 */
TEST_F(HashTableTest, CollisionHandling)
{
    uint16_t id1 = 10;
    uint16_t id2 = static_cast<uint16_t>(10 + HASH_TABLE_SIZE);

    EXPECT_TRUE(hash_insert(&table, id1));
    EXPECT_TRUE(hash_insert(&table, id2));

    EXPECT_NE(hash_find_sector(&table, id1), UINT16_MAX);
    EXPECT_NE(hash_find_sector(&table, id2), UINT16_MAX);

#ifdef HOST_BUILD
    EXPECT_GT(table.collision_count, 0u);
#endif
}

/**
 * @brief Verify the hash table supports a large number of entries.
 */
TEST_F(HashTableTest, LargeInsertion)
{
    for (uint16_t i = 0; i < 10000; i++)
    {
        EXPECT_TRUE(hash_insert(&table, i));
    }

    EXPECT_EQ(hash_size(&table), 10000u);
}

/**
 * @brief Verify modifying one contact's message extent does not
 * affect another contact.
 */
TEST_F(HashTableTest, IndependentMessageExtents)
{
    hash_insert(&table, 1);
    hash_insert(&table, 2);

    HashEntry *a = nullptr;
    HashEntry *b = nullptr;

    ASSERT_TRUE(hash_find_entry(&table, 1, &a));
    ASSERT_TRUE(hash_find_entry(&table, 2, &b));

    a->latest_msg_extent = 100;
    b->latest_msg_extent = 200;

    EXPECT_EQ(hash_find_message(&table, 1), 100);
    EXPECT_EQ(hash_find_message(&table, 2), 200);
}

/**
 * @brief Verify every inserted contact is allocated a unique sector.
 */
TEST_F(HashTableTest, UniqueSectorAllocation)
{
    hash_insert(&table, 10);
    hash_insert(&table, 20);
    hash_insert(&table, 30);

    uint16_t s1 = hash_find_sector(&table, 10);
    uint16_t s2 = hash_find_sector(&table, 20);
    uint16_t s3 = hash_find_sector(&table, 30);

    EXPECT_NE(s1, s2);
    EXPECT_NE(s1, s3);
    EXPECT_NE(s2, s3);
}
