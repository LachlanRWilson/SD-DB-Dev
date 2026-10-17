/*
 * sa_main.c - on-hardware unit tests for the name-ordered sorted array
 * (sorted_array.c).
 *
 * Firmware-side port of tests/test_sorted_array.cpp (the SortedArrayTest and
 * SortedArraySeekTest suites): every TEST_F there has a test_*() function here
 * with the same name and the same checks, run against the physical SD card
 * (sd_storage.c) instead of the heap-backed mock. Contacts are written
 * through the real hash table, so every key tiebreak reads the contact back
 * from the card through htable[slot].sector.
 *
 * Differences from the gtest suite (all forced by the target, not the logic):
 *   - The fixture re-formats the usage bitmap and journal on the card and
 *     resets the RAM table, instead of zeroing a heap buffer. Old contact
 *     sectors are left behind; nothing reads a sector the bitmap marks unused.
 *   - AppendThenSortMatchesInsertOrder inserts SA_SORT_N (240) contacts
 *     instead of 500 to keep the run time over SD reasonable.
 *   - RebuildFromCardMatchesInsertOrder is hardware only: it rebuilds the hash
 *     table from the card (as a boot does), refills the sorted array with
 *     append + sort and checks the order is unchanged.
 *   - A failed check ends that test (gtest's EXPECT_* would carry on).
 *
 * RAM: the hash table, contact free list and sorted array (~240KB) live in
 * RAM_D1. db_main.c's buffers are dropped by --gc-sections because DB_Init()
 * is not called in an SA_TEST build, so both fit.
 *
 * ------------------------------------------------------------------------
 *  Reporting
 * ------------------------------------------------------------------------
 *   The LCD shows each test as it runs and the summary at the end
 *   (htest_ui.c). A failure shows the sa_main.c line of the failed check.
 *   g_sa_report (below) has the same results and can be read over SWD
 *   without halting the core, e.g.
 *
 *     STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <&g_sa_report> 0x100
 *
 *   Build with -DSA_TEST=ON to run this instead of db_main.c. It overwrites
 *   the usage bitmap and journal, so it wipes the DB_TEST_WRITE data set.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sorted_array.h"
#include "hash_table.h"
#include "contact.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "storage.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "htest_ui.h"
#include "sa_main.h"

#define SA_TASK_STACK_SIZE (8 * 1024)
#define SA_TASK_PRIORITY   osPriorityNormal

/* Message allocator is only reset by hash_clear(), never allocated from. */
#define SA_MESSAGE_FLS_SIZE 16

/* Contacts in the append + sort tests (a multiple of the name pool size). */
#define SA_SORT_N 240

#define SA_MAX_TESTS    48
#define SA_REPORT_MAGIC 0x53417E57u

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
    uint32_t magic;                    /* SA_REPORT_MAGIC once the suite has started   */
    uint32_t done;                     /* 1 when every test has run                    */
    uint32_t total;                    /* tests in this build                          */
    uint32_t passed;
    uint32_t failed;
    uint32_t current;                  /* 1-based number of the test running now       */
    uint32_t first_failed;             /* 1-based number of the first failure, 0 none  */
    uint8_t  status[SA_MAX_TESTS];     /* TestStatus per test                          */
    uint16_t fail_line[SA_MAX_TESTS];  /* sa_main.c line of the failed check           */
    uint32_t ms[SA_MAX_TESTS];         /* run time per test                            */
} SaTestReport;

volatile SaTestReport g_sa_report;

/* ------------------------------------------------------------------ *
 *  RAM backing (too large for DTCM / the task stack -> RAM D1)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_contact_fls_mem[HASH_TABLE_ENTRIES];

__attribute__((section(".ram_d1")))
static SortedArrayEntry g_sa_mem[SORTED_ARRAY_ENTRIES];

/* Copies of the array for the sort tests */
__attribute__((section(".ram_d1")))
static SortedArrayEntry g_expected[SA_SORT_N];

