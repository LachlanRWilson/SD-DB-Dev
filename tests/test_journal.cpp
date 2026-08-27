#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "journal.h"
#include "crc.h"
#include "storage.h"
#include "heap_storage.h"
#include "superheader.h"
}

class JournalTest : public ::testing::Test
{
protected:
    Journal journal{};

    Storage *storage = &heap_storage;
    HeapStorageContext storage_ctx;

    uint8_t *storage_mem = nullptr;

    static constexpr uint16_t STORAGE_SECTOR_COUNT = SUPERHEADER_SECTOR_SIZE +
        USAGE_BITMAP_SECTOR_SIZE + JRNL_SECTOR_SIZE + TOTAL_DATA_SECTOR_SIZE;


    void SetUp() override
    {

        ASSERT_EQ(SUPERHEADER_SECTOR_SIZE, 1);
        ASSERT_EQ(USAGE_BITMAP_SECTOR_SIZE, 61);
        ASSERT_EQ(JRNL_SECTOR_SIZE, 3);
        ASSERT_EQ(TOTAL_DATA_SECTOR_SIZE, 30969);
        ASSERT_EQ(STORAGE_SECTOR_COUNT, 31034);
        storage_mem = new uint8_t[SECTOR_SIZE * STORAGE_SECTOR_COUNT];

        ASSERT_NE(storage_mem, nullptr);

        memset(
            storage_mem,
            0,
            SECTOR_SIZE * STORAGE_SECTOR_COUNT
        );

        ASSERT_TRUE(
            HeapStorage_Init(
                &storage_ctx,
                storage_mem,
                SECTOR_SIZE,
                STORAGE_SECTOR_COUNT
            )
        );

        storage->context = &storage_ctx;

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
    JournalHeaderBuffer create_header(
        uint8_t state,
        uint8_t type = JRNL_CONTACT,
        uint16_t sector = 0,
        const uint8_t *content = nullptr,
        const uint8_t *usage_bitmap = nullptr)
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

        if (content != nullptr)
        {
            header.var.content_crc =
                crc32_calculate(
                    content,
                    SECTOR_SIZE
                );
        }

        if (usage_bitmap != nullptr)
        {
            header.var.usage_bitmap_crc =
                crc32_calculate(
                    usage_bitmap,
                    SECTOR_SIZE
                );
        }

        return header;
    }

    /**
     * @brief Write a journal header directly to storage.
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
     * @brief Read the journal header from storage.
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

        memset(content, value, SECTOR_SIZE);
    }
};

/**
 * @brief Verify journal_header_init() creates a valid empty journal.
 */
TEST_F(JournalTest, HeaderInit)
{
    ASSERT_TRUE(journal_header_init(&journal));

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(header.var.data.var.magic, JRNL_MAGIC);
    EXPECT_EQ(header.var.data.var.state, JRNL_EMPTY);
    EXPECT_EQ(header.var.data.var.type, JRNL_CONTACT);
    EXPECT_EQ(header.var.data.var.sector, 0);

    uint32_t expected_header_crc =
        crc32_calculate(
            header.var.data.buffer,
            sizeof(JournalHeaderData)
        );

    EXPECT_EQ(
        header.var.header_crc,
        expected_header_crc
    );

    EXPECT_EQ(header.var.content_crc, 0);
    EXPECT_EQ(header.var.usage_bitmap_crc, 0);
}

/**
 * @brief Verify an uninitialised journal is detected.
 */
TEST_F(JournalTest, StatusUninitialized)
{
    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_UNINITIALIZED
    );
}

/**
 * @brief Verify a valid empty journal is detected.
 */
TEST_F(JournalTest, StatusValid)
{
    JournalHeaderBuffer header =
        create_header(JRNL_EMPTY);

    write_header(header);

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_VALID
    );

    EXPECT_EQ(
        journal.header.var.data.var.magic,
        JRNL_MAGIC
    );

    EXPECT_EQ(
        journal.header.var.data.var.state,
        JRNL_EMPTY
    );
}

/**
 * @brief Verify an active journal requires rollback.
 */
TEST_F(JournalTest, StatusRollback)
{
    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            2
        );

    write_header(header);

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_ROLLBACK
    );

    EXPECT_EQ(
        journal.header.var.data.var.state,
        JRNL_ACTIVE
    );

    EXPECT_EQ(
        journal.header.var.data.var.sector,
        2
    );
}

/**
 * @brief Verify a corrupted journal header is detected.
 */
