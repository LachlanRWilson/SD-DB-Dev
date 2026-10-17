/*
 * database_main.c - on-hardware unit tests for the high level database API
 * (database.c).
 *
 * Firmware-side port of tests/test_database.cpp (the DatabaseTest suite):
 * the test_*() functions here have the same names and checks as the TEST_Fs
 * there, run against the physical SD card (sd_storage.c) through
 * database_init() instead of the heap-backed mock.
 *
 * Differences from the gtest suite (all forced by the target, not the logic):
 *   - The suite setup mounts the card (database_init formats it if blank, and
 *     database_format is run if the mount fails), instead of a
 *     fresh heap buffer per test. A full format clears ~10k ring buffer
 *     sectors, which is too slow to repeat, so the per-test fixture only re-writes the usage
 *     bitmap, the journal header and the first DB_RING_CLEAR_SECTORS of each
 *     ring buffer, then mounts with database_init(). Old contact/message
 *     sectors are left behind; nothing reads a sector the bitmap marks unused.
 *   - CapacityLimit becomes CapacityRebuild: DB_CAPACITY_N (1000) contacts
 *     instead of HASH_TABLE_ENTRIES (10,000), which would take too long over
 *     SD. The full-index rejection is checked by NameIndexFullRollsBack, which
 *     marks the name index as full instead of filling it.
 *   - The free list rebuild checks (from ContactIndexRecoveryTest) run through
 *     the database API and use bitmaps instead of std::set.
 *   - FormatAfterCorruptSuperheader is dropped (the format path runs on a blank card).
 *   - A failed check ends that test (gtest's EXPECT_* would carry on).
 *
 * RAM: database.c's hash table, free lists and name index (~293KB) live in
 * RAM_D1. db_main.c's buffers are dropped by --gc-sections because DB_Init()
 * is not called in a DATABASE_TEST build, so they fit.
 *
 * ------------------------------------------------------------------------
 *  Reporting
 * ------------------------------------------------------------------------
 *   The LCD shows each test as it runs and the summary at the end
 *   (htest_ui.c). A failure shows the database_main.c line of the failed
 *   check. g_db_api_report (below) has the same results and can be read over
 *   SWD without halting the core, e.g.
 *
 *     STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <&g_db_api_report> 0x200
 *
 *   Build with -DDATABASE_TEST=ON to run this instead of db_main.c. It
 *   formats the card, so it wipes any other test or demo data set.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "database.h"
#include "free_list_stack.h"
#include "superheader.h"
#include "usage_bitmap.h"
#include "ring_buffer.h"
#include "storage.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "htest_ui.h"
#include "msg_ts.h"
#include "database_main.h"

#define DB_TASK_STACK_SIZE (12 * 1024)
#define DB_TASK_PRIORITY   osPriorityNormal

/* Contacts in CapacityRebuild */
#define DB_CAPACITY_N 1000

/* Ring buffer sectors the fixture clears (more than any single test writes) */
#define DB_RING_CLEAR_SECTORS 32

/* Contacts read per database_contact_list call by the list helpers */
#define DB_LIST_CHUNK 8

#define DB_MAX_TESTS    64
#define DB_REPORT_MAGIC 0x44427E57u

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
    uint32_t format_ms;                /* run time of the suite setup mount (or format) */
    uint8_t  status[DB_MAX_TESTS];     /* TestStatus per test                          */
    uint16_t fail_line[DB_MAX_TESTS];  /* database_main.c line of the failed check     */
    uint32_t ms[DB_MAX_TESTS];         /* run time per test                            */
} DbApiTestReport;

volatile DbApiTestReport g_db_api_report;

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */
static Storage *const storage = &sd_storage;

static SDStorageContext g_sd_ctx;
static Database         g_db;

/* Scratch buffers (kept off the task stack) */
static ContactBuffer       g_contacts[DB_LIST_CHUNK];
static Message             g_messages[16];
static MessageHistoryEntry g_history[8];
static CallHistoryEntry    g_calls[8];

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
 *  Power cut (mirrors PowerCut_WriteBlock in test_database.cpp)
 * ------------------------------------------------------------------ */
static int g_writes_left = -1; /* -1 = never fail */

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
    return SDStorage_WriteBlock(context, index, in);
}

static bool PowerCut_WriteMultiBlock(void *context, uint32_t index, size_t writeNum, uint8_t *in)
{
    for (size_t i = 0; i < writeNum; i++)
    {
        if (!PowerCut_WriteBlock(context, index + i, in + (i * SECTOR_SIZE)))
        {
            return false;
        }
    }
    return true;
}

/* Make the nth write from now (1 based) and every write after it fail. */
static void cut_power_after(int writes)
{
    g_db.storage.write_block = PowerCut_WriteBlock;
    g_db.storage.write_multiblock = PowerCut_WriteMultiBlock;
    g_writes_left = writes;
}

/* ------------------------------------------------------------------ *
 *  Fixture (mirrors DatabaseTest)
 * ------------------------------------------------------------------ */
static bool storage_bringup(void)
{
    if (!SDStorage_Init(&g_sd_ctx, 0, SECTOR_SIZE))
    {
        return false;
    }
    storage->context = (void *)&g_sd_ctx;
    return true;
}

/* DatabaseTest::SetUp(): an empty database, mounted from the card. */
static bool fixture_setup(void)
{
    Journal journal;
    STRG_RET ret;

    g_writes_left = -1;

    // quick wipe: everything a mount reads, without the full ring buffer clear
    if (!init_usage_bitmap(storage)) { return false; }

    memset(&journal, 0, sizeof(journal));
    journal.storage = storage;
    if (!journal_header_init(&journal)) { return false; }

    if (clear_ring_buffer(storage, DATA_SECTOR_TO_RAW(MESSAGE_HISTORY_DATA_START_SECTOR), DB_RING_CLEAR_SECTORS) != STRG_OK) { return false; }
    if (clear_ring_buffer(storage, DATA_SECTOR_TO_RAW(CALL_HISTORY_DATA_START_SECTOR), DB_RING_CLEAR_SECTORS) != STRG_OK)    { return false; }

    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    ret = database_init(&g_db);

    // superheader left bad by a previous test, or a card in an older layout: full format
    if (ret != STRG_OK)
    {
        ret = database_format(&g_db);
    }

    return (ret == STRG_OK) && (database_contact_count(&g_db) == 0);
}

/* DatabaseTest::TearDown() */
static void fixture_teardown(void)
{
    g_writes_left = -1;
}

