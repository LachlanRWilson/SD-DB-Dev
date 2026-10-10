/*
 * db_main.c - on-hardware unit tests for the phone-keyed contact + message
 * database.
 *
 * This is the firmware-side port of tests/test_hash_table.cpp (the
 * HashTableTest suite): every TEST_F there has a test_*() function here with
 * the same name and the same checks, run against the physical SD card
 * (sd_storage.c) instead of the heap-backed mock gtest uses.
 *
 * On-disk layout is the one described in mem_layout.h:
 *
 *   +-------------+---------+--------------+------------------------+
 *   | Superheader | Journal | Usage bitmap | Contact + message data |
 *   |  1 sector   | 3 sect. |  N sectors   |                        |
 *   +-------------+---------+--------------+------------------------+
 *
 * Differences from the gtest suite (all forced by the target, not the logic):
 *   - The fixture's per-test reset formats the usage bitmap + journal on the
 *     card instead of memset()ing a heap buffer. Stale data sectors from a
 *     previous test are left behind, but nothing reads a sector the bitmap
 *     says is unused.
 *   - rebuild() reconstructs into the same RAM table rather than a second
 *     one: two 14293-entry tables don't fit in RAM_D1.
 *   - The scale tests (5000/10000 contacts, 5000 messages) are host-only:
 *     they were a proof of concept for the hash table, and take ~30 minutes
 *     over the SD card.
 *   - A failed check ends that test (gtest's EXPECT_* would carry on).
 *
 * ------------------------------------------------------------------------
 *  Reporting
 * ------------------------------------------------------------------------
 *   g_db_report (below) is filled in as the suite runs. Read it over SWD
 *   without halting the core, e.g.
 *
 *     STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <&g_db_report> 0x100
 *
 *   The LCD shows each test as it runs and the summary at the end
 *   (htest_ui.c). The PB0 LED also shows the result once the suite finishes:
 *     - fast continuous blink         -> every test passed
 *     - N short pulses, pause, repeat -> test number N (1-based, the order
 *                                        of g_tests[]) was the first to fail
 *
 * ------------------------------------------------------------------------
 *  DB_TEST_WRITE - persistence toggle
 * ------------------------------------------------------------------------
 *   1 (default) : run the whole suite, then leave a known data set on the
 *                 card (PersistSeed).
 *   0           : touch nothing beyond what journal recovery may roll back;
 *                 rebuild the contacts and message chats from a previous
 *                 DB_TEST_WRITE=1 run and check them (PersistVerify).
 *
 *   Flash once with 1, power-cycle or reflash with 0. A pass in the second
 *   run proves the data survived with no writer running.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "htest_ui.h"

#ifndef DB_TEST_WRITE
#define DB_TEST_WRITE 1
#endif

/* Defined in hash_table.c, not exported through the header. */
uint16_t hash_phone(const char *phone);

/* The task stack and TCB are static: configTOTAL_HEAP_SIZE (15KB) can't hold
 * a stack this size alongside the other tasks. Stack size is in bytes. */
#define DATABASE_TASK_STACK_SIZE (16 * 1024)
#define DATABASE_TASK_PRIORITY   osPriorityNormal

/* SD storage is addressed one 512B sector at a time (block == sector). */
#define DB_STORAGE_BLOCK_SIZE   SECTOR_SIZE
#define DB_STORAGE_START_SECTOR 0

#define DB_MAX_TESTS     48
#define DB_REPORT_MAGIC  0xDB7E5701u

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Report (read over SWD)
 * ------------------------------------------------------------------ */
typedef enum
{
    TEST_NOT_RUN = 0,
    TEST_PASS,
    TEST_FAIL
} TestStatus;

typedef struct
{
    uint32_t magic;                    /* DB_REPORT_MAGIC once the suite has started   */
    uint32_t done;                     /* 1 when every test has run                    */
    uint32_t total;                    /* tests in this build                          */
    uint32_t passed;
    uint32_t failed;
    uint32_t current;                  /* 1-based number of the test running now       */
    uint32_t first_failed;             /* 1-based number of the first failure, 0 none  */
    uint8_t  status[DB_MAX_TESTS];     /* TestStatus per test                          */
    uint16_t fail_line[DB_MAX_TESTS];  /* db_main.c line of the failed check           */
    uint32_t ms[DB_MAX_TESTS];         /* run time per test                            */
} DbTestReport;

volatile DbTestReport g_db_report;

/* ------------------------------------------------------------------ *
 *  RAM backing (too large for DTCM / the task stack -> RAM D1)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_contact_fls_mem[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_message_fls_mem[TOTAL_MESSAGE_SECTOR_SIZE];

static Storage *const storage = &sd_storage;

static HashTable        g_table;
static FreeList         g_contact_fls;
static FreeList         g_message_fls;
static Journal          g_journal;
static SDStorageContext g_sd_ctx;

/* ------------------------------------------------------------------ *
 *  Minimal test harness
 * ------------------------------------------------------------------ */
static int g_fail_line;