TEST_F(JournalTest, StatusCorruptedHeader)
{
    JournalHeaderBuffer header =
        create_header(JRNL_EMPTY);

    write_header(header);

    header.var.header_crc ^= 0xFFFFFFFFu;

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_HEADER_SECTOR,
            header.buffer
        )
    );

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_CORRUPTED
    );
}

/**
 * @brief Verify corruption of journal header data is detected.
 */
TEST_F(JournalTest, StatusCorruptedHeaderData)
{
    JournalHeaderBuffer header =
        create_header(JRNL_EMPTY);

    write_header(header);

    header.var.data.var.state = JRNL_ACTIVE;

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_HEADER_SECTOR,
            header.buffer
        )
    );

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_CORRUPTED
    );
}

/**
 * @brief Verify journal_init() initialises an uninitialised journal.
 */
TEST_F(JournalTest, InitUninitializedJournal)
{
    ASSERT_TRUE(
        journal_init(
            &journal,
            storage
        )
    );

    JournalHeaderBuffer header = read_header();

    EXPECT_EQ(
        header.var.data.var.magic,
        JRNL_MAGIC
    );

    EXPECT_EQ(
        header.var.data.var.state,
        JRNL_EMPTY
    );

    EXPECT_EQ(
        header.var.header_crc,
        crc32_calculate(
            header.var.data.buffer,
            sizeof(JournalHeaderData)
        )
    );
}

/**
 * @brief Verify journal_init() succeeds for a valid journal.
 */
TEST_F(JournalTest, InitValidJournal)
{
    JournalHeaderBuffer header =
        create_header(JRNL_EMPTY);

    write_header(header);

    EXPECT_TRUE(
        journal_init(
            &journal,
            storage
        )
    );

    EXPECT_EQ(
        journal.header.var.data.var.state,
        JRNL_EMPTY
    );
}

/**
 * @brief Verify journal_init() performs rollback for an active journal.
 */
TEST_F(JournalTest, InitRollbackJournal)
{
    const uint16_t target_sector = 2;

    uint8_t old_content[SECTOR_SIZE];
    uint8_t old_usage_bitmap[SECTOR_SIZE];

    fill_content(old_content, 0xAA);
    fill_content(old_usage_bitmap, 0x55);

    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            target_sector,
            old_content,
            old_usage_bitmap
        );

    write_header(header);

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_CONTENT_SECTOR,
            old_content
        )
    );

    ASSERT_TRUE(
        storage->write_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            old_usage_bitmap
        )
    );

    memcpy(
        journal.content,
        old_content,
        SECTOR_SIZE
    );

    memcpy(
        journal.usage_bitmap_sector,
        old_usage_bitmap,
        SECTOR_SIZE
    );

    ASSERT_TRUE(
        journal_init(
            &journal,
            storage
        )
    );

    uint8_t result[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            result
        )
    );

    EXPECT_EQ(
        memcmp(
            result,
            old_content,
            SECTOR_SIZE
        ),
        0
    );

    uint8_t usage_result[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            usage_result
        )
    );

    EXPECT_EQ(
        memcmp(
            usage_result,
            old_usage_bitmap,
            SECTOR_SIZE
        ),
        0
    );

    JournalHeaderBuffer final_header =
        read_header();

    EXPECT_EQ(
        final_header.var.data.var.state,
        JRNL_COMMITTED
    );
}

/**
 * @brief Verify journal_write() writes header, content and usage bitmap.
 */
TEST_F(JournalTest, WriteJournal)
{
    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            2
        );

    uint8_t content[SECTOR_SIZE]{};
    uint8_t usage_bitmap[SECTOR_SIZE]{};

    fill_content(content, 0x55);
    fill_content(usage_bitmap, 0xAA);

    ASSERT_TRUE(
        journal_write(
            &journal,
            &header,
            content,
            usage_bitmap
        )
    );

    JournalHeaderBuffer stored_header =
        read_header();

    EXPECT_EQ(
        stored_header.var.data.var.magic,
        JRNL_MAGIC
    );

    EXPECT_EQ(
        stored_header.var.data.var.state,
        JRNL_ACTIVE
    );

    EXPECT_EQ(
        stored_header.var.data.var.type,
        JRNL_CONTACT
    );

    EXPECT_EQ(
        stored_header.var.data.var.sector,
        2
    );

    uint8_t stored_content[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_CONTENT_SECTOR,
            stored_content
        )
    );

    EXPECT_EQ(
        memcmp(
            stored_content,
            content,
            SECTOR_SIZE
        ),
        0
    );

    uint8_t stored_usage_bitmap[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            stored_usage_bitmap
        )
    );

    EXPECT_EQ(
        memcmp(
            stored_usage_bitmap,
            usage_bitmap,
            SECTOR_SIZE
        ),
        0
    );
}

