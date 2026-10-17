/*
 * db_demo.c - on-hardware walk-through of the whole database (database.c),
 * shown on the LCD and stepped with the blue USER button (B1, PC13).
 *
 * Not a test suite (that's database_main.c): it shows an examiner each part
 * of the database working on the real SD card, one section per screen.
 *
 *   0. START      mount the card (hold BLUE during reset for a full format)
 *   1. CONTACTS   add contacts in scrambled order (sector writes per add)
 *   2. LIST       the name index returns them alphabetically
 *   3. SEARCH     prefix search with the name index
 *   4. RULES      duplicate phone, invalid input, unknown contact
 *   5. MESSAGES   chats spanning message sectors, unknown sender
 *   6. HISTORY    latest messages across every chat
 *   7. CALLS      call history, known and unknown callers
 *   8. UPDATE     rename (moves in the list), new number (chat follows)
 *   9. DELETE     remove a contact (chat goes too), remove a chat
 *  10. REBOOT     wipe RAM, mount from the card, everything is rebuilt
 *  11. POWER CUT  cut power part way through a write, journal rollback
 *  12. SCALE      bulk add, timing, rebuild time
 *  13. SUMMARY
 *
 * Every check is printed green (OK) or red (FAIL); the last screen shows the
 * totals. The card is reset to an empty database at the start (usage bitmap,
 * journal and the start of each history ring), so it wipes any other demo or
 * test data set.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "database.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "ring_buffer.h"
#include "free_list_stack.h"
#include "storage.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "lcd_log.h"
#include "db_demo.h"

#define DEMO_TASK_STACK_SIZE (8 * 1024)
#define DEMO_TASK_PRIORITY   osPriorityNormal

/* Pause after each row so the screen can be followed live */
#define DEMO_ROW_MS 120

/* USER button B1 on the NUCLEO-H723ZG (pressed = high) */
#define BTN_PORT        GPIOC
#define BTN_PIN         GPIO_PIN_13
#define BTN_DEBOUNCE_MS 30
#define BTN_SAMPLES     5   // reads in a row, 10 ms apart, that must all be pressed

/* History ring sectors reset at start (far more than the demo writes) */
#define DEMO_RING_CLEAR_SECTORS 64

/* Contacts added in the SCALE section */
#define DEMO_SCALE_N 200

#define DEMO_SECTIONS 13

/* ------------------------------------------------------------------ *
 *  Data set
 * ------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    const char *phone;
} Person;

/* Added in this order. Covers names sharing the 8 character key, a lower
 * case name and the same name twice (ordered by phone). */
static const Person g_people[] = {
    { "Zoe Park",        "0411000001" },
    { "alice Brown",     "0411000002" },
    { "Bob Smith",       "0411000003" },
    { "Alexandra Lee",   "0411000004" },
    { "Alexander Jones", "0411000005" },
    { "Charlie Day",     "0411000006" },
    { "Dana Scully",     "0411000007" },
    { "Bob Smith",       "0411000008" },
};
#define N_PEOPLE (sizeof(g_people) / sizeof(g_people[0]))

static const Person g_sorted[] = {
    { "Alexander Jones", "0411000005" },
    { "Alexandra Lee",   "0411000004" },
    { "alice Brown",     "0411000002" },
    { "Bob Smith",       "0411000003" },
    { "Bob Smith",       "0411000008" },
    { "Charlie Day",     "0411000006" },
    { "Dana Scully",     "0411000007" },
    { "Zoe Park",        "0411000001" },
};

#define ALICE        "0411000002"
#define ALICE_NEW    "0411000022"
#define BOB          "0411000003"
#define ZOE          "0411000001"
#define CHARLIE      "0411000006"
#define UNKNOWN_SMS  "0499000000"
#define UNKNOWN_CALL "0488000000"

