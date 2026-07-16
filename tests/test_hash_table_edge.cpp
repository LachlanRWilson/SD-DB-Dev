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


    HeapStorageContext storage_ctx;

    Storage storage;

    uint8_t *storage_memory = nullptr;


    /**
     * @brief Initialise common test resources.
     */
    void SetUp() override
    {

        entries =
            new HashEntry[HASH_TABLE_SIZE];


        ASSERT_NE(
            entries,
            nullptr);


        memset(
            entries,
            0,
            sizeof(HashEntry) * HASH_TABLE_SIZE);



        storage_memory =
            new uint8_t[
                HASH_TABLE_SIZE * sizeof(Contact)
            ];


        ASSERT_NE(
            storage_memory,
            nullptr);



        memset(
            storage_memory,
            0,
            HASH_TABLE_SIZE * sizeof(Contact));



        ASSERT_TRUE(
            HeapStorage_Init(
                &storage_ctx,
                storage_memory,
                sizeof(Contact),
                HASH_TABLE_SIZE));



        storage = heap_storage;

        storage.context = &storage_ctx;



        uint16_t *pool =
            new uint16_t[HASH_TABLE_SIZE];


        for(uint16_t i = 0;
            i < HASH_TABLE_SIZE;
            i++)
        {
            pool[i] = i;
        }


        ASSERT_TRUE(
            free_list_init(
                &freelist,
                pool,
                HASH_TABLE_SIZE));



        hash_init(
            &table,
            &freelist,
            entries,
            HASH_TABLE_SIZE,
            &storage);
    }



    /**
     * @brief Release test resources.
     */
    void TearDown() override
    {

        hash_clear(
            &table);


        delete[] entries;

        delete[] storage_memory;


        delete[] freelist.free_stack;
    }

};



/**
 * @brief Verify duplicate IDs do not create entries.
 */
TEST_F(HashTableEdgeTest, DuplicateID)
{

    EXPECT_TRUE(
        hash_insert(
            &table,
            42));


    size_t before =
        hash_size(
            &table);


    EXPECT_TRUE(
        hash_insert(
            &table,
            42));


    EXPECT_EQ(
        hash_size(&table),
        before);
}



/**
 * @brief Verify collision resolution.
 *
 * Inserts IDs that map to the same bucket.
 */
TEST_F(HashTableEdgeTest, CollisionHandling)
{

    uint32_t ids[] =
    {
        10,
        10 + HASH_TABLE_SIZE,
        10 + (2 * HASH_TABLE_SIZE),
        10 + (3 * HASH_TABLE_SIZE)
    };


    for(uint32_t id : ids)
    {
        EXPECT_TRUE(
            hash_insert(
                &table,
                id));
    }


    for(uint32_t id : ids)
    {
        EXPECT_NE(
            hash_find(
                &table,
                id),
            UINT32_MAX);
    }


    EXPECT_GT(
        table.collision_count,
        0u);
}



/**
 * @brief Verify missing key lookup.
 */
TEST_F(HashTableEdgeTest, LookupNotFound)
{

    EXPECT_EQ(
        hash_find(
            &table,
            99999),
        UINT32_MAX);
}



/**
 * @brief Verify removing missing key fails.
 */
TEST_F(HashTableEdgeTest, RemoveNotFound)
{

    EXPECT_FALSE(
        hash_remove(
            &table,
            12345));
}



/**
 * @brief Verify zero capacity handling.
 *
 * Ensures the hash table does not crash when
 * created with zero buckets.
 */
TEST_F(HashTableEdgeTest, ZeroBuckets)
{

    HashTable zero_table;


    hash_init(
        &zero_table,
        &freelist,
        entries,
        0,
        &storage);



    EXPECT_EQ(
        hash_size(
            &zero_table),
        0u);


    EXPECT_EQ(
        zero_table.capacity,
        0u);
}



/**
 * @brief Verify clearing an empty table.
 */
TEST_F(HashTableEdgeTest, ClearEmptyTable)
{

    hash_clear(
        &table);


    EXPECT_EQ(
        hash_size(
            &table),
        0u);
}



/**
 * @brief Stress test insertions.
 */
TEST_F(HashTableEdgeTest, StressTest)
{

    constexpr uint32_t N = 1000;


    for(uint32_t i = 0;
        i < N;
        i++)
    {
        EXPECT_TRUE(
            hash_insert(
                &table,
                i));
    }


    EXPECT_EQ(
        hash_size(
            &table),
        N);



    for(uint32_t i = 0;
        i < N;
        i++)
    {
        EXPECT_NE(
            hash_find(
                &table,
                i),
            UINT32_MAX);
    }
}



/**
 * @brief Verify destroying empty table.
 */
TEST_F(HashTableEdgeTest, DestroyEmptyTable)
{

    HashTable temp;


    hash_init(
        &temp,
        &freelist,
        entries,
        10,
        &storage);


    hash_destroy(
        &temp);


    SUCCEED();
}



/**
 * @brief Verify destroying populated table.
 */
TEST_F(HashTableEdgeTest, DestroyPopulatedTable)
{

    for(uint32_t i = 0;
        i < 50;
        i++)
    {
        hash_insert(
            &table,
            i);
    }


    hash_destroy(
        &table);


    SUCCEED();
}
