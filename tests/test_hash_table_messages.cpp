#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "hash_table.h"
#include "message_extent.h"
#include "free_list_stack.h"
#include "heap_storage.h"
}

#define NUM_FLS 2
#define CONTACT_FLS 0
#define MESSAGE_FLS 1


/* ============================================================
 * Heap-backed storage for MessageBlock (simulated SD/flash)
 * ============================================================ */

struct HeapStorage
{
    MessageBlock *blocks;
    uint32_t count;
};

static bool heap_read(void *ctx, uint32_t idx, MessageBlock *out)
{
    HeapStorage *h = (HeapStorage *)ctx;
    if (idx >= h->count) return false;

    *out = h->blocks[idx];
    return true;
}

static bool heap_write(void *ctx, uint32_t idx, const MessageBlock *in)
{
    HeapStorage *h = (HeapStorage *)ctx;
    if (idx >= h->count) return false;

    h->blocks[idx] = *in;
    return true;
}

static uint32_t heap_capacity(void *ctx)
{
    HeapStorage *h = (HeapStorage *)ctx;
    return h->count;
}

extern Storage heap_storage;

/* ============================================================
 * Test fixture
 * ============================================================ */

class HashMessageExtentTest : public ::testing::Test
{
protected:
    static constexpr uint32_t TEST_EXTENTS = 2 * HASH_TABLE_SIZE;

    HashTable table;
    FreeList table_flss[2];

    MessageExtent extent;
    MessageStorage storage;

    HeapStorage storage_ctx;
    MessageBlock *blocks = nullptr;

    /* ---------------- Free list ---------------- */
    uint16_t contact_mem[HASH_TABLE_SIZE]; // Free List stack for contacts
    uint16_t messages_mem[TEST_EXTENTS]; // free list stack for messages


    void SetUp() override
    {
        /* ---------------- Heap storage ---------------- */
        blocks = new MessageBlock[TEST_EXTENTS];
        memset(blocks, 0, sizeof(MessageBlock) * TEST_EXTENTS);

        storage_ctx.blocks = blocks;
        storage_ctx.count = TEST_EXTENTS;

        storage.read_block = heap_read;
        storage.write_block = heap_write;
        storage.capacity = heap_capacity;
        storage.context = &storage_ctx;


        // Message Memory (2/3 of total)
        free_list_init(&table_flss[MESSAGE_FLS], messages_mem, TEST_EXTENTS);

        // Contact Memory (1/3 of total)
        free_list_init(&table_flss[CONTACT_FLS], contact_mem, HASH_TABLE_SIZE);

        /* ---------------- Message extent system ---------------- */
        ASSERT_TRUE(message_extent_init(&extent, &storage, &table_flss[MESSAGE_FLS], TEST_EXTENTS));

        /* ---------------- Hash table ---------------- */
        HashEntry *entries = hash_create_software();
        hash_init(&table, table_flss, entries, HASH_TABLE_SIZE, &heap_storage);
    }

    void TearDown() override
    {
        delete[] blocks;
        blocks = nullptr;
    }
};

/* ============================================================
 * TESTS
 * ============================================================ */

/**
 * Insert a contact and verify it exists
 */
TEST_F(HashMessageExtentTest, InsertAndFind)
{
    uint32_t id = 12345;

    EXPECT_TRUE(hash_insert(&table, id));

    uint32_t result = hash_find_message(&table, id);

    EXPECT_NE(result, UINT32_MAX);
}

/* ------------------------------------------------------------ */

/**
 * Verify writing a message updates the latest extent
 */
TEST_F(HashMessageExtentTest, WriteMessageUpdatesExtent)
{
    uint32_t id = 111;

    hash_insert(&table, id);

    uint32_t sector = hash_find_message(&table, id);

    Message msg = {};
    msg.timestamp = 1;
    strcpy(msg.str, "hello");

    message_extent_append(&extent, &sector, &msg);

    uint32_t new_sector = hash_find_message(&table, id);

    EXPECT_NE(new_sector, UINT32_MAX);
}

/* ------------------------------------------------------------ */

/**
 * Verify reading message persists correctly
 */