/* After UPDATE (Zoe -> Aaron, Alice's new number) and DELETE (first Bob) */
static const Person g_final[] = {
    { "",                UNKNOWN_SMS  },
    { "Aaron Park",      ZOE          },
    { "Alexander Jones", "0411000005" },
    { "Alexandra Lee",   "0411000004" },
    { "alice Brown",     ALICE_NEW    },
    { "Bob Smith",       "0411000008" },
    { "Charlie Day",     CHARLIE      },
    { "Dana Scully",     "0411000007" },
};
#define N_FINAL (sizeof(g_final) / sizeof(g_final[0]))

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */
static Database         g_db;
static SDStorageContext g_sd_ctx;
static Storage          g_real;       // the SD storage interface database_init installed
static uint32_t         g_reads;
static uint32_t         g_writes;
static int              g_writes_left = -1; // power cut budget, -1 = never
static uint32_t         g_ts;               // message timestamps (no RTC)
static uint32_t         g_ok;
static uint32_t         g_bad;

static ContactBuffer       g_page[8];
static Message             g_msgs[8];
static MessageHistoryEntry g_hist[8];
static CallHistoryEntry    g_calls[8];

/* ------------------------------------------------------------------ *
 *  Storage wrapper: counts sector reads/writes and simulates power cuts
 * ------------------------------------------------------------------ */
static bool Demo_ReadBlock(void *context, uint32_t index, uint8_t *out)
{
    g_reads++;
    return g_real.read_block(context, index, out);
}

static bool Demo_ReadMultiBlock(void *context, uint32_t index, size_t n, uint8_t *out)
{
    g_reads += (uint32_t)n;
    return g_real.read_multiblock(context, index, n, out);
}

static bool Demo_WriteBlock(void *context, uint32_t index, uint8_t *in)
{
    if (g_writes_left == 0)
    {
        return false; // power is off
    }
    if (g_writes_left > 0)
    {
        g_writes_left--;
    }
    g_writes++;
    return g_real.write_block(context, index, in);
}

static bool Demo_WriteMultiBlock(void *context, uint32_t index, size_t n, uint8_t *in)
{
    for (size_t i = 0; i < n; i++)
    {
        if (!Demo_WriteBlock(context, index + (uint32_t)i, in + (i * SECTOR_SIZE)))
        {
            return false;
        }
    }
    return true;
}

/* Install the wrapper over whatever database_init put in g_db.storage */
static void wrap_storage(void)
{
    g_real = g_db.storage;
    g_db.storage.read_block = Demo_ReadBlock;
    g_db.storage.read_multiblock = Demo_ReadMultiBlock;
    g_db.storage.write_block = Demo_WriteBlock;
    g_db.storage.write_multiblock = Demo_WriteMultiBlock;
}

/* ------------------------------------------------------------------ *
 *  Button and screen helpers
 * ------------------------------------------------------------------ */
static void button_init(void)
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    init.Pin = BTN_PIN;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(BTN_PORT, &init);
}

static bool button_down(void)
{
    return HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_SET;
}

/* True if the button reads pressed on BTN_SAMPLES reads in a row. PC13 sits
 * next to the SD clock (PC12), so a single high read can be noise */
static bool button_held(void)
{
    for (int i = 0; i < BTN_SAMPLES; i++)
    {
        if (!button_down())
        {
            return false;
        }
        osDelay(10);
    }
    return true;
}

/* Block until the button is pressed and released (debounced) */
static void wait_button(const char *footer)
{
    lcd_log_footer(footer, LOG_COLOR_RUN);

    for (;;)
    {
        while (!button_down())
        {
            osDelay(10);
        }
        if (button_held())
        {
            break;
        }
    }
    while (button_down())
    {
        osDelay(10);
    }
    osDelay(BTN_DEBOUNCE_MS);
}

/* Start a section on a fresh screen */
static void section(uint32_t n, const char *title, const char *what)
{
    char header[24];

    snprintf(header, sizeof(header), "%lu/%u %s", (unsigned long)n, DEMO_SECTIONS, title);
    lcd_log_init(header);
    lcd_log_footer("running...", LOG_COLOR_RUN);
    lcd_log_line(LOG_COLOR_DIM, "%s", what);
}

/* Record a check and print it green/red */
static bool check(bool ok, const char *what)
{
    if (ok)
    {
        g_ok++;
    }
    else
    {
        g_bad++;
    }
    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, "%s %s", ok ? "OK  " : "FAIL", what);
    osDelay(DEMO_ROW_MS);
    return ok;
}

