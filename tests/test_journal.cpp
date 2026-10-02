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
 * @brief Behaviour of the rollback journal (journal.c).
 *
 * Every journal sector (header, content copy, usage bitmap copy) carries the
 * standard CRC-32 trailer. These tests use FailableStorage (which behaves as
 * plain heap storage until a failure is armed) so tests can also simulate a
 * power cut by failing a write. Failure-path coverage lives in
 * test_journal_edge.cpp.
 */
class JournalTest : public ::testing::Test
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

        // journal_add() copies the bitmap sector straight out of the global RAM bitmap
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    }

    /** @brief Raw view of one sector of the simulated card. */
    uint8_t *raw(uint32_t sector)
    {
        return &storage_mem[static_cast<size_t>(sector) * SECTOR_SIZE];
    }

    /** @brief Header currently stored on the card. */
    JournalHeaderBuffer stored_header()
    {
        JournalHeaderBuffer header{};
        std::memcpy(header.buffer, raw(JRNL_HEADER_SECTOR), SECTOR_SIZE);
        return header;
    }

    /** @brief Flip one bit of a raw sector, simulating card corruption. */
    void corrupt(uint32_t sector, uint32_t byte)
    {
        raw(sector)[byte] ^= 0x01;
    }

    /** @brief Make the next n-th write fail (1 = the very next write). */
    void fail_write_in(int n)
    {
        ctx.write_calls = 0;
        ctx.fail_after_write = n;
    }

    /** @brief A sector-sized buffer filled with a pattern and a valid trailer. */
    static std::vector<uint8_t> stamped_sector(uint8_t value)
    {
        std::vector<uint8_t> sector(SECTOR_SIZE, value);
        sector_crc_stamp(sector.data());
        return sector;
    }

    /** @brief The RAM usage bitmap sector journal_add() backs up for a data sector. */
    static uint32_t *bitmap_sector_for(uint16_t data_sector)
    {
        return &usage_bitmap[USAGE_BITMAP_FIND_SECTOR(data_sector) * ELEMENTS_PER_SECTOR];
    }

    /** @brief Give the RAM bitmap sector for a data sector a recognisable, CRC-valid pattern. */
    static void fill_bitmap_sector(uint16_t data_sector, uint8_t value)
    {
        uint8_t *sector = reinterpret_cast<uint8_t *>(bitmap_sector_for(data_sector));
        std::memset(sector, value, SECTOR_SIZE);
        sector_crc_stamp(sector);
    }
};

/* ============================================================================
 * journal_header_init()
 * ========================================================================== */

/**
 * @brief The initial header is EMPTY, carries the magic, and has a valid trailer.
 */
TEST_F(JournalTest, HeaderInitWritesEmptyHeaderWithValidTrailer)
{
    ASSERT_TRUE(journal_header_init(&journal));

    JournalHeaderBuffer header = stored_header();
    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_EMPTY);
    EXPECT_EQ(header.var.data.var.type, 0);
    EXPECT_EQ(header.var.data.var.sector, 0);
    EXPECT_TRUE(sector_crc_valid(header.buffer));
}

/* ============================================================================
 * get_journal_status()
 * ========================================================================== */

/**
 * @brief A never-written (all 0x00) header is uninitialised, not corrupted.
 */
TEST_F(JournalTest, StatusBlankZeroHeaderIsUninitialized)
{
    EXPECT_EQ(get_journal_status(&journal), JRNL_UNINITIALIZED);
}

/**
 * @brief An erased (all 0xFF) header is uninitialised, not corrupted.
 */
TEST_F(JournalTest, StatusBlankErasedHeaderIsUninitialized)
{
    std::memset(raw(JRNL_HEADER_SECTOR), 0xFF, SECTOR_SIZE);
    EXPECT_EQ(get_journal_status(&journal), JRNL_UNINITIALIZED);
}

/**
 * @brief A CRC-valid sector without the journal magic is uninitialised.
 */
TEST_F(JournalTest, StatusValidCrcWrongMagicIsUninitialized)
{
    std::vector<uint8_t> other = stamped_sector(0x5A);
    std::memcpy(raw(JRNL_HEADER_SECTOR), other.data(), SECTOR_SIZE);

    EXPECT_EQ(get_journal_status(&journal), JRNL_UNINITIALIZED);
}

