#include <gtest/gtest.h>
#include <vector>

extern "C" {
#include "storage.h"
#include "mem_layout.h"
#include "free_list_stack.h"
}

class FreeListEdgeTest : public ::testing::Test
{
protected:
    FreeList allocator{};
    std::vector<uint16_t> pool;

    /** @brief Init a normal (fully-free) allocator with `n` sectors. */
    bool init(size_t n)
    {
        pool.assign(n, 0);
        return free_list_init(&allocator, pool.data(), n);
    }

    /** @brief Init an allocator with `n` sectors, all marked used. */
    bool init_empty(size_t n)
    {
        pool.assign(n, 0);
        return free_list_empty_init(&allocator, pool.data(), n);
    }
};

/* ============================================================================
 * free_list_init() / free_list_empty_init() argument validation
 * ========================================================================== */

TEST_F(FreeListEdgeTest, InitRejectsZeroCapacity)
{
    std::vector<uint16_t> p(1, 0);
    EXPECT_FALSE(free_list_init(&allocator, p.data(), 0));
}

TEST_F(FreeListEdgeTest, EmptyInitRejectsZeroCapacity)
{
    std::vector<uint16_t> p(1, 0);
    EXPECT_FALSE(free_list_empty_init(&allocator, p.data(), 0));
}

TEST_F(FreeListEdgeTest, EmptyInitRejectsNullPool)
{
    EXPECT_FALSE(free_list_empty_init(&allocator, nullptr, 10));
}

TEST_F(FreeListEdgeTest, EmptyInitRejectsNullSelf)
{
    std::vector<uint16_t> p(10, 0);
    EXPECT_FALSE(free_list_empty_init(nullptr, p.data(), 10));
}

TEST_F(FreeListEdgeTest, InitSoftwareRejectsZeroCapacity)
{
    FreeList self{};
    EXPECT_FALSE(free_list_init_software(&self, 0));
}

/* ============================================================================
 * Single-slot allocator (the smallest non-degenerate case)
 * ========================================================================== */

TEST_F(FreeListEdgeTest, SingleSlotAllocatorAllocatesThenExhausts)
{
    ASSERT_TRUE(init(1));

    EXPECT_EQ(free_list_allocate(&allocator), 0u);
    EXPECT_EQ(free_list_allocate(&allocator), UINT16_MAX);

    free_list_free(&allocator, 0);
    EXPECT_EQ(free_list_allocate(&allocator), 0u);
}

/* ============================================================================
 * free_list_allocate() on an unusable allocator
 * ========================================================================== */

TEST_F(FreeListEdgeTest, AllocateOnNullSelfReturnsMax)
{
    EXPECT_EQ(free_list_allocate(nullptr), UINT16_MAX);
}

TEST_F(FreeListEdgeTest, AllocateOnZeroInitialisedAllocatorReturnsMax)
{
    FreeList never_initialised{};
    EXPECT_EQ(free_list_allocate(&never_initialised), UINT16_MAX);
}

/* ============================================================================
 * free_list_free() defensive behaviour
 * ========================================================================== */

TEST_F(FreeListEdgeTest, FreeOnFullyFreeAllocatorDoesNotOvergrowOrUnderflow)
{
    ASSERT_TRUE(init(4));

    // Nothing has been allocated: stack_top is already at capacity.
    // Freeing an arbitrary index here must be a no-op, not push a 5th
    // entry onto a 4-slot stack or underflow used_count below zero.
    free_list_free(&allocator, 0);

    EXPECT_EQ(free_list_available(&allocator), 4u);
    EXPECT_EQ(free_list_used(&allocator), 0u);

    // The allocator must still behave normally afterwards.
    for (int i = 0; i < 4; i++)
    {
        EXPECT_NE(free_list_allocate(&allocator), UINT16_MAX);
    }
    EXPECT_EQ(free_list_allocate(&allocator), UINT16_MAX);
}

TEST_F(FreeListEdgeTest, DoubleFreeDoesNotDuplicateSector)
{
    ASSERT_TRUE(init(4));

    uint16_t a = free_list_allocate(&allocator);
    ASSERT_NE(a, UINT16_MAX);

    free_list_free(&allocator, a);
    // Freeing the same sector again: stack_top is now back at capacity,
    // so this must be rejected as a no-op rather than pushing `a` twice.
    free_list_free(&allocator, a);

    EXPECT_EQ(free_list_available(&allocator), 4u);
    EXPECT_EQ(free_list_used(&allocator), 0u);
}

/* ============================================================================
 * free_list_free_range()
 * ========================================================================== */

TEST_F(FreeListEdgeTest, FreeRangeEmptyRangeIsNoOp)
{
    ASSERT_TRUE(init_empty(4));

    free_list_free_range(&allocator, 2, 2); // startIndex == endIndex

    EXPECT_EQ(free_list_available(&allocator), 0u);
}

TEST_F(FreeListEdgeTest, FreeRangeInvertedRangeIsNoOp)
{
    ASSERT_TRUE(init_empty(4));

    free_list_free_range(&allocator, 3, 1); // startIndex > endIndex

    EXPECT_EQ(free_list_available(&allocator), 0u);
}

