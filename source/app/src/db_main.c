/*
 * db_main.c - on-hardware unit tests for the phone-keyed contact + message
 * database.
 *
 * This is the firmware-side mirror of tests/test_hash_table.cpp: it drives
 * the real hash table (contacts AND message chats), the rollback journal
 * and hash-table reconstruction against the physical SD card instead of
 * the heap-backed mock gtest uses.
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
 *                 attach messages (including a multi-sector chat and a
 *                 hash-collision pair), exercise every removal path
 *                 (contact-only, chat-only, and combined), then rebuild
 *                 the RAM table from storage and verify all of it.
 *
 *   0           : "persistence" pass. Touch NOTHING on the card beyond what
 *                 journal recovery may roll back. Reload the usage bitmap
 *                 and reconstruct the contact table AND the message chats
 *                 purely from what a previous DB_TEST_WRITE=1 run
 *                 persisted, then verify it.
 *
 *   Flash once with 1, power-cycle or reflash with 0. A fast (pass) blink
 *   in the second run proves the contacts and messages survived with no
 *   writer running.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hash_table.h"
#include "contact.h"
#include "message.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "db_main.h"

#ifndef DB_TEST_WRITE
#define DB_TEST_WRITE 1
#endif

/* Reconstruction passes sector-sized buffers by value through a couple of
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
    T_STORAGE_INIT,          /* 1  SDStorage_Init failed                            */
    T_BITMAP_INIT,           /* 2  init_usage_bitmap failed                         */
    T_JOURNAL_INIT,          /* 3  journal_init failed                              */
    T_INSERT,                /* 4  hash_insert_contact failed                       */
    T_VERIFY_INSERT,         /* 5  inserted contact not found / wrong               */
    T_MESSAGE_SEND,          /* 6  hash_insert_message failed                       */
    T_MESSAGE_FIND,          /* 7  latest/only message missing or wrong             */
    T_MESSAGE_ROLLOVER,      /* 8  multi-sector chat wrong (order or count)         */
    T_MESSAGE_REMOVE,        /* 9  hash_remove_message failed or chat still present */
    T_REMOVE,                /* 10 hash_remove_contact failed or contact remains    */
    T_REMOVE_ALL,            /* 11 hash_remove (contact+messages) failed            */
    T_COLLISION,             /* 12 colliding phone pair not resolved                */
    T_BITMAP_RELOAD,         /* 13 read_usage_bitmap failed                         */
    T_RECON_CONTACT_RUN,     /* 14 hash_reconstruct_contact failed                  */
    T_RECON_MESSAGE_RUN,     /* 15 hash_reconstruct_message failed                  */
    T_RECON_SIZE,            /* 16 rebuilt table has wrong contact count            */
    T_RECON_FIND,            /* 17 survivor missing / wrong after rebuild           */
    T_RECON_REMOVED,         /* 18 removed contact reappeared                       */
    T_RECON_WRITABLE         /* 19 rebuilt table rejects new contact/message writes */
};

/* ------------------------------------------------------------------ *
 *  Fixed data set (matches the intent of test_hash_table.cpp)
 * ------------------------------------------------------------------ */
typedef enum
{
    REMOVE_NONE,                 /* contact survives to the end                      */
    REMOVE_CONTACT_ONLY,         /* hash_remove_contact() -- never had a chat         */
    REMOVE_CONTACT_AND_MESSAGES  /* hash_remove() -- contact + chat removed together  */
} RemoveMode;

typedef struct
{
    const char *name;
    const char *phone;
    RemoveMode  remove_mode;
    bool        messages_removed; /* contact survives; chat alone removed via hash_remove_message() */
    bool        rollover;         /* sent MESSAGE_BLOCK_CAPACITY + 1 messages to force a 2nd sector  */
    bool        no_messages;      /* no message ever sent to this contact                            */
    uint16_t    base_ts;          /* first message's timestamp; later ones are base_ts + k (ignored
                                      when no_messages is set)                                        */
} TestContact;

static TestContact g_contacts[] = {
    /* name          phone           remove_mode                    msgs_rm rollover no_msg base_ts */
    { "Alice",     "0411111111", REMOVE_NONE,                    false,  false,   false, 1100 },
    { "Bob",       "0422222222", REMOVE_CONTACT_AND_MESSAGES,    false,  false,   false, 1200 },
    { "Charlie",   "0433333333", REMOVE_NONE,                    false,  true,    false, 1300 },
    { "Dana",      "0444444444", REMOVE_NONE,                    true,   false,   false, 1400 },
    { "Collide A", "0400000601", REMOVE_NONE,                    false,  false,   false, 1500 }, /* hash_phone() collides with... */
    { "Collide B", "0400002060", REMOVE_NONE,                    false,  false,   false, 1600 }, /* ...this one                   */
    { "Eve",       "0455555555", REMOVE_CONTACT_ONLY,            false,  false,   true,     0 },
};
#define N_CONTACTS (sizeof(g_contacts) / sizeof(g_contacts[0]))