/* Simulate a reboot: RAM is wiped and the database is mounted from the card. */
static STRG_RET reboot(void)
{
    g_writes_left = -1;
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    return database_init(&g_db);
}

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
static void phone_for(int i, char out[16])
{
    snprintf(out, 16, "04%08d", i);
}

static void name_for(int i, char out[16])
{
    snprintf(out, 16, "Contact%05d", i);
}

/* Contact holds exactly name and phone. */
static bool contact_is(const Contact *c, const char *name, const char *phone)
{
    size_t nl = strlen(name);
    size_t pl = strlen(phone);
    return (c->name_len == nl) && (memcmp(c->name, name, nl) == 0) &&
           (c->phone_len == pl) && (memcmp(c->phone, phone, pl) == 0);
}

/* The full contact list is exactly names[i] / phones[i], in order. */
static bool list_is(const char *const *names, const char *const *phones, size_t n)
{
    size_t start = 0;
    size_t count;

    if (database_contact_count(&g_db) != n)
    {
        return false;
    }

    while (start < n)
    {
        if (database_contact_list(&g_db, start, DB_LIST_CHUNK, &count, g_contacts) != STRG_OK || count == 0)
        {
            return false;
        }
        for (size_t i = 0; (i < count) && (start + i < n); i++)
        {
            if (!contact_is(&g_contacts[i].contact, names[start + i], phones[start + i]))
            {
                return false;
            }
        }
        start += count;
    }

    // nothing past the end
    return database_contact_list(&g_db, n, 1, &count, g_contacts) == STRG_EMPTY;
}

/* Every listed contact can be found by phone, and the list size matches the count. */
static bool list_consistent(void)
{
    size_t start = 0;
    size_t count;
    ContactBuffer found;
    char phone[MAX_PHONE_LEN + 1];

    for (;;)
    {
        STRG_RET ret = database_contact_list(&g_db, start, DB_LIST_CHUNK, &count, g_contacts);
        if (ret == STRG_EMPTY)
        {
            break;
        }
        if (ret != STRG_OK)
        {
            return false;
        }
        for (size_t i = 0; i < count; i++)
        {
            memset(phone, 0, sizeof(phone));
            memcpy(phone, g_contacts[i].contact.phone, g_contacts[i].contact.phone_len);
            if (database_contact_get(&g_db, phone, &found) != STRG_OK)
            {
                return false;
            }
        }
        start += count;
    }

    return start == database_contact_count(&g_db);
}

/* The chat is exactly strs[], newest first. */
static bool chat_is(const char *phone, const char *const *strs, size_t n)
{
    size_t count = 0;

    if (n == 0)
    {
        return database_message_get_chat(&g_db, phone, 0, 1, &count, g_messages) == STRG_EMPTY;
    }

    if (database_message_get_chat(&g_db, phone, 0, 16, &count, g_messages) != STRG_OK || count != n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (strncmp(g_messages[i].str, strs[i], SMS_MAX_MESSAGE_LENGTH) != 0)
        {
            return false;
        }
    }
    return true;
}

/* ================================================================== *
 *  Init / format / mount
 * ================================================================== */

/* A blank card is formatted into an empty database. */
static void test_BlankCardFormatsEmpty(void)
{
    size_t count = 99;

    CHECK(database_contact_count(&g_db) == 0);
    CHECK(database_contact_list(&g_db, 0, 1, &count, g_contacts) == STRG_EMPTY);
    CHECK(count == 0);
    CHECK(database_message_recent(&g_db, 0, 1, &count, g_history) == STRG_OK);
    CHECK(count == 0);
    CHECK(database_call_recent(&g_db, 0, 1, &count, g_calls) == STRG_OK);
    CHECK(count == 0);
}

/* After format, the superheader is valid so the next boot mounts instead of formatting. */
static void test_FormatWritesValidSuperheader(void)
{
    SuperHeaderBuffer sh;
    CHECK(superheader_check(&g_db.storage, &sh) == SUPR_GOOD);
}

/* Mounting an empty database succeeds and stays empty. */
static void test_RebootEmptyDatabase(void)
{
    CHECK(reboot() == STRG_OK);
    CHECK(database_contact_count(&g_db) == 0);
}

/* Contacts, chats and history all survive a reboot. */
static void test_RebootPreservesEverything(void)
{
    static const char *const names[]  = {"alice", "Zed"};
    static const char *const phones[] = {"0400000002", "0400000001"};
    static const char *const msgs[]   = {"two", "one"};
    size_t count = 0;

    CHECK(database_contact_add(&g_db, "Zed", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "alice", "0400000002") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000002", 1, true, (char *)"one") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000002", 2, false, (char *)"two") == STRG_OK);
    CHECK(database_call_add(&g_db, "0400000001") == STRG_OK);

    CHECK(reboot() == STRG_OK);

    CHECK(list_is(names, phones, 2));
    CHECK(chat_is("0400000002", msgs, 2));

    CHECK(database_message_recent(&g_db, 0, 4, &count, g_history) == STRG_OK);
    CHECK(count == 2);
    CHECK(msg_ts(&g_history[0].message) == 2);
    CHECK(msg_ts(&g_history[1].message) == 1);

    CHECK(database_call_recent(&g_db, 0, 4, &count, g_calls) == STRG_OK);
    CHECK(count == 1);
    CHECK(contact_is(&g_calls[0].contact, "Zed", "0400000001"));
}

