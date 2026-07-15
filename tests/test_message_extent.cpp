#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "message_extent.h"
#include "free_list_stack.h"
}

/* ============================================================
 * Simple heap-backed storage for testing
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

/* ============================================================
 * Test fixture
 * ============================================================ */

class MessageExtentTest : public ::testing::Test
{
protected:
    static constexpr uint32_t TEST_EXTENTS = 3 * 14293;

    MessageExtent extent;
    FreeList free_list;

    HeapStorage storage_ctx;
    MessageStorage storage;

    MessageBlock *blocks = nullptr;

    void SetUp() override
    {
        blocks = new MessageBlock[TEST_EXTENTS];
        std::memset(blocks, 0, sizeof(MessageBlock) * TEST_EXTENTS);

        storage_ctx.blocks = blocks;
        storage_ctx.count = TEST_EXTENTS;

        storage.read_block = heap_read;
        storage.write_block = heap_write;
        storage.capacity = heap_capacity;
        storage.context = &storage_ctx;

        uint32_t pool[TEST_EXTENTS];
        for (uint32_t i = 0; i < TEST_EXTENTS; i++)
            pool[i] = i;

        free_list_init(&free_list, pool, TEST_EXTENTS);

        ASSERT_TRUE(message_extent_init(&extent, &storage, &free_list, TEST_EXTENTS));
    }

    void TearDown() override
    {
        delete[] blocks;
        blocks = nullptr;
    }
};

/* ============================================================
 * Tests
 * ============================================================ */

TEST_F(MessageExtentTest, Initialise)
{
    EXPECT_EQ(extent.capacity, 0);
    EXPECT_EQ(extent.total_extents, TEST_EXTENTS);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, AllocateFirstExtent)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    EXPECT_NE(idx, UINT32_MAX);
    EXPECT_EQ(extent.capacity, 1);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, AllocateMultipleExtents)
{
    uint32_t a = message_extent_get(&extent, UINT32_MAX);
    uint32_t b = message_extent_get(&extent, UINT32_MAX);

    EXPECT_NE(a, UINT32_MAX);
    EXPECT_NE(b, UINT32_MAX);
    EXPECT_NE(a, b);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, AppendSingleMessage)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};
    msg.timestamp = 1;
    msg.direction = true;
    strcpy(msg.str, "hello");

    EXPECT_TRUE(message_extent_append(&extent, &idx, &msg));

    MessageBlock block;
    storage.read_block(&storage_ctx, idx, &block);

    EXPECT_EQ(block.header.msg_count, 1u);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, AppendMessagesSingleExtent)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (int i = 0; i < 10; i++)
    {
        msg.timestamp = i;
        EXPECT_TRUE(message_extent_append(&extent, &idx, &msg));
    }

    MessageBlock block;
    storage.read_block(&storage_ctx, idx, &block);

    EXPECT_EQ(block.header.msg_count, 10u);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, AllocateNewExtentWhenFull)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (uint32_t i = 0; i < MESSAGE_BLOCK_CAPACITY + 1; i++)
    {
        msg.timestamp = i;
        EXPECT_TRUE(message_extent_append(&extent, &idx, &msg));
    }

    MessageBlock block;
    storage.read_block(&storage_ctx, idx, &block);

    EXPECT_NE(block.header.prev, UINT32_MAX);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, PreviousExtentLinks)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (uint32_t i = 0; i < MESSAGE_BLOCK_CAPACITY + 2; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    MessageBlock block;
    storage.read_block(&storage_ctx, idx, &block);

    EXPECT_NE(block.header.prev, UINT32_MAX);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, ReadNewestMessage)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};
    msg.timestamp = 999;

    message_extent_append(&extent, &idx, &msg);

    MessageBlock block;
    storage.read_block(&storage_ctx, idx, &block);

    EXPECT_EQ(block.messages[0].timestamp, 999u);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, ReadAcrossExtents)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (int i = 0; i < MESSAGE_BLOCK_CAPACITY * 2; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_GT(message_extent_count(&extent, idx), MESSAGE_BLOCK_CAPACITY);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, DeleteConversation)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (int i = 0; i < 10; i++)
    {
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_TRUE(message_extent_delete(&extent, idx));
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, DeleteEmptyConversation)
{
    EXPECT_TRUE(message_extent_delete(&extent, UINT32_MAX));
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, MessageCount)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (int i = 0; i < 5; i++)
    {
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_EQ(message_extent_count(&extent, idx), 5u);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, OutOfExtents)
{
    uint32_t last = UINT32_MAX;

    for (uint32_t i = 0; i < TEST_EXTENTS; i++)
    {
        last = message_extent_get(&extent, UINT32_MAX);
    }

    uint32_t fail = message_extent_get(&extent, UINT32_MAX);

    EXPECT_EQ(fail, UINT32_MAX);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, Reset)
{
    message_extent_reset(&extent);

    EXPECT_EQ(extent.capacity, 0);
    EXPECT_EQ(extent.bottom_extent, extent.total_extents);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, LargeConversation)
{
    uint32_t idx = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    for (int i = 0; i < 200; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_GT(message_extent_count(&extent, idx), 100u);
}

/* ------------------------------------------------------------ */

TEST_F(MessageExtentTest, MultipleConversations)
{
    uint32_t a = message_extent_get(&extent, UINT32_MAX);
    uint32_t b = message_extent_get(&extent, UINT32_MAX);

    Message msg = {};

    msg.timestamp = 1;
    message_extent_append(&extent, &a, &msg);

    msg.timestamp = 2;
    message_extent_append(&extent, &b, &msg);

    MessageBlock ba, bb;
    storage.read_block(&storage_ctx, a, &ba);
    storage.read_block(&storage_ctx, b, &bb);

    EXPECT_EQ(ba.messages[0].timestamp, 1u);
    EXPECT_EQ(bb.messages[0].timestamp, 2u);
}
