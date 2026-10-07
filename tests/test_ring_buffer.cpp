#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C"
{
#include "ring_buffer.h"
#include "iterator.h"
#include "storage.h"

}

#include "test_support/failable_storage.h"

/**
 * @brief Set the RAM state directly for tests that don't touch storage
 *        (init_ring_buffer() now reconstructs the state from the SD card).
 */
static bool set_ring_buffer(RingBuffer *rb, uint16_t size, uint16_t current_index)
{
    rb->size = size;
    rb->current_index = current_index;
    rb->occupancy = 0;
    rb->seq = 0;
    return true;
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 2));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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

    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 4));
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
        ASSERT_TRUE(set_ring_buffer(&rb, 8, start));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));

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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 6));
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
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 1));
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

/* ================================================================
 * Ring buffer state (RAM cursor / occupancy / sequence)
 * ================================================================ */

/**
 * @brief On a full buffer, move_next() from the last slot wraps to slot 0
 *        instead of stepping outside [0, size).
 */
TEST(RingBufferStateTest, MoveNextWrapsAtSizeWhenFull)
{
    RingBuffer rb{};
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 7));
    rb.occupancy = 8;

    ASSERT_TRUE(move_next_ring_buffer(&rb));
    EXPECT_EQ(rb.current_index, 0);
}

/**
 * @brief The add sequence used by message_history_add() (move_next then
 *        add) never produces an index outside [0, size), and visits slots
 *        0, 1, ..., size-1, 0, 1, ... in order.
 */
TEST(RingBufferStateTest, AddSequenceStaysInRangeAndIsSequential)
{
    constexpr uint16_t kSize = 8;
    RingBuffer rb{};
    ASSERT_TRUE(set_ring_buffer(&rb, kSize, 0));

    for (int n = 0; n < 3 * kSize; n++)
    {
        ASSERT_TRUE(add_ring_buffer(&rb));
        ASSERT_TRUE(move_next_ring_buffer(&rb));
        ASSERT_LT(rb.current_index, kSize) << "write " << n;
        EXPECT_EQ(rb.current_index, n % kSize) << "write " << n;
        EXPECT_EQ(rb.occupancy, std::min(n + 1, (int)kSize)) << "write " << n;
    }
}

/**
 * @brief move_prev() from slot 0 on a full buffer wraps to size-1.
 */
TEST(RingBufferStateTest, MovePrevWrapsToLastSlotWhenFull)
{
    RingBuffer rb{};
    ASSERT_TRUE(set_ring_buffer(&rb, 8, 0));
    rb.occupancy = 8;

    ASSERT_TRUE(move_prev_ring_buffer(&rb));
    EXPECT_EQ(rb.current_index, 7);
}

/**
 * @brief add_ring_buffer() grows occupancy by one per call, capped at size.
 */
TEST(RingBufferStateTest, AddCapsOccupancyAtSize)
{
    RingBuffer rb{};
    ASSERT_TRUE(set_ring_buffer(&rb, 4, 0));

    for (int i = 0; i < 10; i++)
    {
        ASSERT_TRUE(add_ring_buffer(&rb));
    }
    EXPECT_EQ(rb.occupancy, 4);
}

/**
 * @brief inc_seq() increments by one and rolls UINT32_MAX over to 0.
 */
TEST(RingBufferStateTest, IncSeqRollsOverAtUint32Max)
{
    RingBuffer rb{};
    rb.seq = UINT32_MAX - 1;

    ASSERT_TRUE(inc_seq_ring_buffer(&rb));
    EXPECT_EQ(rb.seq, UINT32_MAX);
    ASSERT_TRUE(inc_seq_ring_buffer(&rb));
    EXPECT_EQ(rb.seq, 0u);
    ASSERT_TRUE(inc_seq_ring_buffer(&rb));
    EXPECT_EQ(rb.seq, 1u);
}

/**
 * @brief The RAM helpers reject NULL without crashing.
 */