/* Writes after a reboot continue on from the rebuilt state. */
static void test_WritesAfterRebootAppend(void)
{
    static const char *const msgs[] = {"after", "before"};
    size_t count = 0;

    CHECK(database_message_add(&g_db, "0400000002", 1, true, (char *)"before") == STRG_OK);
    CHECK(reboot() == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000002", 2, true, (char *)"after") == STRG_OK);
    CHECK(database_contact_add(&g_db, "New", "0400000003") == STRG_OK);

    CHECK(chat_is("0400000002", msgs, 2));
    CHECK(database_contact_count(&g_db) == 2);
    CHECK(database_message_recent(&g_db, 0, 4, &count, g_history) == STRG_OK);
    CHECK(count == 2);
}

/* A corrupted superheader is reported and the card is not wiped. */
static void test_CorruptSuperheaderNotWiped(void)
{
    static uint8_t sector[SECTOR_SIZE];

    CHECK(database_contact_add(&g_db, "Ada", "0400000001") == STRG_OK);

    // flip a data byte without restamping the CRC
    CHECK(SDStorage_ReadBlock(&g_sd_ctx, SUPERHEADER_SECTOR, sector));
    sector[8] ^= 0xFF;
    CHECK(SDStorage_WriteBlock(&g_sd_ctx, SUPERHEADER_SECTOR, sector));

    STRG_RET ret = reboot();

    // repair the byte, the contact must still be there
    sector[8] ^= 0xFF;
    CHECK(SDStorage_WriteBlock(&g_sd_ctx, SUPERHEADER_SECTOR, sector));
    CHECK(ret == STRG_CORRUPT);

    CHECK(reboot() == STRG_OK);
    CHECK(database_contact_count(&g_db) == 1);
}

/* An outdated superheader version is reported rather than mounted. */
static void test_OutdatedSuperheaderReported(void)
{
    static SuperHeaderBuffer original;
    static SuperHeaderBuffer patched;

    CHECK(SDStorage_ReadBlock(&g_sd_ctx, SUPERHEADER_SECTOR, original.buffer));
    patched = original;
    patched.var.data.var.version += 1;
    sector_crc_stamp(patched.buffer);
    CHECK(SDStorage_WriteBlock(&g_sd_ctx, SUPERHEADER_SECTOR, patched.buffer));

    STRG_RET ret = reboot();

    CHECK(SDStorage_WriteBlock(&g_sd_ctx, SUPERHEADER_SECTOR, original.buffer));
    CHECK(ret == STRG_CORRUPT);
}

/* A NULL database is rejected. */
static void test_InitNullFails(void)
{
    CHECK(database_init(NULL) == STRG_FAIL);
    CHECK(database_format(NULL) == STRG_FAIL);
}

/* ================================================================== *
 *  Contacts
 * ================================================================== */

/* An added contact can be found by phone number. */
static void test_AddThenGet(void)
{
    ContactBuffer out;
    CHECK(database_contact_add(&g_db, "Ada", "0400000001") == STRG_OK);
    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_OK);
    CHECK(contact_is(&out.contact, "Ada", "0400000001"));
    CHECK(database_contact_count(&g_db) == 1);
}

/* Looking up an unknown phone number returns STRG_EMPTY. */
static void test_GetUnknownIsEmpty(void)
{
    ContactBuffer out;
    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_EMPTY);
}

