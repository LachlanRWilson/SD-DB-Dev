/*
 * sa_demo.c - on-hardware demonstration of the name-ordered sorted array
 * (sorted_array.c), shown on the LCD.
 *
 * Not a test suite (that's sa_main.c): it walks through how the phone uses the
 * sorted array for the contact list, with every contact stored on the SD card
 * through the hash table.
 *
 *   1. ADD       insert contacts in scrambled order, showing where each lands
 *   2. LIST      page through the list with sorted_array_get_range()
 *   3. SEARCH    jump to a prefix with sorted_array_seek()
 *   4. RENAME    hash_update_contact(), then remove + insert (key changed)
 *   5. PHONE     remove, hash_update_phone(), insert at the new hash slot
 *   6. DELETE    remove from the array, then hash_remove_contact()
 *   7. REBOOT    rebuild the hash table from the card, refill the array with
 *                append + sorted_array_sort(), and list it again
 *
 * Each list row is green when it is the contact expected at that rank, red
 * if not. The footer shows the number of correct rows.
 *
 * Formats the usage bitmap and journal, so it wipes the DB_TEST_WRITE and
 * persistence demo data sets.
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
#include "lcd_log.h"
#include "sa_demo.h"

#define DEMO_TASK_STACK_SIZE (8 * 1024)
#define DEMO_TASK_PRIORITY   osPriorityNormal

/* Pause after each row so the screen can be followed live. */
#define DEMO_STEP_MS    300
#define DEMO_SECTION_MS 1500

/* Contacts per sorted_array_get_range() call. */
#define DEMO_PAGE 5

/* Message allocator is never allocated from, the hash table just needs one. */
#define DEMO_MESSAGE_FLS_SIZE 16

/* Defined in hash_table.c but not declared in hash_table.h. */
STRG_RET hash_update_phone(HashTable *table, Journal *journal, const char *old_phone, Contact *new_con);

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Data set
 * ------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    const char *phone;
} Person;

/* Inserted in this order. Covers the tiebreaks: names sharing an 8 character
 * key, the same name in a different case, and the same name twice. */
static const Person g_people[] = {
    { "Zoe Park",        "0411000001" },
    { "alice Brown",     "0411000002" },
    { "Bob Smith",       "0411000003" },
    { "Alexander Jones", "0411000004" },
    { "Alexandra Lee",   "0411000005" },
    { "Charlie Day",     "0411000006" },
    { "bob smith",       "0411000007" },
    { "Alex Chen",       "0411000008" },
    { "Dana Scully",     "0411000009" },
    { "Bob Smith",       "0411000010" },
};
#define N_PEOPLE (sizeof(g_people) / sizeof(g_people[0]))

/* Name (case-insensitive), then exact case ('B' < 'b'), then phone. */
static const Person g_sorted[] = {
    { "Alex Chen",       "0411000008" },
    { "Alexander Jones", "0411000004" },
    { "Alexandra Lee",   "0411000005" },
    { "alice Brown",     "0411000002" },
    { "Bob Smith",       "0411000003" },
    { "Bob Smith",       "0411000010" },
    { "bob smith",       "0411000007" },
    { "Charlie Day",     "0411000006" },
    { "Dana Scully",     "0411000009" },
    { "Zoe Park",        "0411000001" },
};

/* RENAME: Zoe Park -> Aaron Park moves from the end to the front */
static const Person g_rename_old = { "Zoe Park",   "0411000001" };
static const Person g_rename_new = { "Aaron Park", "0411000001" };

/* PHONE: the first Bob Smith gets a new number, so it moves behind the other */
static const Person g_phone_old = { "Bob Smith", "0411000003" };
static const Person g_phone_new = { "Bob Smith", "0411000099" };

/* DELETE */
static const Person g_delete = { "Dana Scully", "0411000009" };

static const Person g_final[] = {
    { "Aaron Park",      "0411000001" },
    { "Alex Chen",       "0411000008" },
    { "Alexander Jones", "0411000004" },
    { "Alexandra Lee",   "0411000005" },
    { "alice Brown",     "0411000002" },
    { "Bob Smith",       "0411000010" },
    { "Bob Smith",       "0411000099" },
    { "bob smith",       "0411000007" },
    { "Charlie Day",     "0411000006" },
};
#define N_FINAL (sizeof(g_final) / sizeof(g_final[0]))

_Static_assert(sizeof(g_sorted) == sizeof(g_people), "g_sorted must hold every contact");

