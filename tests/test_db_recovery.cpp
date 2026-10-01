#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <set>
#include <string>
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
#include "mem_layout.h"
}

/*
 * Storage wrapper that forwards to heap_storage but can simulate a power
 * cut: once g_writes_left reaches zero every further write fails.
 */
static int g_writes_left = -1; // -1 = never fail

static bool PowerCut_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    if (g_writes_left == 0)
    {
        return false;
    }
    if (g_writes_left > 0)
    {
        g_writes_left--;
    }
    return heap_storage.write_block(context, index, in);
}

/**
 * @brief Fixture for on-disk addressing, journal recovery and message
 *        reconstruction across the whole database layout.
 */
class DbRecoveryTest : public ::testing::Test
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
        storage_obj.write_block = PowerCut_WriteBlock;
        g_writes_left = -1;

        ASSERT_TRUE(init_usage_bitmap(storage));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));
    }

    void TearDown() override
    {
        g_writes_left = -1;
    }

    /** @brief Simulate a reboot: reload the bitmap and let the journal roll back. */
    void reboot()
    {
        g_writes_left = -1;
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        ASSERT_TRUE(read_usage_bitmap(storage));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));
    }

    bool sector_is_blank(uint32_t raw_sector)
    {
        const uint8_t *p = &storage_mem[static_cast<size_t>(raw_sector) * SECTOR_SIZE];
        for (uint32_t i = 0; i < SECTOR_SIZE; i++)
        {
            if (p[i] != 0)
            {
                return false;
            }
        }
        return true;
    }
};

/* ============================================================================
 * Bug 1: message sectors must live in the message region
 * ========================================================================== */

/**
 * @brief Message sector 0 used to land on raw sector MESSAGE_DATA_START_SECTOR,
 *        which is inside the contact region. Writing it must not touch contacts.
 */
TEST_F(DbRecoveryTest, MessageSectorDoesNotOverlapContactRegion)
{
    // The contact sector that message sector 0 used to overwrite
    const uint16_t clobbered_contact_sector = MESSAGE_DATA_START_SECTOR - DATA_REGION_START_SECTOR;
    const uint16_t slot = clobbered_contact_sector * CONTACT_SECTOR_CAPACITY;

    ContactBuffer in = create_contact("Ada", "0411111111");
    ContactBuffer out{};
    ASSERT_EQ(write_contact(storage, &journal, slot, &in), STRG_OK);

    MessageBuffer msg = create_message(1, true, (char *)"hello");
    ASSERT_EQ(write_new_message_sector(storage, &journal, "0422222222", 0, &msg), STRG_OK);

    EXPECT_EQ(read_contact(storage, slot, &out), STRG_OK);
    EXPECT_EQ(std::memcmp(in.buffer, out.buffer, sizeof(Contact)), 0);

    // and the message is where the layout says it should be
    EXPECT_FALSE(sector_is_blank(DATA_SECTOR_TO_RAW(MESSAGE_DATA_START_SECTOR)));
}

/**
 * @brief The last message sector fits inside the data region.
 */
TEST_F(DbRecoveryTest, LastMessageSectorIsInsideDataRegion)
{
    MessageBuffer msg = create_message(1, true, (char *)"last");
    ASSERT_EQ(write_new_message_sector(storage, &journal, "0422222222",
                                       TOTAL_MESSAGE_SECTOR_SIZE - 1, &msg), STRG_OK);

    MessageBuffer out{};
    EXPECT_EQ(read_message(storage, TOTAL_MESSAGE_SECTOR_SIZE - 1, 0, &out), STRG_OK);
    EXPECT_EQ(std::memcmp(msg.buffer, out.buffer, sizeof(Message)), 0);
}

/* ============================================================================
 * Bug 2: journal rollback must restore the sector that was journalled
 * ========================================================================== */

/**
 * @brief Power cut after a contact sector write but before the journal is
 *        committed: rollback restores that contact sector, not the sector
 *        numbered by the contact slot.
 */