TEST(RingBufferStateTest, RejectsNull)
{
    EXPECT_FALSE(init_ring_buffer(nullptr, nullptr, 8, 0));
    EXPECT_FALSE(move_next_ring_buffer(nullptr));
    EXPECT_FALSE(move_prev_ring_buffer(nullptr));
    EXPECT_FALSE(add_ring_buffer(nullptr));
    EXPECT_FALSE(inc_seq_ring_buffer(nullptr));
}

/* ================================================================
 * Reconstructing the RAM state from the SD card
 * ================================================================
 *
 * Assumed on-disk semantics (matching binary_search_head()):
 *   - each sector of the ring buffer region carries a RingBufferHeader,
 *   - seq goes up by exactly one per sector, wrapping modulo 2^32,
 *   - state == RB_OCCUPIED for every sector that has been written,
 *   - reconstruct reports current_index / occupancy in sectors.
 *
 * The region sits at kStart and is kSectors long. The sector straight after
 * it is a guard sector marked RB_OCCUPIED: any read past the end of the
 * region (e.g. checking head + 1 when head is the last sector) sees it and
 * reports a wrap that never happened.
 *
 * Every reconstruct call runs with a read budget (fail_after_read): a binary
 * search over kSectors needs ~log2(kSectors) + 3 reads, so hitting the budget
 * means the search is not converging. The budgeted read fails, the search
 * returns an error, and the test fails instead of hanging.
 */
class RingBufferReconstructTest : public ::testing::Test
{
protected:
    static constexpr uint16_t kStart = 4;
    static constexpr uint16_t kSectors = 15;
    static constexpr uint16_t kGuard = kStart + kSectors;
    static constexpr uint32_t kTotalSectors = kGuard + 1;
    static constexpr int kReadBudget = 32;

    FailableStorageCtx ctx{};
    Storage storage{};
    std::vector<uint8_t> mem;
    RingBuffer rb{};

    void SetUp() override
    {
        mem.assign(static_cast<size_t>(SECTOR_SIZE) * kTotalSectors, 0);
        ASSERT_TRUE(FailableStorage_Init(&ctx, mem.data(), SECTOR_SIZE, kTotalSectors));
        storage = FailableStorage_Make(&ctx);

        // Format: every region sector empty with a valid CRC
        for (uint16_t i = 0; i < kSectors; i++)
        {
            write_header(i, 0, RB_EMPTY);
        }
        write_raw_header(kGuard, 0xDEADBEEF, RB_OCCUPIED);

        ASSERT_TRUE(set_ring_buffer(&rb, kSectors, 0));
    }

    void write_raw_header(uint16_t raw, uint32_t seq, RB_SECTOR_STATE state)
    {
        U_RingBufferSector s;
        std::memset(&s, 0, sizeof(s));
        s.sector.header.seq = seq;
        s.sector.header.state = state;
        ASSERT_EQ(write_sector(&storage, raw, s.buffer), STRG_OK);
    }

    /** @brief Write header of ring buffer sector i (region relative). */
    void write_header(uint16_t i, uint32_t seq, RB_SECTOR_STATE state = RB_OCCUPIED)
    {
        write_raw_header(kStart + i, seq, state);
    }

    /** @brief Write seqs[i] to sector i, marking each one occupied. */
    void write_seqs(const std::vector<uint32_t> &seqs)
    {
        for (size_t i = 0; i < seqs.size(); i++)
        {
            write_header(static_cast<uint16_t>(i), seqs[i]);
        }
    }

    /** @brief Simulate n sequential sector writes starting at seq0. */
    void simulate_writes(uint32_t n, uint32_t seq0)
    {
        for (uint32_t k = 0; k < n; k++)
        {
            write_header(static_cast<uint16_t>(k % kSectors), seq0 + k);
        }
    }

    void arm_read_budget()
    {
        ctx.read_calls = 0;
        ctx.fail_after_read = kReadBudget;
    }

