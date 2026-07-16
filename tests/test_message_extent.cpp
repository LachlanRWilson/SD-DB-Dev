#include <gtest/gtest.h>
#include <cstring>

extern "C"
{
#include "hash_table.h"
#include "message_extent.h"
#include "free_list_stack.h"
#include "heap_storage.h"
}

class MessageExtentTest : public ::testing::Test
{
protected:

    static constexpr uint16_t TEST_EXTENTS = 2 * HASH_TABLE_SIZE;

    MessageExtent extent;
    FreeList free_list;

    HeapStorageContext storage_ctx;
    Storage storage;

    uint8_t *storage_memory = nullptr;
    uint16_t *pool = nullptr;

    void SetUp() override
    {
        /*
         * Allocate heap-backed storage.
         */
        storage_memory =
            new uint8_t[
                TEST_EXTENTS * sizeof(MessageBlock)
            ];

        ASSERT_NE(storage_memory, nullptr);

        memset(
            storage_memory,
            0,
            TEST_EXTENTS * sizeof(MessageBlock));

        ASSERT_TRUE(
            HeapStorage_Init(
                &storage_ctx,
                storage_memory,
                sizeof(MessageBlock),
                TEST_EXTENTS));

        storage = heap_storage;
        storage.context = &storage_ctx;

        /*
         * Initialise free list.
         */
        pool = new uint16_t[TEST_EXTENTS];

        ASSERT_NE(pool, nullptr);

        for (uint16_t i = 0; i < TEST_EXTENTS; i++)
        {
            pool[i] = i;
        }

        ASSERT_TRUE(
            free_list_init(
                &free_list,
                pool,
                TEST_EXTENTS));

        /*
         * Initialise extent manager.
         */
        ASSERT_TRUE(
            message_extent_init(
                &extent,
                &storage,
                &free_list,
                TEST_EXTENTS));
    }

    void TearDown() override
    {
        delete[] storage_memory;
        delete[] pool;

        storage_memory = nullptr;
        pool = nullptr;
    }
};

/**
 * @brief Verify initialisation.
 */
TEST_F(MessageExtentTest, Initialise)
{
    EXPECT_EQ(extent.num_extents, 0u);
    EXPECT_EQ(extent.total_extents, TEST_EXTENTS);
}

/**
 * @brief Verify a single extent allocation.
 */
TEST_F(MessageExtentTest, AllocateFirstExtent)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    EXPECT_NE(idx, UINT16_MAX);
    EXPECT_EQ(extent.num_extents, 1u);
}

/**
 * @brief Verify multiple extent allocations.
 */
TEST_F(MessageExtentTest, AllocateMultipleExtents)
{
    uint16_t a = message_extent_get(&extent, UINT16_MAX);
    uint16_t b = message_extent_get(&extent, UINT16_MAX);

    EXPECT_NE(a, UINT16_MAX);
    EXPECT_NE(b, UINT16_MAX);
    EXPECT_NE(a, b);
}

/**
 * @brief Verify appending a single message.
 */
TEST_F(MessageExtentTest, AppendSingleMessage)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};
    msg.timestamp = 1;
    msg.direction = true;
    strcpy(msg.str, "hello");

    EXPECT_TRUE(
        message_extent_append(
            &extent,
            &idx,
            &msg));

    MessageBlockBuffer block;

    storage.read_block(
        storage.context,
        idx,
        block.buffer);

    EXPECT_EQ(block.var.header.msg_count, 1u);
}

/**
 * @brief Verify multiple messages fit within one extent.
 */
TEST_F(MessageExtentTest, AppendMessagesSingleExtent)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (uint16_t i = 0; i < 10; i++)
    {
        msg.timestamp = i;

        EXPECT_TRUE(
            message_extent_append(
                &extent,
                &idx,
                &msg));
    }

    MessageBlockBuffer block;

    storage.read_block(
        storage.context,
        idx,
        block.buffer);

    EXPECT_EQ(block.var.header.msg_count, 10u);
}

/**
 * @brief Verify allocation of a new extent when full.
 */