static const char *strg_name(STRG_RET ret)
{
    switch (ret)
    {
        case STRG_FAIL:    return "FAIL";
        case STRG_OK:      return "OK";
        case STRG_EMPTY:   return "EMPTY";
        case STRG_FULL:    return "FULL";
        case STRG_CORRUPT: return "CORRUPT";
        case STRG_EXISTS:  return "EXISTS";
        default:           return "?";
    }
}

/* Contact name for printing ("(unnamed)" for a number-only contact) */
static const char *name_of(const Contact *c, char out[MAX_NAME_LEN + 1])
{
    if (c->name_len == 0)
    {
        return "(unnamed)";
    }
    memcpy(out, c->name, c->name_len);
    out[c->name_len] = '\0';
    return out;
}

static bool contact_is(const Contact *c, const char *name, const char *phone)
{
    size_t nl = strlen(name);
    size_t pl = strlen(phone);
    return (c->name_len == nl) && (memcmp(c->name, name, nl) == 0) &&
           (c->phone_len == pl) && (memcmp(c->phone, phone, pl) == 0);
}

/* Print n contacts from rank start: "rank name phone" */
static void show_list(size_t start, size_t n)
{
    char name[MAX_NAME_LEN + 1];
    size_t count;

    while (n > 0)
    {
        size_t want = (n < 8) ? n : 8;
        if (database_contact_list(&g_db, start, want, &count, g_page) != STRG_OK || count == 0)
        {
            return;
        }
        for (size_t i = 0; i < count; i++)
        {
            const Contact *c = &g_page[i].contact;
            lcd_log_line(LOG_COLOR_TEXT, "%2u %-15.15s %.*s", (unsigned)(start + i), name_of(c, name),
                         c->phone_len, c->phone);
            osDelay(DEMO_ROW_MS);
        }
        start += count;
        n -= count;
    }
}

/* The whole list is exactly expect[] in order */
static bool list_is(const Person *expect, size_t n)
{
    size_t count;

    if (database_contact_count(&g_db) != n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (database_contact_list(&g_db, i, 1, &count, g_page) != STRG_OK || count != 1 ||
            !contact_is(&g_page[0].contact, expect[i].name, expect[i].phone))
        {
            return false;
        }
    }
    return true;
}

/* Every listed contact can be found by phone, and list size == count */
static bool list_consistent(void)
{
    char phone[MAX_PHONE_LEN + 1];
    ContactBuffer found;
    size_t count;
    size_t i = 0;

    while (database_contact_list(&g_db, i, 1, &count, g_page) == STRG_OK && count == 1)
    {
        memcpy(phone, g_page[0].contact.phone, g_page[0].contact.phone_len);
        phone[g_page[0].contact.phone_len] = '\0';
        if (database_contact_get(&g_db, phone, &found) != STRG_OK)
        {
            return false;
        }
        i++;
    }
    return i == database_contact_count(&g_db);
}

static size_t chat_len(const char *phone)
{
    size_t count = 0;
    if (database_message_get_chat(&g_db, phone, 0, 8, &count, g_msgs) != STRG_OK)
    {
        return 0;
    }
    return count;
}

/* Mount like a power cycle: wipe RAM state, database_init, re-wrap storage */
static STRG_RET boot(uint32_t *ms)
{
    uint32_t t0 = osKernelGetTickCount();

    g_writes_left = -1;
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    STRG_RET ret = database_init(&g_db);
    *ms = osKernelGetTickCount() - t0;
    wrap_storage();
    return ret;
}

/* Reset the card to an empty database without the slow full format: the
 * usage bitmap marks every data sector unused, a fresh journal, and empty
 * starts of both history rings */
static bool quick_reset(void)
{
    Journal journal;
    Storage *sd = &sd_storage;

    if (!SDStorage_Init(&g_sd_ctx, 0, SECTOR_SIZE))
    {
        return false;
    }
    sd->context = (void *)&g_sd_ctx;

    memset(&journal, 0, sizeof(journal));
    journal.storage = sd;

    return init_usage_bitmap(sd) && journal_header_init(&journal) &&
           (clear_ring_buffer(sd, DATA_SECTOR_TO_RAW(MESSAGE_HISTORY_DATA_START_SECTOR),
                              DEMO_RING_CLEAR_SECTORS) == STRG_OK) &&
           (clear_ring_buffer(sd, DATA_SECTOR_TO_RAW(CALL_HISTORY_DATA_START_SECTOR),
                              DEMO_RING_CLEAR_SECTORS) == STRG_OK);
}