TEST_F(DbRecoveryTest, RollbackRestoresJournalledContactSector)
{
    // slot 7 and 8 share contact sector 1
    ContactBuffer a = create_contact("Ada", "0411111111");
    ContactBuffer b = create_contact("Bob", "0422222222");
    ContactBuffer out{};

    ASSERT_EQ(write_contact(storage, &journal, 7, &a), STRG_OK);

    // journal (3) + bitmap (1) + contact sector (1) succeed, journal commit fails
    g_writes_left = 5;
    EXPECT_NE(write_contact(storage, &journal, 8, &b), STRG_OK);

    reboot();

    EXPECT_EQ(read_contact(storage, 7, &out), STRG_OK);
    EXPECT_EQ(std::memcmp(a.buffer, out.buffer, sizeof(Contact)), 0);
    EXPECT_EQ(read_contact(storage, 8, &out), STRG_EMPTY);

    // the old rollback target (data sector 8 == contact sector 8) is untouched
    EXPECT_TRUE(sector_is_blank(DATA_SECTOR_TO_RAW(CONTACT_DATA_START_SECTOR + 8)));
}

/**
 * @brief Power cut after a message sector write but before the journal is
 *        committed: rollback restores the message sector.
 */
TEST_F(DbRecoveryTest, RollbackRestoresJournalledMessageSector)
{
    const uint16_t msg_index = 4;
    MessageBuffer m1 = create_message(1, true, (char *)"first");
    MessageBuffer m2 = create_message(2, false, (char *)"second");

    ASSERT_EQ(write_new_message_sector(storage, &journal, "0411111111", msg_index, &m1), STRG_OK);

    // journal (3) + bitmap (1) + message sector (1) succeed, journal commit fails
    g_writes_left = 5;
    EXPECT_NE(write_message(storage, &journal, msg_index, &m2), STRG_OK);

    reboot();

    MessageSectorBuffer sector{};
    ASSERT_EQ(read_message_sector(storage, msg_index, &sector), STRG_OK);
    EXPECT_EQ(sector.var.header.msg_count, 1);
    EXPECT_EQ(std::memcmp(sector.var.messages[0].buffer, m1.buffer, sizeof(Message)), 0);
    EXPECT_TRUE(check_usage_bit(MESSAGE_DATA_SECTOR(msg_index)));
}

/**
 * @brief A full message sector is detected before journalling, so the
 *        journal isn't left active.
 */
TEST_F(DbRecoveryTest, FullMessageSectorLeavesJournalCommitted)
{
    MessageBuffer m = create_message(1, true, (char *)"x");

    ASSERT_EQ(write_new_message_sector(storage, &journal, "0411111111", 0, &m), STRG_OK);
    for (uint32_t i = 1; i < MESSAGE_BLOCK_CAPACITY; i++)
    {
        ASSERT_EQ(write_message(storage, &journal, 0, &m), STRG_OK);
    }

    EXPECT_EQ(write_message(storage, &journal, 0, &m), STRG_FULL);
    EXPECT_EQ(get_journal_status(&journal), JRNL_VALID);
}

/* ============================================================================
 * Bug 3: message reconstruction
 * ========================================================================== */

/**
 * @brief Rebuilding from storage restores every chat's latest message sector
 *        and leaves exactly the unused message sectors allocatable.
 */
