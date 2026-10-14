#include "sd_baseline.h"

#include <string.h>

#include "stm32h7xx_hal.h"
#include "contact.h"
#include "journal.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "storage.h"
#include "usage_bitmap.h"

#define SDB_STORAGE_START_SECTOR 0  // same as db_main.c DB_STORAGE_START_SECTOR
#define SDB_SCRATCH_SLOT 0          // contact slot whose sector the block tests use

extern Storage sd_storage;

static Storage *const storage = &sd_storage;
static SDStorageContext g_sd_ctx;
static Journal g_journal;
static uint8_t g_block[SECTOR_SIZE] __attribute__((aligned(32)));

/**
 * @brief Bring up the SD storage, format the usage bitmap and the journal
 *        (the same steps as the db_main.c fixture).
 */
bool sd_baseline_init(void)
{
    if (!SDStorage_Init(&g_sd_ctx, SDB_STORAGE_START_SECTOR, SECTOR_SIZE)) { return false; }
    storage->context = (void *)&g_sd_ctx;
    if (!init_usage_bitmap(storage)) { return false; }
    memset(&g_journal, 0, sizeof(g_journal));
    return journal_init(&g_journal, storage);
}

const char *sd_baseline_name(SdbOp op)
{
    static const char *const names[SDB_COUNT] = {"sd_read_block", "sd_write_block", "sd_read_sector_crc", "sd_write_contact_journaled"};
    return names[op];
}

static bool run_once(SdbOp op, uint32_t rep)
{
    uint16_t sector = (uint16_t)CONTACT_DATA_SECTOR(SDB_SCRATCH_SLOT);
    switch (op)
    {
    case SDB_READ_BLOCK:
        return storage->read_block(storage->context, sector, g_block);
    case SDB_WRITE_BLOCK:
        return storage->write_block(storage->context, sector, g_block);
    case SDB_READ_SECTOR:
        return read_sector(storage, sector, g_block) != STRG_FAIL;
    case SDB_WRITE_CONTACT:
    {
        ContactBuffer c;
        memset(&c, 0, sizeof(c));
        c.contact.name_len = 9;
        memcpy(c.contact.name, "Bench Cnt", 9);
        c.contact.phone_len = 10;
        memcpy(c.contact.phone, "0400000000", 10);
        c.contact.phone[9] = (char)('0' + rep % 10u);
        return write_contact(storage, &g_journal, (uint16_t)(rep % CONTACT_SECTOR_CAPACITY), &c) == STRG_OK;
    }
    default:
        return false;
    }
}

/**
 * @brief Time `reps` repetitions of one SD operation with the DWT counter.
 *        Interrupts stay enabled: the SD driver polls and calls osDelay().
 *
 * @retval Number of successful repetitions stored in cycles[].
 */
uint32_t sd_baseline_measure(SdbOp op, uint32_t *cycles, uint32_t reps)
{
    uint32_t stored = 0;
    memset(g_block, 0xA5, sizeof(g_block));
    if (op == SDB_READ_SECTOR)
    {
        sector_crc_stamp(g_block);
        storage->write_block(storage->context, (uint16_t)CONTACT_DATA_SECTOR(SDB_SCRATCH_SLOT), g_block);
    }
    for (uint32_t r = 0; r < reps; r++)
    {
        uint32_t t0 = DWT->CYCCNT;
        bool ok = run_once(op, r);
        uint32_t t1 = DWT->CYCCNT;
        if (ok) { cycles[stored++] = t1 - t0; }
    }
    return stored;
}