/* ================================================================== *
 *  Sections
 * ================================================================== */
static bool step_start(void)
{
    uint32_t ms;
    STRG_RET ret;
    bool full_format = button_held(); // held during reset

    lcd_log_init("UniQOS DATABASE");
    lcd_log_line(LOG_COLOR_SECTION, "Contacts, chats, call and");
    lcd_log_line(LOG_COLOR_SECTION, "message history on SD card");
    lcd_log_line(LOG_COLOR_DIM, " ");
    lcd_log_line(LOG_COLOR_DIM, "STM32H723 @ %lu MHz", (unsigned long)(SystemCoreClock / 1000000U));
    lcd_log_line(LOG_COLOR_DIM, "SD 4-bit 6 MHz, 512 B sectors");
    lcd_log_line(LOG_COLOR_DIM, "max %u contacts", HASH_TABLE_ENTRIES);
    lcd_log_line(LOG_COLOR_DIM, " ");
    lcd_log_line(LOG_COLOR_TEXT, "Every check prints OK / FAIL");
    lcd_log_line(LOG_COLOR_TEXT, "BLUE button = next section");
    lcd_log_line(LOG_COLOR_DIM, " ");

    if (full_format)
    {
        lcd_log_line(LOG_COLOR_RUN, "Full format (~35 s)...");
        lcd_log_footer("formatting...", LOG_COLOR_RUN);
        while (button_down())
        {
            osDelay(10);
        }
        boot(&ms);
        uint32_t t0 = osKernelGetTickCount();
        ret = database_format(&g_db);
        ms = osKernelGetTickCount() - t0;
        wrap_storage();
        lcd_log_line(LOG_COLOR_TEXT, "format: %s in %lu ms", strg_name(ret), (unsigned long)ms);
    }
    else
    {
        lcd_log_line(LOG_COLOR_RUN, "Resetting card to empty DB");
        if (!quick_reset())
        {
            check(false, "SD card reset");
            lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
            return false;
        }
        ret = boot(&ms);
        if (ret != STRG_OK)
        {
            // blank card or old layout: build it properly
            lcd_log_line(LOG_COLOR_RUN, "mount %s, formatting...", strg_name(ret));
            ret = database_format(&g_db);
            wrap_storage();
        }
        lcd_log_line(LOG_COLOR_TEXT, "mounted in %lu ms", (unsigned long)ms);
    }

    if (!check((ret == STRG_OK) && (database_contact_count(&g_db) == 0), "empty database ready"))
    {
        lcd_log_footer("DB FAIL", LOG_COLOR_FAIL);
        return false;
    }
    return true;
}

static void step_contacts(void)
{
    char line[40];

    section(1, "CONTACTS", "Add in scrambled order:");
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        uint32_t w0 = g_writes;
        STRG_RET ret = database_contact_add(&g_db, g_people[i].name, g_people[i].phone);
        lcd_log_line((ret == STRG_OK) ? LOG_COLOR_TEXT : LOG_COLOR_FAIL, "+ %-15.15s %s %luw",
                     g_people[i].name, strg_name(ret), (unsigned long)(g_writes - w0));
        osDelay(DEMO_ROW_MS);
        if (ret == STRG_OK) { g_ok++; } else { g_bad++; }
    }
    lcd_log_line(LOG_COLOR_DIM, "w = sector writes: journal,");
    lcd_log_line(LOG_COLOR_DIM, "contact sector, usage bitmap");
    snprintf(line, sizeof(line), "%u contacts stored", (unsigned)N_PEOPLE);
    check(database_contact_count(&g_db) == N_PEOPLE, line);
}