TEST_F(DbRecoveryTest, ReconstructMessageRestoresLatestSectors)
{
    std::vector<HashEntry> entries(HASH_TABLE_SIZE);
    std::vector<uint16_t> contact_pool(HASH_TABLE_SIZE);
    std::vector<uint16_t> message_pool(TOTAL_MESSAGE_SECTOR_SIZE);
    FreeList contact_fls{};
    FreeList message_fls{};
    HashTable table{};

    ASSERT_TRUE(free_list_init(&contact_fls, contact_pool.data(), HASH_TABLE_SIZE));
    ASSERT_TRUE(free_list_init(&message_fls, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));
    hash_init(&table, storage, &contact_fls, &message_fls, entries.data(), HASH_TABLE_SIZE);

    // "0411111111" spans two message sectors, "0422222222" one
    const char *phones[] = {"0411111111", "0422222222", "0433333333"};
    const int counts[] = {MESSAGE_BLOCK_CAPACITY + 1, 1, 0};

    for (int p = 0; p < 3; p++)
    {
        ContactBuffer c = create_contact("name", phones[p]);
        ASSERT_TRUE(hash_insert_contact(&table, &journal, &c));
        for (int i = 0; i < counts[p]; i++)
        {
            MessageBuffer m = create_message((uint16_t)i, true, (char *)"msg");
            ASSERT_TRUE(hash_insert_message(&table, &journal, phones[p], &m));
        }
    }

    uint16_t expected_latest[3];
    for (int p = 0; p < 3; p++)
    {
        HashEntry *e = nullptr;
        ASSERT_TRUE(hash_find_entry(&table, phones[p], &e));
        expected_latest[p] = e->latest_msg_extent;
    }
    const size_t used_message_sectors = free_list_used(&message_fls);
    ASSERT_EQ(used_message_sectors, 3u);

    // Rebuild into a fresh table, as a boot would
    std::vector<HashEntry> r_entries(HASH_TABLE_SIZE);
    std::vector<uint16_t> r_contact_pool(HASH_TABLE_SIZE);
    std::vector<uint16_t> r_message_pool(TOTAL_MESSAGE_SECTOR_SIZE);
    FreeList r_contact_fls{};
    FreeList r_message_fls{};
    HashTable rebuilt{};

    ASSERT_TRUE(free_list_empty_init(&r_contact_fls, r_contact_pool.data(), HASH_TABLE_SIZE));
    ASSERT_TRUE(free_list_empty_init(&r_message_fls, r_message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));
    hash_init(&rebuilt, storage, &r_contact_fls, &r_message_fls, r_entries.data(), HASH_TABLE_SIZE);

    reboot();
    ASSERT_TRUE(hash_reconstruct_contact(&rebuilt));
    ASSERT_TRUE(hash_reconstruct_message(&rebuilt, &journal));

    for (int p = 0; p < 3; p++)
    {
        HashEntry *e = nullptr;
        ASSERT_TRUE(hash_find_entry(&rebuilt, phones[p], &e));
        EXPECT_EQ(e->state, ENTRY_OCCUPIED) << phones[p];
        EXPECT_EQ(e->latest_msg_extent, expected_latest[p]) << phones[p];
    }

    EXPECT_EQ(free_list_used(&r_message_fls), used_message_sectors);
    EXPECT_EQ(free_list_available(&r_message_fls), TOTAL_MESSAGE_SECTOR_SIZE - used_message_sectors);

    // A new allocation never hands out a sector that is still in use
    uint16_t fresh = free_list_allocate(&r_message_fls);
    ASSERT_NE(fresh, UINT16_MAX);
    EXPECT_FALSE(check_usage_bit(MESSAGE_DATA_SECTOR(fresh)));

    // and the rebuilt table keeps appending to the right chat
    MessageBuffer m = create_message(99, false, (char *)"after reboot");
    ASSERT_TRUE(hash_insert_message(&rebuilt, &journal, phones[1], &m));
    MessageSectorBuffer sector{};
    ASSERT_EQ(read_message_sector(storage, expected_latest[1], &sector), STRG_OK);
    EXPECT_EQ(sector.var.header.msg_count, 2);
}

/**
 * @brief An empty message region reconstructs to a fully free allocator.
 */
TEST_F(DbRecoveryTest, ReconstructMessageEmptyDatabase)
{
    std::vector<HashEntry> entries(HASH_TABLE_SIZE);
    std::vector<uint16_t> contact_pool(HASH_TABLE_SIZE);
    std::vector<uint16_t> message_pool(TOTAL_MESSAGE_SECTOR_SIZE);
    FreeList contact_fls{};
    FreeList message_fls{};
    HashTable table{};

    ASSERT_TRUE(free_list_empty_init(&contact_fls, contact_pool.data(), HASH_TABLE_SIZE));
    ASSERT_TRUE(free_list_empty_init(&message_fls, message_pool.data(), TOTAL_MESSAGE_SECTOR_SIZE));
    hash_init(&table, storage, &contact_fls, &message_fls, entries.data(), HASH_TABLE_SIZE);

    ASSERT_TRUE(hash_reconstruct_contact(&table));
    ASSERT_TRUE(hash_reconstruct_message(&table, &journal));

    EXPECT_EQ(free_list_used(&message_fls), 0u);
    EXPECT_EQ(free_list_available(&message_fls), (size_t)TOTAL_MESSAGE_SECTOR_SIZE);
}
