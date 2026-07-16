#include <gtest/gtest.h>
#include <cstring>

extern "C" {

#include "hash_table.h"
#include "message_extent.h"
#include "free_list_stack.h"
#include "heap_storage.h"

}


/*
 * ============================================================
 * Hash + Message Extent Integration Tests
 * ============================================================
 *
 * Tests the interaction between:
 *
 *  - HashTable
 *      Stores contacts and points to their latest message extent.
 *
 *  - MessageExtent
 *      Manages conversation storage through extents.
 *
 *  - HeapStorage
 *      Simulates persistent storage (SD card / flash).
 *
 * The architecture mirrors the embedded implementation:
 *
 *      HashTable
 *          |
 *          | latest_msg_extent
 *          v
 *      MessageExtent
 *          |
 *          v
 *      Storage abstraction
 *
 * ============================================================
 */



/**
 * @brief Test fixture for HashTable and MessageExtent integration.
 *
 * Provides:
 *
 * - Hash table
 * - Contact free list
 * - Message extent free list
 * - Heap backed storage
 *
 * This represents the complete database memory model.
 */
class HashMessageExtentTest : public ::testing::Test
{
protected:

    static constexpr uint32_t TEST_EXTENTS = 2 * HASH_TABLE_SIZE;


    HashTable table;


    /**
     * @brief Contact allocator.
     */
    FreeList contact_fls;


    /**
     * @brief Message extent allocator.
     */
    FreeList message_fls;



    /**
     * @brief Message extent manager.
     */
    MessageExtent extent;



    /**
     * @brief Storage interface.
     */
    Storage storage;



    /**
     * @brief Heap storage context.
     */
    HeapStorageContext storage_ctx;



    /**
     * @brief Backing memory for message blocks.
     */
    MessageBlock *blocks = nullptr;



    /**
     * @brief Memory pool for contact allocation.
     */
    uint16_t contact_mem[HASH_TABLE_SIZE];



    /**
     * @brief Memory pool for message extent allocation.
     */
    uint16_t message_mem[TEST_EXTENTS];



    /**
     * @brief Hash table backing memory.
     */
    HashEntry *entries = nullptr;



    /**
     * @brief Initialise complete database environment.
     */
    void SetUp() override
    {

        /*
         * ----------------------------------------------------
         * Initialise heap storage
         * ----------------------------------------------------
         */

        blocks = new MessageBlock[TEST_EXTENTS];


        ASSERT_NE( blocks, nullptr);

        memset( blocks, 0, sizeof(MessageBlock) * TEST_EXTENTS);

        ASSERT_TRUE( HeapStorage_Init( &storage_ctx, (uint8_t *)blocks, sizeof(MessageBlock),
                    TEST_EXTENTS));

        storage = heap_storage;

        storage.context = &storage_ctx;



        /*
         * ----------------------------------------------------
         * Initialise free lists
         * ----------------------------------------------------
         */

        ASSERT_TRUE( free_list_init( &contact_fls, contact_mem, HASH_TABLE_SIZE));

        ASSERT_TRUE( free_list_init( &message_fls, message_mem, TEST_EXTENTS));

        /*
         * ----------------------------------------------------
         * Initialise message extent manager
         * ----------------------------------------------------
         */

        ASSERT_TRUE( message_extent_init( &extent, &storage, &message_fls, TEST_EXTENTS));

        /*
         * ----------------------------------------------------
         * Initialise hash table
         * ----------------------------------------------------
         */


        entries = new HashEntry[HASH_TABLE_SIZE];

        ASSERT_NE( entries, nullptr);

        memset( entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        hash_init( &table, &contact_fls, entries, HASH_TABLE_SIZE);
    }



    /**
     * @brief Cleanup database resources.
     */
    void TearDown() override
    {

        hash_clear( &table);

        delete[] entries;

        entries = nullptr;

        delete[] blocks;

        blocks = nullptr;
    }

};



/**
 * @brief Verify a contact can be inserted and found.
 */
TEST_F(HashMessageExtentTest, InsertAndFind)
{
    uint16_t id = 12345;

    HashEntry *entry;

    EXPECT_TRUE( hash_insert( &table, id));

    EXPECT_NE( hash_find_entry( &table, id, &entry), false);
}



/**
 * @brief Verify a message extent can be attached to a contact.
 */
TEST_F(HashMessageExtentTest, WriteMessageUpdatesExtent)
{
    uint16_t id = 111;

    EXPECT_TRUE( hash_insert( &table, id));

    HashEntry *entry = nullptr;

    EXPECT_TRUE( hash_find_entry( &table, id, &entry));

    ASSERT_NE( entry, nullptr);

    entry->latest_msg_extent = message_extent_get( &extent, UINT16_MAX);

    EXPECT_NE( entry->latest_msg_extent, UINT16_MAX); 
}



/**
 * @brief Verify messages persist in heap storage.
 */
TEST_F(HashMessageExtentTest, WriteAndReadMessage)
{
    uint16_t id = 222;


    hash_insert( &table, id);

    HashEntry *entry = nullptr;

    hash_find_entry( &table, id, &entry);

    entry->latest_msg_extent = message_extent_get( &extent, UINT16_MAX);

    Message msg = {};

    msg.timestamp = 42;

    strcpy( msg.str, "test message");

    EXPECT_TRUE( message_extent_append( &extent, &entry->latest_msg_extent, &msg));

    MessageBlockBuffer block;

    EXPECT_TRUE( storage.read_block( storage.context, entry->latest_msg_extent, block.buffer));

    EXPECT_EQ( block.var.messages[0].timestamp, 42u);

    EXPECT_STREQ( block.var.messages[0].str, "test message");
}