static void step_list(void)
{
    section(2, "LIST", "Name index (sorted array):");
    show_list(0, N_PEOPLE);
    lcd_log_line(LOG_COLOR_DIM, "case-insensitive; same name");
    lcd_log_line(LOG_COLOR_DIM, "ordered by phone number");
    check(list_is(g_sorted, N_PEOPLE), "alphabetical order");
    check(list_consistent(), "index matches hash table");
}

static void search(const char *prefix, size_t expect_rank)
{
    char what[40];
    size_t rank = 0;

    STRG_RET ret = database_contact_seek(&g_db, prefix, &rank);
    snprintf(what, sizeof(what), "\"%s\" -> rank %u", prefix, (unsigned)rank);
    check((ret == STRG_OK) && (rank == expect_rank), what);
    show_list(rank, 2);
}

static void step_search(void)
{
    section(3, "SEARCH", "First name >= prefix:");
    search("al", 0);
    search("B", 3);
    search("da", 6);
    search("zz", 8);
    lcd_log_line(LOG_COLOR_DIM, "\"zz\": after every name, so end");
}

static void step_rules(void)
{
    ContactBuffer out;
    char long_name[MAX_NAME_LEN + 2];

    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    section(4, "RULES", "Phone number is the key:");
    STRG_RET ret = database_contact_add(&g_db, "Someone Else", BOB);
    lcd_log_line(LOG_COLOR_TEXT, "add dup %s -> %s", BOB, strg_name(ret));
    check(ret == STRG_EXISTS, "duplicate phone rejected");

    ret = database_contact_add(&g_db, "No Number", "");
    lcd_log_line(LOG_COLOR_TEXT, "add empty phone -> %s", strg_name(ret));
    check(ret == STRG_FAIL, "empty phone rejected");

    ret = database_contact_add(&g_db, long_name, "0411000099");
    lcd_log_line(LOG_COLOR_TEXT, "add 65 char name -> %s", strg_name(ret));
    check(ret == STRG_FAIL, "name > 64 chars rejected");

    ret = database_contact_get(&g_db, "0400000000", &out);
    lcd_log_line(LOG_COLOR_TEXT, "get 0400000000 -> %s", strg_name(ret));
    check(ret == STRG_EMPTY, "unknown number not found");

    check(database_contact_count(&g_db) == N_PEOPLE, "nothing was changed");
}

static void show_chat(const char *phone)
{
    size_t count = 0;

    database_message_get_chat(&g_db, phone, 0, 8, &count, g_msgs);
    for (size_t i = count; i-- > 0;)
    {
        lcd_log_line(LOG_COLOR_TEXT, " %s %.25s", g_msgs[i].direction ? "->" : "<-", g_msgs[i].str);
        osDelay(DEMO_ROW_MS);
    }
}

static void step_messages(void)
{
    char what[40];
    size_t count;
    ContactBuffer out;

    section(5, "MESSAGES", "alice's chat (3 per sector):");
    database_message_add(&g_db, ALICE, ++g_ts, true, (char *)"Lunch tomorrow?");
    database_message_add(&g_db, ALICE, ++g_ts, false, (char *)"Sure, 12:30?");
    database_message_add(&g_db, BOB, ++g_ts, false, (char *)"Did you push the fix?");
    database_message_add(&g_db, ALICE, ++g_ts, true, (char *)"Perfect, see you then");
    database_message_add(&g_db, ALICE, ++g_ts, false, (char *)"Bring the slides");
    show_chat(ALICE);
    count = chat_len(ALICE);
    snprintf(what, sizeof(what), "alice: %u msgs, 2 sectors", (unsigned)count);
    check(count == 4, what);
    check(chat_len(BOB) == 1, "bob's chat kept separate");

    lcd_log_line(LOG_COLOR_DIM, "SMS from an unknown number:");
    database_message_add(&g_db, UNKNOWN_SMS, ++g_ts, false, (char *)"Your parcel is on its way");
    show_chat(UNKNOWN_SMS);
    check((database_contact_get(&g_db, UNKNOWN_SMS, &out) == STRG_OK) && (out.contact.name_len == 0),
          "unnamed contact created");
    check(database_contact_count(&g_db) == N_PEOPLE + 1, "listed first (no name)");
}