static uint16_t g_message_fls_mem[SA_MESSAGE_FLS_SIZE];

static Storage *const storage = &sd_storage;

static SDStorageContext g_sd_ctx;
static Journal          g_journal;
static HashTable        g_table;
static FreeList         g_contact_fls;
static FreeList         g_message_fls;
static SortedArray      g_sa;

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
 *  Fixture (mirrors SortedArrayTest)
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

/* SortedArrayTest::SetUp() */
static bool fixture_setup(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_sa_mem, 0, sizeof(g_sa_mem));

    /* check_usage_bit()/update_usage_bit() use the global in-RAM bitmap, so
     * clear it between tests. Formatting it on the card also gives every
     * bitmap sector a valid CRC trailer for rebuild(). */
    if (!init_usage_bitmap(storage)) { return false; }

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_ENTRIES))      { return false; }
    if (!free_list_init(&g_message_fls, g_message_fls_mem, SA_MESSAGE_FLS_SIZE)) { return false; }

    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    return sorted_array_init(&g_sa, g_entries, storage, g_sa_mem);
}

/* SortedArrayTest::TearDown() */
static void fixture_teardown(void)
{
    hash_clear(&g_table);
}

/* Store a contact in the hash table and return its slot (UINT16_MAX on failure). */
static uint16_t store(const char *name, const char *phone)
{
    ContactBuffer c = create_contact(name, phone);
    uint16_t slot = UINT16_MAX;
    if (hash_insert_contact(&g_table, &g_journal, &c, &slot) != STRG_OK)
    {
        return UINT16_MAX;
    }
    return slot;
}

/* Store a contact in the hash table, then insert it into the sorted array. */
static STRG_RET insert(const char *name, const char *phone)
{
    uint16_t slot = store(name, phone);
    if (slot == UINT16_MAX)
    {
        return STRG_FAIL;
    }
    ContactBuffer c = create_contact(name, phone);
    return sorted_array_insert(&g_sa, &c.contact, slot);
}

/* Read the contact the entry at `rank` points at. */
static bool contact_at(size_t rank, Contact *out)
{
    ContactBuffer c;
    uint16_t sector = g_entries[g_sa.mem[rank].hash_slot].sector;
    if (read_contact(storage, sector, &c) != STRG_OK)
    {
        return false;
    }
    *out = c.contact;
    return true;
}

static bool name_is(size_t rank, const char *name)
{
    Contact c;
    size_t len = strlen(name);
    return contact_at(rank, &c) && (c.name_len == len) && (memcmp(c.name, name, len) == 0);
}

static bool phone_is(size_t rank, const char *phone)
{
    Contact c;
    size_t len = strlen(phone);
    return contact_at(rank, &c) && (c.phone_len == len) && (memcmp(c.phone, phone, len) == 0);
}

/* The whole array holds exactly `names`, in order. */
static bool names_are(const char *const *names, size_t n)
{
    if (g_sa.occupancy != n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!name_is(i, names[i]))
        {
            return false;
        }
    }
    return true;
}

#define NAMES_ARE(...)                                                     \
    names_are((const char *const[]){ __VA_ARGS__ },                        \
              sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(const char *))

/* Insert `n` contacts named `names[i]` with phone 04 + name. */
static bool insert_named(const char *const *names, size_t n)
{
    char phone[MAX_PHONE_LEN + 1];
    for (size_t i = 0; i < n; i++)
    {
        snprintf(phone, sizeof(phone), "04%s", names[i]);
        if (insert(names[i], phone) != STRG_OK)
        {
            return false;
        }
    }
    return true;
}

/* Small xorshift PRNG for the shuffle (fixed seed, repeatable runs). */
static uint32_t g_rng;

static uint32_t rng_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void shuffle(SortedArrayEntry *a, size_t n)
{
    for (size_t i = n; i > 1; i--)
    {
        size_t j = rng_next() % i;
        SortedArrayEntry t = a[i - 1];
        a[i - 1] = a[j];
        a[j] = t;
    }
}