TEST_F(FreeListEdgeTest, FreeRangeClampsAtCapacity)
{
    ASSERT_TRUE(init_empty(4));

    // endIndex (100) is far beyond capacity (4); must free [0, 4) and
    // stop, not walk off the end of the backing array.
    free_list_free_range(&allocator, 0, 100);

    EXPECT_EQ(free_list_available(&allocator), 4u);
    EXPECT_EQ(free_list_used(&allocator), 0u);
}

TEST_F(FreeListEdgeTest, FreeRangePartialOverlapFreesOnlyInRange)
{
    ASSERT_TRUE(init_empty(10));

    free_list_free_range(&allocator, 3, 7); // frees slots 3,4,5,6

    EXPECT_EQ(free_list_available(&allocator), 4u);
    EXPECT_EQ(free_list_used(&allocator), 6u);
}

/* ============================================================================
 * free_list_free_sector_range()
 *
 * Regression coverage: this function used to contain an infinite loop
 * (its for-loop condition checked `startSector < endSector`, which never
 * changes across iterations) plus a copy-paste bug that ignored the loop
 * variable entirely. It's now a single call that converts a *sector*
 * range into a *slot* range via CONTACT_SECTOR_CAPACITY and delegates to
 * free_list_free_range(). These tests exist to (a) prove it terminates at
 * all -- the test suite hanging/timing out is itself a failure signal for
 * the old bug -- and (b) prove the slot-range math is correct.
 * ========================================================================== */

TEST_F(FreeListEdgeTest, FreeSectorRangeTerminatesAndFreesExpectedSlotCount)
{
    const uint16_t sectors = 5;
    const size_t slot_capacity = sectors * CONTACT_SECTOR_CAPACITY;

    ASSERT_TRUE(init_empty(slot_capacity));

    // Free physical sectors [1, 3) -> should free exactly
    // 2 * CONTACT_SECTOR_CAPACITY contact slots.
    free_list_free_sector_range(&allocator, 1, 3);

    EXPECT_EQ(free_list_available(&allocator), 2u * CONTACT_SECTOR_CAPACITY);
}

TEST_F(FreeListEdgeTest, FreeSectorRangeFreesExactSlotWindow)
{
    const uint16_t sectors = 5;
    const size_t slot_capacity = sectors * CONTACT_SECTOR_CAPACITY;

    ASSERT_TRUE(init_empty(slot_capacity));

    free_list_free_sector_range(&allocator, 1, 3);

    // Every slot handed out by allocate() must fall within
    // [1*CAP, 3*CAP) -- i.e. exactly the freed window, nothing outside it.
    const uint16_t low = (uint16_t)(1 * CONTACT_SECTOR_CAPACITY);
    const uint16_t high = (uint16_t)(3 * CONTACT_SECTOR_CAPACITY);

    uint16_t got;
    while ((got = free_list_allocate(&allocator)) != UINT16_MAX)
    {
        EXPECT_GE(got, low);
        EXPECT_LT(got, high);
    }
}

TEST_F(FreeListEdgeTest, FreeSectorRangeEmptyRangeIsNoOp)
{
    const uint16_t sectors = 3;
    const size_t slot_capacity = sectors * CONTACT_SECTOR_CAPACITY;

    ASSERT_TRUE(init_empty(slot_capacity));

    free_list_free_sector_range(&allocator, 2, 2);

    EXPECT_EQ(free_list_available(&allocator), 0u);
}

/* ============================================================================
 * free_list_reset()
 * ========================================================================== */

TEST_F(FreeListEdgeTest, ResetOnNeverInitialisedAllocatorIsSafeNoOp)
{
    FreeList never_initialised{};
    // free_stack is NULL; reset must not dereference it.
    free_list_reset(&never_initialised);
    EXPECT_EQ(free_list_allocate(&never_initialised), UINT16_MAX);
}

TEST_F(FreeListEdgeTest, ResetAfterFullExhaustionRestoresEverything)
{
    ASSERT_TRUE(init(5));

    for (int i = 0; i < 5; i++)
    {
        free_list_allocate(&allocator);
    }
    ASSERT_EQ(free_list_allocate(&allocator), UINT16_MAX);

    free_list_reset(&allocator);

    EXPECT_EQ(free_list_used(&allocator), 0u);
    EXPECT_EQ(free_list_available(&allocator), 5u);
}

/* ============================================================================
 * LIFO ordering under interleaved alloc/free
 * ========================================================================== */

TEST_F(FreeListEdgeTest, InterleavedAllocateFreeStaysLifo)
{
    ASSERT_TRUE(init(3));

    uint16_t a = free_list_allocate(&allocator); // 2
    uint16_t b = free_list_allocate(&allocator); // 1
    free_list_free(&allocator, a);               // push 2 back on top
    uint16_t c = free_list_allocate(&allocator); // must reuse 2, not 0

    EXPECT_EQ(c, a);
    EXPECT_NE(b, a);
}