/**
 * @brief A freshly initialised (EMPTY) journal is valid.
 */
TEST_F(JournalTest, StatusEmptyIsValid)
{
    ASSERT_TRUE(journal_header_init(&journal));
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}

/**
 * @brief A committed journal is valid (no rollback needed).
 */
TEST_F(JournalTest, StatusCommittedIsValid)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
    ASSERT_EQ(journal_free(&journal), STRG_OK);

    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}

/**
 * @brief An active journal needs rollback, and the header is loaded into RAM.
 */
TEST_F(JournalTest, StatusActiveIsRollbackAndLoadsHeader)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_MESSAGE, 42, content.data()));

    std::memset(&journal.header, 0, sizeof(journal.header));
    EXPECT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);

    EXPECT_EQ(journal.header.var.data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(journal.header.var.data.var.type, JRNL_MESSAGE);
    EXPECT_EQ(journal.header.var.data.var.sector, 42);
}

/**
 * @brief A bit flip in the header data (state) is corruption. The damaged
 *        header must not be trusted or loaded into RAM.
 */
TEST_F(JournalTest, StatusCorruptHeaderDataIsCorrupted)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));

    corrupt(JRNL_HEADER_SECTOR, offsetof(JournalHeaderData, state));

    std::memset(&journal.header, 0, sizeof(journal.header));
    EXPECT_EQ(get_journal_status(&journal), JRNL_CORRUPTED);
    EXPECT_EQ(journal.header.var.data.var.magic, 0u);
}

/**
 * @brief A bit flip in the magic is corruption, not an uninitialised journal
 *        (which would silently discard a pending rollback).
 */
TEST_F(JournalTest, StatusCorruptMagicIsCorrupted)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));

    corrupt(JRNL_HEADER_SECTOR, offsetof(JournalHeaderData, magic));

    EXPECT_EQ(get_journal_status(&journal), JRNL_CORRUPTED);
}

/**
 * @brief The trailer CRC also covers the header padding.
 */
TEST_F(JournalTest, StatusCorruptPaddingIsCorrupted)
{
    ASSERT_TRUE(journal_header_init(&journal));

    corrupt(JRNL_HEADER_SECTOR, offsetof(JournalHeader, padding) + 100);

    EXPECT_EQ(get_journal_status(&journal), JRNL_CORRUPTED);
}

/* ============================================================================
 * journal_data_init()
 * ========================================================================== */

/**
 * @brief journal_data_init() fills in an ACTIVE header for the given entry.
 */
TEST_F(JournalTest, DataInitFillsActiveHeader)
{
    JournalHeaderDataB data{};
    journal_data_init(&data, JRNL_MESSAGE, 1234);

    EXPECT_EQ(data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(data.var.type, JRNL_MESSAGE);
    EXPECT_EQ(data.var.sector, 1234);
}

/* ============================================================================
 * journal_write()
 * ========================================================================== */

/**
 * @brief journal_write() stores all three sectors, each with a valid trailer.
 */
TEST_F(JournalTest, WriteStoresAllSectorsWithValidTrailers)
{
    JournalHeaderBuffer header{};
    journal_data_init(&header.var.data, JRNL_CONTACT, 7);
    std::vector<uint8_t> content(SECTOR_SIZE, 0xAB);
    std::vector<uint8_t> bitmap(SECTOR_SIZE, 0xCD);

    ASSERT_EQ(journal_write(&journal, &header, content.data(), bitmap.data()), STRG_OK);

    uint8_t out[SECTOR_SIZE];
    EXPECT_EQ(read_sector(&storage, JRNL_HEADER_SECTOR, out), STRG_OK);
    EXPECT_EQ(read_sector(&storage, JRNL_CONTENT_SECTOR, out), STRG_OK);
    EXPECT_EQ(std::memcmp(out, content.data(), SECTOR_PAYLOAD_BYTES), 0);
    EXPECT_EQ(read_sector(&storage, JRNL_USAGE_SECTOR, out), STRG_OK);
    EXPECT_EQ(std::memcmp(out, bitmap.data(), SECTOR_PAYLOAD_BYTES), 0);
}

/**
 * @brief The header is written last: if the bitmap or content write fails
 *        the header on the card is still the previous (committed) one.
 */
TEST_F(JournalTest, WriteHeaderIsWrittenLast)
{
    ASSERT_TRUE(journal_header_init(&journal));

    JournalHeaderBuffer header{};
    journal_data_init(&header.var.data, JRNL_CONTACT, 7);
    std::vector<uint8_t> content(SECTOR_SIZE, 0xAB);
    std::vector<uint8_t> bitmap(SECTOR_SIZE, 0xCD);

    // 1st write (bitmap) fails: nothing changes
    fail_write_in(1);
    EXPECT_NE(journal_write(&journal, &header, content.data(), bitmap.data()), STRG_OK);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);

    // 2nd write (content) fails: bitmap copy written, header untouched
    fail_write_in(2);
    EXPECT_NE(journal_write(&journal, &header, content.data(), bitmap.data()), STRG_OK);
    EXPECT_EQ(raw(JRNL_USAGE_SECTOR)[0], 0xCD);
    EXPECT_EQ(raw(JRNL_CONTENT_SECTOR)[0], 0x00);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);

    // 3rd write (header) fails: both copies written, header untouched
    fail_write_in(3);
    EXPECT_NE(journal_write(&journal, &header, content.data(), bitmap.data()), STRG_OK);
    EXPECT_EQ(raw(JRNL_CONTENT_SECTOR)[0], 0xAB);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}