/* ================================================================== *
 *  sorted_array_init()
 * ================================================================== */
static void test_InitRejectsNullArguments(void)
{
    SortedArray other;
    CHECK(!sorted_array_init(NULL, g_entries, storage, g_sa_mem));
    CHECK(!sorted_array_init(&other, g_entries, NULL, g_sa_mem));
    CHECK(!sorted_array_init(&other, g_entries, storage, NULL));
}

static void test_InitStartsEmpty(void)
{
    CHECK(g_sa.occupancy == 0);
}

/* ================================================================== *
 *  sorted_array_insert()
 * ================================================================== */
static void test_InsertKeepsNameOrder(void)
{
    static const char *const in[] = { "Mallory", "Alice", "Trent", "Bob", "Eve", "Carol" };
    CHECK(insert_named(in, 6));
    CHECK(NAMES_ARE("Alice", "Bob", "Carol", "Eve", "Mallory", "Trent"));
}

static void test_InsertOrderIsCaseInsensitive(void)
{
    CHECK(insert("carol", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);
    CHECK(insert("alice", "0400000003") == STRG_OK);
    CHECK(NAMES_ARE("alice", "Bob", "carol"));
}

/* Names sharing the first 8 characters have equal keys, so the order comes
 * from the full name read back from the card. */
static void test_InsertTiebreaksOnFullNamePastKey(void)
{
    CHECK(insert("Alexandra",       "0400000001") == STRG_OK);
    CHECK(insert("Alexander Smith", "0400000002") == STRG_OK);
    CHECK(insert("Alexander",       "0400000003") == STRG_OK);
    CHECK(insert("Alexander Jones", "0400000004") == STRG_OK);
    CHECK(NAMES_ARE("Alexander", "Alexander Jones", "Alexander Smith", "Alexandra"));
}

static void test_InsertSameNameDifferentCaseOrdersByExactCase(void)
{
    CHECK(insert("bob", "0400000001") == STRG_OK);
    CHECK(insert("Bob", "0400000002") == STRG_OK);

    /* 'B' (0x42) sorts before 'b' (0x62) */
    CHECK(NAMES_ARE("Bob", "bob"));
}

static void test_InsertSameNameOrdersByPhone(void)
{
    CHECK(insert("Sam", "0400000003") == STRG_OK);
    CHECK(insert("Sam", "0400000001") == STRG_OK);
    CHECK(insert("Sam", "0400000002") == STRG_OK);

    CHECK(g_sa.occupancy == 3);
    CHECK(phone_is(0, "0400000001"));
    CHECK(phone_is(1, "0400000002"));
    CHECK(phone_is(2, "0400000003"));
}

static void test_InsertDuplicateIsRejected(void)
{
    uint16_t slot = store("Alice", "0400000001");
    CHECK(slot != UINT16_MAX);
    ContactBuffer c = create_contact("Alice", "0400000001");

    CHECK(sorted_array_insert(&g_sa, &c.contact, slot) == STRG_OK);
    CHECK(sorted_array_insert(&g_sa, &c.contact, slot) != STRG_OK);
    CHECK(g_sa.occupancy == 1);
}

static void test_InsertWhenFullReturnsFull(void)
{
    uint16_t slot = store("Alice", "0400000001");
    CHECK(slot != UINT16_MAX);
    ContactBuffer c = create_contact("Alice", "0400000001");

    g_sa.occupancy = SORTED_ARRAY_ENTRIES;
    STRG_RET ret = sorted_array_insert(&g_sa, &c.contact, slot);
    uint16_t occupancy = g_sa.occupancy;
    g_sa.occupancy = 0;

    CHECK(ret == STRG_FULL);
    CHECK(occupancy == SORTED_ARRAY_ENTRIES);
}

static void test_InsertStoresKeyAndSlot(void)
{
    uint16_t slot = store("ALEXANDER", "0400000001");
    CHECK(slot != UINT16_MAX);
    ContactBuffer c = create_contact("ALEXANDER", "0400000001");
    CHECK(sorted_array_insert(&g_sa, &c.contact, slot) == STRG_OK);

    CHECK(memcmp(g_sa.mem[0].key, "alexande", KEY_LEN) == 0);
    CHECK(g_sa.mem[0].hash_slot == slot);
}

static void test_ShortNameKeyIsZeroPadded(void)
{
    static const char expected[KEY_LEN] = { 'a', 'l', 0, 0, 0, 0, 0, 0 };
    CHECK(insert("Al", "0400000001") == STRG_OK);
    CHECK(memcmp(g_sa.mem[0].key, expected, KEY_LEN) == 0);
}

/* ================================================================== *
 *  sorted_array_find()
 * ================================================================== */
static void test_FindExactMatch(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    ContactBuffer c = create_contact("Bob", "0400000002");
    size_t rank = SIZE_MAX;
    bool match = false;
    CHECK(sorted_array_find(&g_sa, &c.contact, &rank, &match) == STRG_OK);
    CHECK(match);
    CHECK(rank == 1);
}

static void test_FindMissingGivesInsertionRank(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    ContactBuffer c = create_contact("Bob", "0400000002");
    size_t rank = SIZE_MAX;
    bool match = true;
    CHECK(sorted_array_find(&g_sa, &c.contact, &rank, &match) == STRG_OK);
    CHECK(!match);
    CHECK(rank == 1);
}

static void test_FindSameNameDifferentPhoneIsNotAMatch(void)
{
    CHECK(insert("Sam", "0400000001") == STRG_OK);

    ContactBuffer c = create_contact("Sam", "0400000009");
    size_t rank = SIZE_MAX;
    bool match = true;
    CHECK(sorted_array_find(&g_sa, &c.contact, &rank, &match) == STRG_OK);
    CHECK(!match);
    CHECK(rank == 1);
}

static void test_FindInEmptyArray(void)
{
    ContactBuffer c = create_contact("Alice", "0400000001");
    size_t rank = SIZE_MAX;
    bool match = true;
    CHECK(sorted_array_find(&g_sa, &c.contact, &rank, &match) == STRG_OK);
    CHECK(!match);
    CHECK(rank == 0);
}

/* ================================================================== *
 *  sorted_array_remove()
 * ================================================================== */
static void test_RemoveMiddleKeepsOrder(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    ContactBuffer c = create_contact("Bob", "0400000002");
    CHECK(sorted_array_remove(&g_sa, &c.contact));
    CHECK(NAMES_ARE("Alice", "Carol"));
}

static void test_RemoveFirstAndLast(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    ContactBuffer first = create_contact("Alice", "0400000001");
    ContactBuffer last = create_contact("Carol", "0400000003");
    CHECK(sorted_array_remove(&g_sa, &first.contact));
    CHECK(sorted_array_remove(&g_sa, &last.contact));
    CHECK(NAMES_ARE("Bob"));
}

static void test_RemoveOnlyTheMatchingPhone(void)
{
    CHECK(insert("Sam", "0400000001") == STRG_OK);
    CHECK(insert("Sam", "0400000002") == STRG_OK);
    CHECK(insert("Sam", "0400000003") == STRG_OK);

    ContactBuffer c = create_contact("Sam", "0400000002");
    CHECK(sorted_array_remove(&g_sa, &c.contact));

    CHECK(g_sa.occupancy == 2);
    CHECK(phone_is(0, "0400000001"));
    CHECK(phone_is(1, "0400000003"));
}

static void test_RemoveMissingLeavesArrayUnchanged(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    ContactBuffer c = create_contact("Bob", "0400000002");
    CHECK(!sorted_array_remove(&g_sa, &c.contact));
    CHECK(g_sa.occupancy == 2);
}

static void test_RemoveFromEmptyFails(void)
{
    ContactBuffer c = create_contact("Alice", "0400000001");
    CHECK(!sorted_array_remove(&g_sa, &c.contact));
    CHECK(g_sa.occupancy == 0);
}

/* ================================================================== *
 *  sorted_array_seek() (SortedArraySeekTest)
 * ================================================================== */

/* SortedArraySeekTest::SetUp().
 * Order: Adam, Alexander Jones, Alexander Smith, Alice, Bob, Brian, Carl */
static bool seek_setup(void)
{
    return (insert("Bob",             "0400000001") == STRG_OK) &&
           (insert("Adam",            "0400000002") == STRG_OK) &&
           (insert("Carl",            "0400000003") == STRG_OK) &&
           (insert("Alexander Smith", "0400000004") == STRG_OK) &&
           (insert("Brian",           "0400000005") == STRG_OK) &&
           (insert("Alice",           "0400000006") == STRG_OK) &&
           (insert("Alexander Jones", "0400000007") == STRG_OK);
}

/* Seek `prefix` and check it lands on `want`. */
static bool seek_is(const char *prefix, size_t want)
{
    size_t rank = SIZE_MAX;
    return (sorted_array_seek(&g_sa, prefix, &rank) == STRG_OK) && (rank == want);
}

static void test_EmptyPrefixSeeksToStart(void)
{
    CHECK(seek_setup());
    CHECK(seek_is("", 0));
}

static void test_PrefixSeeksToFirstMatch(void)
{
    CHECK(seek_setup());
    CHECK(seek_is("A", 0));
    CHECK(seek_is("Al", 1));
    CHECK(seek_is("Ali", 3));
    CHECK(seek_is("B", 4));
    CHECK(seek_is("Br", 5));
    CHECK(seek_is("C", 6));
}

static void test_PrefixIsCaseInsensitive(void)
{
    CHECK(seek_setup());
    CHECK(seek_is("br", 5));
    CHECK(seek_is("BR", 5));
}

/* Prefixes longer than the key fall through to the full name tiebreak. */
static void test_PrefixLongerThanKey(void)
{
    CHECK(seek_setup());
    CHECK(seek_is("Alexander", 1));
    CHECK(seek_is("Alexander S", 2));
    CHECK(seek_is("alexander smith", 2));
}

static void test_PrefixWithNoMatchSeeksToInsertionPoint(void)
{
    CHECK(seek_setup());
    CHECK(seek_is("Bz", 6));
    CHECK(seek_is("Z", 7));
}

static void test_NullPrefixFails(void)
{
    size_t rank;
    CHECK(seek_setup());
    CHECK(sorted_array_seek(&g_sa, NULL, &rank) != STRG_OK);
}

static void test_TooLongPrefixFails(void)
{
    char prefix[MAX_NAME_LEN + 2];
    size_t rank;
    CHECK(seek_setup());
    memset(prefix, 'a', MAX_NAME_LEN + 1);
    prefix[MAX_NAME_LEN + 1] = '\0';
    CHECK(sorted_array_seek(&g_sa, prefix, &rank) != STRG_OK);
}

/* ================================================================== *
 *  sorted_array_get_range()
 * ================================================================== */
static void test_GetRangeCopiesEntries(void)
{
    SortedArrayEntry out[2];
    size_t count = 0;

    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);
    CHECK(insert("Carol", "0400000003") == STRG_OK);

    CHECK(sorted_array_get_range(&g_sa, 1, 2, &count, out));
    CHECK(count == 2);
    CHECK(memcmp(out, &g_sa.mem[1], 2 * sizeof(SortedArrayEntry)) == 0);
}

