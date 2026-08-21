#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "journal.h"
#include "crc.h"
#include "storage.h"
#include "heap_storage.h"
}

class JournalTest : public ::testing::Test
{
protected:
    // Journal Struct
    Journal journal{};

    // Storage Struct (heap storage)
    Storage *storage = &heap_storage;

    // Heap storage context
    HeapStorageContext storage_ctx;

    // Mock SD card storage
    uint8_t *storage_mem = nullptr;

    // Number of sectors in mock storage
    static constexpr uint16_t STORAGE_SECTOR_COUNT = 4;

    void SetUp() override
    {
        // Allocate mock SD card storage
        storage_mem = new uint8_t[SECTOR_SIZE * STORAGE_SECTOR_COUNT];

        ASSERT_NE(storage_mem, nullptr);

        // Clear mock SD card storage
        memset(storage_mem, 0, SECTOR_SIZE * STORAGE_SECTOR_COUNT);

        // Initialise heap storage
        ASSERT_TRUE(
            HeapStorage_Init(
                &storage_ctx,
                storage_mem,
                SECTOR_SIZE,
                STORAGE_SECTOR_COUNT
            )
        );

        // Allocate HeapStorageContext struct to storage
        storage->context = &storage_ctx;

        // Initialise journal
        memset(&journal, 0, sizeof(Journal));
        journal.storage = storage;
    }

    void TearDown() override
    {
        delete[] storage_mem;

        storage_mem = nullptr;
    }

    /**
     * @brief Create a valid journal header.
     */
    JournalHeaderBuffer create_header(uint8_t state, uint8_t type = 0,
                                      uint16_t sector = 0)
    {
        JournalHeaderBuffer header{};

        header.var.data.var.magic = JRNL_MAGIC;
        header.var.data.var.state = state;
        header.var.data.var.type = type;
        header.var.data.var.sector = sector;

        header.var.header_crc =
            crc32_calculate(
                header.var.data.buffer,
                sizeof(JournalHeaderData)
            );

        return header;
    }

    /**
     * @brief Write a journal header directly to mock storage.
     */
    void write_header(JournalHeaderBuffer& header)
    {
        ASSERT_TRUE(
            storage->write_block(
                storage->context,
                JRNL_HEADER_SECTOR,
                header.buffer
            )
        );
    }

    /**
     * @brief Read the journal header from mock storage.
     */
    JournalHeaderBuffer read_header()
    {
        JournalHeaderBuffer header{};

        EXPECT_TRUE(
            storage->read_block(
                storage->context,
                JRNL_HEADER_SECTOR,
                header.buffer
            )
        );

        return header;
    }

    /**
     * @brief Fill a sector with a specified value.
     */
    void fill_content(uint8_t *content, uint8_t value)
    {
        ASSERT_NE(content, nullptr);

        for (uint32_t i = 0; i < SECTOR_SIZE; i++)
        {
            content[i] = value;
        }
    }
};

/**
 * @brief Verify journal_header_init() creates a valid empty journal header.
 */
TEST_F(JournalTest, HeaderInit)
{
    ASSERT_TRUE(journal_header_init(&journal));

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_EMPTY);
    EXPECT_EQ(header.var.data.var.type, 0);
    EXPECT_EQ(header.var.data.var.sector, 0);

    uint32_t expected_crc = crc32_calculate( header.var.data.buffer, sizeof(JournalHeaderData));

    EXPECT_EQ(header.var.header_crc, expected_crc);
}

/**
 * @brief Verify an uninitialised journal is detected.
 */
TEST_F(JournalTest, StatusUninitialized)
{
    EXPECT_EQ(get_journal_status(&journal), JRNL_UNINITIALIZED);
}


/**
 * @brief Verify a valid empty journal is detected.
 */
TEST_F(JournalTest, StatusValid)
{
    JournalHeaderBuffer header = create_header(JRNL_EMPTY);

    write_header(header);

    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);

    EXPECT_EQ(journal.header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_EMPTY);
}


/**
 * @brief Verify a committed journal is detected as valid.
 */
TEST_F(JournalTest, StatusCommitted)
{
    JournalHeaderBuffer header = create_header(JRNL_COMMITTED);

    write_header(header);

    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_COMMITTED);
}


/**
 * @brief Verify an active journal requires rollback.
 */
TEST_F(JournalTest, StatusRollback)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, 0, 2);

    write_header(header);

    EXPECT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);
    EXPECT_EQ(journal.header.var.data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(journal.header.var.data.var.sector, 2);
}


