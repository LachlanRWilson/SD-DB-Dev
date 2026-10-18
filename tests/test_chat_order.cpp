#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

extern "C"
{
#include "chat_order.h"
}

/**
 * @brief Fixture for the recency ordered ChatOrder (chat_order.c).
 *
 * The chat order only holds hash slots in RAM, so no storage is needed. mem
 * is CHAT_ORDER_ENTRIES long like co_mem in database.c.
 */
class ChatOrderTest : public ::testing::Test
{
protected:
    ChatOrder co{};
    std::vector<uint16_t> mem = std::vector<uint16_t>(CHAT_ORDER_ENTRIES);

    void SetUp() override
    {
        ASSERT_TRUE(chat_order_init(&co, mem.data()));
    }

    /** @brief Every hash slot in rank order. */
    std::vector<uint16_t> all()
    {
        std::vector<uint16_t> out(co.occupancy);
        size_t count = 0;

        if (co.occupancy == 0 || !chat_order_get_range(&co, 0, co.occupancy, &count, out.data()))
        {
            return {};
        }
        out.resize(count);
        return out;
    }
};

TEST_F(ChatOrderTest, InitIsEmpty)
{
    EXPECT_EQ(co.occupancy, 0u);
    EXPECT_TRUE(all().empty());
}

TEST_F(ChatOrderTest, InitRejectsNull)
{
    EXPECT_FALSE(chat_order_init(nullptr, mem.data()));
    EXPECT_FALSE(chat_order_init(&co, nullptr));
}

/**
 * @brief New chats go in at the front, so the latest touched is rank 0.
 */
TEST_F(ChatOrderTest, TouchNewInsertsAtFront)
{
    EXPECT_EQ(chat_order_touch(&co, 10), STRG_OK);
    EXPECT_EQ(chat_order_touch(&co, 20), STRG_OK);
    EXPECT_EQ(chat_order_touch(&co, 30), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{30, 20, 10}));
}

/**
 * @brief Touching an existing chat moves it to the front without duplicating it.
 */
TEST_F(ChatOrderTest, TouchExistingMovesToFront)
{
    chat_order_touch(&co, 10);
    chat_order_touch(&co, 20);
    chat_order_touch(&co, 30);

    EXPECT_EQ(chat_order_touch(&co, 10), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{10, 30, 20}));
    EXPECT_EQ(co.occupancy, 3u);

    // already at the front: nothing moves
    EXPECT_EQ(chat_order_touch(&co, 10), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{10, 30, 20}));
}

TEST_F(ChatOrderTest, FindGivesRank)
{
    size_t rank = 99;

    chat_order_touch(&co, 10);
    chat_order_touch(&co, 20);

    EXPECT_TRUE(chat_order_find(&co, 20, &rank));
    EXPECT_EQ(rank, 0u);
    EXPECT_TRUE(chat_order_find(&co, 10, &rank));
    EXPECT_EQ(rank, 1u);
    EXPECT_FALSE(chat_order_find(&co, 30, &rank));
}

/**
 * @brief Removing the first, middle and last chats keeps the rest in order.
 */
TEST_F(ChatOrderTest, RemoveFirstMiddleLast)
{
    for (uint16_t s : {1, 2, 3, 4, 5})
    {
        chat_order_touch(&co, s);
    }
    // order: 5 4 3 2 1

    EXPECT_EQ(chat_order_remove(&co, 5), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{4, 3, 2, 1}));
    EXPECT_EQ(chat_order_remove(&co, 3), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{4, 2, 1}));
    EXPECT_EQ(chat_order_remove(&co, 1), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{4, 2}));
}

TEST_F(ChatOrderTest, RemoveMissingIsEmpty)
{
    chat_order_touch(&co, 1);
    EXPECT_EQ(chat_order_remove(&co, 2), STRG_EMPTY);
    EXPECT_EQ(all(), (std::vector<uint16_t>{1}));

    chat_order_remove(&co, 1);
    EXPECT_EQ(chat_order_remove(&co, 1), STRG_EMPTY);
    EXPECT_EQ(co.occupancy, 0u);
}

/**
 * @brief Replace changes the slot in place, the rank doesn't move.
 */
TEST_F(ChatOrderTest, ReplaceKeepsRank)
{
    chat_order_touch(&co, 1);
    chat_order_touch(&co, 2);
    chat_order_touch(&co, 3);

    EXPECT_EQ(chat_order_replace(&co, 2, 200), STRG_OK);
    EXPECT_EQ(all(), (std::vector<uint16_t>{3, 200, 1}));
    EXPECT_EQ(chat_order_replace(&co, 2, 300), STRG_EMPTY);
}

/**
 * @brief Ranges clamp to the end, and a start past the end fails.
 */
TEST_F(ChatOrderTest, GetRange)
{
    uint16_t out[4] = {0};
    size_t count = 0;

    for (uint16_t s : {1, 2, 3, 4, 5})
    {
        chat_order_touch(&co, s);
    }

    ASSERT_TRUE(chat_order_get_range(&co, 1, 2, &count, out));
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(out[0], 4u);
    EXPECT_EQ(out[1], 3u);

    ASSERT_TRUE(chat_order_get_range(&co, 3, 4, &count, out));
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(out[0], 2u);
    EXPECT_EQ(out[1], 1u);

    EXPECT_FALSE(chat_order_get_range(&co, 5, 1, &count, out));
}

/**
 * @brief Append adds to the back (start up rebuild order).
 */
TEST_F(ChatOrderTest, AppendAddsToBack)
{
    EXPECT_TRUE(chat_order_append(&co, 1));
    EXPECT_TRUE(chat_order_append(&co, 2));
    EXPECT_EQ(all(), (std::vector<uint16_t>{1, 2}));
}

/**
 * @brief A full chat order rejects new chats but still moves existing ones.
 */
TEST_F(ChatOrderTest, FullRejectsNewChat)
{
    for (uint16_t s = 0; s < CHAT_ORDER_ENTRIES; s++)
    {
        ASSERT_TRUE(chat_order_append(&co, s));
    }

    EXPECT_FALSE(chat_order_append(&co, CHAT_ORDER_ENTRIES));
    EXPECT_EQ(chat_order_touch(&co, CHAT_ORDER_ENTRIES), STRG_FULL);
    EXPECT_EQ(co.occupancy, CHAT_ORDER_ENTRIES);

    // the last chat moves to the front
    EXPECT_EQ(chat_order_touch(&co, CHAT_ORDER_ENTRIES - 1), STRG_OK);
    EXPECT_EQ(mem[0], CHAT_ORDER_ENTRIES - 1);
    EXPECT_EQ(mem[1], 0u);
}