static void test_GetRangeClampsToOccupancy(void)
{
    SortedArrayEntry out[10];
    size_t count = 0;

    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);

    CHECK(sorted_array_get_range(&g_sa, 1, 10, &count, out));
    CHECK(count == 1);
    CHECK(out[0].hash_slot == g_sa.mem[1].hash_slot);
}

static void test_GetRangeStartPastEndFails(void)
{
    SortedArrayEntry out[1];
    size_t count = 0;

    CHECK(insert("Alice", "0400000001") == STRG_OK);

    CHECK(!sorted_array_get_range(&g_sa, 1, 1, &count, out));
    CHECK(!sorted_array_get_range(&g_sa, 0, 1, &count, NULL));
}

/* ================================================================== *
 *  sorted_array_append() / sorted_array_sort()
 * ================================================================== */
static void test_AppendWhenFullFails(void)
{
    SortedArrayEntry e;
    memset(&e, 0, sizeof(e));

    g_sa.occupancy = SORTED_ARRAY_ENTRIES;
    bool ok = sorted_array_append(&g_sa, &e);
    uint16_t occupancy = g_sa.occupancy;
    g_sa.occupancy = 0;

    CHECK(!ok);
    CHECK(occupancy == SORTED_ARRAY_ENTRIES);
}

static void test_SortEmptyAndSingle(void)
{
    CHECK(sorted_array_sort(&g_sa) == STRG_OK);

    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(sorted_array_sort(&g_sa) == STRG_OK);
    CHECK(name_is(0, "Alice"));
}