static void step_history(void)
{
    char name[MAX_NAME_LEN + 1];
    size_t count = 0;

    section(6, "HISTORY", "Latest messages, all chats:");
    STRG_RET ret = database_message_recent(&g_db, 0, 6, &count, g_hist);
    for (size_t i = 0; i < count; i++)
    {
        // which chat: the message sector header holds the contact index
        MessageSectorBuffer sector;
        ContactBuffer c;
        const char *who = "?";
        if ((read_message_sector(&g_db.storage, g_hist[i].msgInd, &sector) == STRG_OK) &&
            (read_contact(&g_db.storage, sector.var.header.contact_index, &c) == STRG_OK))
        {
            who = name_of(&c.contact, name);
        }
        lcd_log_line(LOG_COLOR_TEXT, "%-9.9s %s %.15s", who, g_hist[i].message.direction ? "->" : "<-",
                     g_hist[i].message.str);
        osDelay(DEMO_ROW_MS);
    }
    lcd_log_line(LOG_COLOR_DIM, "ring buffer on the card,");
    lcd_log_line(LOG_COLOR_DIM, "newest first, wraps when full");
    check((ret == STRG_OK) && (count == 6), "6 newest messages");
    check((count > 0) && (g_hist[0].message.timestamp_lo == (uint16_t)g_ts), "newest is the parcel SMS");
}

static void step_calls(void)
{
    char name[MAX_NAME_LEN + 1];
    size_t count = 0;

    section(7, "CALLS", "Log 3 calls:");
    database_call_add(&g_db, ZOE);
    database_call_add(&g_db, UNKNOWN_CALL);
    database_call_add(&g_db, CHARLIE);

    STRG_RET ret = database_call_recent(&g_db, 0, 8, &count, g_calls);
    for (size_t i = 0; i < count; i++)
    {
        lcd_log_line(LOG_COLOR_TEXT, "%u %-15.15s %.*s", (unsigned)i, name_of(&g_calls[i].contact, name),
                     g_calls[i].contact.phone_len, g_calls[i].contact.phone);
        osDelay(DEMO_ROW_MS);
    }
    check((ret == STRG_OK) && (count == 3), "3 calls, newest first");
    check((count == 3) && contact_is(&g_calls[0].contact, "Charlie Day", CHARLIE), "known caller has name");
    check((count == 3) && (g_calls[1].contact_index == UINT16_MAX), "unknown caller: no name");
    check(database_contact_count(&g_db) == N_PEOPLE + 1, "unknown caller not added");
}

static void step_update(void)
{
    ContactBuffer out;

    section(8, "UPDATE", "Rename Zoe Park -> Aaron Park:");
    check(database_contact_update(&g_db, ZOE, "Aaron Park", ZOE) == STRG_OK, "renamed");
    show_list(0, 3);
    lcd_log_line(LOG_COLOR_DIM, "moved from last to near top");

    lcd_log_line(LOG_COLOR_DIM, "alice gets a new number:");
    check(database_contact_update(&g_db, ALICE, "alice Brown", ALICE_NEW) == STRG_OK, ALICE " -> " ALICE_NEW);
    check(database_contact_get(&g_db, ALICE, &out) == STRG_EMPTY, "old number gone");
    check(chat_len(ALICE_NEW) == 4, "chat followed (4 msgs)");
}

static void step_delete(void)
{
    ContactBuffer out;
    size_t count = 0;

    section(9, "DELETE", "Remove first Bob Smith:");
    check(database_contact_remove(&g_db, BOB) == STRG_OK, "removed " BOB);
    check(database_contact_get(&g_db, BOB, &out) == STRG_EMPTY, "not found any more");
    check(database_message_get_chat(&g_db, BOB, 0, 8, &count, g_msgs) == STRG_EMPTY, "his chat was deleted");
    check(database_contact_get(&g_db, "0411000008", &out) == STRG_OK, "other Bob Smith kept");

    lcd_log_line(LOG_COLOR_DIM, "Delete parcel chat only:");
    check(database_message_remove_chat(&g_db, UNKNOWN_SMS) == STRG_OK, "chat removed");
    check(database_contact_get(&g_db, UNKNOWN_SMS, &out) == STRG_OK, "contact kept");

    lcd_log_line(LOG_COLOR_DIM, "List now:");
    show_list(0, N_FINAL);
    check(list_is(g_final, N_FINAL), "list as expected");
}