/* ------------------------------------------------------------------ *
 *  RAM backing (too large for DTCM / the task stack -> RAM D1)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_contact_fls_mem[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_message_fls_mem[TOTAL_MESSAGE_SECTOR_SIZE];

static HashTable        g_table;
static FreeList          g_contact_fls;
static FreeList          g_message_fls;
static Journal           g_journal;
static SDStorageContext  g_sd_ctx;

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

static bool send_message(const char *phone, uint16_t timestamp, bool direction, char *text)
{
    MessageBuffer m = create_message(timestamp, direction, text);
    return hash_insert_message(&g_table, &g_journal, phone, &m);
}
#endif /* DB_TEST_WRITE */

static bool contact_name_is(const ContactBuffer *c, const char *name)
{
    size_t nl = strlen(name);
    return c->contact.name_len == nl &&
           memcmp(c->contact.name, name, nl) == 0;
}

static int message_count_for(const TestContact *c)
{
    return c->rollover ? (MESSAGE_BLOCK_CAPACITY + 1) : 1;
}

static size_t expected_survivors(void)
{
    size_t n = 0;
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (g_contacts[i].remove_mode == REMOVE_NONE) { n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 *  Shared verification, used both right before reconstruction (to
 *  confirm the write pass itself behaved) and again right after (to
 *  confirm reconstruction reproduced the same state).
 * ------------------------------------------------------------------ */
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

        bool found = hash_find_contact(&g_table, g_contacts[i].phone, &got);

        if (g_contacts[i].remove_mode != REMOVE_NONE)
        {
            if (found) { return T_RECON_REMOVED; }
            continue;
        }

        if (!found) { return T_RECON_FIND; }
        if (!contact_name_is(&got, g_contacts[i].name)) { return T_RECON_FIND; }
    }

    return T_OK;
}

/* Every contact's message-chat state matches the fixed data set: gone for
 * anything removed (whole contact, or chat-only), present and correct
 * (including full multi-sector ordering for the rollover case) for
 * everything else. Used both pre- and post-reconstruction, since a
 * correct reconstruction should reproduce exactly the same state. */
static int verify_messages(void)
{
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        const TestContact *c = &g_contacts[i];
        bool should_have_chat = (c->remove_mode == REMOVE_NONE) &&
                                 !c->no_messages && !c->messages_removed;

        MessageBuffer latest;
        memset(&latest, 0, sizeof(latest));
        bool found = hash_find_message(&g_table, c->phone, &latest);

        if (!should_have_chat)
        {
            if (found) { return T_MESSAGE_FIND; }
            continue;
        }

        int n = message_count_for(c);
        uint16_t expected_latest_ts = (uint16_t)(c->base_ts + n - 1);

        if (!found || latest.msg.timestamp != expected_latest_ts)
        {
            return T_MESSAGE_FIND;
        }

        if (!c->rollover) { continue; }

        {
            MessageBuffer all[MESSAGE_BLOCK_CAPACITY + 1];
            int got = hash_find_n_message(&g_table, c->phone, n, all);
            if (got != n) { return T_MESSAGE_ROLLOVER; }

            for (int k = 0; k < n; k++)
            {
                uint16_t expected_ts = (uint16_t)(c->base_ts + (n - 1 - k));
                if (all[k].msg.timestamp != expected_ts) { return T_MESSAGE_ROLLOVER; }
            }
        }
    }

    return T_OK;
}

/* ------------------------------------------------------------------ *
 *  Shared: reconstruct the RAM table (contacts, then messages) from
 *  persisted storage.
 * ------------------------------------------------------------------ */