static void test_SortWithoutHashTableFails(void)
{
    CHECK(insert("Alice", "0400000001") == STRG_OK);
    CHECK(insert("Bob",   "0400000002") == STRG_OK);

    g_sa.htable_mem = NULL;
    STRG_RET ret = sorted_array_sort(&g_sa);
    g_sa.htable_mem = g_entries;

    CHECK(ret == STRG_FAIL);
}

/* Name pool sharing key prefixes, so sorting exercises every tiebreak level
 * (full name, exact case, phone). */
static const char *const g_pool[] = {
    "Alexander", "alexander", "Alexander Jones", "Alexandra", "Bob", "bob",
    "Carol", "Christopher", "Christophe", "Dave", "Eve", "Zed",
};
#define POOL_N (sizeof(g_pool) / sizeof(g_pool[0]))

_Static_assert(SA_SORT_N % POOL_N == 0, "SA_SORT_N should be a multiple of the pool size");

static bool insert_pool(size_t n)
{
    char phone[MAX_PHONE_LEN + 1];
    for (size_t i = 0; i < n; i++)
    {
        snprintf(phone, sizeof(phone), "04%08u", (unsigned)i);
        if (insert(g_pool[i % POOL_N], phone) != STRG_OK)
        {
            return false;
        }
    }
    return true;
}