/**
 * @brief Verify journal_add() calculates and stores all three CRCs.
 */
TEST_F(JournalTest, AddJournalEntry)
{
    const uint16_t target_sector = 2;

    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = JRNL_CONTACT;
    data.var.sector = target_sector;

    uint8_t content[SECTOR_SIZE]{};
    uint8_t usage_bitmap[SECTOR_SIZE]{};

    fill_content(content, 0xA5);
    fill_content(usage_bitmap, 0x5A);

    uint32_t expected_header_crc =
        crc32_calculate(
            data.buffer,
            sizeof(JournalHeaderData)
        );

    uint32_t expected_content_crc =
        crc32_calculate(
            content,
            SECTOR_SIZE
        );

    uint32_t expected_usage_bitmap_crc =
        crc32_calculate(
            usage_bitmap,
            SECTOR_SIZE
        );

    ASSERT_TRUE(
        journal_add(
            &journal,
            data,
            content,
            usage_bitmap
        )
    );

    JournalHeaderBuffer stored =
        read_header();

    EXPECT_EQ(
        stored.var.header_crc,
        expected_header_crc
    );

    EXPECT_EQ(
        stored.var.content_crc,
        expected_content_crc
    );

    EXPECT_EQ(
        stored.var.usage_bitmap_crc,
        expected_usage_bitmap_crc
    );

    EXPECT_EQ(
        stored.var.data.var.magic,
        JRNL_MAGIC
    );

    EXPECT_EQ(
        stored.var.data.var.state,
        JRNL_ACTIVE
    );

    EXPECT_EQ(
        stored.var.data.var.sector,
        target_sector
    );
}

/**
 * @brief Verify journal_header_read() reads the complete journal header.
 */
TEST_F(JournalTest, HeaderRead)
{
    uint8_t content[SECTOR_SIZE]{};
    uint8_t usage_bitmap[SECTOR_SIZE]{};

    fill_content(content, 0x11);
    fill_content(usage_bitmap, 0x22);

    JournalHeaderBuffer expected =
        create_header(
            JRNL_ACTIVE,
            JRNL_MESSAGE,
            2,
            content,
            usage_bitmap
        );

    write_header(expected);

    JournalHeaderBuffer result{};

    ASSERT_TRUE(
        journal_header_read(
            &journal,
            &result
        )
    );

    EXPECT_EQ(
        memcmp(
            result.buffer,
            expected.buffer,
            sizeof(JournalHeader)
        ),
        0
    );
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

    ASSERT_TRUE(
        journal_content_read(
            &journal
        )
    );

    EXPECT_EQ(
        memcmp(
            journal.content,
            expected,
            SECTOR_SIZE
        ),
        0
    );
}

/**
 * @brief Verify journal_rollback() restores both content and usage bitmap.
 */
TEST_F(JournalTest, Rollback)
{
    const uint16_t target_sector = 2;

    uint8_t original_content[SECTOR_SIZE]{};
    uint8_t original_usage_bitmap[SECTOR_SIZE]{};

    fill_content(original_content, 0xAB);
    fill_content(original_usage_bitmap, 0xCD);

    JournalHeaderDataB data = {
        .var = {
            .magic = JRNL_MAGIC,
            .state = JRNL_ACTIVE,
            .type = JRNL_CONTACT,
            .sector = target_sector
        }
    };

    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            target_sector,
            original_content,
            original_usage_bitmap
        );

    journal.header = header;

    ASSERT_TRUE(journal_add(&journal, data, original_content, original_usage_bitmap));
    ASSERT_TRUE(
        journal_rollback(&journal)
    );

    uint8_t restored_content[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            restored_content
        )
    );

    EXPECT_EQ(
        memcmp(
            restored_content,
            original_content,
            SECTOR_SIZE
        ),
        0
    );

    uint8_t restored_usage_bitmap[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            restored_usage_bitmap
        )
    );

    EXPECT_EQ(
        memcmp(
            restored_usage_bitmap,
            original_usage_bitmap,
            SECTOR_SIZE
        ),
        0
    );

    JournalHeaderBuffer final_header =
        read_header();

    EXPECT_EQ(
        final_header.var.data.var.state,
        JRNL_COMMITTED
    );
}

/**
 * @brief Verify rollback fails when journal content CRC is invalid.
 */