/* Fail the running test and return from it. */
#define CHECK(cond)                       \
    do                                    \
    {                                     \
        if (!(cond))                      \
        {                                 \
            if (g_fail_line == 0)         \
            {                             \
                g_fail_line = __LINE__;   \
            }                             \
            return;                       \
        }                                 \
    } while (0)

typedef struct
{
    const char *name;
    void (*fn)(void);
} TestCase;

/* ------------------------------------------------------------------ *
 *  Helpers (mirror the HashTableTest fixture helpers)
 * ------------------------------------------------------------------ */

#if DB_TEST_WRITE
/* Same rule as the gtest make_contact(): an over-long field gives an empty contact. */
static ContactBuffer make_contact(const char *name, const char *phone)
{
    ContactBuffer contact;
    size_t nl = strlen(name);
    size_t pl = strlen(phone);

    memset(&contact, 0, sizeof(contact));

    if (nl > MAX_NAME_LEN || pl > MAX_PHONE_LEN)
    {
        return contact;
    }

    contact.contact.name_len = (uint8_t)nl;
    memcpy(contact.contact.name, name, nl);

    contact.contact.phone_len = (uint8_t)pl;
    memcpy(contact.contact.phone, phone, pl);

    return contact;
}

static bool insert(const char *name, const char *phone)
{
    ContactBuffer c = make_contact(name, phone);
    return hash_insert_contact(&g_table, &g_journal, &c);
}

static bool send(const char *phone, uint16_t timestamp, bool direction, const char *text)
{
    MessageBuffer m = create_message(timestamp, direction, (char *)text);
    return hash_insert_message(&g_table, &g_journal, phone, &m);
}

/* Sector a phone number's entry points at, UINT16_MAX if it has none. */
static uint16_t sector_for(const char *phone)
{
    HashEntry *entry = NULL;
    if (!hash_find_entry(&g_table, phone, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return UINT16_MAX;
    }
    return entry->sector;
}
#endif /* DB_TEST_WRITE */

/* EXPECT_STREQ for a fixed-size char field that may not be terminated. */
static bool field_eq(const char *field, size_t cap, const char *expected)
{
    size_t n = strlen(expected);
    return n <= cap && memcmp(field, expected, n) == 0 && (n == cap || field[n] == '\0');
}

#define NAME_IS(cb, s)  field_eq((cb).contact.name, MAX_NAME_LEN, (s))
#define PHONE_IS(cb, s) field_eq((cb).contact.phone, MAX_PHONE_LEN, (s))
#define TEXT_IS(mb, s)  field_eq((mb).msg.str, SMS_MAX_MESSAGE_LENGTH, (s))

#if DB_TEST_WRITE
/* Format the i-th generated phone number the list tests use ("04%08d"). */
static void numbered_phone(char *buf, size_t len, int i)
{
    snprintf(buf, len, "04%08d", i);
}

typedef char PhoneStr[MAX_PHONE_LEN + 1];

static int phone_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Copy each returned contact's phone into a terminated string. */
static void collect_phones(const ContactBuffer *in, int n, PhoneStr *out)
{
    for (int i = 0; i < n; i++)
    {
        uint8_t len = in[i].contact.phone_len;
        if (len > MAX_PHONE_LEN) { len = MAX_PHONE_LEN; }
        memcpy(out[i], in[i].contact.phone, len);
        out[i][len] = '\0';
    }
}

/* Two phone lists hold the same set (EXPECT_EQ on sorted vectors). */
static bool same_phone_set(PhoneStr *a, PhoneStr *b, int n)
{
    qsort(a, (size_t)n, sizeof(PhoneStr), phone_cmp);
    qsort(b, (size_t)n, sizeof(PhoneStr), phone_cmp);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(a[i], b[i]) != 0) { return false; }
    }
    return true;
}
#endif /* DB_TEST_WRITE */

/* ------------------------------------------------------------------ *
 *  Fixture
 * ------------------------------------------------------------------ */
static bool storage_bringup(void)
{
    if (!SDStorage_Init(&g_sd_ctx, DB_STORAGE_START_SECTOR, DB_STORAGE_BLOCK_SIZE))
    {
        return false;
    }
    storage->context = (void *)&g_sd_ctx;
    return true;
}

#if DB_TEST_WRITE
/* HashTableTest::SetUp() */
static bool fixture_setup(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    /* check_usage_bit()/update_usage_bit() use the global in-RAM bitmap, so
     * clear it between tests. Formatting it on the card also gives every
     * bitmap sector a valid CRC trailer for rebuild(). */
    if (!init_usage_bitmap(storage)) { return false; }

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))           { return false; }
    if (!free_list_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE)) { return false; }

    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);
    return true;
}
#endif /* DB_TEST_WRITE */

/* HashTableTest::TearDown() */
static void fixture_teardown(void)
{
    hash_clear(&g_table);
}

/*
 * HashTableTest::rebuild(): simulate a power-cycle by starting a fresh,
 * empty table over the SAME storage, reloading the usage bitmap from the
 * card and reconstructing the contacts. The message allocator is kept, as
 * in the gtest fixture.
 */