/* Start-up path: entries appended in arbitrary order then sorted must end up
 * identical to building the array with insert. */
static void test_AppendThenSortMatchesInsertOrder(void)
{
    static SortedArrayEntry shuffled[SA_SORT_N];
    const size_t n = SA_SORT_N;

    CHECK(insert_pool(n));
    CHECK(g_sa.occupancy == n);

    memcpy(g_expected, g_sa.mem, n * sizeof(SortedArrayEntry));
    memcpy(shuffled, g_sa.mem, n * sizeof(SortedArrayEntry));
    g_rng = 1234u;
    shuffle(shuffled, n);

    g_sa.occupancy = 0;
    for (size_t i = 0; i < n; i++)
    {
        CHECK(sorted_array_append(&g_sa, &shuffled[i]));
    }
    CHECK(sorted_array_sort(&g_sa) == STRG_OK);

    CHECK(g_sa.occupancy == n);
    for (size_t i = 0; i < n; i++)
    {
        CHECK(g_sa.mem[i].hash_slot == g_expected[i].hash_slot);
        CHECK(memcmp(g_sa.mem[i].key, g_expected[i].key, KEY_LEN) == 0);
    }
}

static void test_SortReversedInput(void)
{
    static const char *const in[] = { "Alice", "Bob", "Carol", "Dave", "Eve" };
    CHECK(insert_named(in, 5));

    for (size_t i = 0, j = g_sa.occupancy - 1; i < j; i++, j--)
    {
        SortedArrayEntry t = g_sa.mem[i];
        g_sa.mem[i] = g_sa.mem[j];
        g_sa.mem[j] = t;
    }

    CHECK(sorted_array_sort(&g_sa) == STRG_OK);
    CHECK(NAMES_ARE("Alice", "Bob", "Carol", "Dave", "Eve"));
}

