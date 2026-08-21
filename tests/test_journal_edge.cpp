#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "journal.h"
#include "crc.h"
#include "storage.h"
}

/**
 * @brief Storage mock used to inject read/write failures.
 */
struct JournalTestStorage
{
    uint8_t *memory = nullptr;
    uint16_t sector_count = 0;

    bool fail_read = false;
    bool fail_write = false;
};


/**
 * @brief Mock storage read callback.
 */
static bool journal_test_read_block(
    void *context,
    uint32_t index,
    uint8_t *outBuf)
{
    JournalTestStorage *storage =
        static_cast<JournalTestStorage *>(context);

    if (storage->fail_read)
    {
        return false;
    }

    if (index >= storage->sector_count)
    {
        return false;
    }

    memcpy(
        outBuf,
        &storage->memory[index * SECTOR_SIZE],
        SECTOR_SIZE
    );

    return true;
}


/**
 * @brief Mock storage write callback.
 */
static bool journal_test_write_block(
    void *context,
    uint32_t index,
    uint8_t *inBuf)
{
    JournalTestStorage *storage =
        static_cast<JournalTestStorage *>(context);

    if (storage->fail_write)
    {
        return false;
    }

    if (index >= storage->sector_count)
    {
        return false;
    }

    memcpy(
        &storage->memory[index * SECTOR_SIZE],
        inBuf,
        SECTOR_SIZE
    );

    return true;
}


/**
 * @brief Mock storage capacity callback.
 */
static uint32_t journal_test_capacity(void *context)
{
    JournalTestStorage *storage =
        static_cast<JournalTestStorage *>(context);

    return storage->sector_count;
}

class JournalEdgeTest : public ::testing::Test
{
protected:
    Journal journal{};

    Storage storage{};

    JournalTestStorage storage_ctx{};

    uint8_t *storage_mem = nullptr;

    static constexpr uint16_t STORAGE_SECTOR_COUNT = 4;

void SetUp() override
{
    storage_mem =
        new uint8_t[SECTOR_SIZE * STORAGE_SECTOR_COUNT];

    ASSERT_NE(storage_mem, nullptr);

    memset(
        storage_mem,
        0,
        SECTOR_SIZE * STORAGE_SECTOR_COUNT
    );

    storage_ctx.memory = storage_mem;
    storage_ctx.sector_count = STORAGE_SECTOR_COUNT;

    storage.read_block = journal_test_read_block;
    storage.write_block = journal_test_write_block;
    storage.capacity = journal_test_capacity;
    storage.context = &storage_ctx;

    memset(&journal, 0, sizeof(Journal));
    journal.storage = &storage;
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
            storage.write_block(
                storage.context,
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
            storage.read_block(
                storage.context,
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


/* ============================================================================
 * journal_header_init() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_header_init() fails when the header write fails.
 */
TEST_F(JournalEdgeTest, HeaderInitWriteFailure)
{
    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_header_init(&journal));
}


/* ============================================================================
 * get_journal_status() error cases
 * ========================================================================== */

/**
 * @brief Verify journal status returns JRNL_READ_ERROR when reading fails.
 */
TEST_F(JournalEdgeTest, StatusReadFailure)
{
    storage_ctx.fail_read = true;

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_READ_ERROR
    );
}


/* ============================================================================
 * journal_init() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_init() fails when the journal header cannot be read.
 */
TEST_F(JournalEdgeTest, InitHeaderReadFailure)
{
    storage_ctx.fail_read = true;

    EXPECT_FALSE(journal_init(&journal, &storage));
}


/**
 * @brief Verify journal_init() fails when initialising the journal header
 *        fails.
 */
TEST_F(JournalEdgeTest, InitHeaderWriteFailure)
{
    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_init(&journal, &storage));
}


/**
 * @brief Verify journal_init() fails when rollback cannot restore the
 *        database sector.
 */
TEST_F(JournalEdgeTest, InitRollbackWriteFailure)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xAA);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, target_sector);

    header.var.content_crc =
        crc32_calculate(content, SECTOR_SIZE);

    write_header(header);

    memcpy(journal.content, content, SECTOR_SIZE);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_init(&journal, &storage));
}


/* ============================================================================
 * journal_write() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_write() fails when writing the header fails.
 */