TEST_F(HashMessageExtentTest, WriteAndReadMessage)
{
    uint32_t id = 222;

    hash_insert(&table, id);

    uint32_t sector = hash_find_message(&table, id);

    Message msg = {};
    msg.timestamp = 42;
    strcpy(msg.str, "test message");

    message_extent_append(&extent, &sector, &msg);

    MessageBlock block;
    storage.read_block(&storage_ctx, sector, &block);

    EXPECT_EQ(block.messages[0].timestamp, 42u);
    EXPECT_STREQ(block.messages[0].str, "test message");
}

/* ------------------------------------------------------------ */

/**
 * Verify multiple messages persist in same extent
 */
TEST_F(HashMessageExtentTest, MultipleMessagesSameContact)
{
    uint32_t id = 333;

    hash_insert(&table, id);

    uint32_t sector = hash_find_message(&table, id);

    Message msg = {};

    for (int i = 0; i < 10; i++)
    {
        msg.timestamp = i;
        sprintf(msg.str, "msg %d", i);

        message_extent_append(&extent, &sector, &msg);
    }

    MessageBlock block;
    storage.read_block(&storage_ctx, sector, &block);

    char test_msg[SMS_MAX_MESSAGE_LENGTH] = {0};

    // Check every message
    for (int i = 0; i < 10; i++) {

        sprintf(test_msg, "msg %d", i);
        EXPECT_STREQ(block.messages[i].str, test_msg);
    }


    EXPECT_EQ(block.header.msg_count, 10u);
}

/* ------------------------------------------------------------ */

/**
 * Verify multiple contacts remain independent
 */
TEST_F(HashMessageExtentTest, MultipleContactsIsolation)
{
    uint32_t a = 1, b = 2;

    hash_insert(&table, a);
    hash_insert(&table, b);

    uint32_t sa = hash_find_message(&table, a);
    uint32_t sb = hash_find_message(&table, b);

    Message msg = {};
    msg.timestamp = 100;

    message_extent_append(&extent, &sa, &msg);

    msg.timestamp = 200;
    message_extent_append(&extent, &sb, &msg);

    MessageBlock ba, bb;
    storage.read_block(&storage_ctx, sa, &ba);
    storage.read_block(&storage_ctx, sb, &bb);

    EXPECT_EQ(ba.messages[0].timestamp, 100u);
    EXPECT_EQ(bb.messages[0].timestamp, 200u);
}

/* ------------------------------------------------------------ */

/**
 * Verify overwrite updates latest extent pointer
 */
TEST_F(HashMessageExtentTest, UpdateLatestExtent)
{
    uint32_t id = 999;

    hash_insert(&table, id);

    uint32_t extent_offset = hash_find_message(&table, id);

    Message msg = {};
    msg.timestamp = 1;

    for (int i = 0; i < MESSAGE_BLOCK_CAPACITY + 1; i++)
    {
        message_extent_append(&extent, &extent_offset, &msg);
    }

    uint32_t updated = hash_find_message(&table, id);

    // read block
    MessageBlock block;
    storage.read_block(&storage_ctx, updated, &block);

    EXPECT_NE(updated, UINT32_MAX);
    EXPECT_NE(updated, extent_offset);

    // Check the block is at max capacity
    EXPECT_EQ(block.header.msg_count, 1);
}

/**
 * Verify holds max messates
 */
TEST_F(HashMessageExtentTest, FillExtent)
{
    uint32_t id = 999;

    hash_insert(&table, id);

    uint32_t extent_offset = hash_find_message(&table, id);

    Message msg = {};
    msg.timestamp = 1;

    for (int i = 0; i < MESSAGE_BLOCK_CAPACITY; i++)
    {
        message_extent_append(&extent, &extent_offset, &msg);
    }

    uint32_t updated = hash_find_message(&table, id);

    MessageBlock block;
    storage.read_block(&storage_ctx, updated, &block);

    EXPECT_NE(updated, UINT32_MAX);

    // Check the extent hasn't been changed
    EXPECT_EQ(updated, extent_offset);

    // Check the block is at max capacity
    EXPECT_EQ(block.header.msg_count, MESSAGE_BLOCK_CAPACITY);
}


/* ------------------------------------------------------------ */

/**
 * Verify removal works
 */
TEST_F(HashMessageExtentTest, RemoveContact)
{
    uint32_t id = 555;

    hash_insert(&table, id);

    EXPECT_TRUE(hash_remove(&table, id));

    EXPECT_EQ(hash_find_message(&table, id), UINT32_MAX);
}