    bool reconstruct()
    {
        arm_read_budget();
        return reconstruct_ring_buffer(&storage, &rb, kStart);
    }

    void expect_state(uint16_t index, uint16_t occupancy, uint32_t seq)
    {
        EXPECT_EQ(rb.current_index, index);
        EXPECT_EQ(rb.occupancy, occupancy);
        EXPECT_EQ(rb.seq, seq);
        EXPECT_LT(ctx.read_calls, kReadBudget) << "binary search did not converge";
    }
};

/**
 * @brief A freshly formatted region reconstructs to an empty buffer.
 */
TEST_F(RingBufferReconstructTest, EmptyRegion)
{
    ASSERT_TRUE(reconstruct());
    EXPECT_EQ(rb.current_index, 0);
    EXPECT_EQ(rb.occupancy, 0);
}

/**
 * @brief One written sector: head 0, occupancy 1.
 */
TEST_F(RingBufferReconstructTest, SingleSector)
{
    write_seqs({1});
    ASSERT_TRUE(reconstruct());
    expect_state(0, 1, 1);
}

/**
 * @brief Partially filled, never wrapped (6 of 15).
 */
TEST_F(RingBufferReconstructTest, PartialFillNotWrapped)
{
    write_seqs({1, 2, 3, 4, 5, 6});
    ASSERT_TRUE(reconstruct());
    expect_state(5, 6, 6);
}

/**
 * @brief Exactly full, never wrapped: the head is the last sector, so the
 *        wrap check must not read past the end of the region (guard sector).
 */