/* A second contact with the same phone number is rejected and not indexed twice. */
static void test_DuplicatePhoneRejected(void)
{
    static const char *const names[]  = {"Ada"};
    static const char *const phones[] = {"0400000001"};

    CHECK(database_contact_add(&g_db, "Ada", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Other", "0400000001") == STRG_EXISTS);
    CHECK(list_is(names, phones, 1));
}

/* Invalid arguments are rejected without changing the database. */
static void test_AddRejectsInvalidArgs(void)
{
    char long_name[MAX_NAME_LEN + 2];
    char long_phone[MAX_PHONE_LEN + 1]; // must leave room for the terminator

    memset(long_name, 'a', MAX_NAME_LEN + 1);
    long_name[MAX_NAME_LEN + 1] = '\0';
    memset(long_phone, '1', MAX_PHONE_LEN);
    long_phone[MAX_PHONE_LEN] = '\0';

    CHECK(database_contact_add(NULL, "Ada", "0400000001") == STRG_FAIL);
    CHECK(database_contact_add(&g_db, NULL, "0400000001") == STRG_FAIL);
    CHECK(database_contact_add(&g_db, "Ada", NULL) == STRG_FAIL);
    CHECK(database_contact_add(&g_db, "Ada", "") == STRG_FAIL);
    CHECK(database_contact_add(&g_db, long_name, "0400000001") == STRG_FAIL);
    CHECK(database_contact_add(&g_db, "Ada", long_phone) == STRG_FAIL);
    CHECK(database_contact_count(&g_db) == 0);
}

/* The longest allowed name and phone number are accepted and round trip. */
static void test_AddMaxLengthFields(void)
{
    char name[MAX_NAME_LEN + 1];
    char phone[MAX_PHONE_LEN];
    ContactBuffer out;

    memset(name, 'n', MAX_NAME_LEN);
    name[MAX_NAME_LEN] = '\0';
    memset(phone, '9', MAX_PHONE_LEN - 1);
    phone[MAX_PHONE_LEN - 1] = '\0';

    CHECK(database_contact_add(&g_db, name, phone) == STRG_OK);
    CHECK(database_contact_get(&g_db, phone, &out) == STRG_OK);
    CHECK(contact_is(&out.contact, name, phone));
}

/* The list is alphabetical (case insensitive) regardless of insert order. */
static void test_ListIsAlphabetical(void)
{
    static const char *const names[]  = {"alice", "Bob", "Zed"};
    static const char *const phones[] = {"0400000002", "0400000003", "0400000001"};

    CHECK(database_contact_add(&g_db, "Zed", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "alice", "0400000002") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Bob", "0400000003") == STRG_OK);
    CHECK(list_is(names, phones, 3));
}

/* Contacts with the same name are both kept, ordered by phone number. */
static void test_SameNameDifferentPhones(void)
{
    static const char *const names[]  = {"Sam", "Sam"};
    static const char *const phones[] = {"0400000001", "0400000009"};

    CHECK(database_contact_add(&g_db, "Sam", "0400000009") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Sam", "0400000001") == STRG_OK);
    CHECK(list_is(names, phones, 2));
}

/* Names sharing the first 8 characters (the key) are ordered by tiebreak. */
static void test_LongSharedPrefixOrdered(void)
{
    static const char *const names[]  = {"Alexandra Amy", "Alexandra Zed"};
    static const char *const phones[] = {"0400000002", "0400000001"};

    CHECK(database_contact_add(&g_db, "Alexandra Zed", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Alexandra Amy", "0400000002") == STRG_OK);
    CHECK(list_is(names, phones, 2));
}

/* Paging through the list returns the right window and clips at the end. */
static void test_ListPaging(void)
{
    char name[16];
    char phone[16];
    size_t count = 0;

    for (int i = 0; i < 10; i++)
    {
        name_for(i, name);
        phone_for(i, phone);
        CHECK(database_contact_add(&g_db, name, phone) == STRG_OK);
    }

    CHECK(database_contact_list(&g_db, 4, 4, &count, g_contacts) == STRG_OK);
    CHECK(count == 4);
    name_for(4, name);
    phone_for(4, phone);
    CHECK(contact_is(&g_contacts[0].contact, name, phone));

    CHECK(database_contact_list(&g_db, 8, 4, &count, g_contacts) == STRG_OK);
    CHECK(count == 2);

    CHECK(database_contact_list(&g_db, 10, 4, &count, g_contacts) == STRG_EMPTY);
    CHECK(count == 0);
}

/* Seek finds the rank of the first name >= the prefix, ignoring case. */
static void test_SeekPrefix(void)
{
    size_t rank = 99;
    size_t count = 0;

    CHECK(database_contact_add(&g_db, "Adam", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "bella", "0400000002") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Bob", "0400000003") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Carl", "0400000004") == STRG_OK);

    CHECK(database_contact_seek(&g_db, "B", &rank) == STRG_OK);
    CHECK(rank == 1);
    CHECK(database_contact_list(&g_db, rank, 1, &count, g_contacts) == STRG_OK);
    CHECK(contact_is(&g_contacts[0].contact, "bella", "0400000002"));

    CHECK(database_contact_seek(&g_db, "bo", &rank) == STRG_OK);
    CHECK(rank == 2);

    CHECK(database_contact_seek(&g_db, "z", &rank) == STRG_OK);
    CHECK(rank == 4);
}

/* Renaming a contact moves it in the list. */
static void test_UpdateNameReorders(void)
{
    static const char *const names[]  = {"Bob", "Zara"};
    static const char *const phones[] = {"0400000002", "0400000001"};

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Bob", "0400000002") == STRG_OK);
    CHECK(database_contact_update(&g_db, "0400000001", "Zara", "0400000001") == STRG_OK);
    CHECK(list_is(names, phones, 2));

    CHECK(reboot() == STRG_OK);
    CHECK(list_is(names, phones, 2));
}

/* Changing the phone number moves the contact and keeps its chat. */
static void test_UpdatePhoneKeepsChat(void)
{
    static const char *const names[]  = {"Amy"};
    static const char *const phones[] = {"0400000009"};
    static const char *const msgs[]   = {"hello"};
    ContactBuffer out;

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000001", 1, true, (char *)"hello") == STRG_OK);
    CHECK(database_contact_update(&g_db, "0400000001", "Amy", "0400000009") == STRG_OK);

    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_EMPTY);
    CHECK(database_contact_get(&g_db, "0400000009", &out) == STRG_OK);
    CHECK(chat_is("0400000009", msgs, 1));
    CHECK(list_is(names, phones, 1));

    CHECK(reboot() == STRG_OK);
    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_EMPTY);
    CHECK(chat_is("0400000009", msgs, 1));
    CHECK(list_is(names, phones, 1));
}

/* Changing to a phone number already in use is rejected and nothing changes. */
static void test_UpdateToExistingPhoneRejected(void)
{
    static const char *const names[]  = {"Amy", "Bob"};
    static const char *const phones[] = {"0400000001", "0400000002"};

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Bob", "0400000002") == STRG_OK);
    CHECK(database_contact_update(&g_db, "0400000001", "Amy", "0400000002") == STRG_EXISTS);
    CHECK(list_is(names, phones, 2));
}

/* Updating an unknown contact returns STRG_EMPTY. */
static void test_UpdateUnknownIsEmpty(void)
{
    CHECK(database_contact_update(&g_db, "0400000001", "Amy", "0400000001") == STRG_EMPTY);
}

/* Removing a contact drops it from lookup, the list and the count. */
static void test_RemoveContact(void)
{
    static const char *const names[]  = {"Bob"};
    static const char *const phones[] = {"0400000002"};
    ContactBuffer out;

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Bob", "0400000002") == STRG_OK);
    CHECK(database_contact_remove(&g_db, "0400000001") == STRG_OK);

    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_EMPTY);
    CHECK(list_is(names, phones, 1));

    CHECK(reboot() == STRG_OK);
    CHECK(list_is(names, phones, 1));
}

/* Removing one of two same-named contacts removes the right one. */
static void test_RemoveSameNameRemovesRightOne(void)
{
    static const char *const names[]  = {"Sam"};
    static const char *const phones[] = {"0400000001"};

    CHECK(database_contact_add(&g_db, "Sam", "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Sam", "0400000002") == STRG_OK);
    CHECK(database_contact_remove(&g_db, "0400000002") == STRG_OK);
    CHECK(list_is(names, phones, 1));
}

/* Removing a contact deletes its chat, so re-adding the number starts empty. */
static void test_RemoveContactDeletesChat(void)
{
    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000001", 1, true, (char *)"gone") == STRG_OK);
    CHECK(database_contact_remove(&g_db, "0400000001") == STRG_OK);
    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(chat_is("0400000001", NULL, 0));
}

/* Removing an unknown contact returns STRG_EMPTY. */
static void test_RemoveUnknownIsEmpty(void)
{
    CHECK(database_contact_remove(&g_db, "0400000001") == STRG_EMPTY);
}

/* A removed contact's slot is reused, and the count stays correct. */
static void test_AddRemoveChurn(void)
{
    char name[16];
    char phone[16];

    for (int round = 0; round < 2; round++)
    {
        for (int i = 0; i < 20; i++)
        {
            name_for(i, name);
            phone_for(i, phone);
            CHECK(database_contact_add(&g_db, name, phone) == STRG_OK);
        }
        for (int i = 0; i < 20; i += 2)
        {
            phone_for(i, phone);
            CHECK(database_contact_remove(&g_db, phone) == STRG_OK);
        }
        CHECK(database_contact_count(&g_db) == 10);
        for (int i = 1; i < 20; i += 2)
        {
            phone_for(i, phone);
            CHECK(database_contact_remove(&g_db, phone) == STRG_OK);
        }
        CHECK(database_contact_count(&g_db) == 0);
    }

    CHECK(reboot() == STRG_OK);
    CHECK(database_contact_count(&g_db) == 0);
}

/* ================================================================== *
 *  Messages
 * ================================================================== */

/* A chat spanning several message sectors reads back newest first. */
static void test_ChatSpansSectors(void)
{
    static const char *const msgs[] = {"msg13", "msg12", "msg11", "msg10", "msg9", "msg8", "msg7",
                                       "msg6", "msg5", "msg4", "msg3", "msg2", "msg1"};
    const int n = (int)(MESSAGE_SECTOR_CAPACITY * 4 + 1);
    char s[8];

    _Static_assert(MESSAGE_SECTOR_CAPACITY * 4 + 1 == 13, "update msgs[]");

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    for (int i = 1; i <= n; i++)
    {
        snprintf(s, sizeof(s), "msg%d", i);
        CHECK(database_message_add(&g_db, "0400000001", i, (i % 2) == 0, s) == STRG_OK);
    }
    CHECK(chat_is("0400000001", msgs, 13));

    CHECK(reboot() == STRG_OK);
    CHECK(chat_is("0400000001", msgs, 13));
}

/* Reading a chat from an offset skips the newest messages. */
static void test_ChatOffset(void)
{
    size_t count = 0;

    for (int i = 1; i <= 7; i++)
    {
        CHECK(database_message_add(&g_db, "0400000001", i, true, (char *)"m") == STRG_OK);
    }

    CHECK(database_message_get_chat(&g_db, "0400000001", 2, 3, &count, g_messages) == STRG_OK);
    CHECK(count == 3);
    CHECK(msg_ts(&g_messages[0]) == 5);
    CHECK(msg_ts(&g_messages[2]) == 3);
}

/* Chats are kept separate per phone number. */
static void test_ChatsAreSeparate(void)
{
    static const char *const amy[] = {"from amy", "to amy"};
    static const char *const bob[] = {"to bob"};

    CHECK(database_message_add(&g_db, "0400000001", 1, true, (char *)"to amy") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000002", 2, true, (char *)"to bob") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000001", 3, false, (char *)"from amy") == STRG_OK);

    CHECK(chat_is("0400000001", amy, 2));
    CHECK(chat_is("0400000002", bob, 1));
}

/* A message from an unknown number creates an unnamed contact in the list. */
static void test_MessageFromUnknownCreatesContact(void)
{
    static const char *const names[]  = {"", "Amy"};
    static const char *const phones[] = {"0499999999", "0400000001"};

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_message_add(&g_db, "0499999999", 1, false, (char *)"who?") == STRG_OK);
    CHECK(list_is(names, phones, 2));

    CHECK(reboot() == STRG_OK);
    CHECK(list_is(names, phones, 2));
}

