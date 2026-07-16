#include <gtest/gtest.h>
#include <climits>
#include <cstring>

extern "C" {

#include "hash_table.h"
#include "heap_storage.h"

}


/**
 * @brief Test fixture for HashTable unit tests.
 *
 * Provides:
 * - Hash table
 * - Free list allocator
 * - Heap-backed storage
 *
 * This mirrors the STM32 storage architecture while using
 * RAM instead of an SD card.
 */
class HashTableTest : public ::testing::Test
{
protected:

    static constexpr uint32_t TEST_BLOCKS = HASH_TABLE_SIZE;


    HashTable table;            /**< Hash table instance */

    HashEntry *entries = nullptr; /**< Hash table entries */

    FreeList freelist;           /**< Sector allocator */


    HeapStorageContext storage_ctx; /**< Heap storage context */

    Storage storage;               /**< Storage interface */


    uint8_t *storage_memory = nullptr;


    /**
     * @brief Initialise test environment.
     *
     * Creates RAM-backed storage, initialises the free list,
     * and creates the hash table.
     */
    void SetUp() override
    {

        /*
         * Allocate hash table memory
         */
        entries = new HashEntry[HASH_TABLE_SIZE];


        ASSERT_NE(entries, nullptr);


        memset(
            entries,
            0,
            sizeof(HashEntry) * HASH_TABLE_SIZE);



        /*
         * Allocate heap storage
         */
        storage_memory =
            new uint8_t[
                TEST_BLOCKS * sizeof(Contact)
            ];


        ASSERT_NE(storage_memory, nullptr);


        memset( storage_memory, 0, TEST_BLOCKS * sizeof(Contact));



        /*
         * Initialise heap storage backend
         */
        ASSERT_TRUE( HeapStorage_Init( &storage_ctx, storage_memory, sizeof(Contact), TEST_BLOCKS));



        storage = heap_storage;

        storage.context = &storage_ctx;



        /*
         * Initialise free list
         */
        uint16_t *pool = new uint16_t[HASH_TABLE_SIZE];


        for(uint16_t i = 0; i < HASH_TABLE_SIZE; i++)
        {
            pool[i] = i;
        }


        ASSERT_TRUE( free_list_init( &freelist, pool, HASH_TABLE_SIZE));



        /*
         * Initialise hash table
         */
        hash_init( &table, &freelist, entries, HASH_TABLE_SIZE, &storage);
    }



    /**
     * @brief Cleanup test resources.
     */
    void TearDown() override
    {

        hash_clear(&table);


        delete[] entries;

        entries = nullptr;


        delete[] storage_memory;

        storage_memory = nullptr;


        delete[] freelist.free_stack;
    }

};



/**
 * @brief Verify basic insertion.
 */
TEST_F(HashTableTest, InsertBasic)
{
    EXPECT_TRUE(hash_insert(&table, 100));

    EXPECT_TRUE(hash_insert(&table, 200));


    EXPECT_EQ(
        hash_size(&table),
        2u);
}



/**
 * @brief Verify duplicate insertion.
 */
TEST_F(HashTableTest, DuplicateInsert)
{
    EXPECT_TRUE(
        hash_insert(
            &table,
            55));


    size_t before =
        hash_size(&table);


    EXPECT_TRUE(
        hash_insert(
            &table,
            55));


    EXPECT_EQ(
        hash_size(&table),
        before);
}



/**
 * @brief Verify lookup of existing entries.
 */
TEST_F(HashTableTest, FindExisting)
{
    EXPECT_TRUE(
        hash_insert(
            &table,
            1234));


    uint32_t sector =
        hash_find(
            &table,
            1234);


    EXPECT_NE(
        sector,
        UINT32_MAX);
}



/**
 * @brief Verify lookup failure.
 */
TEST_F(HashTableTest, FindMissing)
{
    EXPECT_EQ(
        hash_find(
            &table,
            99999),
        UINT32_MAX);
}



/**
 * @brief Verify removal.
 */
TEST_F(HashTableTest, RemoveEntry)
{
    hash_insert(
        &table,
        777);


    EXPECT_TRUE(
        hash_remove(
            &table,
            777));


    EXPECT_EQ(
        hash_find(
            &table,
            777),
        UINT32_MAX);
}



/**
 * @brief Verify failed removal.
 */
TEST_F(HashTableTest, RemoveMissing)
{
    EXPECT_FALSE(
        hash_remove(
            &table,
            404));
}



/**
 * @brief Verify size tracking.
 */
TEST_F(HashTableTest, SizeTracking)
{
    for(uint32_t i = 0; i < 50; i++)
    {
        EXPECT_TRUE(
            hash_insert(
                &table,
                i));
    }


    EXPECT_EQ(
        hash_size(&table),
        50u);
}



/**
 * @brief Verify table reset.
 */
TEST_F(HashTableTest, ClearResetsTable)
{
    hash_insert(&table,1);
    hash_insert(&table,2);
    hash_insert(&table,3);


    hash_clear(&table);


    EXPECT_EQ(
        hash_size(&table),
        0u);
}



/**
 * @brief Verify storage allocation.
 *
 * Confirms inserted IDs are assigned unique storage blocks.
 */
TEST_F(HashTableTest, StorageAllocation)
{
    hash_insert(&table,10);

    hash_insert(&table,20);


    uint32_t sector1 =
        hash_find(
            &table,
            10);


    uint32_t sector2 =
        hash_find(
            &table,
            20);


    EXPECT_NE(
        sector1,
        sector2);
}



/**
 * @brief Verify collision handling.
 */
TEST_F(HashTableTest, CollisionHandling)
{
    uint32_t id1 = 10;

    uint32_t id2 =
        10 + HASH_TABLE_SIZE;


    EXPECT_TRUE(
        hash_insert(
            &table,
            id1));


    EXPECT_TRUE(
        hash_insert(
            &table,
            id2));


    EXPECT_NE(
        hash_find(&table,id1),
        UINT32_MAX);


    EXPECT_NE(
        hash_find(&table,id2),
        UINT32_MAX);


    EXPECT_GT(
        table.collision_count,
        0u);
}



/**
 * @brief Verify large insertion workload.
 */
TEST_F(HashTableTest, FullFillStress)
{
    for(uint32_t i = 1; i <= 10000; i++)
    {
        EXPECT_TRUE(
            hash_insert(
                &table,
                i));
    }


    EXPECT_EQ(
        hash_size(&table),
        10000u);
}
