#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "journal.h"
#include "crc.h"
#include "storage.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
}

#include "test_support/failable_storage.h"

/**
 * @brief Failure-path/boundary coverage for journal.c, complementing
 *        test_journal.cpp's happy-path and CRC-corruption tests. Uses
 *        FailableStorageCtx to make a specific read/write fail on demand
 *        -- something the real HeapStorage-backed fixture in
 *        test_journal.cpp can't do, since it only fails on out-of-range
 *        access.
 */
class JournalEdgeTest : public ::testing::Test
{
protected:
    Journal journal{};

    FailableStorageCtx ctx{};
    Storage storage{};

    uint8_t *storage_mem = nullptr;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        SUPERHEADER_SECTOR_SIZE + USAGE_BITMAP_SECTOR_SIZE + JRNL_SECTOR_SIZE + TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        storage_mem = new uint8_t[SECTOR_SIZE * STORAGE_SECTOR_COUNT];
        ASSERT_NE(storage_mem, nullptr);
        memset(storage_mem, 0, SECTOR_SIZE * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(FailableStorage_Init(&ctx, storage_mem, SECTOR_SIZE, STORAGE_SECTOR_COUNT));
        storage = FailableStorage_Make(&ctx);

        memset(&journal, 0, sizeof(Journal));
        journal.storage = &storage;

        memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    }

    void TearDown() override
    {
        delete[] storage_mem;
        storage_mem = nullptr;
    }

    JournalHeaderBuffer create_header(uint8_t state, uint8_t type = JRNL_CONTACT, uint16_t sector = 0,
                                       const uint8_t *content = nullptr,
                                       const uint8_t *usage_bitmap_backup = nullptr)
    {
        JournalHeaderBuffer header{};

        header.var.data.var.magic = JRNL_MAGIC;
        header.var.data.var.state = state;
        header.var.data.var.type = type;
        header.var.data.var.sector = sector;

        header.var.header_crc = crc32_calculate(header.var.data.buffer, sizeof(JournalHeaderData));

        if (content != nullptr)
        {
            header.var.content_crc = crc32_calculate(content, SECTOR_SIZE);
        }
        if (usage_bitmap_backup != nullptr)
        {
            header.var.usage_bitmap_crc = crc32_calculate(usage_bitmap_backup, SECTOR_SIZE);
        }

        return header;
    }

    void write_header(JournalHeaderBuffer &header)
    {
        ASSERT_TRUE(storage.write_block(storage.context, JRNL_HEADER_SECTOR, header.buffer));
    }

    void fill_pattern(uint8_t *buffer, uint8_t value)
    {
        memset(buffer, value, SECTOR_SIZE);
    }
};

/* ============================================================================
 * journal_header_init() failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, HeaderInitFailsWhenWriteFails)
{
    ctx.fail_after_write = 1;

    EXPECT_FALSE(journal_header_init(&journal));
}

/* ============================================================================
 * get_journal_status() failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, StatusReportsReadErrorWhenReadFails)
{
    ctx.fail_after_read = 1;

    EXPECT_EQ(get_journal_status(&journal), JRNL_READ_ERROR);
}

/* ============================================================================
 * journal_add() failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, AddFailsWhenHeaderWriteFails)
{
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAB);

    // journal_write() writes header, then content, then usage bitmap, in
    // that order -- the 1st write call is the header.
    ctx.fail_after_write = 1;

    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 2, content));
}

TEST_F(JournalEdgeTest, AddFailsWhenContentWriteFails)
{
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAB);

    // 2nd write call is the content sector.
    ctx.fail_after_write = 2;

    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 2, content));

    // The header write (1st call) already landed -- journal_add() doesn't
    // roll that back on a later failure. Documenting the actual behaviour:
    // a half-written journal entry (valid header, no matching content) is
    // left on storage.
    JournalHeaderBuffer header{};
    ASSERT_TRUE(storage.read_block(storage.context, JRNL_HEADER_SECTOR, header.buffer));
    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_ACTIVE);
}

TEST_F(JournalEdgeTest, AddFailsWhenUsageBitmapWriteFails)
{
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAB);

    // 3rd write call is the usage-bitmap backup.
    ctx.fail_after_write = 3;

    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, 2, content));
}

/* ============================================================================
 * journal_write() failure (the primitive journal_add() sits on top of)
 * ========================================================================== */