static bool rebuild(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    /* Reconstruction starts from an allocator with nothing free and frees
     * back the slots the usage bitmap says are unused. */
    if (!free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))
    {
        return false;
    }

    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    /* Drop the in-RAM bitmap and reload it from storage, as a real boot would. */
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    if (!read_usage_bitmap(storage))
    {
        return false;
    }

    return hash_reconstruct_contact(&g_table);
}

#if DB_TEST_WRITE
/* ================================================================== *
 *  hash_phone()
 * ================================================================== */

static void test_PhoneHashIgnoresNonDigits(void)
{
    CHECK(hash_phone("0412345678") == hash_phone("0412 345 678"));
    CHECK(hash_phone("0412345678") == hash_phone("(04) 1234-5678"));
    CHECK(hash_phone("0412345678") != hash_phone("0412345679"));
}

/* ================================================================== *
 *  Contacts: insert / find / remove
 * ================================================================== */

static void test_InsertContact(void)
{
    CHECK(insert("Alice", "0412345678"));
    CHECK(hash_size(&g_table) == 1u);
}

static void test_FindContact(void)
{
    ContactBuffer original = make_contact("Alice", "0412345678");
    CHECK(hash_insert_contact(&g_table, &g_journal, &original));

    ContactBuffer result;
    memset(&result, 0, sizeof(result));
    CHECK(hash_find_contact(&g_table, "0412345678", &result));

    CHECK(result.contact.name_len == original.contact.name_len);
    CHECK(result.contact.phone_len == original.contact.phone_len);
    CHECK(NAME_IS(result, "Alice"));
    CHECK(PHONE_IS(result, "0412345678"));
}

static void test_FindMissingContact(void)
{
    CHECK(insert("Alice", "0412345678"));

    ContactBuffer result;
    CHECK(!hash_find_contact(&g_table, "0400000000", &result));
}

static void test_RemoveContact(void)
{
    CHECK(insert("Alice", "0412345678"));
    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer removed;
    memset(&removed, 0, sizeof(removed));
    CHECK(hash_remove_contact(&g_table, &g_journal, "0412345678", &removed));

    CHECK(NAME_IS(removed, "Alice"));
    CHECK(PHONE_IS(removed, "0412345678"));
    CHECK(hash_size(&g_table) == 0u);

    ContactBuffer result;
    CHECK(!hash_find_contact(&g_table, "0412345678", &result));
}

static void test_RemoveMissingContact(void)
{
    ContactBuffer removed;
    CHECK(!hash_remove_contact(&g_table, &g_journal, "0412345678", &removed));
}

static void test_RemoveByPhoneRemovesContactAndMessages(void)
{
    CHECK(insert("Alice", "0412345678"));
    CHECK(send("0412345678", 100, true, "hi"));

    HashEntry *removed = NULL;
    CHECK(hash_remove(&g_table, &g_journal, "0412345678", &removed));
    CHECK(removed != NULL);
    CHECK(removed->state == ENTRY_DELETED);
    CHECK(hash_size(&g_table) == 0u);

    ContactBuffer c_result;
    CHECK(!hash_find_contact(&g_table, "0412345678", &c_result));

    MessageBuffer m_result;
    CHECK(!hash_find_message(&g_table, "0412345678", &m_result));
}

static void test_RemoveByPhoneMissingFails(void)
{
    HashEntry *removed = NULL;
    CHECK(!hash_remove(&g_table, &g_journal, "0412345678", &removed));
    CHECK(removed == NULL);
}

static void test_MultipleContacts(void)
{
    CHECK(insert("Alice", "0411111111"));
    CHECK(insert("Bob", "0422222222"));
    CHECK(insert("Charlie", "0433333333"));

    CHECK(hash_size(&g_table) == 3u);

    ContactBuffer result;

    CHECK(hash_find_contact(&g_table, "0411111111", &result));
    CHECK(NAME_IS(result, "Alice"));

    CHECK(hash_find_contact(&g_table, "0422222222", &result));
    CHECK(NAME_IS(result, "Bob"));

    CHECK(hash_find_contact(&g_table, "0433333333", &result));
    CHECK(NAME_IS(result, "Charlie"));
}

static void test_DuplicateInsertUpdatesInPlace(void)
{
    CHECK(insert("Alice", "0412345678"));
    uint16_t first_sector = sector_for("0412345678");

    CHECK(insert("Alice Updated", "0412345678"));
    uint16_t second_sector = sector_for("0412345678");

    /* Same phone -> same slot/sector, no extra contact. */
    CHECK(second_sector == first_sector);
    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer result;
    CHECK(hash_find_contact(&g_table, "0412345678", &result));
    CHECK(NAME_IS(result, "Alice Updated"));
}

/* "0400000601" and "0400002060" both hash to the same value via hash_phone(). */
static void test_PhoneHashCollisionResolved(void)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    CHECK(hash_phone(phone_a) == hash_phone(phone_b));

    CHECK(insert("Collide A", phone_a));
    CHECK(insert("Collide B", phone_b));

    CHECK(sector_for(phone_a) != sector_for(phone_b));
    CHECK(hash_size(&g_table) == 2u);

    ContactBuffer result;

    CHECK(hash_find_contact(&g_table, phone_a, &result));
    CHECK(NAME_IS(result, "Collide A"));
    CHECK(PHONE_IS(result, phone_a));

    CHECK(hash_find_contact(&g_table, phone_b, &result));
    CHECK(NAME_IS(result, "Collide B"));
    CHECK(PHONE_IS(result, phone_b));
}