/* ============================================================================
 * journal_add()
 * ========================================================================== */

/**
 * @brief journal_add() writes an ACTIVE header, the content copy and the
 *        matching usage bitmap sector, and keeps the header in RAM.
 */
TEST_F(JournalTest, AddWritesActiveEntry)
{
    const uint16_t data_sector = 5;
    std::vector<uint8_t> content = stamped_sector(0x3C);
    fill_bitmap_sector(data_sector, 0x77);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, content.data()));

    JournalHeaderBuffer header = stored_header();
    EXPECT_TRUE(sector_crc_valid(header.buffer));
    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(header.var.data.var.type, JRNL_CONTACT);
    EXPECT_EQ(header.var.data.var.sector, data_sector);

    // RAM copy matches what was written, so journal_free() commits this entry
    EXPECT_EQ(std::memcmp(journal.header.var.data.buffer, header.var.data.buffer,
                          sizeof(JournalHeaderData)), 0);

    EXPECT_EQ(std::memcmp(raw(JRNL_CONTENT_SECTOR), content.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(std::memcmp(raw(JRNL_USAGE_SECTOR), bitmap_sector_for(data_sector), SECTOR_SIZE), 0);
}

/**
 * @brief journal_add() backs up the bitmap sector that owns the data sector,
 *        including across a bitmap sector boundary.
 */
TEST_F(JournalTest, AddSelectsCorrectBitmapSector)
{
    const uint16_t in_sector_0 = USAGE_BITS_PER_SECTOR - 1;
    const uint16_t in_sector_1 = USAGE_BITS_PER_SECTOR;
    std::vector<uint8_t> content = stamped_sector(0x00);

    fill_bitmap_sector(in_sector_0, 0x11);
    fill_bitmap_sector(in_sector_1, 0x22);

    ASSERT_TRUE(journal_add(&journal, JRNL_MESSAGE, in_sector_0, content.data()));
    EXPECT_EQ(raw(JRNL_USAGE_SECTOR)[0], 0x11);

    ASSERT_TRUE(journal_add(&journal, JRNL_MESSAGE, in_sector_1, content.data()));
    EXPECT_EQ(raw(JRNL_USAGE_SECTOR)[0], 0x22);
}

/**
 * @brief A blank (never-used) sector can be journalled. write_sector()
 *        stamps the caller's buffer, so the stored copy is CRC-valid.
 */
TEST_F(JournalTest, AddStampsBlankContent)
{
    std::vector<uint8_t> blank(SECTOR_SIZE, 0x00);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 0, blank.data()));

    EXPECT_TRUE(sector_crc_valid(blank.data()));
    uint8_t out[SECTOR_SIZE];
    EXPECT_EQ(read_sector(&storage, JRNL_CONTENT_SECTOR, out), STRG_OK);
}

/* ============================================================================
 * journal_free()
 * ========================================================================== */

/**
 * @brief journal_free() commits the active entry, keeping its type and sector.
 */