TEST_F(RingBufferReconstructTest, ExactlyFullHeadAtLastSector)
{
    write_seqs({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    ASSERT_TRUE(reconstruct());
    expect_state(14, 15, 15);
}

/**
 * @brief Worked example: [15..19, 5..14] -> head 4, full.
 */
TEST_F(RingBufferReconstructTest, WrappedBuffer)
{
    write_seqs({15, 16, 17, 18, 19, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14});
    ASSERT_TRUE(reconstruct());
    expect_state(4, 15, 19);
}

/**
 * @brief Wrapped with the head at sector 0 (just started a new lap).
 */
TEST_F(RingBufferReconstructTest, WrappedHeadAtSectorZero)
{
    write_seqs({16, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    ASSERT_TRUE(reconstruct());
    expect_state(0, 15, 16);
}

/**
 * @brief Worked example: seq rolls over UINT32_MAX -> 0 inside the current
 *        lap. Head is sector 6 with seq 3.
 */
TEST_F(RingBufferReconstructTest, SeqRollsOverInsideCurrentLap)
{
    write_seqs({0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF, 0, 1, 2, 3,
                0xFFFFFFF5, 0xFFFFFFF6, 0xFFFFFFF7, 0xFFFFFFF8,
                0xFFFFFFF9, 0xFFFFFFFA, 0xFFFFFFFB, 0xFFFFFFFC});
    ASSERT_TRUE(reconstruct());
    expect_state(6, 15, 3);
}

/**
 * @brief Exhaustive: for every write count up to three laps, reconstruct
 *        matches the state the writer had (from seq 1 and from just below
 *        UINT32_MAX so the rollover lands at every position).
 */
TEST_F(RingBufferReconstructTest, MatchesWriterForEveryWriteCount)
{
    for (uint32_t seq0 : {1u, 0xFFFFFFF0u})
    {
        for (uint32_t n = 1; n <= 3u * kSectors; n++)
        {
            SetUp();
            simulate_writes(n, seq0);

            ASSERT_TRUE(reconstruct()) << "seq0=" << seq0 << " n=" << n;
            uint16_t head = static_cast<uint16_t>((n - 1) % kSectors);
            uint16_t occ = static_cast<uint16_t>(std::min<uint32_t>(n, kSectors));
            EXPECT_EQ(rb.current_index, head) << "seq0=" << seq0 << " n=" << n;
            EXPECT_EQ(rb.occupancy, occ) << "seq0=" << seq0 << " n=" << n;
            EXPECT_EQ(rb.seq, seq0 + n - 1) << "seq0=" << seq0 << " n=" << n;
        }
    }
}

/**
 * @brief A sector with a bad CRC is reported as an error, never treated as
 *        unoccupied (which would silently truncate the history).
 */
TEST_F(RingBufferReconstructTest, CorruptSectorFails)
{
    write_seqs({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    // Flip a payload byte in every sector the search could probe
    for (uint16_t i = 0; i < kSectors; i++)
    {
        mem[static_cast<size_t>(kStart + i) * SECTOR_SIZE + RING_BUFFER_HEADER_BYTES] ^= 0xFF;
    }
    EXPECT_FALSE(reconstruct());
}

/**
 * @brief A storage read failure is reported as an error.
 */
TEST_F(RingBufferReconstructTest, ReadFailureFails)
{
    write_seqs({1, 2, 3});
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    EXPECT_FALSE(reconstruct_ring_buffer(&storage, &rb, kStart));
}

/**
 * @brief binary_search_head() on its own: empty region -> STRG_EMPTY,
 *        written region -> STRG_OK with the head and its seq.
 */
TEST_F(RingBufferReconstructTest, BinarySearchHeadDirect)
{
    uint16_t head = 0xFFFF;
    uint32_t seq = 0;

    arm_read_budget();
    EXPECT_EQ(binary_search_head(&storage, kStart, kSectors, &head, &seq), STRG_EMPTY);

    write_seqs({15, 16, 17, 18, 19, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14});
    arm_read_budget();
    ASSERT_EQ(binary_search_head(&storage, kStart, kSectors, &head, &seq), STRG_OK);
    EXPECT_EQ(head, 4);
    EXPECT_EQ(seq, 19u);
}

/**
 * @brief is_ring_buffer_wrapped(): occupied next sector -> STRG_OK,
 *        empty next sector -> STRG_EMPTY, read failure -> STRG_FAIL.
 */
TEST_F(RingBufferReconstructTest, IsWrappedDirect)
{
    write_seqs({1, 2, 3});
    EXPECT_EQ(is_ring_buffer_wrapped(&storage, kStart, 2), STRG_EMPTY);
    EXPECT_EQ(is_ring_buffer_wrapped(&storage, kStart, 1), STRG_OK);

    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    EXPECT_EQ(is_ring_buffer_wrapped(&storage, kStart, 1), STRG_FAIL);
}

/**
 * @brief init_ring_buffer() on a freshly formatted region resets every RAM
 *        field, regardless of what the struct held before.
 */
TEST_F(RingBufferReconstructTest, InitOnEmptyRegionResetsAllFields)
{
    std::memset(&rb, 0xAB, sizeof(rb));
    arm_read_budget();

    ASSERT_TRUE(init_ring_buffer(&storage, &rb, kSectors, kStart));
    EXPECT_EQ(rb.size, kSectors);
    EXPECT_EQ(rb.current_index, 0);
    EXPECT_EQ(rb.occupancy, 0);
    EXPECT_EQ(rb.seq, 0u);
}

/**
 * @brief init_ring_buffer() sets the size and reconstructs the state from
 *        the card (worked example [15..19, 5..14]).
 */
TEST_F(RingBufferReconstructTest, InitReconstructsFromStorage)
{
    write_seqs({15, 16, 17, 18, 19, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14});
    std::memset(&rb, 0xAB, sizeof(rb));
    arm_read_budget();

    ASSERT_TRUE(init_ring_buffer(&storage, &rb, kSectors, kStart));
    EXPECT_EQ(rb.size, kSectors);
    expect_state(4, 15, 19);
}

/**
 * @brief init_ring_buffer() rejects a zero size.
 */
TEST_F(RingBufferReconstructTest, InitRejectsZeroSize)
{
    EXPECT_FALSE(init_ring_buffer(&storage, &rb, 0, kStart));
}