TEST_F(MessageExtentTest, AllocateNewExtentWhenFull)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (uint16_t i = 0; i < MESSAGE_BLOCK_CAPACITY + 1; i++)
    {
        msg.timestamp = i;

        EXPECT_TRUE(
            message_extent_append(
                &extent,
                &idx,
                &msg));
    }

    MessageBlockBuffer block;

    storage.read_block(
        storage.context,
        idx,
        block.buffer);

    EXPECT_NE(block.var.header.prev, UINT16_MAX);
}

/**
 * @brief Verify previous extent links are maintained.
 */
TEST_F(MessageExtentTest, PreviousExtentLinks)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (uint16_t i = 0; i < MESSAGE_BLOCK_CAPACITY + 2; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    MessageBlockBuffer block;

    storage.read_block(
        storage.context,
        idx,
        block.buffer);

    EXPECT_NE(block.var.header.prev, UINT16_MAX);
}

/**
 * @brief Verify newest message contents.
 */
TEST_F(MessageExtentTest, ReadNewestMessage)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};
    msg.timestamp = 999;

    message_extent_append(&extent, &idx, &msg);

    MessageBlockBuffer block;

    storage.read_block(
        storage.context,
        idx,
        block.buffer);

    EXPECT_EQ(block.var.messages[0].timestamp, 999u);
}

/**
 * @brief Verify counting across multiple extents.
 */
TEST_F(MessageExtentTest, ReadAcrossExtents)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (uint16_t i = 0; i < MESSAGE_BLOCK_CAPACITY * 2; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_GT(
        message_extent_count(&extent, idx),
        MESSAGE_BLOCK_CAPACITY);
}

/**
 * @brief Verify deleting a populated conversation.
 */
TEST_F(MessageExtentTest, DeleteConversation)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (int i = 0; i < 10; i++)
    {
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_TRUE(
        message_extent_delete(
            &extent,
            idx));
}

/**
 * @brief Verify deleting an empty conversation.
 */
TEST_F(MessageExtentTest, DeleteEmptyConversation)
{
    EXPECT_TRUE(
        message_extent_delete(
            &extent,
            UINT16_MAX));
}

/**
 * @brief Verify message counting.
 */
TEST_F(MessageExtentTest, MessageCount)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (int i = 0; i < 5; i++)
    {
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_EQ(
        message_extent_count(&extent, idx),
        5u);
}

/**
 * @brief Verify allocator exhaustion.
 */
TEST_F(MessageExtentTest, OutOfExtents)
{
    for (uint16_t i = 0; i < TEST_EXTENTS; i++)
    {
        message_extent_get(&extent, UINT16_MAX);
    }

    EXPECT_EQ(
        message_extent_get(&extent, UINT16_MAX),
        UINT16_MAX);
}

/**
 * @brief Verify reset restores initial state.
 */
TEST_F(MessageExtentTest, Reset)
{
    message_extent_reset(&extent);

    EXPECT_EQ(extent.num_extents, 0u);
    EXPECT_EQ(extent.bottom_extent, extent.total_extents);
}

/**
 * @brief Verify a large conversation spanning multiple extents.
 */
TEST_F(MessageExtentTest, LargeConversation)
{
    uint16_t idx = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    for (uint16_t i = 0; i < 200; i++)
    {
        msg.timestamp = i;
        message_extent_append(&extent, &idx, &msg);
    }

    EXPECT_GT(
        message_extent_count(&extent, idx),
        100u);
}

/**
 * @brief Verify independent conversations remain isolated.
 */
TEST_F(MessageExtentTest, MultipleConversations)
{
    uint16_t a = message_extent_get(&extent, UINT16_MAX);
    uint16_t b = message_extent_get(&extent, UINT16_MAX);

    Message msg = {};

    msg.timestamp = 1;
    message_extent_append(&extent, &a, &msg);

    msg.timestamp = 2;
    message_extent_append(&extent, &b, &msg);

    MessageBlockBuffer block_a;
    MessageBlockBuffer block_b;

    storage.read_block(storage.context, a, block_a.buffer);
    storage.read_block(storage.context, b, block_b.buffer);

    EXPECT_EQ(block_a.var.messages[0].timestamp, 1u);
    EXPECT_EQ(block_b.var.messages[0].timestamp, 2u);
}