TEST_F(JournalTest, FreeCommitsActiveEntry)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_MESSAGE, 9, content.data()));

    ASSERT_EQ(journal_free(&journal), STRG_OK);

    JournalHeaderBuffer header = stored_header();
    EXPECT_TRUE(sector_crc_valid(header.buffer));
    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_COMMITTED);
    EXPECT_EQ(header.var.data.var.type, JRNL_MESSAGE);
    EXPECT_EQ(header.var.data.var.sector, 9);
}

/* ============================================================================
 * journal_rollback()
 * ========================================================================== */

/**
 * @brief Rollback restores the journalled data sector and its bitmap sector,
 *        then commits the journal.
 */
TEST_F(JournalTest, RollbackRestoresSectorAndBitmap)
{
    const uint16_t data_sector = 2;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    fill_bitmap_sector(data_sector, 0xCD);
    std::vector<uint8_t> original_bitmap(SECTOR_SIZE);
    std::memcpy(original_bitmap.data(), bitmap_sector_for(data_sector), SECTOR_SIZE);

    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

    // the transaction then overwrites the data sector before power is lost
    std::vector<uint8_t> modified = stamped_sector(0x99);
    ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), modified.data()), STRG_OK);

    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    ASSERT_TRUE(journal_rollback(&journal));

    EXPECT_EQ(std::memcmp(raw(DATA_SECTOR_TO_RAW(data_sector)), original.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(std::memcmp(raw(USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_FIND_SECTOR(data_sector)),
                          original_bitmap.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_COMMITTED);
}

/**
 * @brief The data sector index is a data-region index: rollback writes to
 *        DATA_SECTOR_TO_RAW(sector) and nowhere else in the data region.
 */
TEST_F(JournalTest, RollbackTargetsDataRegionSector)
{
    const uint16_t data_sector = MESSAGE_DATA_SECTOR(4);
    std::vector<uint8_t> original = stamped_sector(0xAB);

    ASSERT_TRUE(journal_add(&journal, JRNL_MESSAGE, data_sector, original.data()));
    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    ASSERT_TRUE(journal_rollback(&journal));

    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(data_sector))[0], 0xAB);
    // the raw sector with the same number (the old, wrong target) is untouched
    EXPECT_EQ(raw(data_sector)[0], 0x00);
}

/**
 * @brief A corrupted journal content copy aborts rollback before the data
 *        sector is touched, and the journal stays active.
 */
TEST_F(JournalTest, RollbackCorruptContentCopyFails)
{
    const uint16_t data_sector = 2;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

    std::vector<uint8_t> current = stamped_sector(0x99);
    ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), current.data()), STRG_OK);

    corrupt(JRNL_CONTENT_SECTOR, 10);

    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    EXPECT_FALSE(journal_rollback(&journal));

    EXPECT_EQ(std::memcmp(raw(DATA_SECTOR_TO_RAW(data_sector)), current.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_ACTIVE);
}

/**
 * @brief A corrupted bitmap copy fails rollback and leaves the journal active
 *        (so the next boot retries), and the bitmap sector is not overwritten.
 */
TEST_F(JournalTest, RollbackCorruptBitmapCopyFails)
{
    const uint16_t data_sector = 2;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    fill_bitmap_sector(data_sector, 0xCD);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

    corrupt(JRNL_USAGE_SECTOR, 10);

    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    EXPECT_FALSE(journal_rollback(&journal));

    EXPECT_EQ(raw(USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_FIND_SECTOR(data_sector))[0], 0x00);
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_ACTIVE);
}

/**
 * @brief Rolling back a message history entry restores the history sector,
 *        leaves the usage bitmap alone (history has no usage bits), and
 *        commits the journal.
 */
TEST_F(JournalTest, RollbackHistoryEntrySkipsBitmapAndCommits)
{
    const uint16_t data_sector = 100;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    fill_bitmap_sector(data_sector, 0xCD);

    ASSERT_TRUE(journal_add(&journal, JRNL_MSG_HIST, data_sector, original.data()));
    ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    ASSERT_TRUE(journal_rollback(&journal));

    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(data_sector))[0], 0xAB);
    EXPECT_EQ(raw(USAGE_BITMAP_START_SECTOR + USAGE_BITMAP_FIND_SECTOR(data_sector))[0], 0x00);
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_COMMITTED);
}

/* ============================================================================
 * journal_init()
 * ========================================================================== */