/* ================================================================== *
 *  Persistence (hardware only)
 * ================================================================== */

/* Build the entry for the contact in `slot` the same way make_key() does:
 * first KEY_LEN characters, lower case, zero padded. */
static bool entry_for_slot(uint16_t slot, SortedArrayEntry *out)
{
    ContactBuffer c;
    if (read_contact(storage, g_entries[slot].sector, &c) != STRG_OK)
    {
        return false;
    }

    memset(out->key, 0, KEY_LEN);
    for (size_t i = 0; (i < KEY_LEN) && (i < c.contact.name_len); i++)
    {
        char ch = c.contact.name[i];
        out->key[i] = ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
    }
    out->hash_slot = slot;
    return true;
}

/* HashTableTest::rebuild(): empty the RAM table, reload the usage bitmap
 * from the card and reconstruct the contacts, as a boot does. */
static bool rebuild(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    if (!free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_ENTRIES))
    {
        return false;
    }

    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    if (!read_usage_bitmap(storage))
    {
        return false;
    }

    return hash_reconstruct_contact(&g_table);
}

/* After a reboot, the slots differ (reconstruction re-hashes in sector
 * order), but rebuilding the array with append + sort over the occupied
 * slots gives the same contacts in the same order. */
static void test_RebuildFromCardMatchesInsertOrder(void)
{
    static uint16_t sectors[SA_SORT_N];
    const size_t n = SA_SORT_N;

    CHECK(insert_pool(n));

    /* Hash slots move on reconstruction, contact sectors don't */
    for (size_t i = 0; i < n; i++)
    {
        sectors[i] = g_entries[g_sa.mem[i].hash_slot].sector;
    }

    CHECK(rebuild());
    CHECK(sorted_array_init(&g_sa, g_entries, storage, g_sa_mem));

    for (uint16_t slot = 0; slot < HASH_TABLE_SIZE; slot++)
    {
        if (g_entries[slot].state != ENTRY_OCCUPIED)
        {
            continue;
        }
        SortedArrayEntry e;
        CHECK(entry_for_slot(slot, &e));
        CHECK(sorted_array_append(&g_sa, &e));
    }
    CHECK(g_sa.occupancy == n);
    CHECK(sorted_array_sort(&g_sa) == STRG_OK);

    for (size_t i = 0; i < n; i++)
    {
        CHECK(g_entries[g_sa.mem[i].hash_slot].sector == sectors[i]);
    }
}

/* ================================================================== *
 *  Test table
 * ================================================================== */
#define TEST(fn) { #fn, test_##fn }

