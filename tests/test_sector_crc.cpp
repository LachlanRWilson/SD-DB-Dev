#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C"
{
#include "storage.h"
#include "heap_storage.h"
#include "contact.h"
#include "message.h"
#include "hash_table.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "superheader.h"
#include "mem_layout.h"
}

/*
 * Storage wrapper that forwards to heap_storage and remembers the last
 * sector written, so tests can corrupt a sector without hardcoding how
 * each record type maps to a raw SD sector index.
 */
static uint32_t g_last_write_index = UINT32_MAX;

static bool Tracking_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    g_last_write_index = index;
    return heap_storage.write_block(context, index, in);
}

static bool Tracking_WriteMultiBlock(void *context, uint32_t startIndex, size_t writeNum, uint8_t *in)
{
    g_last_write_index = startIndex;
    return heap_storage.write_multiblock(context, startIndex, writeNum, in);
}

/**
 * @brief Fixture for per-sector CRC protection across every on-disk sector type.
 */
class SectorCrcTest : public ::testing::Test
{
protected:
    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE;

    std::vector<uint8_t> storage_mem;
    HeapStorageContext storage_ctx{};
    Storage storage_obj{};
    Storage *storage = &storage_obj;

    Journal journal{};

    void SetUp() override
    {
        storage_mem.assign(static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT, 0);
        ASSERT_TRUE(HeapStorage_Init(&storage_ctx, storage_mem.data(), SECTOR_SIZE, STORAGE_SECTOR_COUNT));

        storage_obj = heap_storage;
        storage_obj.context = &storage_ctx;
        storage_obj.write_block = Tracking_WriteBlock;
        storage_obj.write_multiblock = Tracking_WriteMultiBlock;
        g_last_write_index = UINT32_MAX;

        ASSERT_TRUE(init_usage_bitmap(storage));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));
    }

    /** @brief Flip one bit of a raw SD sector, simulating card corruption. */
    void corrupt(uint32_t raw_sector, uint32_t byte = 100)
    {
        storage_mem[static_cast<size_t>(raw_sector) * SECTOR_SIZE + byte] ^= 0x01;
    }
};

/* ============================================================================
 * CRC helpers
 * ========================================================================== */

/**
 * @brief A stamped sector is valid, and flipping any single payload or
 *        trailer byte invalidates it.
 */
TEST_F(SectorCrcTest, StampDetectsEverySingleByteFlip)
{
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++)
    {
        sector[i] = static_cast<uint8_t>(i * 7);
    }

    sector_crc_stamp(sector);
    ASSERT_TRUE(sector_crc_valid(sector));

    for (uint32_t i = 0; i < SECTOR_SIZE; i++)
    {
        sector[i] ^= 0x80;
        EXPECT_FALSE(sector_crc_valid(sector)) << "flip at byte " << i << " not detected";
        sector[i] ^= 0x80;
    }
}

/**
 * @brief read_sector() reports corruption, read_sector_raw() does not check.
 */
TEST_F(SectorCrcTest, ReadSectorReportsCorruption)
{
    const uint16_t raw = DATA_REGION_START_SECTOR;
    uint8_t in[SECTOR_SIZE];
    uint8_t out[SECTOR_SIZE];
    std::memset(in, 0x5A, sizeof(in));

    ASSERT_EQ(write_sector(storage, raw, in), STRG_OK);
    EXPECT_TRUE(sector_crc_valid(in)); // caller's buffer now matches disk
    EXPECT_EQ(read_sector(storage, raw, out), STRG_OK);

    corrupt(raw);

    EXPECT_EQ(read_sector(storage, raw, out), STRG_CORRUPT);
    EXPECT_EQ(read_sector_raw(storage, raw, out), STRG_OK);
}

/**
 * @brief A never-written (all zero) sector does not pass the CRC check.
 */
TEST_F(SectorCrcTest, BlankSectorIsNotValid)
{
    uint8_t out[SECTOR_SIZE];
    EXPECT_EQ(read_sector(storage, DATA_REGION_START_SECTOR, out), STRG_CORRUPT);
}