static void test_RemoveFromCollisionChain(void)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    CHECK(hash_phone(phone_a) == hash_phone(phone_b));

    CHECK(insert("Collide A", phone_a));
    CHECK(insert("Collide B", phone_b));

    ContactBuffer removed;
    CHECK(hash_remove_contact(&g_table, &g_journal, phone_a, &removed));
    CHECK(NAME_IS(removed, "Collide A"));

    ContactBuffer result;

    /* The other colliding contact is still reachable past the tombstone. */
    CHECK(hash_find_contact(&g_table, phone_b, &result));
    CHECK(NAME_IS(result, "Collide B"));

    /* The removed one is gone. */
    CHECK(!hash_find_contact(&g_table, phone_a, &result));
}

static void test_ReinsertAfterRemove(void)
{
    CHECK(insert("Alice", "0412345678"));

    ContactBuffer removed;
    CHECK(hash_remove_contact(&g_table, &g_journal, "0412345678", &removed));
    CHECK(hash_size(&g_table) == 0u);

    CHECK(insert("Alice Again", "0412345678"));
    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer result;
    CHECK(hash_find_contact(&g_table, "0412345678", &result));
    CHECK(NAME_IS(result, "Alice Again"));
}

static void test_SizeTracksContacts(void)
{
    CHECK(hash_size(&g_table) == 0u);

    CHECK(insert("Alice", "0411111111"));
    CHECK(hash_size(&g_table) == 1u);

    CHECK(insert("Bob", "0422222222"));
    CHECK(hash_size(&g_table) == 2u);

    ContactBuffer removed;
    CHECK(hash_remove_contact(&g_table, &g_journal, "0411111111", &removed));
    CHECK(hash_size(&g_table) == 1u);
}

static void test_RejectsBadArguments(void)
{
    ContactBuffer c = make_contact("Alice", "0412345678");
    ContactBuffer out;

    CHECK(!hash_insert_contact(NULL, &g_journal, &c));
    CHECK(!hash_insert_contact(&g_table, &g_journal, NULL));

    CHECK(!hash_find_contact(NULL, "0412345678", &out));
    CHECK(!hash_find_contact(&g_table, NULL, &out));

    CHECK(!hash_remove_contact(&g_table, &g_journal, NULL, &out));
}

/* ================================================================== *
 *  hash_find_entry()
 * ================================================================== */

static void test_FindEntryMatchesStoredPhone(void)
{
    CHECK(insert("Alice", "0412345678"));
    uint16_t sector = sector_for("0412345678");
    CHECK(sector != UINT16_MAX);

    HashEntry *hit = NULL;
    CHECK(hash_find_entry(&g_table, "0412345678", &hit));
    CHECK(hit != NULL);
    CHECK(hit->state == ENTRY_OCCUPIED);
    CHECK(hit->sector == sector);
    CHECK(hit->id == hash_phone("0412345678"));

    HashEntry *miss = NULL;
    CHECK(hash_find_entry(&g_table, "0400000000", &miss));
    CHECK(miss != NULL);
    CHECK(miss->state != ENTRY_OCCUPIED); /* an insertion point, not a match */
}

static void test_FindEntryDistinguishesCollidingPhones(void)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    CHECK(hash_phone(phone_a) == hash_phone(phone_b));

    CHECK(insert("Collide A", phone_a));
    CHECK(insert("Collide B", phone_b));

    HashEntry *ea = NULL;
    HashEntry *eb = NULL;
    CHECK(hash_find_entry(&g_table, phone_a, &ea));
    CHECK(hash_find_entry(&g_table, phone_b, &eb));

    CHECK(ea->state == ENTRY_OCCUPIED);
    CHECK(eb->state == ENTRY_OCCUPIED);
    CHECK(ea != eb);
}

/* ================================================================== *
 *  create_message()
 * ================================================================== */

static void test_CreateMessageRejectsBadArguments(void)
{
    MessageBuffer empty;
    memset(&empty, 0, sizeof(empty));

    MessageBuffer zero_ts = create_message(0, true, (char *)"hello");
    CHECK(memcmp(zero_ts.buffer, empty.buffer, sizeof(MessageBuffer)) == 0);

    MessageBuffer null_str = create_message(100, true, NULL);
    CHECK(memcmp(null_str.buffer, empty.buffer, sizeof(MessageBuffer)) == 0);
}

static void test_CreateMessageStoresContent(void)
{
    MessageBuffer msg = create_message(1234, true, (char *)"hello");

    CHECK(msg.msg.timestamp == 1234);
    CHECK(msg.msg.direction);
    CHECK(TEXT_IS(msg, "hello"));
}