TEST_F(JournalTest, RollbackCorruptedContent)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    uint8_t different_content[SECTOR_SIZE]{};
    uint8_t usage_bitmap[SECTOR_SIZE]{};

    fill_content(content, 0x11);
    fill_content(different_content, 0x22);
    fill_content(usage_bitmap, 0x33);

    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            target_sector,
            different_content,
            usage_bitmap
        );

    journal.header = header;

    memcpy(
        journal.content,
        content,
        SECTOR_SIZE
    );

    memcpy(
        journal.usage_bitmap_sector,
        usage_bitmap,
        SECTOR_SIZE
    );

    EXPECT_FALSE(
        journal_rollback(&journal)
    );
}

/**
 * @brief Verify rollback fails when journal usage bitmap CRC is invalid.
 */
TEST_F(JournalTest, RollbackCorruptedUsageBitmap)
{
    const uint16_t target_sector = 2;

    uint8_t content[SECTOR_SIZE]{};
    uint8_t usage_bitmap[SECTOR_SIZE]{};
    uint8_t different_usage_bitmap[SECTOR_SIZE]{};

    fill_content(content, 0x11);
    fill_content(usage_bitmap, 0x22);
    fill_content(different_usage_bitmap, 0x33);

    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            target_sector,
            content,
            different_usage_bitmap
        );

    journal.header = header;

    memcpy(
        journal.content,
        content,
        SECTOR_SIZE
    );

    memcpy(
        journal.usage_bitmap_sector,
        usage_bitmap,
        SECTOR_SIZE
    );

    EXPECT_FALSE(
        journal_rollback(&journal)
    );
}

/**
 * @brief Verify journal_free() changes the journal state to committed.
 */
TEST_F(JournalTest, Free)
{
    JournalHeaderBuffer header =
        create_header(
            JRNL_ACTIVE,
            JRNL_CONTACT,
            2
        );

    journal.header = header;

    write_header(header);

    ASSERT_TRUE(
        journal_free(&journal)
    );

    EXPECT_EQ(
        journal.header.var.data.var.state,
        JRNL_COMMITTED
    );

    JournalHeaderBuffer stored =
        read_header();

    EXPECT_EQ(
        stored.var.data.var.state,
        JRNL_COMMITTED
    );
}

/**
 * @brief Verify the complete journal initialisation and rollback lifecycle.
 */
TEST_F(JournalTest, CompleteRollbackLifecycle)
{
    const uint16_t target_sector = 2;

    uint8_t original_content[SECTOR_SIZE]{};
    uint8_t original_usage_bitmap[SECTOR_SIZE]{};

    fill_content(original_content, 0xCC);
    fill_content(original_usage_bitmap, 0xDD);

    EXPECT_EQ(
        get_journal_status(&journal),
        JRNL_UNINITIALIZED
    );

    ASSERT_TRUE(
        journal_init(
            &journal,
            storage
        )
    );

    JournalHeaderBuffer initial_header =
        read_header();

    EXPECT_EQ(
        initial_header.var.data.var.magic,
        JRNL_MAGIC
    );

    EXPECT_EQ(
        initial_header.var.data.var.state,
        JRNL_EMPTY
    );

    JournalHeaderDataB data{};

    data.var.magic = JRNL_MAGIC;
    data.var.state = JRNL_ACTIVE;
    data.var.type = JRNL_CONTACT;
    data.var.sector = target_sector;

    ASSERT_TRUE(
        journal_add(
            &journal,
            data,
            original_content,
            original_usage_bitmap
        )
    );

    JournalHeaderBuffer active_header =
        read_header();

    EXPECT_EQ(
        active_header.var.data.var.state,
        JRNL_ACTIVE
    );

    Journal recovered_journal{};
    recovered_journal.storage = storage;

    ASSERT_TRUE(
        journal_content_read(
            &recovered_journal
        )
    );

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            recovered_journal.usage_bitmap_sector
        )
    );

    ASSERT_TRUE(
        journal_init(
            &recovered_journal,
            storage
        )
    );

    uint8_t restored_content[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            target_sector,
            restored_content
        )
    );

    EXPECT_EQ(
        memcmp(
            restored_content,
            original_content,
            SECTOR_SIZE
        ),
        0
    );

    uint8_t restored_usage_bitmap[SECTOR_SIZE]{};

    ASSERT_TRUE(
        storage->read_block(
            storage->context,
            JRNL_USAGE_SECTOR,
            restored_usage_bitmap
        )
    );

    EXPECT_EQ(
        memcmp(
            restored_usage_bitmap,
            original_usage_bitmap,
            SECTOR_SIZE
        ),
        0
    );

    JournalHeaderBuffer final_header =
        read_header();

    EXPECT_EQ(
        final_header.var.data.var.state,
        JRNL_COMMITTED
    );
}

