#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "usage_bitmap.h"
#include "storage.h"
#include "superheader.h"
#include "mem_layout.h"
}

#include "test_support/failable_storage.h"

class UsageBitmapEdgeTest : public ::testing::Test
{
protected:
    FailableStorageCtx ctx{};
    Storage storage{};

    uint8_t *storage_mem = nullptr;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        SUPERHEADER_SECTOR_SIZE + USAGE_BITMAP_SECTOR_SIZE + TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        storage_mem = new uint8_t[SECTOR_SIZE * STORAGE_SECTOR_COUNT];
        ASSERT_NE(storage_mem, nullptr);
        memset(storage_mem, 0, SECTOR_SIZE * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(FailableStorage_Init(&ctx, storage_mem, SECTOR_SIZE, STORAGE_SECTOR_COUNT));
        storage = FailableStorage_Make(&ctx);

        memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    }

    void TearDown() override
    {
        delete[] storage_mem;
        storage_mem = nullptr;
    }
};

/* ============================================================================
 * init_usage_bitmap()
 * ========================================================================== */

/**
 * @brief init_usage_bitmap() zeroes both the RAM bitmap and every bitmap
 *        sector on storage, even if bits were previously set.
 */
TEST_F(UsageBitmapEdgeTest, InitClearsPreviouslySetBitsInRamAndStorage)
{
    ASSERT_TRUE(update_usage_bit(&storage, 10, true));
    ASSERT_TRUE(update_usage_bit(&storage, 5000, true));
    ASSERT_TRUE(check_usage_bit(10));

    ASSERT_TRUE(init_usage_bitmap(&storage));

    EXPECT_FALSE(check_usage_bit(10));
    EXPECT_FALSE(check_usage_bit(5000));

    uint8_t sector0[SECTOR_SIZE]{};
    ASSERT_TRUE(storage.read_block(storage.context, USAGE_BITMAP_START_SECTOR, sector0));

    uint8_t zero[SECTOR_SIZE]{};
    EXPECT_EQ(memcmp(sector0, zero, SECTOR_SIZE), 0);
}

/**
 * @brief init_usage_bitmap() propagates a storage write failure rather
 *        than reporting success with a zeroed RAM bitmap that storage
 *        doesn't actually agree with.
 */
TEST_F(UsageBitmapEdgeTest, InitPropagatesStorageWriteFailure)
{
    ctx.fail_after_write = 1;

    EXPECT_FALSE(init_usage_bitmap(&storage));
}

/* ============================================================================
 * read_usage_bitmap()
 * ========================================================================== */

/**
 * @brief read_usage_bitmap() propagates a storage read failure.
 */
TEST_F(UsageBitmapEdgeTest, ReadPropagatesStorageReadFailure)
{
    ctx.fail_after_read = 1;

    EXPECT_FALSE(read_usage_bitmap(&storage));
}

/**
 * @brief A read_usage_bitmap() failure does not corrupt bits already in
 *        RAM -- the call is a straight passthrough to read_multiblock(),
 *        which either fills the whole buffer or (per HeapStorage) leaves
 *        it untouched on failure.
 */
TEST_F(UsageBitmapEdgeTest, FailedReadLeavesExistingRamBitsIntact)
{
    ASSERT_TRUE(update_usage_bit(&storage, 7, true));

    ctx.fail_after_read = 1;
    EXPECT_FALSE(read_usage_bitmap(&storage));

    EXPECT_TRUE(check_usage_bit(7));
}

/* ============================================================================
 * update_usage_bit()
 * ========================================================================== */

/**
 * @brief update_usage_bit() propagates a storage write failure as
 *        STRG_FAIL rather than reporting success.
 */
TEST_F(UsageBitmapEdgeTest, UpdatePropagatesStorageWriteFailure)
{
    ctx.fail_after_write = 1;

    EXPECT_EQ(update_usage_bit(&storage, 3, true), STRG_FAIL);
}

/**
 * @brief Characterisation test: update_usage_bit() mutates the global RAM
 *        bitmap *before* attempting the storage write. If that write
 *        fails, RAM and storage are left disagreeing -- check_usage_bit()
 *        reports the new state even though storage still has the old
 *        one. This isn't asserting that's *correct* (a caller relying on
 *        RAM/storage staying in sync across a failed write would be
 *        wrong to), just pinning down what actually happens today so a
 *        future change to the ordering is a deliberate decision, not an
 *        accident.
 */