static void test_CreateMessageTruncatesOverlongBody(void)
{
    char long_body[SMS_MAX_MESSAGE_LENGTH + 50 + 1];
    memset(long_body, 'x', sizeof(long_body) - 1);
    long_body[sizeof(long_body) - 1] = '\0';

    MessageBuffer msg = create_message(1, false, long_body);

    CHECK(strnlen(msg.msg.str, SMS_MAX_MESSAGE_LENGTH) == (size_t)(SMS_MAX_MESSAGE_LENGTH - 1));
}

/* ================================================================== *
 *  Messages: insert / find / remove
 * ================================================================== */

static void test_InsertFirstMessageCreatesContact(void)
{
    CHECK(send("0412345678", 100, true, "hello"));
    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer contact;
    memset(&contact, 0, sizeof(contact));
    CHECK(hash_find_contact(&g_table, "0412345678", &contact));
    CHECK(PHONE_IS(contact, "0412345678"));

    MessageBuffer result;
    memset(&result, 0, sizeof(result));
    CHECK(hash_find_message(&g_table, "0412345678", &result));
    CHECK(result.msg.timestamp == 100);
    CHECK(TEXT_IS(result, "hello"));
}

static void test_InsertMessageAttachesToExistingContact(void)
{
    CHECK(insert("Alice", "0412345678"));
    CHECK(send("0412345678", 100, true, "hello"));

    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer contact;
    CHECK(hash_find_contact(&g_table, "0412345678", &contact));
    CHECK(NAME_IS(contact, "Alice"));
}

static void test_FindMessageReturnsLatest(void)
{
    CHECK(send("0412345678", 100, true, "first"));
    CHECK(send("0412345678", 200, false, "second"));

    MessageBuffer result;
    memset(&result, 0, sizeof(result));
    CHECK(hash_find_message(&g_table, "0412345678", &result));
    CHECK(result.msg.timestamp == 200);
    CHECK(TEXT_IS(result, "second"));
    CHECK(!result.msg.direction);
}

static void test_FindMessageMissingContactFails(void)
{
    MessageBuffer result;
    CHECK(!hash_find_message(&g_table, "0400000000", &result));
}

static void test_FindNMessagesWithinOneSector(void)
{
    CHECK(send("0412345678", 100, true, "first"));
    CHECK(send("0412345678", 200, false, "second"));

    MessageBuffer results[2];
    memset(results, 0, sizeof(results));
    int n = hash_find_n_message(&g_table, "0412345678", 2, results);

    CHECK(n == 2);
    CHECK(results[0].msg.timestamp == 200);
    CHECK(TEXT_IS(results[0], "second"));
    CHECK(results[1].msg.timestamp == 100);
    CHECK(TEXT_IS(results[1], "first"));
}

/* MESSAGE_BLOCK_CAPACITY is small (computed from a 512B sector), so
 * capacity + 1 messages is enough to force a rollover. */
static void test_MessageChatRollsOverToNewSector(void)
{
    char text[16];

    for (int i = 0; i < (int)MESSAGE_BLOCK_CAPACITY + 1; i++)
    {
        snprintf(text, sizeof(text), "msg %d", i);
        CHECK(send("0412345678", (uint16_t)(100 + i), true, text));
    }

    MessageBuffer latest;
    memset(&latest, 0, sizeof(latest));
    CHECK(hash_find_message(&g_table, "0412345678", &latest));
    CHECK(latest.msg.timestamp == 100 + MESSAGE_BLOCK_CAPACITY);
}

static void test_FindNMessagesAcrossSectorBoundary(void)
{
    const int total = (int)MESSAGE_BLOCK_CAPACITY + 1;
    char text[16];

    for (int i = 0; i < total; i++)
    {
        snprintf(text, sizeof(text), "msg %d", i);
        CHECK(send("0412345678", (uint16_t)(100 + i), true, text));
    }

    MessageBuffer results[MESSAGE_BLOCK_CAPACITY + 1];
    memset(results, 0, sizeof(results));
    int n = hash_find_n_message(&g_table, "0412345678", total, results);

    CHECK(n == total);
    for (int i = 0; i < total; i++)
    {
        /* Newest first: message (total - 1 - i) was the i-th most recent. */
        uint16_t expected_ts = (uint16_t)(100 + (total - 1 - i));
        CHECK(results[i].msg.timestamp == expected_ts);
    }
}

static void test_RemoveMessageChatClearsMessagesOnly(void)
{
    CHECK(insert("Alice", "0412345678"));
    CHECK(send("0412345678", 100, true, "hello"));

    MessageBuffer out;
    CHECK(hash_remove_message(&g_table, &g_journal, "0412345678", &out));

    MessageBuffer result;
    CHECK(!hash_find_message(&g_table, "0412345678", &result));

    /* Contact itself must still be present. */
    ContactBuffer contact;
    CHECK(hash_find_contact(&g_table, "0412345678", &contact));
    CHECK(NAME_IS(contact, "Alice"));
}

static void test_RemoveMessageChatAcrossMultipleSectors(void)
{
    const int total = (int)MESSAGE_BLOCK_CAPACITY + 1;
    char text[16];

    for (int i = 0; i < total; i++)
    {
        snprintf(text, sizeof(text), "msg %d", i);
        CHECK(send("0412345678", (uint16_t)(100 + i), true, text));
    }

    MessageBuffer out;
    CHECK(hash_remove_message(&g_table, &g_journal, "0412345678", &out));

    MessageBuffer result;
    CHECK(!hash_find_message(&g_table, "0412345678", &result));
}