static void step_reboot(void)
{
    uint32_t ms;
    char what[40];

    section(10, "REBOOT", "Wipe RAM, mount from card:");
    STRG_RET ret = boot(&ms);
    snprintf(what, sizeof(what), "mounted in %lu ms", (unsigned long)ms);
    check(ret == STRG_OK, what);
    lcd_log_line(LOG_COLOR_DIM, "rebuilt from the card:");
    lcd_log_line(LOG_COLOR_DIM, " hash table, free lists,");
    lcd_log_line(LOG_COLOR_DIM, " name index, ring heads");
    check(list_is(g_final, N_FINAL), "same contacts, same order");
    check(chat_len(ALICE_NEW) == 4, "alice's chat intact");
    check(chat_len(UNKNOWN_SMS) == 0, "deleted chat still gone");
    check(free_list_available(&g_db.contact_allocator) + free_list_used(&g_db.contact_allocator) ==
          HASH_TABLE_ENTRIES, "contact free list exact");

    size_t count = 0;
    database_call_recent(&g_db, 0, 8, &count, g_calls);
    check(count == 3, "call history intact");
}

static void step_powercut(void)
{
    uint32_t ms;
    uint32_t w0;
    char what[40];
    size_t before = database_contact_count(&g_db);
    ContactBuffer out;

    section(11, "POWER CUT", "Every write is journalled:");
    w0 = g_writes;
    database_contact_add(&g_db, "Eve Normal", "0411000010");
    snprintf(what, sizeof(what), "normal add: %lu writes", (unsigned long)(g_writes - w0));
    lcd_log_line(LOG_COLOR_TEXT, "%s", what);
    before = database_contact_count(&g_db);

    lcd_log_line(LOG_COLOR_RUN, "Cut power after 2 writes:");
    g_writes_left = 2;
    STRG_RET ret = database_contact_add(&g_db, "Half Written", "0411000011");
    lcd_log_line(LOG_COLOR_TEXT, "add -> %s (power lost)", strg_name(ret));
    ret = boot(&ms);
    snprintf(what, sizeof(what), "reboot + rollback %lu ms", (unsigned long)ms);
    check(ret == STRG_OK, what);
    check(database_contact_count(&g_db) == before, "half add rolled back");
    check(database_contact_get(&g_db, "0411000011", &out) == STRG_EMPTY, "no trace of it");
    check(list_consistent(), "list consistent");

    lcd_log_line(LOG_COLOR_RUN, "Cut power after 6 writes:");
    g_writes_left = 6;
    ret = database_contact_add(&g_db, "Nearly Done", "0411000012");
    lcd_log_line(LOG_COLOR_TEXT, "add -> %s (power lost)", strg_name(ret));
    boot(&ms);
    bool landed = (database_contact_get(&g_db, "0411000012", &out) == STRG_OK);
    lcd_log_line(LOG_COLOR_TEXT, "after reboot: %s", landed ? "contact kept" : "rolled back");
    check(database_contact_count(&g_db) == before + (landed ? 1 : 0), "old or new, never half");
    check(list_consistent(), "list consistent");
}