TEST_F(UsageBitmapEdgeTest, FailedWriteLeavesRamAheadOfStorage)
{
    ctx.fail_after_write = 1;

    EXPECT_EQ(update_usage_bit(&storage, 3, true), STRG_FAIL);

    // RAM already reflects "set"...
    EXPECT_TRUE(check_usage_bit(3));

    // ...but storage was never actually written.
    uint8_t sector0[SECTOR_SIZE]{};
    ASSERT_TRUE(storage.read_block(storage.context, USAGE_BITMAP_START_SECTOR, sector0));
    uint8_t zero[SECTOR_SIZE]{};
    EXPECT_EQ(memcmp(sector0, zero, SECTOR_SIZE), 0);
}

/**
 * @brief Setting an already-set bit is idempotent: RAM and storage still
 *        read back exactly one bit set, not corrupted by the repeat.
 */
TEST_F(UsageBitmapEdgeTest, SettingAlreadySetBitIsIdempotent)
{
    ASSERT_TRUE(update_usage_bit(&storage, 42, true));
    ASSERT_TRUE(update_usage_bit(&storage, 42, true));

    EXPECT_TRUE(check_usage_bit(42));
    EXPECT_FALSE(check_usage_bit(41));
    EXPECT_FALSE(check_usage_bit(43));
}

/**
 * @brief Clearing a bit that was never set is a safe no-op.
 */
TEST_F(UsageBitmapEdgeTest, ClearingAlreadyClearBitIsNoOp)
{
    ASSERT_TRUE(update_usage_bit(&storage, 99, false));

    EXPECT_FALSE(check_usage_bit(99));
}

/* ============================================================================
 * Addressable-range boundaries
 *
 * USAGE_BITMAP_STORAGE_SIZE is rounded up to a whole number of 512B
 * sectors, so the global `usage_bitmap` array is sized larger than
 * TOTAL_DATA_SECTOR_SIZE bits -- there's a padding region above the last
 * real data-sector index that's still safely within the array bounds.
 * These tests characterise that boundary; they deliberately do NOT probe
 * past USAGE_BITMAP_STORAGE_SIZE * 32 - 1, since check_usage_bit() and
 * update_usage_bit() do no bounds checking at all and an index at or
 * beyond that point indexes past the end of the array (undefined
 * behaviour, not a `false`/`STRG_FAIL` return) -- see the recommended
 * additional tests in notes/unit_tests for more on this.
 * ========================================================================== */

/**
 * @brief The first index past the last real data sector (still inside
 *        the padded array) round-trips mechanically, even though it
 *        doesn't correspond to any real sector.
 */
TEST_F(UsageBitmapEdgeTest, FirstPaddingBitAfterLastDataSectorIsUsable)
{
    const uint32_t index = TOTAL_DATA_SECTOR_SIZE;

    ASSERT_LT(index, USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT)
        << "test assumption: there is padding above TOTAL_DATA_SECTOR_SIZE";

    ASSERT_TRUE(update_usage_bit(&storage, index, true));
    EXPECT_TRUE(check_usage_bit(index));
}

/**
 * @brief The very last bit the global array can address round-trips.
 */
TEST_F(UsageBitmapEdgeTest, LastAddressableArrayBitIsUsable)
{
    const uint32_t index = USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT - 1;

    ASSERT_TRUE(update_usage_bit(&storage, index, true));
    EXPECT_TRUE(check_usage_bit(index));
}

/* ============================================================================
 * Degenerate / undersized storage
 * ========================================================================== */

/**
 * @brief update_usage_bit() doesn't itself validate that the target
 *        bitmap sector actually exists on the given storage -- it relies
 *        entirely on the underlying write_block()'s own bounds check.
 *        Confirms that protection is actually there: a storage far too
 *        small to hold USAGE_BITMAP_START_SECTOR must fail the write
 *        rather than write out of range.
 */
TEST_F(UsageBitmapEdgeTest, UpdateFailsWhenTargetSectorExceedsStorageCapacity)
{
    FailableStorageCtx small_ctx{};
    uint8_t small_mem[SECTOR_SIZE]{};
    ASSERT_TRUE(FailableStorage_Init(&small_ctx, small_mem, SECTOR_SIZE, 1)); // 1 sector total

    ASSERT_GT(USAGE_BITMAP_START_SECTOR, 0u)
        << "test assumption: bit 0's bitmap sector is not sector 0 on real storage";

    Storage small_storage = FailableStorage_Make(&small_ctx);

    EXPECT_EQ(update_usage_bit(&small_storage, 0, true), STRG_FAIL);
}