static void test_RemoveMessageChatMissingContactFails(void)
{
    MessageBuffer out;
    CHECK(!hash_remove_message(&g_table, &g_journal, "0400000000", &out));
}

static void test_RejectsBadMessageArguments(void)
{
    MessageBuffer m = create_message(1, true, (char *)"hi");

    CHECK(!hash_insert_message(NULL, &g_journal, "0412345678", &m));
    CHECK(!hash_insert_message(&g_table, &g_journal, NULL, &m));
    CHECK(!hash_insert_message(&g_table, &g_journal, "0412345678", NULL));
}

/* ================================================================== *
 *  hash_reconstruct_contact()
 * ================================================================== */
#endif /* DB_TEST_WRITE */

typedef struct
{
    const char *name;
    const char *phone;
} Person;

static const Person g_people[] = {
    { "Alice",   "0411111111" },
    { "Bob",     "0422222222" },
    { "Charlie", "0433333333" },
    { "Dana",    "0444444444" },
    { "Erin",    "0455555555" },
};
#define N_PEOPLE (sizeof(g_people) / sizeof(g_people[0]))

#if DB_TEST_WRITE
static void test_ReconstructFindsAllContacts(void)
{
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        CHECK(insert(g_people[i].name, g_people[i].phone));
    }

    CHECK(rebuild());

    CHECK(hash_size(&g_table) == N_PEOPLE);

    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        ContactBuffer result;
        memset(&result, 0, sizeof(result));
        CHECK(hash_find_contact(&g_table, g_people[i].phone, &result));
        CHECK(NAME_IS(result, g_people[i].name));
        CHECK(PHONE_IS(result, g_people[i].phone));
    }
}

static void test_ReconstructEmptyDatabase(void)
{
    CHECK(rebuild());
    CHECK(hash_size(&g_table) == 0u);

    ContactBuffer result;
    CHECK(!hash_find_contact(&g_table, "0412345678", &result));
}

static void test_ReconstructSkipsRemovedContacts(void)
{
    CHECK(insert("Alice", "0411111111"));
    CHECK(insert("Bob", "0422222222"));
    CHECK(insert("Charlie", "0433333333"));

    ContactBuffer removed;
    CHECK(hash_remove_contact(&g_table, &g_journal, "0422222222", &removed));

    CHECK(rebuild());

    CHECK(hash_size(&g_table) == 2u);

    ContactBuffer result;
    CHECK(hash_find_contact(&g_table, "0411111111", &result));
    CHECK(hash_find_contact(&g_table, "0433333333", &result));
    CHECK(!hash_find_contact(&g_table, "0422222222", &result));
}

static void test_ReconstructPreservesCollisionChain(void)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    CHECK(hash_phone(phone_a) == hash_phone(phone_b));

    CHECK(insert("Collide A", phone_a));
    CHECK(insert("Collide B", phone_b));

    CHECK(rebuild());

    CHECK(hash_size(&g_table) == 2u);

    ContactBuffer result;
    CHECK(hash_find_contact(&g_table, phone_a, &result));
    CHECK(NAME_IS(result, "Collide A"));
    CHECK(hash_find_contact(&g_table, phone_b, &result));
    CHECK(NAME_IS(result, "Collide B"));
}

static void test_ReconstructedTableAcceptsNewWrites(void)
{
    CHECK(insert("Alice", "0411111111"));

    CHECK(rebuild());

    CHECK(hash_size(&g_table) == 1u);

    ContactBuffer bob = make_contact("Bob", "0422222222");
    CHECK(hash_insert_contact(&g_table, &g_journal, &bob));
    CHECK(hash_size(&g_table) == 2u);

    ContactBuffer result;
    CHECK(hash_find_contact(&g_table, "0422222222", &result));

    ContactBuffer removed;
    memset(&removed, 0, sizeof(removed));
    CHECK(hash_remove_contact(&g_table, &g_journal, "0411111111", &removed));
    CHECK(NAME_IS(removed, "Alice"));
    CHECK(hash_size(&g_table) == 1u);
}

/* ================================================================== *
 *  hash_get_contact_list_by_usage() / hash_get_contact_list()
 * ================================================================== */

typedef STRG_RET (*ContactListFn)(HashTable *table, int start, int n, ContactBuffer *out);

/* Insert ten contacts "04" + 8 x digit i and check one 10-contact page
 * returns exactly that set. */
static void check_first_ten(ContactListFn list)
{
    PhoneStr phones[10];
    PhoneStr returned[10];
    ContactBuffer results[10];
    char name[16];

    for (int i = 0; i < 10; i++)
    {
        snprintf(phones[i], sizeof(phones[i]), "04%c%c%c%c%c%c%c%c",
                 '0' + i, '0' + i, '0' + i, '0' + i, '0' + i, '0' + i, '0' + i, '0' + i);
        snprintf(name, sizeof(name), "Contact %d", i);
        CHECK(insert(name, phones[i]));
    }

    memset(results, 0, sizeof(results));
    CHECK(list(&g_table, 0, 10, results) == STRG_OK);

    collect_phones(results, 10, returned);
    CHECK(same_phone_set(phones, returned, 10));
}