/* Naming an unnamed contact (created by a message) moves it in the list and keeps the chat. */
static void test_NameUnknownContact(void)
{
    static const char *const names[]  = {"Amy", "Zoe"};
    static const char *const phones[] = {"0400000001", "0499999999"};
    static const char *const msgs[]   = {"hi"};

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_message_add(&g_db, "0499999999", 1, false, (char *)"hi") == STRG_OK);
    CHECK(database_contact_update(&g_db, "0499999999", "Zoe", "0499999999") == STRG_OK);

    CHECK(list_is(names, phones, 2));
    CHECK(chat_is("0499999999", msgs, 1));
}

/* Removing a chat keeps the contact, and new messages start a new chat. */
static void test_RemoveChatKeepsContact(void)
{
    static const char *const fresh[] = {"fresh"};
    ContactBuffer out;

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    for (int i = 1; i <= 5; i++)
    {
        CHECK(database_message_add(&g_db, "0400000001", i, true, (char *)"x") == STRG_OK);
    }

    CHECK(database_message_remove_chat(&g_db, "0400000001") == STRG_OK);
    CHECK(database_contact_get(&g_db, "0400000001", &out) == STRG_OK);
    CHECK(chat_is("0400000001", NULL, 0));
    CHECK(database_message_remove_chat(&g_db, "0400000001") == STRG_EMPTY);

    CHECK(database_message_add(&g_db, "0400000001", 6, true, (char *)"fresh") == STRG_OK);
    CHECK(chat_is("0400000001", fresh, 1));

    CHECK(reboot() == STRG_OK);
    CHECK(chat_is("0400000001", fresh, 1));
}

/* Chat reads for unknown numbers or contacts without messages return STRG_EMPTY. */
static void test_ChatEmptyCases(void)
{
    size_t count = 99;

    CHECK(database_message_get_chat(&g_db, "0400000001", 0, 1, &count, g_messages) == STRG_EMPTY);
    CHECK(count == 0);

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_message_get_chat(&g_db, "0400000001", 0, 1, &count, g_messages) == STRG_EMPTY);
    CHECK(database_message_remove_chat(&g_db, "0400000001") == STRG_EMPTY);
}

/* Invalid message arguments are rejected. */
static void test_MessageRejectsInvalidArgs(void)
{
    CHECK(database_message_add(&g_db, "0400000001", 0, true, (char *)"t0") == STRG_FAIL);
    CHECK(database_message_add(&g_db, "0400000001", 1, true, NULL) == STRG_FAIL);
    CHECK(database_message_add(&g_db, "", 1, true, (char *)"x") == STRG_FAIL);
    CHECK(database_message_add(NULL, "0400000001", 1, true, (char *)"x") == STRG_FAIL);
    CHECK(database_contact_count(&g_db) == 0);
}