/* ------------------------------------------------------------------ *
 *  RAM backing (too large for DTCM / the task stack -> RAM D1)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_contact_fls_mem[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static SortedArrayEntry g_sa_mem[SORTED_ARRAY_ENTRIES];

static uint16_t g_message_fls_mem[DEMO_MESSAGE_FLS_SIZE];

static Storage *const storage = &sd_storage;

static SDStorageContext g_sd_ctx;
static Journal          g_journal;
static HashTable        g_table;
static FreeList         g_contact_fls;
static FreeList         g_message_fls;
static SortedArray      g_sa;

static uint32_t g_ok;
static uint32_t g_bad;

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
#define SECTION(...) lcd_log_line(LOG_COLOR_SECTION, __VA_ARGS__)

static void tally(bool ok)
{
    if (ok) { g_ok++; } else { g_bad++; }
}

static bool storage_bringup(void)
{
    if (!SDStorage_Init(&g_sd_ctx, 0, SECTOR_SIZE))
    {
        return false;
    }
    storage->context = (void *)&g_sd_ctx;
    return true;
}

/* Empty usage bitmap, journal, hash table and sorted array. */
static bool format(void)
{
    memset(g_entries, 0, sizeof(g_entries));

    if (!init_usage_bitmap(storage)) { return false; }

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))        { return false; }
    if (!free_list_init(&g_message_fls, g_message_fls_mem, DEMO_MESSAGE_FLS_SIZE)) { return false; }

    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);
    return sorted_array_init(&g_sa, g_entries, storage, g_sa_mem);
}

/* Hash slot holding `phone`, UINT16_MAX if it isn't stored. */
static uint16_t slot_of(const char *phone)
{
    HashEntry *e = NULL;
    if (!hash_find_entry(&g_table, phone, &e) || (e == NULL) || (e->state != ENTRY_OCCUPIED))
    {
        return UINT16_MAX;
    }
    return (uint16_t)(e - g_entries);
}

/* Rank of a contact in the array, SIZE_MAX if it isn't there. */
static size_t rank_of(const Person *p)
{
    ContactBuffer c = create_contact(p->name, p->phone);
    size_t rank;
    bool match = false;
    if ((sorted_array_find(&g_sa, &c.contact, &rank, &match) != STRG_OK) || !match)
    {
        return SIZE_MAX;
    }
    return rank;
}

/* Read the contact the entry points at back from the card. */
static bool read_entry(const SortedArrayEntry *e, Contact *out)
{
    ContactBuffer c;
    if (read_contact(storage, g_entries[e->hash_slot].sector, &c) != STRG_OK)
    {
        return false;
    }
    *out = c.contact;
    return true;
}

static bool contact_is(const Contact *c, const Person *p)
{
    size_t nl = strlen(p->name);
    size_t pl = strlen(p->phone);
    return (c->name_len == nl) && (memcmp(c->name, p->name, nl) == 0) &&
           (c->phone_len == pl) && (memcmp(c->phone, p->phone, pl) == 0);
}

/* Print one list row: rank, name, phone. */
static void show_row(uint16_t color, size_t rank, const Contact *c)
{
    lcd_log_line(color, " %2u %-16.*s %.*s", (unsigned)rank,
                 (int)c->name_len, c->name, (int)c->phone_len, c->phone);
}

/* Page through the whole list and check each rank against `want`. */
static void show_list(const Person *want, size_t n)
{
    SortedArrayEntry page[DEMO_PAGE];
    size_t start = 0;
    size_t count = 0;

    while ((start < g_sa.occupancy) && sorted_array_get_range(&g_sa, start, DEMO_PAGE, &count, page))
    {
        lcd_log_line(LOG_COLOR_DIM, " get_range(%u, %u) -> %u", (unsigned)start, DEMO_PAGE, (unsigned)count);
        for (size_t i = 0; i < count; i++)
        {
            size_t rank = start + i;
            Contact c;
            bool read_ok = read_entry(&page[i], &c);
            bool ok = read_ok && (rank < n) && contact_is(&c, &want[rank]);
            if (read_ok)
            {
                show_row(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, rank, &c);
            }
            else
            {
                lcd_log_line(LOG_COLOR_FAIL, " %2u <read failed>", (unsigned)rank);
            }
            tally(ok);
            osDelay(DEMO_STEP_MS);
        }
        start += count;
    }

    bool size_ok = (g_sa.occupancy == n);
    lcd_log_line(size_ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " %u contacts (want %u)",
                 g_sa.occupancy, (unsigned)n);
    tally(size_ok);
}

