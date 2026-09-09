/*
 * db_main.c - on-hardware unit tests for the phone-keyed contact database.
 *
 * This is the firmware-side mirror of tests/test_hash_table.cpp: it drives
 * the real hash table, the rollback journal and hash-table reconstruction
 * against the physical SD card instead of the heap-backed mock.
 *
 * On-disk layout is the one described in mem_layout.h:
 *
 *   +-------------+--------------+---------+------------------------+
 *   | Superheader | Usage bitmap | Journal | Contact + message data |
 *   |  1 sector   |  N sectors   | 3 sect. |                        |
 *   +-------------+--------------+---------+------------------------+
 *
 * Result is reported on the PB0 LED (see blink_forever()):
 *   - fast continuous blink            -> every check passed
 *   - N short pulses, pause, repeat    -> check number N failed
 *
 * ------------------------------------------------------------------------
 *  DB_TEST_WRITE - persistence toggle
 * ------------------------------------------------------------------------
 *   1 (default) : "write" pass. Wipe the journal + usage bitmap, insert the
 *                 fixed contact set through the journalled write path,
 *                 exercise remove / hash-collision handling, then rebuild
 *                 the RAM table from storage and verify it.
 *
 *   0           : "persistence" pass. Touch NOTHING on the card beyond what
 *                 journal recovery may roll back. Reload the usage bitmap
 *                 and reconstruct the table purely from what a previous
 *                 DB_TEST_WRITE=1 run persisted, then verify it.
 *
 *   Flash once with 1, power-cycle or reflash with 0. A fast (pass) blink
 *   in the second run proves the contacts survived with no writer running.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hash_table_phone.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "contact.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "db_main.h"

#ifndef DB_TEST_WRITE
#define DB_TEST_WRITE 1
#endif

/* Reconstruction passes a 512B ContactSector by value through a couple of
 * frames, so give the task some head-room. */
#define DATABASE_TASK_STACK_SIZE 8192
#define DATABASE_TASK_PRIORITY   osPriorityNormal

/* SD storage is addressed one 512B sector at a time (block == sector). */
#define DB_STORAGE_BLOCK_SIZE SECTOR_SIZE
#define DB_STORAGE_START_SECTOR 0

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Failure codes -> LED blink counts
 * ------------------------------------------------------------------ */
enum
{
    T_OK = 0,
    T_STORAGE_INIT,          /* 1  SDStorage_Init failed                */
    T_BITMAP_INIT,           /* 2  init_usage_bitmap failed             */
    T_JOURNAL_INIT,          /* 3  journal_init failed                  */
    T_INSERT,                /* 4  hash_insert_contact_by_phone failed  */
    T_VERIFY_INSERT,         /* 5  inserted contact not found / wrong   */
    T_REMOVE,                /* 6  remove failed or contact still there */
    T_COLLISION,             /* 7  colliding phone pair not resolved    */
    T_BITMAP_RELOAD,         /* 8  read_usage_bitmap failed             */
    T_RECON_RUN,             /* 9  hash_reconstruct_contact failed      */
    T_RECON_SIZE,            /* 10 rebuilt table has wrong size         */
    T_RECON_FIND,            /* 11 survivor missing / wrong after rebuild */
    T_RECON_REMOVED,         /* 12 removed contact reappeared           */
    T_RECON_WRITABLE         /* 13 rebuilt table rejects new writes     */
};

/* ------------------------------------------------------------------ *
 *  Fixed data set (matches the intent of test_hash_table.cpp)
 * ------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    const char *phone;
    bool removed; /* expected ABSENT after the tests / reconstruction */
} TestContact;

static TestContact g_contacts[] = {
    { "Alice",     "0411111111", false },
    { "Bob",       "0422222222", true  }, /* removed during the write pass */
    { "Charlie",   "0433333333", false },
    { "Dana",      "0444444444", false },
    { "Collide A", "0400000601", false }, /* hash_phone() collides with... */
    { "Collide B", "0400002060", false }, /* ...this one                   */
};
#define N_CONTACTS (sizeof(g_contacts) / sizeof(g_contacts[0]))