/**
 * @brief Verify a corrupted journal header is detected.
 */
TEST_F(JournalTest, StatusCorruptedHeader)
{
    JournalHeaderBuffer header = create_header(JRNL_EMPTY);

    write_header(header);

    // Corrupt the stored CRC
    header.var.header_crc ^= 0xFFFFFFFFu;

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_HEADER_SECTOR,
            header.buffer
        )
    );

    EXPECT_EQ(get_journal_status(&journal), JRNL_CORRUPTED);
}


/**
 * @brief Verify corruption of journal header data is detected.
 */
TEST_F(JournalTest, StatusCorruptedHeaderData)
{
    JournalHeaderBuffer header = create_header(JRNL_EMPTY);

    write_header(header);

    // Corrupt the state without updating the CRC
    header.var.data.var.state = JRNL_ACTIVE;

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_HEADER_SECTOR,
            header.buffer
        )
    );

    EXPECT_EQ(get_journal_status(&journal), JRNL_CORRUPTED);
}

/**
 * @brief Verify journal_init() initialises an uninitialised journal.
 */
TEST_F(JournalTest, InitUninitializedJournal)
{
    ASSERT_TRUE(journal_init(&journal, storage));

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_EMPTY);
}


/**
 * @brief Verify journal_init() succeeds for a valid journal.
 *
 * NOTE:
 * The current journal_init() implementation does not explicitly handle
 * JRNL_VALID. This test will expose that issue.
 */
TEST_F(JournalTest, InitValidJournal)
{
    JournalHeaderBuffer header = create_header(JRNL_EMPTY);

    write_header(header);

    EXPECT_TRUE(journal_init(&journal, storage));
}


/**
 * @brief Verify journal_init() performs rollback for an active journal.
 */
TEST_F(JournalTest, InitRollbackJournal)
{
    const uint16_t target_sector = 2;

    uint8_t old_content[SECTOR_SIZE];
    fill_content(old_content, 0xAA);

    uint32_t content_crc =
        crc32_calculate(old_content, SECTOR_SIZE);

    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, 0, target_sector);
    header.var.content_crc = content_crc;

    write_header(header);

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_CONTENT_SECTOR,
            old_content
        )
    );

    memcpy(journal.content, old_content, SECTOR_SIZE);

    ASSERT_TRUE(journal_init(&journal, storage));

    uint8_t result[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            result
        )
    );

    EXPECT_EQ(memcmp(result, old_content, SECTOR_SIZE), 0);

    JournalHeaderBuffer final_header = read_header();

    EXPECT_EQ(final_header.var.data.var.state, JRNL_COMMITTED);
}

/**
 * @brief Verify journal_write() writes the journal header.
 */
TEST_F(JournalTest, WriteHeader)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, 1, 2);

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0x55);

    ASSERT_TRUE(journal_write(&journal, &header, content));

    JournalHeaderBuffer stored = read_header();

    EXPECT_EQ(stored.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(stored.var.data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(stored.var.data.var.type, 1);
    EXPECT_EQ(stored.var.data.var.sector, 2);
}

/**
 * @brief Verify journal_add() calculates and stores both CRCs.
 */
TEST_F(JournalTest, AddJournalEntry)
{
    const uint16_t target_sector = 2;

    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = 1;
    data.var.sector = target_sector;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xA5);

    uint32_t expected_header_crc =
        crc32_calculate(
            data.buffer,
            sizeof(JournalHeaderData)
        );

    uint32_t expected_content_crc =
        crc32_calculate(content, SECTOR_SIZE);

    ASSERT_TRUE(journal_add(&journal, data, content));

    JournalHeaderBuffer stored = read_header();

    EXPECT_EQ(stored.var.header_crc, expected_header_crc);
    EXPECT_EQ(stored.var.content_crc, expected_content_crc);

    EXPECT_EQ(stored.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(stored.var.data.var.state, JRNL_ACTIVE);
    EXPECT_EQ(stored.var.data.var.sector, target_sector);
}

/**
 * @brief Verify journal_header_read() reads the journal header.
 */
TEST_F(JournalTest, HeaderRead)
{
    JournalHeaderBuffer expected = create_header(JRNL_ACTIVE, 1, 2);

    write_header(expected);

    JournalHeaderBuffer result{};

    ASSERT_TRUE(journal_header_read(&journal, &result));

    EXPECT_EQ(result.var.data.var.magic, expected.var.data.var.magic);
    EXPECT_EQ(result.var.data.var.state, expected.var.data.var.state);
    EXPECT_EQ(result.var.data.var.type, expected.var.data.var.type);
    EXPECT_EQ(result.var.data.var.sector, expected.var.data.var.sector);
    EXPECT_EQ(result.var.header_crc, expected.var.header_crc);
}

/**
 * @brief Verify journal_content_read() reads journal content.
 */
TEST_F(JournalTest, ContentRead)
{
    uint8_t expected[SECTOR_SIZE]{};
    fill_content(expected, 0x5A);

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_CONTENT_SECTOR,
            expected
        )
    );

    uint8_t result[SECTOR_SIZE]{};

    ASSERT_TRUE(journal_content_read(&journal, result));

    EXPECT_EQ(memcmp(result, expected, SECTOR_SIZE), 0);
}