/* Message history is newest first across all chats and records the chat sector. */
static void test_MessageHistoryAcrossChats(void)
{
    size_t count = 0;

    CHECK(database_message_add(&g_db, "0400000001", 1, true, (char *)"a") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000002", 2, true, (char *)"b") == STRG_OK);
    CHECK(database_message_add(&g_db, "0400000001", 3, true, (char *)"c") == STRG_OK);

    CHECK(database_message_recent(&g_db, 0, 8, &count, g_history) == STRG_OK);
    CHECK(count == 3);
    CHECK(msg_ts(&g_history[0].message) == 3);
    CHECK(msg_ts(&g_history[1].message) == 2);
    CHECK(msg_ts(&g_history[2].message) == 1);
    CHECK(g_history[0].msgInd == g_history[2].msgInd); // same chat, same (only) sector
    CHECK(g_history[0].msgInd != g_history[1].msgInd);
}

/* Message history pages across ring buffer sectors. */
static void test_MessageHistoryPaging(void)
{
    const int total = (int)(MESSAGE_HISTORY_SECTOR_CAPACITY * 3 + 1);
    size_t count = 0;

    for (int i = 1; i <= total; i++)
    {
        CHECK(database_message_add(&g_db, "0400000001", i, true, (char *)"x") == STRG_OK);
    }

    CHECK(database_message_recent(&g_db, 2, 4, &count, g_history) == STRG_OK);
    CHECK(count == 4);
    for (size_t i = 0; i < count; i++)
    {
        CHECK(msg_ts(&g_history[i].message) == (uint32_t)(total - 2 - (int)i));
    }

    CHECK(reboot() == STRG_OK);
    CHECK(database_message_recent(&g_db, 0, 4, &count, g_history) == STRG_OK);
    CHECK(count == 4);
    CHECK(msg_ts(&g_history[0].message) == (uint32_t)total);
}

/* ================================================================== *
 *  Call history
 * ================================================================== */

/* Calls from a known contact store the contact, unknown callers only the phone. */
static void test_CallKnownAndUnknown(void)
{
    size_t count = 0;

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
    CHECK(database_call_add(&g_db, "0400000001") == STRG_OK);
    CHECK(database_call_add(&g_db, "0499999999") == STRG_OK);

    CHECK(database_call_recent(&g_db, 0, 4, &count, g_calls) == STRG_OK);
    CHECK(count == 2);

    // newest first
    CHECK(g_calls[0].contact_index == UINT16_MAX);
    CHECK(contact_is(&g_calls[0].contact, "", "0499999999"));
    CHECK(g_calls[1].contact_index != UINT16_MAX);
    CHECK(contact_is(&g_calls[1].contact, "Amy", "0400000001"));

    // an unknown caller does not become a contact
    CHECK(database_contact_count(&g_db) == 1);
}

/* Call history pages across ring buffer sectors and survives a reboot. */
static void test_CallHistoryPaging(void)
{
    const int total = (int)(CALL_HISTORY_SECTOR_CAPACITY * 2 + 1);
    char phone[16];
    size_t count = 0;

    for (int i = 0; i < total; i++)
    {
        phone_for(i, phone);
        CHECK(database_call_add(&g_db, phone) == STRG_OK);
    }

    CHECK(reboot() == STRG_OK);

    CHECK(database_call_recent(&g_db, 0, 4, &count, g_calls) == STRG_OK);
    CHECK(count == 4);
    phone_for(total - 1, phone);
    CHECK(contact_is(&g_calls[0].contact, "", phone));
    phone_for(total - 4, phone);
    CHECK(contact_is(&g_calls[3].contact, "", phone));
}

/* Invalid call arguments are rejected. */
static void test_CallRejectsInvalidArgs(void)
{
    size_t count = 0;

    CHECK(database_call_add(&g_db, "") == STRG_FAIL);
    CHECK(database_call_add(NULL, "0400000001") == STRG_FAIL);
    CHECK(database_call_recent(&g_db, 0, 1, NULL, g_calls) == STRG_FAIL);
    CHECK(database_call_recent(&g_db, 0, 1, &count, NULL) == STRG_FAIL);
}

/* ================================================================== *
 *  Contact free list rebuild (ports of the ContactIndexRecoveryTest checks)
 * ================================================================== */

/* One bit per contact slot: live slots, and slots seen on the free list */
static uint8_t g_slot_live[(HASH_TABLE_ENTRIES + 7) / 8];
static uint8_t g_slot_seen[(HASH_TABLE_ENTRIES + 7) / 8];

static bool bit_get(const uint8_t *bits, uint16_t i)
{
    return (bits[i / 8] >> (i % 8)) & 1U;
}

static void bit_set(uint8_t *bits, uint16_t i)
{
    bits[i / 8] |= (uint8_t)(1U << (i % 8));
}

/* Mark the contact slot of every occupied hash entry, return how many */
static size_t mark_live_slots(void)
{
    size_t live = 0;

    memset(g_slot_live, 0, sizeof(g_slot_live));
    for (size_t i = 0; i < g_db.htable.size; i++)
    {
        if (g_db.htable.htable[i].state == ENTRY_OCCUPIED)
        {
            bit_set(g_slot_live, g_db.htable.htable[i].sector);
            live++;
        }
    }
    return live;
}

/* Add n contacts with phones 04110000xx */
static bool add_numbered(int n)
{
    char name[16];
    char phone[16];

    for (int i = 0; i < n; i++)
    {
        snprintf(name, sizeof(name), "C%d", i);
        snprintf(phone, sizeof(phone), "04110000%02d", i);
        if (database_contact_add(&g_db, name, phone) != STRG_OK)
        {
            return false;
        }
    }
    return true;
}

/* After a reboot the contact free list holds every unused slot exactly once
 * and no slot a live contact owns. The 8 contacts span two contact sectors
 * and one is removed, so both sectors have free positions between used ones. */
static void test_ReconstructContactFreeListHasEveryFreeSlotOnce(void)
{
    FreeList *fls = &g_db.contact_allocator;

    CHECK(add_numbered(8));
    CHECK(database_contact_remove(&g_db, "0411000005") == STRG_OK);
    CHECK(reboot() == STRG_OK);

    size_t live = mark_live_slots();
    CHECK(live == 7);
    CHECK(free_list_used(fls) == live);
    CHECK(free_list_available(fls) + free_list_used(fls) == HASH_TABLE_ENTRIES);

    // each free slot once, never a live one
    memset(g_slot_seen, 0, sizeof(g_slot_seen));
    for (size_t i = 0; i < fls->stack_top; i++)
    {
        uint16_t slot = fls->free_stack[i];
        CHECK(slot < HASH_TABLE_ENTRIES);
        CHECK(!bit_get(g_slot_seen, slot));
        CHECK(!bit_get(g_slot_live, slot));
        bit_set(g_slot_seen, slot);
    }

    // no unused slot was lost
    for (uint16_t slot = 0; slot < HASH_TABLE_ENTRIES; slot++)
    {
        CHECK(bit_get(g_slot_live, slot) || bit_get(g_slot_seen, slot));
    }
}

/* Contacts added after a reboot must never be given a slot a live contact
 * owns (which would overwrite it on the card). Drains the allocator in RAM
 * only; the next fixture remounts from the card. */
static void test_InsertAfterReconstructNeverReusesLiveSlot(void)
{
    char phone[16];
    size_t handed_out = 0;
    uint16_t slot;

    CHECK(add_numbered(8));
    CHECK(reboot() == STRG_OK);

    size_t live = mark_live_slots();
    memset(g_slot_seen, 0, sizeof(g_slot_seen));
    while ((slot = free_list_allocate(&g_db.contact_allocator)) != UINT16_MAX)
    {
        CHECK(slot < HASH_TABLE_ENTRIES);
        CHECK(!bit_get(g_slot_live, slot)); // belongs to a live contact
        CHECK(!bit_get(g_slot_seen, slot)); // handed out twice
        bit_set(g_slot_seen, slot);
        handed_out++;
    }
    CHECK(handed_out + live == HASH_TABLE_ENTRIES);

    // the old contacts are untouched
    for (int i = 0; i < 8; i++)
    {
        ContactBuffer out;
        snprintf(phone, sizeof(phone), "04110000%02d", i);
        CHECK(database_contact_get(&g_db, phone, &out) == STRG_OK);
    }
}

/* ================================================================== *
 *  Capacity
 * ================================================================== */

/* Hardware only: a full name index rejects the add and rolls it back out of
 * the hash table (stands in for filling all HASH_TABLE_ENTRIES over SD). */
static void test_NameIndexFullRollsBack(void)
{
    ContactBuffer out;
    uint16_t occupancy;

    CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);

    occupancy = g_db.name_index.occupancy;
    g_db.name_index.occupancy = HASH_TABLE_ENTRIES;
    STRG_RET ret = database_contact_add(&g_db, "Extra", "0400000002");
    g_db.name_index.occupancy = occupancy;

    CHECK(ret == STRG_FULL);
    CHECK(database_contact_count(&g_db) == 1);
    CHECK(database_contact_get(&g_db, "0400000002", &out) == STRG_EMPTY);

    // the rolled back slot is reusable
    CHECK(database_contact_add(&g_db, "Extra", "0400000002") == STRG_OK);
    CHECK(list_consistent());
}