static const TestCase g_tests[] = {
    TEST(InitRejectsNullArguments),
    TEST(InitStartsEmpty),
    TEST(InsertKeepsNameOrder),
    TEST(InsertOrderIsCaseInsensitive),
    TEST(InsertTiebreaksOnFullNamePastKey),
    TEST(InsertSameNameDifferentCaseOrdersByExactCase),
    TEST(InsertSameNameOrdersByPhone),
    TEST(InsertDuplicateIsRejected),
    TEST(InsertWhenFullReturnsFull),
    TEST(InsertStoresKeyAndSlot),
    TEST(ShortNameKeyIsZeroPadded),
    TEST(FindExactMatch),
    TEST(FindMissingGivesInsertionRank),
    TEST(FindSameNameDifferentPhoneIsNotAMatch),
    TEST(FindInEmptyArray),
    TEST(RemoveMiddleKeepsOrder),
    TEST(RemoveFirstAndLast),
    TEST(RemoveOnlyTheMatchingPhone),
    TEST(RemoveMissingLeavesArrayUnchanged),
    TEST(RemoveFromEmptyFails),
    TEST(EmptyPrefixSeeksToStart),
    TEST(PrefixSeeksToFirstMatch),
    TEST(PrefixIsCaseInsensitive),
    TEST(PrefixLongerThanKey),
    TEST(PrefixWithNoMatchSeeksToInsertionPoint),
    TEST(NullPrefixFails),
    TEST(TooLongPrefixFails),
    TEST(GetRangeCopiesEntries),
    TEST(GetRangeClampsToOccupancy),
    TEST(GetRangeStartPastEndFails),
    TEST(AppendWhenFullFails),
    TEST(SortEmptyAndSingle),
    TEST(SortWithoutHashTableFails),
    TEST(AppendThenSortMatchesInsertOrder),
    TEST(SortReversedInput),
    TEST(RebuildFromCardMatchesInsertOrder),
};

#define N_TESTS (sizeof(g_tests) / sizeof(g_tests[0]))

_Static_assert(N_TESTS <= SA_MAX_TESTS, "raise SA_MAX_TESTS");

/* ------------------------------------------------------------------ *
 *  Runner
 * ------------------------------------------------------------------ */
static void run_test(size_t i)
{
    uint32_t start = osKernelGetTickCount();

    g_fail_line = 0;
    g_sa_report.current = (uint32_t)(i + 1);
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

    g_sa_report.ms[i] = osKernelGetTickCount() - start;
    g_sa_report.fail_line[i] = (uint16_t)g_fail_line;
    htest_ui_result((uint32_t)i, g_fail_line == 0, (uint32_t)g_fail_line);

    if (g_fail_line == 0)
    {
        g_sa_report.status[i] = TEST_PASS;
        g_sa_report.passed++;
    }
    else
    {
        g_sa_report.status[i] = TEST_FAIL;
        g_sa_report.failed++;
        if (g_sa_report.first_failed == 0)
        {
            g_sa_report.first_failed = (uint32_t)(i + 1);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Task
 * ------------------------------------------------------------------ */
static void saTask(void *arg)
{
    (void)arg;

    memset((void *)&g_sa_report, 0, sizeof(g_sa_report));
    g_sa_report.total = (uint32_t)N_TESTS;
    g_sa_report.magic = SA_REPORT_MAGIC;

    htest_ui_init("SORTED ARRAY", (uint32_t)N_TESTS);

    if (!storage_bringup())
    {
        /* Nothing can run without the card: report it as test 1 failing. */
        g_sa_report.first_failed = 1;
        g_sa_report.failed = 1;
        g_sa_report.status[0] = TEST_FAIL;
        g_sa_report.fail_line[0] = (uint16_t)__LINE__;
        htest_ui_start(0, "SD card init");
        htest_ui_result(0, false, g_sa_report.fail_line[0]);
    }
    else
    {
        for (size_t i = 0; i < N_TESTS; i++)
        {
            run_test(i);
        }
    }

    g_sa_report.current = 0;
    g_sa_report.done = 1;
    htest_ui_done(g_sa_report.passed, g_sa_report.failed);

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_sa_task_cb;
static uint8_t g_sa_task_stack[SA_TASK_STACK_SIZE] __attribute__((aligned(8)));

void SA_Test_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "SA Test Task",
        .cb_mem = &g_sa_task_cb,
        .cb_size = sizeof(g_sa_task_cb),
        .stack_mem = g_sa_task_stack,
        .stack_size = sizeof(g_sa_task_stack),
        .priority = SA_TASK_PRIORITY};

    osThreadNew(saTask, NULL, &task_attr);
}