static int reconstruct_table(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    /* Reconstruction starts from allocators with nothing free and hands
     * back the slots the usage bitmap says are unused. */
    if (!free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))
    {
        return T_RECON_CONTACT_RUN;
    }
    if (!free_list_empty_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE))
    {
        return T_RECON_MESSAGE_RUN;
    }

    hash_init(&g_table, &sd_storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    /* Drop the in-RAM bitmap and reload it from storage, as a real boot would. */
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    if (!read_usage_bitmap(&sd_storage))
    {
        return T_BITMAP_RELOAD;
    }

    if (!hash_reconstruct_contact(&g_table))
    {
        return T_RECON_CONTACT_RUN;
    }

    if (!hash_reconstruct_message(&g_table, &g_journal))
    {
        return T_RECON_MESSAGE_RUN;
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
    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))   { return T_INSERT; }
    if (!free_list_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE)) { return T_INSERT; }
    hash_init(&g_table, &sd_storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    /* Insert the whole set through the journalled write path. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        ContactBuffer c = make_contact(g_contacts[i].name, g_contacts[i].phone);
        if (!hash_insert_contact(&g_table, &g_journal, &c))
        {
            return T_INSERT;
        }
    }

    /* Every contact is findable with the right name. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        ContactBuffer got;
        memset(&got, 0, sizeof(got));
        if (!hash_find_contact(&g_table, g_contacts[i].phone, &got))
        {
            return T_VERIFY_INSERT;
        }
        if (!contact_name_is(&got, g_contacts[i].name))
        {
            return T_VERIFY_INSERT;
        }
    }

    /* Attach messages: one to most contacts, a rollover chat to Charlie
     * (MESSAGE_BLOCK_CAPACITY + 1 messages, forcing a second linked
     * sector), none at all to Eve. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (g_contacts[i].no_messages) { continue; }

        int n = message_count_for(&g_contacts[i]);
        for (int k = 0; k < n; k++)
        {
            uint16_t ts = (uint16_t)(g_contacts[i].base_ts + k);
            if (!send_message(g_contacts[i].phone, ts, true, "test message"))
            {
                return T_MESSAGE_SEND;
            }
        }
    }

    /* Remove just the chat (contact survives) for messages_removed entries. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (!g_contacts[i].messages_removed) { continue; }

        MessageBuffer rm;
        memset(&rm, 0, sizeof(rm));
        if (!hash_remove_message(&g_table, &g_journal, g_contacts[i].phone, &rm))
        {
            return T_MESSAGE_REMOVE;
        }

        MessageBuffer got;
        if (hash_find_message(&g_table, g_contacts[i].phone, &got))
        {
            return T_MESSAGE_REMOVE;
        }

        ContactBuffer still_there;
        if (!hash_find_contact(&g_table, g_contacts[i].phone, &still_there))
        {
            return T_MESSAGE_REMOVE;
        }
    }

    /* Remove contact-only (no chat ever existed) for REMOVE_CONTACT_ONLY entries. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (g_contacts[i].remove_mode != REMOVE_CONTACT_ONLY) { continue; }

        ContactBuffer rm;
        memset(&rm, 0, sizeof(rm));
        if (!hash_remove_contact(&g_table, &g_journal, g_contacts[i].phone, &rm))
        {
            return T_REMOVE;
        }

        ContactBuffer got;
        if (hash_find_contact(&g_table, g_contacts[i].phone, &got))
        {
            return T_REMOVE;
        }
    }

    /* Remove contact + chat together for REMOVE_CONTACT_AND_MESSAGES entries. */
    for (size_t i = 0; i < N_CONTACTS; i++)
    {
        if (g_contacts[i].remove_mode != REMOVE_CONTACT_AND_MESSAGES) { continue; }

        if (!hash_remove(&g_table, &g_journal, g_contacts[i].phone, NULL))
        {
            return T_REMOVE_ALL;
        }

        ContactBuffer got_c;
        if (hash_find_contact(&g_table, g_contacts[i].phone, &got_c))
        {
            return T_REMOVE_ALL;
        }

        MessageBuffer got_m;
        if (hash_find_message(&g_table, g_contacts[i].phone, &got_m))
        {
            return T_REMOVE_ALL;
        }
    }

    /* The colliding phone pair is still resolved by the exact phone string. */
    {
        ContactBuffer a;
        ContactBuffer b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));

        if (!hash_find_contact(&g_table, "0400000601", &a) ||
            !hash_find_contact(&g_table, "0400002060", &b))
        {
            return T_COLLISION;
        }
        if (!contact_name_is(&a, "Collide A") || !contact_name_is(&b, "Collide B"))
        {
            return T_COLLISION;
        }
    }

    /* Final message state, before touching storage any further. */
    if ((rc = verify_messages()) != T_OK) { return rc; }

    /* ---- Reconstruct the table over the same storage ---- */
    if ((rc = reconstruct_table()) != T_OK)    { return rc; }
    if ((rc = verify_reconstructed()) != T_OK) { return rc; }
    if ((rc = verify_messages()) != T_OK)      { return rc; }

    /* The rebuilt table accepts new contact + message writes. This DOES
     * modify the card, so it only runs in write mode; the scratch data is
     * removed again so the persisted set still matches g_contacts for a
     * later DB_TEST_WRITE=0 run. */
    {
        ContactBuffer scratch = make_contact("Scratch", "0400009999");
        ContactBuffer got_c;
        ContactBuffer rm_c;
        MessageBuffer got_m;
        MessageBuffer rm_m;

        if (!hash_insert_contact(&g_table, &g_journal, &scratch))
        {
            return T_RECON_WRITABLE;
        }
        if (!hash_find_contact(&g_table, "0400009999", &got_c))
        {
            return T_RECON_WRITABLE;
        }

        if (!send_message("0400009999", 9999, true, "scratch"))
        {
            return T_RECON_WRITABLE;
        }
        if (!hash_find_message(&g_table, "0400009999", &got_m))
        {
            return T_RECON_WRITABLE;
        }

        memset(&rm_m, 0, sizeof(rm_m));
        if (!hash_remove_message(&g_table, &g_journal, "0400009999", &rm_m))
        {
            return T_RECON_WRITABLE;
        }

        memset(&rm_c, 0, sizeof(rm_c));
        if (!hash_remove_contact(&g_table, &g_journal, "0400009999", &rm_c))
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

    if ((rc = reconstruct_table()) != T_OK)    { return rc; }
    if ((rc = verify_reconstructed()) != T_OK) { return rc; }
    if ((rc = verify_messages()) != T_OK)      { return rc; }

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