/* DB_CAPACITY_N contacts (every name shares the 8 character key, so every
 * insert takes the tiebreak path) are rebuilt in order on reboot. */
static void test_CapacityRebuild(void)
{
    char name[16];
    char phone[16];
    size_t start = 0;
    size_t count = 0;
    int expect = 0;

    for (int i = 0; i < DB_CAPACITY_N; i++)
    {
        name_for(i, name);
        phone_for(i, phone);
        CHECK(database_contact_add(&g_db, name, phone) == STRG_OK);
    }
    CHECK(database_contact_count(&g_db) == DB_CAPACITY_N);

    CHECK(reboot() == STRG_OK);
    CHECK(database_contact_count(&g_db) == DB_CAPACITY_N);

    // names are zero padded, so list order is index order
    while (database_contact_list(&g_db, start, DB_LIST_CHUNK, &count, g_contacts) == STRG_OK)
    {
        for (size_t i = 0; i < count; i++)
        {
            name_for(expect, name);
            phone_for(expect, phone);
            CHECK(contact_is(&g_contacts[i].contact, name, phone));
            expect++;
        }
        start += count;
    }
    CHECK(expect == DB_CAPACITY_N);

    // removing one frees space for another
    phone_for(0, phone);
    CHECK(database_contact_remove(&g_db, phone) == STRG_OK);
    phone_for(DB_CAPACITY_N, phone);
    CHECK(database_contact_add(&g_db, "Extra", phone) == STRG_OK);
    CHECK(database_contact_count(&g_db) == DB_CAPACITY_N);
}

/* ================================================================== *
 *  Power loss
 * ================================================================== */

/* Cutting power at every write of a contact add leaves either the old or
 * the new state after reboot, never a half added contact. */
static void test_PowerCutDuringContactAdd(void)
{
    static const char *const names[]  = {"Amy", "Bob"};
    static const char *const phones[] = {"0400000001", "0400000002"};

    for (int cut = 0; cut < 8; cut++)
    {
        CHECK(fixture_setup());
        CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);

        cut_power_after(cut);
        STRG_RET ret = database_contact_add(&g_db, "Bob", "0400000002");

        CHECK(reboot() == STRG_OK);
        CHECK(list_consistent());

        if (ret == STRG_OK)
        {
            CHECK(list_is(names, phones, 2));
        } else {
            // a cut after the commit (e.g. journal free) reports failure but the add is already durable
            CHECK(list_is(names, phones, 1) || list_is(names, phones, 2));
        }
    }
}

/* Cutting power at every write of a contact remove leaves either the
 * contact and chat intact or fully removed after reboot. */
static void test_PowerCutDuringContactRemove(void)
{
    static const char *const names[]  = {"Amy"};
    static const char *const phones[] = {"0400000001"};
    size_t count = 0;

    for (int cut = 0; cut < 12; cut++)
    {
        CHECK(fixture_setup());
        CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);
        CHECK(database_contact_add(&g_db, "Bob", "0400000002") == STRG_OK);
        for (int i = 1; i <= 4; i++)
        {
            CHECK(database_message_add(&g_db, "0400000002", i, true, (char *)"x") == STRG_OK);
        }

        cut_power_after(cut);
        STRG_RET ret = database_contact_remove(&g_db, "0400000002");

        CHECK(reboot() == STRG_OK);
        CHECK(list_consistent());

        if (ret == STRG_OK)
        {
            CHECK(list_is(names, phones, 1));
        } else {
            // contact may survive, but whatever is left must still be readable
            STRG_RET chat_ret = database_message_get_chat(&g_db, "0400000002", 0, 16, &count, g_messages);
            CHECK((chat_ret == STRG_OK) || (chat_ret == STRG_EMPTY));
            for (size_t i = 0; (chat_ret == STRG_OK) && (i < count); i++)
            {
                CHECK(strcmp(g_messages[i].str, "x") == 0);
            }
        }
    }
}

/* Cutting power at every write of a message add never loses earlier
 * messages in the chat. */
static void test_PowerCutDuringMessageAdd(void)
{
    static const char *const before[] = {"m3", "m2", "m1"};
    static const char *const after[]  = {"new", "m3", "m2", "m1"};
    char s[8];

    _Static_assert(MESSAGE_SECTOR_CAPACITY == 3, "update before[] / after[]");

    for (int cut = 0; cut < 8; cut++)
    {
        CHECK(fixture_setup());
        // fill the first sector so the next message needs a new one
        for (int i = 1; i <= (int)MESSAGE_SECTOR_CAPACITY; i++)
        {
            snprintf(s, sizeof(s), "m%d", i);
            CHECK(database_message_add(&g_db, "0400000001", i, true, s) == STRG_OK);
        }

        cut_power_after(cut);
        STRG_RET ret = database_message_add(&g_db, "0400000001", 100, true, (char *)"new");

        CHECK(reboot() == STRG_OK);

        if (ret == STRG_OK)
        {
            CHECK(chat_is("0400000001", after, 4));
        } else {
            // the new message may or may not have landed, the old ones must be intact
            CHECK(chat_is("0400000001", before, 3) || chat_is("0400000001", after, 4));
        }
    }
}