static bool starts_with_nocase(const Contact *c, const char *prefix)
{
    size_t n = strlen(prefix);
    if (c->name_len < n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        char a = c->name[i];
        char b = prefix[i];
        if ((a >= 'A') && (a <= 'Z')) { a = (char)(a + ('a' - 'A')); }
        if ((b >= 'A') && (b <= 'Z')) { b = (char)(b + ('a' - 'A')); }
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

/* Seek to `prefix` and show every contact that starts with it. */
static void search(const char *prefix, size_t want_rank, size_t want_count)
{
    size_t rank = SIZE_MAX;
    size_t found = 0;

    bool ok = (sorted_array_seek(&g_sa, prefix, &rank) == STRG_OK) && (rank == want_rank);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " seek(\"%s\") -> rank %u", prefix, (unsigned)rank);
    tally(ok);
    if (rank == SIZE_MAX)
    {
        return;
    }

    for (size_t r = rank; r < g_sa.occupancy; r++)
    {
        Contact c;
        if (!read_entry(&g_sa.mem[r], &c) || !starts_with_nocase(&c, prefix))
        {
            break;
        }
        show_row(LOG_COLOR_TEXT, r, &c);
        found++;
        osDelay(DEMO_STEP_MS);
    }

    ok = (found == want_count);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " %u match%s", (unsigned)found, found == 1 ? "" : "es");
    tally(ok);
}

/* Entry for the contact in `slot`, keyed the same way as make_key() in
 * sorted_array.c: the first KEY_LEN characters, lower case, zero padded. */
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

/* ================================================================== *
 *  Steps
 * ================================================================== */
static void step_add(void)
{
    SECTION("1. ADD (scrambled order)");
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        const Person *p = &g_people[i];
        ContactBuffer c = create_contact(p->name, p->phone);
        uint16_t slot = UINT16_MAX;

        bool ok = (hash_insert_contact(&g_table, &g_journal, &c, &slot) == STRG_OK) &&
                  (sorted_array_insert(&g_sa, &c.contact, slot) == STRG_OK);
        size_t rank = ok ? rank_of(p) : SIZE_MAX;
        ok = ok && (rank != SIZE_MAX);

        lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " + %-16s -> %u of %u",
                     p->name, (unsigned)rank, g_sa.occupancy);
        tally(ok);
        osDelay(DEMO_STEP_MS);
    }
    osDelay(DEMO_SECTION_MS);
}

static void step_list(void)
{
    SECTION("2. CONTACT LIST");
    show_list(g_sorted, N_PEOPLE);
    osDelay(DEMO_SECTION_MS);
}

static void step_search(void)
{
    SECTION("3. SEARCH");
    search("ali", 3, 1);
    search("bob", 4, 3);
    search("alexand", 1, 2);
    osDelay(DEMO_SECTION_MS);
}

/* The name is the sort key: update it in the hash table (same phone, same
 * slot), then move the entry by removing the old name and inserting the new. */
static void step_rename(void)
{
    ContactBuffer old_c = create_contact(g_rename_old.name, g_rename_old.phone);
    ContactBuffer new_c = create_contact(g_rename_new.name, g_rename_new.phone);
    size_t before = rank_of(&g_rename_old);
    uint16_t slot = slot_of(g_rename_old.phone);

    SECTION("4. RENAME");
    lcd_log_line(LOG_COLOR_TEXT, " %s -> %s", g_rename_old.name, g_rename_new.name);

    /* Remove first: the tiebreak reads the stored (old) name from the card */
    bool ok = sorted_array_remove(&g_sa, &old_c.contact) &&
              (hash_update_contact(&g_table, &g_journal, &old_c.contact, &new_c.contact) == STRG_OK) &&
              (slot_of(g_rename_new.phone) == slot) &&
              (sorted_array_insert(&g_sa, &new_c.contact, slot) == STRG_OK);
    size_t after = rank_of(&g_rename_new);

    ok = ok && (after == 0);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " rank %u -> %u, slot %u kept",
                 (unsigned)before, (unsigned)after, slot);
    tally(ok);
    osDelay(DEMO_SECTION_MS);
}

/* The phone picks the hash slot, so the entry has to point at the new slot.
 * Phone is also the last tiebreak, so a same-name contact can change rank. */