/**
 * @brief On a blank card journal_init() writes a fresh EMPTY header.
 */
TEST_F(JournalTest, InitBlankCardInitialisesHeader)
{
    ASSERT_TRUE(journal_init(&journal, &storage));

    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_EMPTY);
}

/**
 * @brief On a valid, committed journal journal_init() writes nothing.
 */
TEST_F(JournalTest, InitValidJournalWritesNothing)
{
    ASSERT_TRUE(journal_header_init(&journal));

    ctx.write_calls = 0;
    EXPECT_TRUE(journal_init(&journal, &storage));
    EXPECT_EQ(ctx.write_calls, 0);
}

/**
 * @brief A corrupted header stops initialisation without writing anything.
 */
TEST_F(JournalTest, InitCorruptedHeaderFailsWithoutWriting)
{
    std::vector<uint8_t> content = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 3, content.data()));
    corrupt(JRNL_HEADER_SECTOR, offsetof(JournalHeaderData, sector));

    ctx.write_calls = 0;
    EXPECT_FALSE(journal_init(&journal, &storage));
    EXPECT_EQ(ctx.write_calls, 0);
}

/**
 * @brief Power cut mid-transaction: journal_init() on the next boot rolls the
 *        data sector back and commits the journal.
 */
TEST_F(JournalTest, InitRollsBackInterruptedTransaction)
{
    const uint16_t data_sector = 2;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

    std::vector<uint8_t> modified = stamped_sector(0x99);
    ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), modified.data()), STRG_OK);

    // reboot
    std::memset(&journal, 0, sizeof(Journal));
    ASSERT_TRUE(journal_init(&journal, &storage));

    EXPECT_EQ(std::memcmp(raw(DATA_SECTOR_TO_RAW(data_sector)), original.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}

/**
 * @brief Power cut before the ACTIVE header is written: the previous header
 *        is still committed, so no rollback happens on the next boot.
 */
TEST_F(JournalTest, InitDoesNotRollBackWhenHeaderWasNeverWritten)
{
    const uint16_t data_sector = 2;

    // a committed earlier transaction on a different sector
    std::vector<uint8_t> earlier = stamped_sector(0x11);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, 7, earlier.data()));
    ASSERT_EQ(journal_free(&journal), STRG_OK);

    std::vector<uint8_t> current = stamped_sector(0x55);
    ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), current.data()), STRG_OK);

    // the new journal_add loses power on its 3rd write (the header)
    std::vector<uint8_t> original = stamped_sector(0xAB);
    fail_write_in(3);
    EXPECT_FALSE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));
    ctx.fail_after_write = -1;

    // reboot
    std::memset(&journal, 0, sizeof(Journal));
    ctx.write_calls = 0;
    ASSERT_TRUE(journal_init(&journal, &storage));

    EXPECT_EQ(ctx.write_calls, 0);
    EXPECT_EQ(std::memcmp(raw(DATA_SECTOR_TO_RAW(data_sector)), current.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(raw(DATA_SECTOR_TO_RAW(7))[0], 0x00);
}

/**
 * @brief A rollback interrupted before its commit is safely repeated on the
 *        next boot.
 */
TEST_F(JournalTest, InitRepeatsInterruptedRollback)
{
    const uint16_t data_sector = 2;
    std::vector<uint8_t> original = stamped_sector(0xAB);
    ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, data_sector, original.data()));

    std::vector<uint8_t> modified = stamped_sector(0x99);
    ASSERT_EQ(write_sector(&storage, DATA_SECTOR_TO_RAW(data_sector), modified.data()), STRG_OK);

    // 1st boot: rollback writes sector (1) and bitmap (2), then loses power on the commit (3)
    std::memset(&journal, 0, sizeof(Journal));
    fail_write_in(3);
    EXPECT_FALSE(journal_init(&journal, &storage));
    ctx.fail_after_write = -1;
    EXPECT_EQ(stored_header().var.data.var.state, JRNL_ACTIVE);

    // 2nd boot
    std::memset(&journal, 0, sizeof(Journal));
    ASSERT_TRUE(journal_init(&journal, &storage));

    EXPECT_EQ(std::memcmp(raw(DATA_SECTOR_TO_RAW(data_sector)), original.data(), SECTOR_SIZE), 0);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}