/* ------------------------------------------------------------------ *
 *  RAM backing (too large for DTCM / the task stack -> RAM D1)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_fls_mem[HASH_TABLE_SIZE];

static HashTable       g_table;
static FreeList        g_fls;
static Journal         g_journal;
static SDStorageContext g_sd_ctx;

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
#if DB_TEST_WRITE
static ContactBuffer make_contact(const char *name, const char *phone)
{
    ContactBuffer c;
    size_t nl = strlen(name);
    size_t pl = strlen(phone);

    memset(&c, 0, sizeof(c));

    if (nl > MAX_NAME_LEN)  { nl = MAX_NAME_LEN; }
    if (pl > MAX_PHONE_LEN) { pl = MAX_PHONE_LEN; }

    c.contact.name_len = (uint8_t)nl;
    memcpy(c.contact.name, name, nl);

    c.contact.phone_len = (uint8_t)pl;
    memcpy(c.contact.phone, phone, pl);

    return c;
}
#endif /* DB_TEST_WRITE */

static bool contact_name_is(const ContactBuffer *c, const char *name)
{
    size_t nl = strlen(name);
    return c->contact.name_len == nl &&
           memcmp(c->contact.name, name, nl) == 0;
}

static size_t expected_survivors(void)
{
    size_t n = 0;
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (!g_contacts[i].removed) { n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 *  Shared: reconstruct the RAM table from persisted storage
 * ------------------------------------------------------------------ */
static int reconstruct_table(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    /* Reconstruction starts from an allocator with nothing free and hands
     * back the slots the usage bitmap says are unused. */
    if (!free_list_empty_init(&g_fls, g_fls_mem, HASH_TABLE_SIZE))
    {
        return T_RECON_RUN;
    }

    hash_init(&g_table, &sd_storage, &g_fls, g_entries, HASH_TABLE_SIZE);

    /* Drop the in-RAM bitmap and reload it from storage, as a real boot would. */
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    if (!read_usage_bitmap(&sd_storage))
    {
        return T_BITMAP_RELOAD;
    }

    if (!hash_reconstruct_contact(&g_table))
    {
        return T_RECON_RUN;
    }

    return T_OK;
}

/* Verify the rebuilt table against the fixed data set. */
static int verify_reconstructed(void)
{
    if (hash_size(&g_table) != expected_survivors())
    {
        return T_RECON_SIZE;
    }

    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        ContactBuffer got;
        memset(&got, 0, sizeof(got));

        bool found = hash_find_contact_by_phone(&g_table, g_contacts[i].phone, &got);

        if (g_contacts[i].removed)
        {
            if (found) { return T_RECON_REMOVED; }
            continue;
        }

        if (!found) { return T_RECON_FIND; }
        if (!contact_name_is(&got, g_contacts[i].name)) { return T_RECON_FIND; }
    }

    return T_OK;
}

static int storage_bringup(void)
{
    if (!SDStorage_Init(&g_sd_ctx, DB_STORAGE_START_SECTOR, DB_STORAGE_BLOCK_SIZE))
    {
        return T_STORAGE_INIT;
    }
    sd_storage.context = (void *)&g_sd_ctx;
    return T_OK;
}

/* ================================================================== *
 *  DB_TEST_WRITE == 1 : full write + reconstruct pass
 * ================================================================== */
#if DB_TEST_WRITE

static int db_run_write(void)
{
    int rc;

    if ((rc = storage_bringup()) != T_OK) { return rc; }

    /* Start from a known-clean journal + usage bitmap. */
    if (!init_usage_bitmap(&sd_storage)) { return T_BITMAP_INIT; }

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, &sd_storage)) { return T_JOURNAL_INIT; }

    /* Fresh live table. */
    memset(g_entries, 0, sizeof(g_entries));
    if (!free_list_init(&g_fls, g_fls_mem, HASH_TABLE_SIZE)) { return T_INSERT; }
    hash_init(&g_table, &sd_storage, &g_fls, g_entries, HASH_TABLE_SIZE);

    /* Insert the whole set through the journalled write path. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        ContactBuffer c = make_contact(g_contacts[i].name, g_contacts[i].phone);
        if (hash_insert_contact_by_phone(&g_table, &g_journal, &c) == UINT16_MAX)
        {
            return T_INSERT;
        }
    }

    /* Every contact is findable with the right name. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        ContactBuffer got;
        memset(&got, 0, sizeof(got));
        if (!hash_find_contact_by_phone(&g_table, g_contacts[i].phone, &got))
        {
            return T_VERIFY_INSERT;
        }
        if (!contact_name_is(&got, g_contacts[i].name))
        {
            return T_VERIFY_INSERT;
        }
    }

    /* Remove the flagged contacts; check they are gone. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (!g_contacts[i].removed) { continue; }

        ContactBuffer rm;
        memset(&rm, 0, sizeof(rm));
        if (!hash_remove_contact_by_phone(&g_table, &g_journal, g_contacts[i].phone, &rm))
        {
            return T_REMOVE;
        }

        ContactBuffer got;
        if (hash_find_contact_by_phone(&g_table, g_contacts[i].phone, &got))
        {
            return T_REMOVE;
        }
    }

    /* The colliding phone pair is still resolved by the exact phone string. */
    {
        ContactBuffer a;
        ContactBuffer b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));

        if (!hash_find_contact_by_phone(&g_table, "0400000601", &a) ||
            !hash_find_contact_by_phone(&g_table, "0400002060", &b))
        {
            return T_COLLISION;
        }
        if (!contact_name_is(&a, "Collide A") || !contact_name_is(&b, "Collide B"))
        {
            return T_COLLISION;
        }
    }

    /* ---- Reconstruct the table over the same storage ---- */
    if ((rc = reconstruct_table()) != T_OK)  { return rc; }
    if ((rc = verify_reconstructed()) != T_OK) { return rc; }

    /* The rebuilt table accepts new writes. This DOES modify the card, so
     * it only runs in write mode; the scratch contact is removed again so
     * the persisted set still matches g_contacts for a later read-only run. */
    {
        ContactBuffer scratch = make_contact("Scratch", "0400009999");
        ContactBuffer got;
        ContactBuffer rm;

        if (hash_insert_contact_by_phone(&g_table, &g_journal, &scratch) == UINT16_MAX)
        {
            return T_RECON_WRITABLE;
        }
        if (!hash_find_contact_by_phone(&g_table, "0400009999", &got))
        {
            return T_RECON_WRITABLE;
        }
        memset(&rm, 0, sizeof(rm));
        if (!hash_remove_contact_by_phone(&g_table, &g_journal, "0400009999", &rm))
        {
            return T_RECON_WRITABLE;
        }
    }

    return T_OK;
}