/* ============================================================================
 * Contacts
 * ========================================================================== */

/**
 * @brief A corrupted contact sector is reported by read_contact().
 */
TEST_F(SectorCrcTest, ContactSectorCorruptionDetected)
{
    ContactBuffer in = create_contact("Ada", "0412345678");
    ContactBuffer out{};

    ASSERT_EQ(write_contact(storage, &journal, 0, &in), STRG_OK);
    ASSERT_EQ(read_contact(storage, 0, &out), STRG_OK);
    EXPECT_EQ(std::memcmp(in.buffer, out.buffer, sizeof(Contact)), 0);

    corrupt(DATA_REGION_START_SECTOR + CONTACT_DATA_START_SECTOR);

    EXPECT_EQ(read_contact(storage, 0, &out), STRG_CORRUPT);
}

/**
 * @brief Writing the first contact into an unused sector does not trip
 *        the CRC check (the blank sector is never read).
 */
TEST_F(SectorCrcTest, FirstContactInUnusedSectorSucceeds)
{
    ContactBuffer in = create_contact("Bob", "0400000000");
    ContactBuffer out{};

    const uint16_t slot = 3 * CONTACT_SECTOR_CAPACITY + 2;
    ASSERT_FALSE(check_usage_bit(slot / CONTACT_SECTOR_CAPACITY));

    ASSERT_EQ(write_contact(storage, &journal, slot, &in), STRG_OK);
    EXPECT_EQ(read_contact(storage, slot, &out), STRG_OK);
}

/**
 * @brief write_contact() refuses to modify a corrupted sector rather than
 *        re-stamping garbage with a fresh, valid CRC.
 */
TEST_F(SectorCrcTest, WriteContactRefusesCorruptSector)
{
    ContactBuffer a = create_contact("Ada", "0411111111");
    ContactBuffer b = create_contact("Bob", "0422222222");

    ASSERT_EQ(write_contact(storage, &journal, 0, &a), STRG_OK);
    corrupt(DATA_REGION_START_SECTOR + CONTACT_DATA_START_SECTOR);

    EXPECT_NE(write_contact(storage, &journal, 1, &b), STRG_OK);

    uint8_t raw[SECTOR_SIZE];
    ASSERT_EQ(read_sector_raw(storage, DATA_REGION_START_SECTOR + CONTACT_DATA_START_SECTOR, raw), STRG_OK);
    EXPECT_FALSE(sector_crc_valid(raw));
}

/* ============================================================================
 * Messages
 * ========================================================================== */

/**
 * @brief A corrupted message sector is reported by read_message_sector().
 */
TEST_F(SectorCrcTest, MessageSectorCorruptionDetected)
{
    MessageSectorBuffer in{};
    MessageSectorBuffer out{};
    create_new_message_sector(&in, "0412345678", UINT16_MAX);

    ASSERT_EQ(write_message_sector(storage, 5, &in), STRG_OK);
    const uint32_t raw = g_last_write_index;
    ASSERT_EQ(read_message_sector(storage, 5, &out), STRG_OK);

    corrupt(raw);

    EXPECT_EQ(read_message_sector(storage, 5, &out), STRG_CORRUPT);
}

/* ============================================================================
 * Usage bitmap
 * ========================================================================== */

/**
 * @brief Every bitmap sector keeps a valid trailer through bit updates.
 */
TEST_F(SectorCrcTest, BitmapUpdatesKeepTrailerValid)
{
    ASSERT_EQ(update_usage_bit(storage, 10, true), STRG_OK);
    ASSERT_EQ(update_usage_bit(storage, USAGE_BITS_PER_SECTOR + 3, true), STRG_OK);
    ASSERT_EQ(update_usage_bit(storage, 10, false), STRG_OK);

    std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    ASSERT_TRUE(read_usage_bitmap(storage));

    EXPECT_FALSE(check_usage_bit(10));
    EXPECT_TRUE(check_usage_bit(USAGE_BITS_PER_SECTOR + 3));
}