/* Insert twenty contacts and check the pages at 0 and 10 partition them:
 * no duplicates between the pages and nothing missing. */
static void check_second_ten(ContactListFn list)
{
    PhoneStr phones[20];
    PhoneStr returned[20];
    ContactBuffer pages[20];
    char name[16];

    for (int i = 0; i < 20; i++)
    {
        numbered_phone(phones[i], sizeof(phones[i]), i);
        snprintf(name, sizeof(name), "Contact %d", i);
        CHECK(insert(name, phones[i]));
    }

    memset(pages, 0, sizeof(pages));
    CHECK(list(&g_table, 0, 10, &pages[0]) == STRG_OK);
    CHECK(list(&g_table, 10, 10, &pages[10]) == STRG_OK);

    collect_phones(pages, 20, returned);
    CHECK(same_phone_set(phones, returned, 20));
}

static void test_GetContactListReturnsFirstTenContacts(void)      { check_first_ten(hash_get_contact_list_by_usage); }
static void test_GetContactListReturnsSecondTenContacts(void)     { check_second_ten(hash_get_contact_list_by_usage); }
static void test_GetContactListIterReturnsFirstTenContacts(void)  { check_first_ten(hash_get_contact_list); }
static void test_GetContactListIterReturnsSecondTenContacts(void) { check_second_ten(hash_get_contact_list); }
#endif /* DB_TEST_WRITE */

/* ================================================================== *
 *  Persistence across a real power-cycle (DB_TEST_WRITE toggle)
 * ================================================================== */

/* Chat left on the card for Charlie: forces a second linked sector. */
#define PERSIST_CHAT_PHONE "0433333333"
#define PERSIST_CHAT_LEN   ((int)MESSAGE_BLOCK_CAPACITY + 1)

#if DB_TEST_WRITE

/* Leave g_people plus a multi-sector chat on the card for a later
 * DB_TEST_WRITE=0 run. Runs last; the fixture's teardown only clears RAM. */
static void test_PersistSeed(void)
{
    char text[16];

    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        CHECK(insert(g_people[i].name, g_people[i].phone));
    }

    for (int i = 0; i < PERSIST_CHAT_LEN; i++)
    {
        snprintf(text, sizeof(text), "msg %d", i);
        CHECK(send(PERSIST_CHAT_PHONE, (uint16_t)(1 + i), true, text));
    }
}

#else /* DB_TEST_WRITE == 0 */

/* Rebuild contacts AND chats purely from what PersistSeed left on the card. */
static void test_PersistVerify(void)
{
    MessageBuffer latest;

    CHECK(free_list_empty_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE));
    CHECK(rebuild());
    CHECK(hash_reconstruct_message(&g_table, &g_journal));

    CHECK(hash_size(&g_table) == N_PEOPLE);

    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        ContactBuffer result;
        memset(&result, 0, sizeof(result));
        CHECK(hash_find_contact(&g_table, g_people[i].phone, &result));
        CHECK(NAME_IS(result, g_people[i].name));
    }

    memset(&latest, 0, sizeof(latest));
    CHECK(hash_find_message(&g_table, PERSIST_CHAT_PHONE, &latest));
    CHECK(latest.msg.timestamp == PERSIST_CHAT_LEN);

    MessageBuffer chat[PERSIST_CHAT_LEN];
    char expected[16];
    memset(chat, 0, sizeof(chat));
    CHECK(hash_find_n_message(&g_table, PERSIST_CHAT_PHONE, PERSIST_CHAT_LEN, chat) == PERSIST_CHAT_LEN);
    for (int i = 0; i < PERSIST_CHAT_LEN; i++)
    {
        /* Newest first; PersistSeed sent "msg k" with timestamp k + 1. */
        uint16_t ts = (uint16_t)(PERSIST_CHAT_LEN - i);
        snprintf(expected, sizeof(expected), "msg %d", ts - 1);
        CHECK(chat[i].msg.timestamp == ts);
        CHECK(TEXT_IS(chat[i], expected));
    }

    CHECK(!hash_find_message(&g_table, g_people[0].phone, &latest));
}

#endif /* DB_TEST_WRITE */

/* ------------------------------------------------------------------ *
 *  Test list (same order as test_hash_table.cpp)
 * ------------------------------------------------------------------ */
#define TEST(fn) { #fn, test_##fn }