/* Cutting power during a rename never leaves the contact missing from
 * the list or listed twice. */
static void test_PowerCutDuringUpdate(void)
{
    static const char *const names[]  = {"Zara"};
    static const char *const phones[] = {"0400000009"};

    for (int cut = 0; cut < 8; cut++)
    {
        CHECK(fixture_setup());
        CHECK(database_contact_add(&g_db, "Amy", "0400000001") == STRG_OK);

        cut_power_after(cut);
        STRG_RET ret = database_contact_update(&g_db, "0400000001", "Zara", "0400000009");

        CHECK(reboot() == STRG_OK);
        CHECK(list_consistent());

        if (ret == STRG_OK)
        {
            CHECK(list_is(names, phones, 1));
        } else {
            CHECK(database_contact_count(&g_db) == 1);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Test table
 * ------------------------------------------------------------------ */
#define TEST(fn) { #fn, test_##fn }

static const TestCase g_tests[] = {
    TEST(BlankCardFormatsEmpty),
    TEST(FormatWritesValidSuperheader),
    TEST(RebootEmptyDatabase),
    TEST(RebootPreservesEverything),
    TEST(WritesAfterRebootAppend),
    TEST(CorruptSuperheaderNotWiped),
    TEST(OutdatedSuperheaderReported),
    TEST(InitNullFails),
    TEST(AddThenGet),
    TEST(GetUnknownIsEmpty),
    TEST(DuplicatePhoneRejected),
    TEST(AddRejectsInvalidArgs),
    TEST(AddMaxLengthFields),
    TEST(ListIsAlphabetical),
    TEST(SameNameDifferentPhones),
    TEST(LongSharedPrefixOrdered),
    TEST(ListPaging),
    TEST(SeekPrefix),
    TEST(UpdateNameReorders),
    TEST(UpdatePhoneKeepsChat),
    TEST(UpdateToExistingPhoneRejected),
    TEST(UpdateUnknownIsEmpty),
    TEST(RemoveContact),
    TEST(RemoveSameNameRemovesRightOne),
    TEST(RemoveContactDeletesChat),
    TEST(RemoveUnknownIsEmpty),
    TEST(AddRemoveChurn),
    TEST(ChatSpansSectors),
    TEST(ChatOffset),
    TEST(ChatsAreSeparate),
    TEST(MessageFromUnknownCreatesContact),
    TEST(NameUnknownContact),
    TEST(RemoveChatKeepsContact),
    TEST(ChatEmptyCases),
    TEST(MessageRejectsInvalidArgs),
    TEST(MessageHistoryAcrossChats),
    TEST(MessageHistoryPaging),
    TEST(CallKnownAndUnknown),
    TEST(CallHistoryPaging),
    TEST(CallRejectsInvalidArgs),
    TEST(ReconstructContactFreeListHasEveryFreeSlotOnce),
    TEST(InsertAfterReconstructNeverReusesLiveSlot),
    TEST(NameIndexFullRollsBack),
    TEST(CapacityRebuild),
    TEST(PowerCutDuringContactAdd),
    TEST(PowerCutDuringContactRemove),
    TEST(PowerCutDuringMessageAdd),
    TEST(PowerCutDuringUpdate),
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
    g_db_api_report.current = (uint32_t)(i + 1);
    htest_ui_start((uint32_t)i, g_tests[i].name);

    if (!fixture_setup())
    {
        g_fail_line = __LINE__;
    }
    else
    {
        g_tests[i].fn();
        fixture_teardown();
    }

    g_db_api_report.ms[i] = osKernelGetTickCount() - start;
    g_db_api_report.fail_line[i] = (uint16_t)g_fail_line;
    htest_ui_result((uint32_t)i, g_fail_line == 0, (uint32_t)g_fail_line);

    if (g_fail_line == 0)
    {
        g_db_api_report.status[i] = TEST_PASS;
        g_db_api_report.passed++;
    }
    else
    {
        g_db_api_report.status[i] = TEST_FAIL;
        g_db_api_report.failed++;
        if (g_db_api_report.first_failed == 0)
        {
            g_db_api_report.first_failed = (uint32_t)(i + 1);
        }
    }
}

/* Report a suite-level failure (card or format) as test 1 failing. */
static void fail_suite(const char *what, int line)
{
    g_db_api_report.first_failed = 1;
    g_db_api_report.failed = 1;
    g_db_api_report.status[0] = TEST_FAIL;
    g_db_api_report.fail_line[0] = (uint16_t)line;
    htest_ui_start(0, what);
    htest_ui_result(0, false, (uint32_t)line);
}

/* ------------------------------------------------------------------ *
 *  Task
 * ------------------------------------------------------------------ */
static void dbApiTask(void *arg)
{
    (void)arg;
    uint32_t start;

    memset((void *)&g_db_api_report, 0, sizeof(g_db_api_report));
    g_db_api_report.total = (uint32_t)N_TESTS;
    g_db_api_report.magic = DB_REPORT_MAGIC;

    htest_ui_init("DATABASE", (uint32_t)N_TESTS);

    if (!storage_bringup())
    {
        fail_suite("SD card init", __LINE__);
    }
    else
    {
        // suite setup: mount (a blank card is formatted by database_init), full format if the mount fails (bad superheader or older layout)
        htest_ui_start(0, "Mounting card");
        start = osKernelGetTickCount();
        STRG_RET ret = database_init(&g_db);
        ret = (ret != STRG_OK) ? database_format(&g_db) : ret;
        g_db_api_report.format_ms = osKernelGetTickCount() - start;

        if (ret != STRG_OK)
        {
            fail_suite("database_format", __LINE__);
        }
        else
        {
            for (size_t i = 0; i < N_TESTS; i++)
            {
                run_test(i);
            }
        }
    }

    g_db_api_report.current = 0;
    g_db_api_report.done = 1;
    htest_ui_done(g_db_api_report.passed, g_db_api_report.failed);

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_db_api_task_cb;
static uint8_t g_db_api_task_stack[DB_TASK_STACK_SIZE] __attribute__((aligned(8)));

void Database_Test_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "DB API Test Task",
        .cb_mem = &g_db_api_task_cb,
        .cb_size = sizeof(g_db_api_task_cb),
        .stack_mem = g_db_api_task_stack,
        .stack_size = sizeof(g_db_api_task_stack),
        .priority = DB_TASK_PRIORITY};

    osThreadNew(dbApiTask, NULL, &task_attr);
}