TEST_F(JournalEdgeTest, WriteFailsWhenHeaderWriteFails)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, JRNL_CONTACT, 2);
    uint8_t content[SECTOR_SIZE]{};
    uint8_t bitmap_backup[SECTOR_SIZE]{};

    ctx.fail_after_write = 1;

    EXPECT_EQ(journal_write(&journal, &header, content, bitmap_backup), STRG_FAIL);
}

TEST_F(JournalEdgeTest, WriteFailsWhenContentWriteFails)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, JRNL_CONTACT, 2);
    uint8_t content[SECTOR_SIZE]{};
    uint8_t bitmap_backup[SECTOR_SIZE]{};

    ctx.fail_after_write = 2;

    EXPECT_EQ(journal_write(&journal, &header, content, bitmap_backup), STRG_FAIL);
}

/* ============================================================================
 * journal_header_read() / journal_content_read() / journal_usage_read()
 * failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, HeaderReadFailsWhenStorageReadFails)
{
    JournalHeaderBuffer header{};
    ctx.fail_after_read = 1;

    EXPECT_FALSE(journal_header_read(&journal, &header));
}

TEST_F(JournalEdgeTest, ContentReadFailsWhenStorageReadFails)
{
    ctx.fail_after_read = 1;

    EXPECT_FALSE(journal_content_read(&journal));
}

TEST_F(JournalEdgeTest, UsageReadFailsWhenStorageReadFails)
{
    ctx.fail_after_read = 1;

    EXPECT_FALSE(journal_usage_read(&journal));
}

/* ============================================================================
 * journal_rollback() failure paths
 * ========================================================================== */

/**
 * @brief Rollback fails if it can't even read back the journalled content
 *        (as opposed to the CRC-mismatch cases already covered in
 *        test_journal.cpp, which succeed at reading but find bad data).
 */
TEST_F(JournalEdgeTest, RollbackFailsWhenContentReadFails)
{
    const uint16_t target_sector = 2;
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAA);

    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, JRNL_CONTACT, target_sector, content, content);
    journal.header = header;
    write_header(header);

    ctx.fail_after_read = 1; // journal_content_read()'s read

    EXPECT_FALSE(journal_rollback(&journal));
}

/**
 * @brief Rollback fails if restoring the target data sector fails, and
 *        does not mark the journal committed -- so a retry on the next
 *        boot will attempt the rollback again rather than treating the
 *        (failed) recovery as done.
 */
TEST_F(JournalEdgeTest, RollbackFailsAndStaysActiveWhenSectorWriteFails)
{
    const uint16_t target_sector = 2;
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAA);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, target_sector, content));
    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);

    ctx.write_calls = 0; // journal_add() above already made 3 writes; start counting fresh
    ctx.fail_after_write = 1; // the very next write: restoring the target sector

    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_ACTIVE);
}

/**
 * @brief Rollback fails if restoring the real usage-bitmap sector fails,
 *        even though the data sector write (the step before it) already
 *        succeeded.
 */
TEST_F(JournalEdgeTest, RollbackFailsWhenUsageBitmapWriteFails)
{
    const uint16_t target_sector = 2;
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAA);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, target_sector, content));
    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);

    ctx.write_calls = 0; // journal_add() above already made 3 writes; start counting fresh
    ctx.fail_after_write = 2; // 1st write (target sector) succeeds, 2nd (bitmap) fails

    EXPECT_FALSE(journal_rollback(&journal));
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_ACTIVE);
}

/* ============================================================================
 * journal_free() failure
 * ========================================================================== */