static const TestCase g_tests[] = {
#if DB_TEST_WRITE
    TEST(PhoneHashIgnoresNonDigits),
    TEST(InsertContact),
    TEST(FindContact),
    TEST(FindMissingContact),
    TEST(RemoveContact),
    TEST(RemoveMissingContact),
    TEST(RemoveByPhoneRemovesContactAndMessages),
    TEST(RemoveByPhoneMissingFails),
    TEST(MultipleContacts),
    TEST(DuplicateInsertUpdatesInPlace),
    TEST(PhoneHashCollisionResolved),
    TEST(RemoveFromCollisionChain),
    TEST(ReinsertAfterRemove),
    TEST(SizeTracksContacts),
    TEST(RejectsBadArguments),
    TEST(FindEntryMatchesStoredPhone),
    TEST(FindEntryDistinguishesCollidingPhones),
    TEST(CreateMessageRejectsBadArguments),
    TEST(CreateMessageStoresContent),
    TEST(CreateMessageTruncatesOverlongBody),
    TEST(InsertFirstMessageCreatesContact),
    TEST(InsertMessageAttachesToExistingContact),
    TEST(FindMessageReturnsLatest),
    TEST(FindMessageMissingContactFails),
    TEST(FindNMessagesWithinOneSector),
    TEST(MessageChatRollsOverToNewSector),
    TEST(FindNMessagesAcrossSectorBoundary),
    TEST(RemoveMessageChatClearsMessagesOnly),
    TEST(RemoveMessageChatAcrossMultipleSectors),
    TEST(RemoveMessageChatMissingContactFails),
    TEST(RejectsBadMessageArguments),
    TEST(ReconstructFindsAllContacts),
    TEST(ReconstructEmptyDatabase),
    TEST(ReconstructSkipsRemovedContacts),
    TEST(ReconstructPreservesCollisionChain),
    TEST(ReconstructedTableAcceptsNewWrites),
    TEST(GetContactListReturnsFirstTenContacts),
    TEST(GetContactListReturnsSecondTenContacts),
    TEST(GetContactListIterReturnsFirstTenContacts),
    TEST(GetContactListIterReturnsSecondTenContacts),
    TEST(PersistSeed),
#else
    TEST(PersistVerify),
#endif
};
#define N_TESTS (sizeof(g_tests) / sizeof(g_tests[0]))

_Static_assert(N_TESTS <= DB_MAX_TESTS, "raise DB_MAX_TESTS");

/* ------------------------------------------------------------------ *
 *  Runner
 * ------------------------------------------------------------------ */
static void run_test(size_t i)
{
    uint32_t start = osKernelGetTickCount();

    g_fail_line = 0;
    g_db_report.current = (uint32_t)(i + 1);
    htest_ui_start((uint32_t)i, g_tests[i].name);

#if DB_TEST_WRITE
    bool ready = fixture_setup();
#else
    /* journal_init() only validates here (it may roll back an interrupted
     * write from the previous run, which is the correct recovery). No
     * bitmap format, no writes. */
    memset(&g_journal, 0, sizeof(g_journal));
    bool ready = journal_init(&g_journal, storage);
#endif

    if (!ready)
    {
        g_fail_line = __LINE__;
    }
    else
    {
        g_tests[i].fn();
        fixture_teardown();
    }

    g_db_report.ms[i] = osKernelGetTickCount() - start;
    g_db_report.fail_line[i] = (uint16_t)g_fail_line;
    htest_ui_result((uint32_t)i, g_fail_line == 0, (uint32_t)g_fail_line);

    if (g_fail_line == 0)
    {
        g_db_report.status[i] = TEST_PASS;
        g_db_report.passed++;
    }
    else
    {
        g_db_report.status[i] = TEST_FAIL;
        g_db_report.failed++;
        if (g_db_report.first_failed == 0)
        {
            g_db_report.first_failed = (uint32_t)(i + 1);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  LED reporting (PB0)
 * ------------------------------------------------------------------ */
static void blink_forever(uint32_t code)
{
    if (code == 0)
    {
        for (;;)
        {
            GPIOB->ODR ^= (1U << 0);
            osDelay(150);
        }
    }

    for (;;)
    {
        for (uint32_t i = 0; i < code; i++)
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

    memset((void *)&g_db_report, 0, sizeof(g_db_report));
    g_db_report.total = (uint32_t)N_TESTS;
    g_db_report.magic = DB_REPORT_MAGIC;

    htest_ui_init((uint32_t)N_TESTS);

    if (!storage_bringup())
    {
        /* Nothing can run without the card: report it as test 1 failing. */
        g_db_report.first_failed = 1;
        g_db_report.failed = 1;
        g_db_report.status[0] = TEST_FAIL;
        g_db_report.fail_line[0] = (uint16_t)__LINE__;
        htest_ui_start(0, "SD card init");
        htest_ui_result(0, false, g_db_report.fail_line[0]);
    }
    else
    {
        for (size_t i = 0; i < N_TESTS; i++)
        {
            run_test(i);
        }
    }

    g_db_report.current = 0;
    g_db_report.done = 1;
    htest_ui_done(g_db_report.passed, g_db_report.failed);

    blink_forever(g_db_report.first_failed);
}

static StaticTask_t g_db_task_cb;
static uint8_t g_db_task_stack[DATABASE_TASK_STACK_SIZE] __attribute__((aligned(8)));

void DB_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "Database Task",
        .cb_mem = &g_db_task_cb,
        .cb_size = sizeof(g_db_task_cb),
        .stack_mem = g_db_task_stack,
        .stack_size = sizeof(g_db_task_stack),
        .priority = DATABASE_TASK_PRIORITY};

    osThreadNew(dbTask, NULL, &task_attr);
}
