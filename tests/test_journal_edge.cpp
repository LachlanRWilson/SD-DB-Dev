#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C"
{
#include "journal.h"
#include "storage.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
}

#include "test_support/failable_storage.h"

/**
 * @brief Failure-path and boundary coverage for journal.c, complementing
 *        test_journal.cpp. Uses FailableStorageCtx to make a specific read
 *        or write fail on demand.
 */
class JournalEdgeTest : public ::testing::Test
{
protected:
    Journal journal{};

    FailableStorageCtx ctx{};
    Storage storage{};

    std::vector<uint8_t> storage_mem;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        storage_mem.assign(static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT, 0);
        ASSERT_TRUE(FailableStorage_Init(&ctx, storage_mem.data(), SECTOR_SIZE, STORAGE_SECTOR_COUNT));
        storage = FailableStorage_Make(&ctx);

        std::memset(&journal, 0, sizeof(Journal));
        journal.storage = &storage;

        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    }

    uint8_t *raw(uint32_t sector)
    {
        return &storage_mem[static_cast<size_t>(sector) * SECTOR_SIZE];
    }

    JRNL_STATE stored_state()
    {
        JournalHeaderBuffer header{};
        std::memcpy(header.buffer, raw(JRNL_HEADER_SECTOR), SECTOR_SIZE);
        return header.var.data.var.state;
    }

    /** @brief Make the n-th read from now fail (1 = the very next read). */
    void fail_read_in(int n)
    {
        ctx.read_calls = 0;
        ctx.fail_after_read = n;
    }

    /** @brief Make the n-th write from now fail (1 = the very next write). */
    void fail_write_in(int n)
    {
        ctx.write_calls = 0;
        ctx.fail_after_write = n;
    }

    static std::vector<uint8_t> stamped_sector(uint8_t value)
    {
        std::vector<uint8_t> sector(SECTOR_SIZE, value);
        sector_crc_stamp(sector.data());
        return sector;
    }

    /**
     * @brief Leave an active journal entry for data_sector on the card, with
     *        the data sector modified afterwards, and load the header into RAM
     *        as journal_init() would.
     */
    void make_pending_rollback(uint16_t data_sector)
    {
        std::vector<uint8_t> original = stamped_sector(0xAB);
        ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

        std::vector<uint8_t> modified = stamped_sector(0x99);
        ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), modified.data()), STRG_OK);

        ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    }
};

/* ============================================================================
 * journal_header_init()
 * ========================================================================== */

TEST_F(JournalEdgeTest, HeaderInitFailsWhenWriteFails)
{
    fail_write_in(1);
    EXPECT_FALSE(journal_header_init(&journal));
}

/* ============================================================================
 * get_journal_status()
 * ========================================================================== */

TEST_F(JournalEdgeTest, StatusReportsReadErrorWhenReadFails)
{
    fail_read_in(1);
    EXPECT_EQ(get_journal_status(&journal), JRNL_READ_ERROR);
}

/* ============================================================================
 * journal_add() / journal_write() failures
 *
 * Write order is bitmap copy (1), content copy (2), header (3).
 * ========================================================================== */

TEST_F(JournalEdgeTest, AddFailsWhenBitmapWriteFails)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    fail_write_in(1);
    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
}

TEST_F(JournalEdgeTest, AddFailsWhenContentWriteFails)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    fail_write_in(2);
    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
}

TEST_F(JournalEdgeTest, AddFailsWhenHeaderWriteFails)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    fail_write_in(3);
    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
}

/**
 * @brief A failed add never leaves an ACTIVE header behind, because the
 *        header is the last write.
 */
TEST_F(JournalEdgeTest, FailedAddNeverLeavesActiveHeader)
{
    ASSERT_TRUE(journal_header_init(&journal));
    std::vector<uint8_t> content = stamped_sector(0x11);

    for (int n = 1; n <= 3; n++)
    {
        fail_write_in(n);
        EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
        EXPECT_EQ(stored_state(), JRNL_EMPTY) << "failed write " << n;
    }
}

/* ============================================================================
 * journal_free() failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, FreeFailsWhenHeaderWriteFails)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));

    fail_write_in(1);
    EXPECT_EQ(journal_free(&journal), STRG_FAIL);
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

/* ============================================================================
 * journal_rollback() failures
 *
 * Reads: content copy (1), bitmap copy (2).
 * Writes: data sector (1), bitmap sector (2), commit header (3).
 * Every failure must leave the journal ACTIVE so the next boot retries.
 * ========================================================================== */

TEST_F(JournalEdgeTest, RollbackFailsWhenContentReadFails)
{
    make_pending_rollback(2);

    fail_read_in(1);
    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(2))[0], 0x99); // untouched
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

TEST_F(JournalEdgeTest, RollbackFailsWhenBitmapReadFails)
{
    make_pending_rollback(2);

    fail_read_in(2);
    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

TEST_F(JournalEdgeTest, RollbackFailsWhenSectorWriteFails)
{
    make_pending_rollback(2);

    fail_write_in(1);
    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(2))[0], 0x99);
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

TEST_F(JournalEdgeTest, RollbackFailsWhenBitmapWriteFails)
{
    make_pending_rollback(2);

    fail_write_in(2);
    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

TEST_F(JournalEdgeTest, RollbackFailsWhenCommitWriteFails)
{
    make_pending_rollback(2);

    fail_write_in(3);
    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(2))[0], 0xAB); // restored, just not committed
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

/* ============================================================================
 * journal_init() failures
 * ========================================================================== */

TEST_F(JournalEdgeTest, InitFailsWhenStatusReadFails)
{
    fail_read_in(1);
    EXPECT_FALSE(journal_init(&journal, &storage));
}

TEST_F(JournalEdgeTest, InitFailsWhenUninitialisedHeaderWriteFails)
{
    fail_write_in(1);
    EXPECT_FALSE(journal_init(&journal, &storage));
}

TEST_F(JournalEdgeTest, InitPropagatesRollbackFailure)
{
    make_pending_rollback(2);
    std::memset(&journal, 0, sizeof(Journal));

    fail_write_in(1); // the rollback's data sector restore
    EXPECT_FALSE(journal_init(&journal, &storage));
    EXPECT_EQ(stored_state(), JRNL_ACTIVE);
}

/* ============================================================================
 * Boundaries
 * ========================================================================== */

/**
 * @brief The first data sector round-trips through add and rollback.
 */
TEST_F(JournalEdgeTest, RollbackFirstDataSector)
{
    make_pending_rollback(0);
    ASSERT_TRUE(journal_rollback(&journal));

    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(0))[0], 0xAB);
    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(0) - 1)[0], 0x00); // last bitmap sector untouched by the data write
}

/**
 * @brief The last data sector round-trips through add and rollback, using
 *        the last usage bitmap sector.
 */
TEST_F(JournalEdgeTest, RollbackLastDataSector)
{
    const uint16_t last = TOTAL_DATA_SECTOR_SIZE - 1;

    make_pending_rollback(last);
    EXPECT_EQ(journal.header.var.data.var.sector, last);
    ASSERT_TRUE(journal_rollback(&journal));

    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(last))[0], 0xAB);
    EXPECT_TRUE(sector_crc_valid(raw(USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_FIND_SECTOR(last))));
}