/**
 * @brief A corrupted bitmap sector fails read_usage_bitmap().
 */
TEST_F(SectorCrcTest, BitmapCorruptionDetected)
{
    ASSERT_EQ(update_usage_bit(storage, USAGE_BITS_PER_SECTOR + 3, true), STRG_OK);

    corrupt(USAGE_BITMAP_START_SECTOR + 1, 0);

    EXPECT_FALSE(read_usage_bitmap(storage));
}

/**
 * @brief The bitmap iterator never reports a CRC trailer bit as a used sector.
 */
TEST_F(SectorCrcTest, BitmapWalkSkipsTrailer)
{
    const uint16_t last_in_sector_0 = USAGE_BITS_PER_SECTOR - 1;
    const uint16_t first_in_sector_1 = USAGE_BITS_PER_SECTOR;

    ASSERT_EQ(update_usage_bit(storage, last_in_sector_0, true), STRG_OK);
    ASSERT_EQ(update_usage_bit(storage, first_in_sector_1, true), STRG_OK);

    // the trailer word of sector 0 is non-zero (it holds a CRC) but must be ignored
    ASSERT_NE(usage_bitmap[USAGE_WORDS_PER_SECTOR], 0u);

    EXPECT_EQ(get_next_bit(0, USAGE_BITMAP_TOTAL_BITS - 1), last_in_sector_0);
    EXPECT_EQ(get_next_bit(last_in_sector_0 + 1, USAGE_BITMAP_TOTAL_BITS - 1), first_in_sector_1);
    EXPECT_EQ(get_prev_bit(0, first_in_sector_1 - 1), last_in_sector_0);
}

/* ============================================================================
 * Superheader
 * ========================================================================== */

static bool Failing_ReadBlock(void *, uint32_t, uint8_t *)
{
    return false;
}

/** @brief A superheader matching the compiled layout. */
static SuperHeaderBuffer make_superheader()
{
    SuperHeaderBuffer sh{};
    sh.var.data.var.magic = SUPR_HEAD_MAGIC;
    sh.var.data.var.version = DB_CURRENT_VERSION;
    sh.var.data.var.db_start = DATA_REGION_START_SECTOR;
    sh.var.data.var.db_end = DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE;
    return sh;
}

/**
 * @brief A written superheader reads back as good, and its CRC is the
 *        standard sector trailer (read_sector() accepts it).
 */
TEST_F(SectorCrcTest, SuperheaderRoundTripIsGood)
{
    SuperHeaderBuffer sh = make_superheader();
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);
    EXPECT_TRUE(sector_crc_valid(sh.buffer)); // caller's buffer stamped too

    uint8_t raw[SECTOR_SIZE];
    EXPECT_EQ(read_sector(storage, SUPERHEADER_SECTOR, raw), STRG_OK);

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_GOOD);
    EXPECT_EQ(std::memcmp(read.buffer, sh.buffer, SECTOR_SIZE), 0);
}

/**
 * @brief The CRC covers all of SuperHeaderData (db_end is the last field).
 */
TEST_F(SectorCrcTest, SuperheaderCrcCoversAllData)
{
    SuperHeaderBuffer sh = make_superheader();
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);

    corrupt(SUPERHEADER_SECTOR, offsetof(SuperHeaderData, db_end));

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_CORRUPTED);
}

/**
 * @brief The trailer CRC also covers the padding.
 */
TEST_F(SectorCrcTest, SuperheaderPaddingIsCrcProtected)
{
    SuperHeaderBuffer sh = make_superheader();
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);

    corrupt(SUPERHEADER_SECTOR, offsetof(SuperHeader, padding) + 100);

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_CORRUPTED);
}

/**
 * @brief A bit flip in the magic of a formatted card is corruption, not an
 *        uninitialised card (which would trigger a format and wipe the DB).
 */