TEST_F(JournalEdgeTest, FreeFailsWhenHeaderWriteFails)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, JRNL_CONTACT, 2);
    journal.header = header;
    write_header(header); // consumes 1 write; reset before targeting journal_free()'s own write

    ctx.write_calls = 0;
    ctx.fail_after_write = 1;

    EXPECT_FALSE(journal_free(&journal));

    // journal_free() updates journal->header in RAM unconditionally
    // before attempting the write, so the in-RAM struct now says
    // COMMITTED even though the write failed and storage still says
    // ACTIVE. Documenting this so it isn't mistaken for a synced state.
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_COMMITTED);

    JournalHeaderBuffer stored{};
    ASSERT_TRUE(storage.read_block(storage.context, JRNL_HEADER_SECTOR, stored.buffer));
    EXPECT_EQ(stored.var.data.var.state, JRNL_ACTIVE);
}

/* ============================================================================
 * journal_init() failure paths not covered by test_journal.cpp
 * ========================================================================== */

TEST_F(JournalEdgeTest, InitFailsWhenStatusReadFails)
{
    ctx.fail_after_read = 1;

    EXPECT_FALSE(journal_init(&journal, &storage));
}

TEST_F(JournalEdgeTest, InitFailsWhenUninitialisedHeaderWriteFails)
{
    // Storage starts zeroed -> JRNL_UNINITIALIZED -> journal_header_init().
    ctx.fail_after_write = 1;

    EXPECT_FALSE(journal_init(&journal, &storage));
}

TEST_F(JournalEdgeTest, InitPropagatesRollbackFailure)
{
    const uint16_t target_sector = 2;
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0xAA);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, target_sector, content));

    // A fresh Journal struct, as if this were a real reboot.
    Journal recovered{};
    recovered.storage = &storage;

    ctx.write_calls = 0; // journal_add() above already made 3 writes; start counting fresh
    ctx.fail_after_write = 1; // fail the rollback's sector-restore write

    EXPECT_FALSE(journal_init(&recovered, &storage));
}

/* ============================================================================
 * Boundary sector values
 * ========================================================================== */

TEST_F(JournalEdgeTest, AddAcceptsSectorZero)
{
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0x11);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 0, content));

    JournalHeaderBuffer header{};
    ASSERT_TRUE(storage.read_block(storage.context, JRNL_HEADER_SECTOR, header.buffer));
    EXPECT_EQ(header.var.data.var.sector, 0);
}

/**
 * @brief journal_add() preserves a large-but-legitimate sector index (the
 *        last index the real usage bitmap actually covers) in the header.
 *
 * NOT tested here: literal UINT16_MAX. journal_add() computes
 * `USAGE_BITMAP_FIND_SECTOR(index) * ELEMENTS_PER_SECTOR` to locate the
 * slice of the *global* `usage_bitmap` array to back up, with no bounds
 * check against that array's real size (USAGE_BITMAP_STORAGE_SIZE). Any
 * index >= USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT (32768 for the
 * current mem_layout.h, only slightly above TOTAL_DATA_SECTOR_SIZE
 * ~30969) makes journal_add() read out of bounds of that array --
 * verified by hand: passing UINT16_MAX here segfaults the process. This
 * is a real latent bug in journal_add()/journal_rollback(), not a test
 * gap; see the recommended-tests notes for a death-test-based way to
 * pin it down without crashing the whole suite, and consider adding an
 * explicit bounds check on `index` in journal.c itself.
 */
TEST_F(JournalEdgeTest, AddPreservesLargeInRangeSectorValue)
{
    uint8_t content[SECTOR_SIZE]{};
    fill_pattern(content, 0x22);

    const uint16_t largest_safe_sector = TOTAL_DATA_SECTOR_SIZE - 1;

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, largest_safe_sector, content));

    JournalHeaderBuffer header{};
    ASSERT_TRUE(storage.read_block(storage.context, JRNL_HEADER_SECTOR, header.buffer));
    EXPECT_EQ(header.var.data.var.sector, largest_safe_sector);
}