static void step_scale(void)
{
    char name[24];
    char phone[16];
    char what[40];
    uint32_t ms;
    size_t start = database_contact_count(&g_db);
    uint32_t added = 0;

    section(12, "SCALE", "Bulk add contacts:");
    uint32_t t0 = osKernelGetTickCount();
    for (uint32_t i = 0; i < DEMO_SCALE_N; i++)
    {
        snprintf(name, sizeof(name), "Contact %03lu", (unsigned long)i);
        snprintf(phone, sizeof(phone), "0422%06lu", (unsigned long)i);
        if (database_contact_add(&g_db, name, phone) == STRG_OK)
        {
            added++;
        }
        if (((i + 1) % 50) == 0)
        {
            lcd_log_line(LOG_COLOR_TEXT, "  %lu added (%lu ms)", (unsigned long)(i + 1),
                         (unsigned long)(osKernelGetTickCount() - t0));
        }
    }
    uint32_t dt = osKernelGetTickCount() - t0;
    snprintf(what, sizeof(what), "%lu added, %lu ms each", (unsigned long)added,
             (unsigned long)(dt / DEMO_SCALE_N));
    check(added == DEMO_SCALE_N, what);

    STRG_RET ret = boot(&ms);
    snprintf(what, sizeof(what), "%u contacts mount %lu ms", (unsigned)database_contact_count(&g_db),
             (unsigned long)ms);
    check((ret == STRG_OK) && (database_contact_count(&g_db) == start + DEMO_SCALE_N), what);

    size_t rank = 0;
    database_contact_seek(&g_db, "contact 1", &rank);
    lcd_log_line(LOG_COLOR_DIM, "search \"contact 1\":");
    show_list(rank, 2);
    check(list_consistent(), "consistent after reboot");
}

static void step_summary(uint32_t elapsed_ms)
{
    char footer[24];

    lcd_log_init("13/13 SUMMARY");
    lcd_log_line(LOG_COLOR_SECTION, "Shown on the real SD card:");
    lcd_log_line(LOG_COLOR_TEXT, " contacts: add/get/update/rm");
    lcd_log_line(LOG_COLOR_TEXT, " sorted list + prefix search");
    lcd_log_line(LOG_COLOR_TEXT, " chats across message sectors");
    lcd_log_line(LOG_COLOR_TEXT, " message + call history rings");
    lcd_log_line(LOG_COLOR_TEXT, " reboot rebuild from card");
    lcd_log_line(LOG_COLOR_TEXT, " journal power-cut rollback");
    lcd_log_line(LOG_COLOR_DIM, " ");
    lcd_log_line(LOG_COLOR_TEXT, "%lu contacts on the card", (unsigned long)database_contact_count(&g_db));
    lcd_log_line(LOG_COLOR_TEXT, "%lu reads, %lu writes", (unsigned long)g_reads, (unsigned long)g_writes);
    lcd_log_line(LOG_COLOR_TEXT, "demo time %lu s", (unsigned long)(elapsed_ms / 1000U));
    lcd_log_line(LOG_COLOR_DIM, " ");
    lcd_log_line((g_bad == 0) ? LOG_COLOR_PASS : LOG_COLOR_FAIL, "%lu / %lu checks passed",
                 (unsigned long)g_ok, (unsigned long)(g_ok + g_bad));
    lcd_log_line(LOG_COLOR_DIM, "reset to run again");

    snprintf(footer, sizeof(footer), "%lu/%lu OK", (unsigned long)g_ok, (unsigned long)(g_ok + g_bad));
    lcd_log_footer(footer, (g_bad == 0) ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

/* ================================================================== *
 *  Task
 * ================================================================== */
static void run_demo(void)
{
    typedef void (*Step)(void);
    static const Step steps[] = {
        step_contacts, step_list, step_search, step_rules, step_messages, step_history,
        step_calls, step_update, step_delete, step_reboot, step_powercut, step_scale,
    };
    char footer[24];
    uint32_t t0 = osKernelGetTickCount();

    if (!step_start())
    {
        return;
    }

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    {
        snprintf(footer, sizeof(footer), "BLUE: next %u/%u", (unsigned)(i + 1), DEMO_SECTIONS);
        wait_button(footer);
        steps[i]();
    }

    wait_button("BLUE: summary");
    step_summary(osKernelGetTickCount() - t0);
}

static void demoTask(void *arg)
{
    (void)arg;

    button_init();
    run_demo();

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_demo_task_cb;
static uint8_t g_demo_task_stack[DEMO_TASK_STACK_SIZE] __attribute__((aligned(8)));

void DB_Demo_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "DB Demo",
        .cb_mem = &g_demo_task_cb,
        .cb_size = sizeof(g_demo_task_cb),
        .stack_mem = g_demo_task_stack,
        .stack_size = sizeof(g_demo_task_stack),
        .priority = DEMO_TASK_PRIORITY};

    osThreadNew(demoTask, NULL, &task_attr);
}