TEST_F(SectorCrcTest, SuperheaderCorruptMagicIsNotUninitialised)
{
    SuperHeaderBuffer sh = make_superheader();
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);

    corrupt(SUPERHEADER_SECTOR, offsetof(SuperHeaderData, magic));

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_CORRUPTED);
}

/**
 * @brief A never-written (all 0x00) sector is uninitialised.
 */
TEST_F(SectorCrcTest, SuperheaderBlankZeroSectorIsUninitialised)
{
    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_UNINITIALISED);
}

/**
 * @brief An erased (all 0xFF) sector is uninitialised.
 */
TEST_F(SectorCrcTest, SuperheaderBlankErasedSectorIsUninitialised)
{
    std::memset(&storage_mem[SUPERHEADER_SECTOR * SECTOR_SIZE], 0xFF, SECTOR_SIZE);

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_UNINITIALISED);
}

/**
 * @brief A valid sector with a different magic (another format) is uninitialised.
 */
TEST_F(SectorCrcTest, SuperheaderValidCrcWrongMagicIsUninitialised)
{
    SuperHeaderBuffer sh = make_superheader();
    sh.var.data.var.magic = 0x12345678u;
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_UNINITIALISED);
}

/**
 * @brief A valid superheader from another database version is outdated.
 */
TEST_F(SectorCrcTest, SuperheaderOtherVersionIsOutdated)
{
    SuperHeaderBuffer sh = make_superheader();
    sh.var.data.var.version = DB_CURRENT_VERSION + 1;
    ASSERT_EQ(write_superheader(storage, &sh), STRG_OK);

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_OUTDATED);
}

/**
 * @brief A storage read failure is reported as SUPR_FAIL.
 */
TEST_F(SectorCrcTest, SuperheaderReadFailureIsFail)
{
    storage_obj.read_block = Failing_ReadBlock;

    SuperHeaderBuffer read{};
    EXPECT_EQ(superheader_check(storage, &read), SUPR_FAIL);
}

/* ============================================================================
 * Reconstruction
 * ========================================================================== */

/**
 * @brief hash_reconstruct_contact() skips a corrupted contact sector instead
 *        of aborting, and still rebuilds contacts in healthy sectors.
 */
TEST_F(SectorCrcTest, ReconstructSkipsCorruptContactSector)
{
    std::vector<HashEntry> entries(HASH_TABLE_SIZE);
    std::vector<uint16_t> contact_pool(HASH_TABLE_SIZE);
    std::vector<uint16_t> message_pool(TOTAL_MESSAGE_SECTOR_SIZE);
    FreeList contact_fls{};
    FreeList message_fls{};
    HashTable table{};

    ASSERT_TRUE(free_list_empty_init(&contact_fls, contact_pool.data(), HASH_TABLE_SIZE));
    ASSERT_TRUE(free_list_init(&message_fls, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));

    // contact 0 in sector 0 (corrupted), contact in sector 1 (healthy)
    ContactBuffer bad = create_contact("Bad", "0400000001");
    ContactBuffer good = create_contact("Good", "0400000002");
    ASSERT_EQ(write_contact(storage, &journal, 0, &bad), STRG_OK);
    ASSERT_EQ(write_contact(storage, &journal, CONTACT_SECTOR_CAPACITY, &good), STRG_OK);

    corrupt(DATA_REGION_START_SECTOR + CONTACT_DATA_START_SECTOR);

    hash_init(&table, storage, &contact_fls, &message_fls, entries.data(), HASH_TABLE_SIZE);
    ASSERT_TRUE(read_usage_bitmap(storage));
    ASSERT_TRUE(hash_reconstruct_contact(&table));

    HashEntry *entry = nullptr;
    ASSERT_TRUE(hash_find_entry(&table, "0400000002", &entry));
    EXPECT_EQ(entry->state, ENTRY_OCCUPIED);
    EXPECT_EQ(entry->sector, CONTACT_SECTOR_CAPACITY);

    ASSERT_TRUE(hash_find_entry(&table, "0400000001", &entry));
    EXPECT_NE(entry->state, ENTRY_OCCUPIED);
}