/**
 * @brief Verify journal_rollback() restores the original sector contents.
 */
TEST_F(JournalTest, Rollback)
{
    const uint16_t target_sector = 2;

    uint8_t original_content[SECTOR_SIZE]{};
    fill_content(original_content, 0xAB);

    uint32_t content_crc =
        crc32_calculate(original_content, SECTOR_SIZE);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 1, target_sector);

    header.var.content_crc = content_crc;

    journal.header = header;

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_HEADER_SECTOR,
            header.buffer
        )
    );

    memcpy(journal.content, original_content, SECTOR_SIZE);

    ASSERT_TRUE(journal_rollback(&journal));

    uint8_t restored[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            restored
        )
    );

    EXPECT_EQ(memcmp(restored, original_content, SECTOR_SIZE), 0);

    JournalHeaderBuffer final_header = read_header();

    EXPECT_EQ(final_header.var.data.var.state, JRNL_COMMITTED);
}


/**
 * @brief Verify rollback fails when journal content CRC is invalid.
 */
TEST_F(JournalTest, RollbackCorruptedContent)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0x11);

    uint8_t different_content[SECTOR_SIZE]{};
    fill_content(different_content, 0x22);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 1, target_sector);

    header.var.content_crc =
        crc32_calculate(different_content, SECTOR_SIZE);

    journal.header = header;

    memcpy(journal.content, content, SECTOR_SIZE);

    EXPECT_FALSE(journal_rollback(&journal));
}

/**
 * @brief Verify journal_free() changes the journal state to committed.
 */
TEST_F(JournalTest, Free)
{
    JournalHeaderBuffer header = create_header(JRNL_ACTIVE, 1, 2);

    journal.header = header;
    write_header(header);

    ASSERT_TRUE(journal_free(&journal));

    EXPECT_EQ(journal.header.var.data.var.state, JRNL_COMMITTED);

    JournalHeaderBuffer stored = read_header();

    EXPECT_EQ(stored.var.data.var.state, JRNL_COMMITTED);
}

/**
 * @brief Verify the complete journal initialisation and rollback lifecycle.
 */
TEST_F(JournalTest, CompleteRollbackLifecycle)
{
    const uint16_t target_sector = 2;

    uint8_t original_content[SECTOR_SIZE]{};
    fill_content(original_content, 0xCC);

    // Journal should initially be uninitialised
    EXPECT_EQ(get_journal_status(&journal), JRNL_UNINITIALIZED);

    // Initialise journal
    ASSERT_TRUE(journal_init(&journal, storage));

    JournalHeaderBuffer initial_header = read_header();

    EXPECT_EQ(initial_header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(initial_header.var.data.var.state, JRNL_EMPTY);

    // Create active journal entry
    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = 1;
    data.var.sector = target_sector;

    ASSERT_TRUE(journal_add(&journal, data, original_content));

    // Verify journal is active
    JournalHeaderBuffer active_header = read_header();

    EXPECT_EQ(active_header.var.data.var.state, JRNL_ACTIVE);

    // Simulate power failure by creating a new Journal instance
    Journal recovered_journal{};
    recovered_journal.storage = storage;

    // Read journal content into recovered journal
    ASSERT_TRUE( journal_content_read( &recovered_journal, recovered_journal.content));

    // Initialise recovered journal
    ASSERT_TRUE(journal_init(&recovered_journal, storage));

    // Verify original database sector was restored
    uint8_t restored[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            restored
        )
    );

    EXPECT_EQ(memcmp(restored, original_content, SECTOR_SIZE), 0);

    // Journal should now be committed
    JournalHeaderBuffer final_header = read_header();

    EXPECT_EQ(final_header.var.data.var.state, JRNL_COMMITTED);
}
