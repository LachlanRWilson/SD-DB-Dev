#include <gtest/gtest.h>
#include <cstdint>

extern "C"
{
#include "ring_buffer.h"
#include "iterator.h"
}

class RingBufferIteratorTest : public ::testing::Test
{
protected:
    RingBuffer rb{};

    /** @brief Simulate `occupancy` entries having been written, starting from index 0. */
    void fill(uint16_t occupancy)
    {
        rb.occupancy = occupancy;
    }
};

/**
 * @brief A freshly initialised iterator starts positioned at the ring
 *        buffer's own current_index, within [0, occupancy].
 */
TEST_F(RingBufferIteratorTest, GetReturnsRingBuffersCurrentIndex)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 2));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    uint16_t value = 0;
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 2);
}

/**
 * @brief next() walks sequentially through the occupied range.
 */
TEST_F(RingBufferIteratorTest, NextWalksSequentiallyThroughOccupiedRange)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    for (uint16_t expected = 1; expected <= 4; expected++)
    {
        ASSERT_TRUE(iterator_next_fn(&it));
        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        EXPECT_EQ(value, expected);
    }
}

/**
 * @brief next() wraps from the upper limit (occupancy) back to the lower
 *        limit (0) rather than running off the end of the buffer.
 */
TEST_F(RingBufferIteratorTest, NextWrapsAtUpperLimit)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    // Walk to the upper limit (occupancy).
    for (int i = 0; i < 4; i++)
    {
        ASSERT_TRUE(iterator_next_fn(&it));
    }
    uint16_t value = 0;
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    ASSERT_EQ(value, 4);

    // One more step wraps back to the lower limit.
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 0);
}

/**
 * @brief prev() wraps from the lower limit (0) back to the upper limit
 *        (occupancy) rather than going negative.
 */
TEST_F(RingBufferIteratorTest, PrevWrapsAtLowerLimit)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    uint16_t value = 0;
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    ASSERT_EQ(value, 0);

    ASSERT_TRUE(iterator_prev_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 4);
}

/**
 * @brief The iterator's cursor is independent of the ring buffer's own
 *        current_index: advancing one must not move the other.
 */
TEST_F(RingBufferIteratorTest, IteratorCursorIsIndependentOfRingBufferCursor)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_next_fn(&it));

    uint16_t iter_value = 0;
    ASSERT_TRUE(iterator_get_fn(&it, &iter_value));
    EXPECT_EQ(iter_value, 2);

    // Driving the ring buffer's own cursor must not disturb the iterator.
    ASSERT_TRUE(move_next_ring_buffer(&rb));
    ASSERT_TRUE(move_next_ring_buffer(&rb));
    ASSERT_TRUE(move_next_ring_buffer(&rb));

    ASSERT_TRUE(iterator_get_fn(&it, &iter_value));
    EXPECT_EQ(iter_value, 2);
}

/**
 * @brief The iterator functions reject a NULL iterator, and get() rejects
 *        a NULL output pointer, without crashing.
 */
TEST_F(RingBufferIteratorTest, RejectsNullArguments)
{
    EXPECT_FALSE(ring_iterator_next(nullptr));
    EXPECT_FALSE(ring_iterator_prev(nullptr));

    uint16_t value = 0;
    EXPECT_FALSE(ring_iterator_get(nullptr, &value));

    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);
    EXPECT_FALSE(iterator_get_fn(&it, nullptr));
}

/**
 * @brief prev() walks sequentially down through the occupied range.
 */
TEST_F(RingBufferIteratorTest, PrevWalksSequentiallyThroughOccupiedRange)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 4));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    for (int expected = 3; expected >= 0; expected--)
    {
        ASSERT_TRUE(iterator_prev_fn(&it));
        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        EXPECT_EQ(value, expected);
    }
}

/**
 * @brief Stepping next() once per position in [0, occupancy] brings the
 *        iterator back to where it started, from any starting index.
 */
TEST_F(RingBufferIteratorTest, NextFullCycleReturnsToStart)
{
    constexpr uint16_t kOccupancy = 5;

    for (uint16_t start = 0; start <= kOccupancy; start++)
    {
        ASSERT_TRUE(init_ring_buffer(&rb, 8, start));
        fill(kOccupancy);

        RBIteratorCtx ctx;
        Iterator it = ring_buffer_iterator_init(&ctx, &rb);

        for (int i = 0; i < kOccupancy + 1; i++)
        {
            ASSERT_TRUE(iterator_next_fn(&it));
        }

        uint16_t value = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &value));
        EXPECT_EQ(value, start) << "start index " << start;
    }
}

/**
 * @brief next() followed by prev() is a no-op, including across the
 *        wrap-around point.
 */
TEST_F(RingBufferIteratorTest, NextThenPrevReturnsToSameIndex)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    for (int i = 0; i < 6; i++)
    {
        uint16_t before = 0;
        uint16_t after = 0;
        ASSERT_TRUE(iterator_get_fn(&it, &before));

        ASSERT_TRUE(iterator_next_fn(&it));
        ASSERT_TRUE(iterator_prev_fn(&it));

        ASSERT_TRUE(iterator_get_fn(&it, &after));
        EXPECT_EQ(after, before);

        // Move on so the next round starts from a different index.
        ASSERT_TRUE(iterator_next_fn(&it));
    }
}

/**
 * @brief With an empty ring buffer (occupancy 0) the iterator stays at
 *        index 0 in both directions.
 */
TEST_F(RingBufferIteratorTest, EmptyBufferStaysAtIndexZero)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 0));

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    uint16_t value = 1;
    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 0);

    value = 1;
    ASSERT_TRUE(iterator_prev_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 0);
}

/**
 * @brief If the ring buffer's current_index lies outside [0, occupancy],
 *        get() fails until next() brings the iterator back into range.
 */
TEST_F(RingBufferIteratorTest, GetFailsWhenStartIsOutsideOccupiedRange)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 6));
    fill(4);

    RBIteratorCtx ctx;
    Iterator it = ring_buffer_iterator_init(&ctx, &rb);

    uint16_t value = 0;
    EXPECT_FALSE(iterator_get_fn(&it, &value));

    ASSERT_TRUE(iterator_next_fn(&it));
    ASSERT_TRUE(iterator_get_fn(&it, &value));
    EXPECT_EQ(value, 0);
}

/**
 * @brief Two iterators over the same ring buffer keep independent cursors.
 */
TEST_F(RingBufferIteratorTest, MultipleIteratorsAreIndependent)
{
    ASSERT_TRUE(init_ring_buffer(&rb, 8, 1));
    fill(4);

    RBIteratorCtx ctx_a;
    RBIteratorCtx ctx_b;
    Iterator a = ring_buffer_iterator_init(&ctx_a, &rb);
    Iterator b = ring_buffer_iterator_init(&ctx_b, &rb);

    ASSERT_TRUE(iterator_next_fn(&a));
    ASSERT_TRUE(iterator_next_fn(&a));
    ASSERT_TRUE(iterator_prev_fn(&b));

    uint16_t value_a = 0;
    uint16_t value_b = 0;
    ASSERT_TRUE(iterator_get_fn(&a, &value_a));
    ASSERT_TRUE(iterator_get_fn(&b, &value_b));
    EXPECT_EQ(value_a, 3);
    EXPECT_EQ(value_b, 0);
}