static void step_phone(void)
{
    ContactBuffer old_c = create_contact(g_phone_old.name, g_phone_old.phone);
    ContactBuffer new_c = create_contact(g_phone_new.name, g_phone_new.phone);
    size_t before = rank_of(&g_phone_old);
    uint16_t old_slot = slot_of(g_phone_old.phone);

    SECTION("5. CHANGE PHONE");
    lcd_log_line(LOG_COLOR_TEXT, " %s %s", g_phone_old.name, g_phone_old.phone);
    lcd_log_line(LOG_COLOR_TEXT, "   -> %s", g_phone_new.phone);

    /* Remove before the hash update, while the old slot and stored phone are valid */
    bool ok = sorted_array_remove(&g_sa, &old_c.contact) &&
              (hash_update_phone(&g_table, &g_journal, g_phone_old.phone, &new_c.contact) == STRG_OK);
    uint16_t new_slot = slot_of(g_phone_new.phone);
    ok = ok && (new_slot != UINT16_MAX) &&
         (sorted_array_insert(&g_sa, &new_c.contact, new_slot) == STRG_OK);
    size_t after = rank_of(&g_phone_new);

    ok = ok && (after == before + 1);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " rank %u -> %u, slot %u -> %u",
                 (unsigned)before, (unsigned)after, old_slot, new_slot);
    tally(ok);
    osDelay(DEMO_SECTION_MS);
}

static void step_delete(void)
{
    ContactBuffer c = create_contact(g_delete.name, g_delete.phone);
    ContactBuffer removed;
    size_t before = rank_of(&g_delete);

    SECTION("6. DELETE");
    lcd_log_line(LOG_COLOR_TEXT, " %s (rank %u)", g_delete.name, (unsigned)before);

    /* Remove from the array first, the tiebreak still needs the contact */
    bool ok = sorted_array_remove(&g_sa, &c.contact) &&
              hash_remove_contact(&g_table, &g_journal, g_delete.phone, &removed) &&
              (rank_of(&g_delete) == SIZE_MAX);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " removed, %u contacts left", g_sa.occupancy);
    tally(ok);

    SECTION("   LIST AFTER EDITS");
    show_list(g_final, N_FINAL);
    osDelay(DEMO_SECTION_MS);
}

/* Boot path: only the card survives. Rebuild the hash table from it, then
 * append every contact unsorted and sort once. */
static void step_reboot(void)
{
    uint32_t t0;
    bool ok;

    SECTION("7. REBOOT (RAM wiped)");

    memset(g_entries, 0, sizeof(g_entries));
    memset(g_sa_mem, 0, sizeof(g_sa_mem));
    memset(usage_bitmap, 0, sizeof(usage_bitmap));

    ok = free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE);
    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);
    ok = ok && read_usage_bitmap(storage) && hash_reconstruct_contact(&g_table);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " hash table rebuilt: %u contacts",
                 (unsigned)hash_size(&g_table));
    tally(ok);
    if (!ok)
    {
        return;
    }

    ok = sorted_array_init(&g_sa, g_entries, storage, g_sa_mem);
    for (uint16_t slot = 0; ok && (slot < HASH_TABLE_SIZE); slot++)
    {
        SortedArrayEntry e;
        if (g_entries[slot].state != ENTRY_OCCUPIED)
        {
            continue;
        }
        ok = entry_for_slot(slot, &e) && sorted_array_append(&g_sa, &e);
    }
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " appended %u (hash slot order)", g_sa.occupancy);
    tally(ok);

    t0 = osKernelGetTickCount();
    ok = ok && (sorted_array_sort(&g_sa) == STRG_OK);
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " sorted in %lu ms",
                 (unsigned long)(osKernelGetTickCount() - t0));
    tally(ok);
    osDelay(DEMO_STEP_MS);

    SECTION("   LIST AFTER REBOOT");
    show_list(g_final, N_FINAL);
}

static void run_demo(void)
{
    char text[24];

    lcd_log_init("SORTED ARRAY");
    lcd_log_footer("running...", LOG_COLOR_RUN);

    if (!storage_bringup() || !format())
    {
        lcd_log_line(LOG_COLOR_FAIL, "SD card init/format failed");
        lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
        return;
    }
    lcd_log_line(LOG_COLOR_DIM, "%u contacts, %u-char keys", (unsigned)N_PEOPLE, KEY_LEN);

    step_add();
    step_list();
    step_search();
    step_rename();
    step_phone();
    step_delete();
    step_reboot();

    snprintf(text, sizeof(text), "%lu/%lu OK", (unsigned long)g_ok, (unsigned long)(g_ok + g_bad));
    lcd_log_footer(text, g_bad == 0 ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

/* ================================================================== *
 *  Task
 * ================================================================== */
static void demoTask(void *arg)
{
    (void)arg;

    run_demo();

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_demo_task_cb;
static uint8_t g_demo_task_stack[DEMO_TASK_STACK_SIZE] __attribute__((aligned(8)));

void SA_Demo_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "SA Demo",
        .cb_mem = &g_demo_task_cb,
        .cb_size = sizeof(g_demo_task_cb),
        .stack_mem = g_demo_task_stack,
        .stack_size = sizeof(g_demo_task_stack),
        .priority = DEMO_TASK_PRIORITY};

    osThreadNew(demoTask, NULL, &task_attr);
}