TEST_F(JournalEdgeTest, WriteHeaderFailure)
{
    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, 2);

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0x55);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(
        journal_write(
            &journal,
            &header,
            content
        )
    );
}


/* ============================================================================
 * journal_add() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_add() fails when storage write fails.
 */
TEST_F(JournalEdgeTest, AddWriteFailure)
{
    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = 1;
    data.var.sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xA5);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(
        journal_add(
            &journal,
            data,
            content
        )
    );
}


/* ============================================================================
 * journal_header_read() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_header_read() fails when storage read fails.
 */
TEST_F(JournalEdgeTest, HeaderReadFailure)
{
    JournalHeaderBuffer header{};

    storage_ctx.fail_read = true;

    EXPECT_FALSE(
        journal_header_read(
            &journal,
            &header
        )
    );
}


/* ============================================================================
 * journal_content_read() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_content_read() fails when storage read fails.
 */
TEST_F(JournalEdgeTest, ContentReadFailure)
{
    uint8_t content[SECTOR_SIZE]{};

    storage_ctx.fail_read = true;

    EXPECT_FALSE(
        journal_content_read(
            &journal,
            content
        )
    );
}


/* ============================================================================
 * journal_rollback() error cases
 * ========================================================================== */

/**
 * @brief Verify rollback fails when restoring the database sector fails.
 */
TEST_F(JournalEdgeTest, RollbackWriteFailure)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xAA);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, target_sector);

    header.var.content_crc =
        crc32_calculate(content, SECTOR_SIZE);

    journal.header = header;

    memcpy(journal.content, content, SECTOR_SIZE);

    write_header(header);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_rollback(&journal));
}


/**
 * @brief Verify rollback fails when the journal content CRC is invalid.
 */
TEST_F(JournalEdgeTest, RollbackContentCrcFailure)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xAA);

    uint8_t different_content[SECTOR_SIZE]{};
    fill_content(different_content, 0xBB);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, target_sector);

    header.var.content_crc =
        crc32_calculate(different_content, SECTOR_SIZE);

    journal.header = header;

    memcpy(journal.content, content, SECTOR_SIZE);

    write_header(header);

    EXPECT_FALSE(journal_rollback(&journal));
}


/**
 * @brief Verify a failed rollback does not mark the journal committed.
 */
TEST_F(JournalEdgeTest, RollbackWriteFailureLeavesJournalActive)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0xAA);

    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, target_sector);

    header.var.content_crc =
        crc32_calculate(content, SECTOR_SIZE);

    journal.header = header;

    memcpy(journal.content, content, SECTOR_SIZE);

    write_header(header);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_rollback(&journal));

    // journal_free() should not have been called.
    EXPECT_EQ(
        journal.header.var.data.var.state,
        JRNL_ACTIVE
    );
}


/* ============================================================================
 * journal_free() error cases
 * ========================================================================== */

/**
 * @brief Verify journal_free() fails when writing the committed header fails.
 */
TEST_F(JournalEdgeTest, FreeWriteFailure)
{
    JournalHeaderBuffer header =
        create_header(JRNL_ACTIVE, 0, 2);

    journal.header = header;

    write_header(header);

    storage_ctx.fail_write = true;

    EXPECT_FALSE(journal_free(&journal));
}


/* ============================================================================
 * Boundary conditions
 * ========================================================================== */

/**
 * @brief Verify a journal entry can target sector zero.
 */
TEST_F(JournalEdgeTest, SectorZero)
{
    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = 1;
    data.var.sector = 0;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0x11);

    ASSERT_TRUE(
        journal_add(
            &journal,
            data,
            content
        )
    );

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(header.var.data.var.sector, 0);
}


/**
 * @brief Verify the maximum uint16_t sector value is preserved.
 *
 * This assumes the storage implementation can address this sector.
 * The test only verifies the journal header representation.
 */
TEST_F(JournalEdgeTest, MaximumSectorValue)
{
    const uint16_t sector = UINT16_MAX;

    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = 1;
    data.var.sector = sector;

    uint8_t content[SECTOR_SIZE]{};
    fill_content(content, 0x22);

    ASSERT_TRUE(
        journal_add(
            &journal,
            data,
            content
        )
    );

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(
        header.var.data.var.sector,
        UINT16_MAX
    );
}