#else /* DB_TEST_WRITE == 0 */

/* ================================================================== *
 *  DB_TEST_WRITE == 0 : persistence / reconstruct-only pass
 * ================================================================== */
static int db_run_persist(void)
{
    int rc;

    if ((rc = storage_bringup()) != T_OK) { return rc; }

    /* journal_init() only reads/validates here (it may roll back an
     * interrupted write from a previous run - that is the correct
     * recovery behaviour). No inserts, removes or bitmap init. */
    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, &sd_storage)) { return T_JOURNAL_INIT; }

    if ((rc = reconstruct_table()) != T_OK)  { return rc; }
    if ((rc = verify_reconstructed()) != T_OK) { return rc; }

    return T_OK;
}

#endif /* DB_TEST_WRITE */

/* ------------------------------------------------------------------ *
 *  LED reporting (PB0)
 * ------------------------------------------------------------------ */
static void blink_forever(int code)
{
    if (code == T_OK)
    {
        for (;;)
        {
            GPIOB->ODR ^= (1U << 0);
            osDelay(150);
        }
    }

    for (;;)
    {
        for (int i = 0; i < code; i++)
        {
            GPIOB->ODR |=  (1U << 0);
            osDelay(200);
            GPIOB->ODR &= ~(1U << 0);
            osDelay(200);
        }
        osDelay(1500);
    }
}

/* ------------------------------------------------------------------ *
 *  Task
 * ------------------------------------------------------------------ */
void dbTask(void *arg)
{
    (void)arg;

#if DB_TEST_WRITE
    int code = db_run_write();
#else
    int code = db_run_persist();
#endif

    blink_forever(code);
}

void DB_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "Database Task",
        .stack_size = DATABASE_TASK_STACK_SIZE,
        .priority = DATABASE_TASK_PRIORITY};

    osThreadNew(dbTask, NULL, &task_attr);
}
