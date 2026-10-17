/*
 * persist_demo.c - on-hardware demonstration of the database surviving a
 * power-cycle, shown on the LCD.
 *
 * Not a test suite (that's db_main.c): it writes one small, readable data set
 * to the SD card, and a second build reads it back from the card alone.
 *
 * ------------------------------------------------------------------------
 *  PERSIST_DEMO_WRITE - which half runs
 * ------------------------------------------------------------------------
 *   1 (default) : format the usage bitmap, journal and message history ring
 *                 buffer, then write g_people and g_msgs, showing each record
 *                 on the screen as it goes to the card.
 *   0           : write nothing (beyond any journal rollback); rebuild the
 *                 hash table, the message chats and the message history ring
 *                 buffer from the card, and show what was read. Each line is
 *                 green if it matches the data set the write build used, red
 *                 if not.
 *
 *   Flash with 1, power-cycle, flash with 0 (or power-cycle a 0 build).
 *
 *   cmake -S . -B build -DPERSIST_DEMO=ON -DPERSIST_DEMO_WRITE=ON
 *   cmake -S . -B build -DPERSIST_DEMO=ON -DPERSIST_DEMO_WRITE=OFF
 *
 * ------------------------------------------------------------------------
 *  Screen
 * ------------------------------------------------------------------------
 *   +--------------------------+
 *   | SD WRITE DEMO            |  header, full-size font (16x24)
 *   |--------------------------|
 *   | CONTACTS                 |  one row per record, half-size font
 *   |  Alice    0411111111     |  (8x12); > = sent, < = received
 *   |  ...                     |
 *   |--------------------------|
 *   | WROTE 10/10              |  footer: progress, then the result
 *   +--------------------------+
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hash_table.h"
#include "contact.h"
#include "message.h"
#include "message_history.h"
#include "ring_buffer.h"
#include "free_list_stack.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "lcd_log.h"
#include "persist_demo.h"
#include "msg_ts.h"

#ifndef PERSIST_DEMO_WRITE
#define PERSIST_DEMO_WRITE 1
#endif

#define DEMO_TASK_STACK_SIZE (8 * 1024)
#define DEMO_TASK_PRIORITY   osPriorityNormal

/* Pause after each row so the screen can be followed live. */
#if PERSIST_DEMO_WRITE
#define DEMO_STEP_MS 400
#else
#define DEMO_STEP_MS 150
#endif

/* TOTAL_MESSAGE_HISTORY_SECTOR_SIZE is not set yet, so the demo sizes the
 * message history ring buffer itself. Five messages fill two of these. */
#define DEMO_MH_SECTORS  4
#define DEMO_MH_RAW_START (DATA_REGION_START_SECTOR + MESSAGE_HISTORY_DATA_START_SECTOR)

#define DEMO_MAX_CONTACTS 8
#define DEMO_MAX_CHAT     4

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Data set (the read build checks against the same tables)
 * ------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    const char *phone;
} Person;

typedef struct
{
    const char *phone;
    uint16_t    timestamp; /* create_message() rejects 0 */
    bool        sent;
    const char *text;
} SeedMessage;

static const Person g_people[] = {
    { "Alice",   "0411111111" },
    { "Bob",     "0422222222" },
    { "Charlie", "0433333333" },
    { "Dana",    "0444444444" },
    { "Erin",    "0455555555" },
};
#define N_PEOPLE (sizeof(g_people) / sizeof(g_people[0]))

/* Charlie's 3 messages need two linked chat sectors (MESSAGE_SECTOR_CAPACITY
 * is 2), and all 5 span two message history sectors. */
static const SeedMessage g_msgs[] = {
    { "0411111111", 1, true,  "Hi Alice!" },
    { "0433333333", 2, true,  "Lunch today?" },
    { "0411111111", 3, false, "Hey, how are you?" },
    { "0433333333", 4, false, "Sure, 12:30" },
    { "0433333333", 5, true,  "See you there" },
};
#define N_MSGS (sizeof(g_msgs) / sizeof(g_msgs[0]))

/* ------------------------------------------------------------------ *
 *  Database state (RAM D1: too large for DTCM / the task stack)
 * ------------------------------------------------------------------ */
__attribute__((section(".ram_d1")))
static HashEntry g_entries[HASH_TABLE_SIZE];

__attribute__((section(".ram_d1")))
static uint16_t g_contact_fls_mem[HASH_TABLE_ENTRIES];

__attribute__((section(".ram_d1")))
static uint16_t g_message_fls_mem[TOTAL_MESSAGE_SECTOR_SIZE];

static Storage *const storage = &sd_storage;

static HashTable        g_table;
static FreeList         g_contact_fls;
static FreeList         g_message_fls;
static Journal          g_journal;
static RingBuffer       g_history;
static SDStorageContext g_sd_ctx;

/* ================================================================== *
 *  Record rows: green = ok, red = failed / doesn't match
 * ================================================================== */
static uint32_t g_ok;
static uint32_t g_bad;

#define SECTION(...) lcd_log_line(LOG_COLOR_SECTION, __VA_ARGS__)

static void record(bool ok, const char *fmt, ...)
{
    va_list args;
    char text[LCD_LOG_ROW_CHARS + 1];

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, "%s", text);
    if (ok) { g_ok++; } else { g_bad++; }

    osDelay(DEMO_STEP_MS);
}

static char dir_char(bool sent)
{
    return sent ? '>' : '<';
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

#if PERSIST_DEMO_WRITE
/* ================================================================== *
 *  Write
 * ================================================================== */

/* Empty database: fresh usage bitmap + journal on the card, and every
 * message history sector marked empty. Stale contact/message sectors are
 * left behind, but the bitmap says they are unused. */
static bool format_card(void)
{
    MessageHistorySectorB empty;

    memset(g_entries, 0, sizeof(g_entries));
    if (!init_usage_bitmap(storage)) { return false; }

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_ENTRIES))           { return false; }
    if (!free_list_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE)) { return false; }
    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    memset(&empty, 0, sizeof(empty));
    empty.sector.header.state = RB_EMPTY;
    for (uint16_t i = 0; i < DEMO_MH_SECTORS; i++)
    {
        if (write_sector(storage, DEMO_MH_RAW_START + i, empty.buffer) != STRG_OK) { return false; }
    }

    return init_ring_buffer(storage, &g_history, DEMO_MH_SECTORS, DEMO_MH_RAW_START);
}

/* Name for a seed phone number, for the message rows. */
static const char *name_for(const char *phone)
{
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        if (strcmp(g_people[i].phone, phone) == 0)
        {
            return g_people[i].name;
        }
    }
    return "?";
}

static bool seed_contact(const Person *p)
{
    ContactBuffer c;
    memset(&c, 0, sizeof(c));
    c.contact.name_len = (uint8_t)strlen(p->name);
    memcpy(c.contact.name, p->name, c.contact.name_len);
    c.contact.phone_len = (uint8_t)strlen(p->phone);
    memcpy(c.contact.phone, p->phone, c.contact.phone_len);
    uint16_t slot;
    return hash_insert_contact(&g_table, &g_journal, &c, &slot) == STRG_OK;
}

/* One message goes to two places: its contact's chat and the message
 * history ring buffer. The history entry records the chat sector the
 * message was written to (the contact's latest extent after the insert). */
static bool seed_message(const SeedMessage *s)
{
    MessageBuffer m = create_message(s->timestamp, s->sent, (char *)s->text);
    MessageHistoryEntry e;
    HashEntry *entry;

    if (!hash_insert_message(&g_table, &g_journal, s->phone, &m) ||
        !hash_find_entry(&g_table, s->phone, &entry) || entry == NULL)
    {
        return false;
    }

    memset(&e, 0, sizeof(e));
    e.msgInd = entry->latest_msg_extent;
    e.message = m.msg;
    return message_history_add(&g_history, &g_journal, storage, &e) == STRG_OK;
}

static void run_demo(void)
{
    char text[24];

    lcd_log_init("SD WRITE DEMO");
    lcd_log_footer("writing...", LOG_COLOR_RUN);

    if (!storage_bringup() || !format_card())
    {
        lcd_log_line(LOG_COLOR_FAIL, "SD card init/format failed");
        lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
        return;
    }
    lcd_log_line(LOG_COLOR_DIM, "card formatted (empty DB)");

    SECTION("CONTACTS -> SD");
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        record(seed_contact(&g_people[i]), " %-8.8s %s", g_people[i].name, g_people[i].phone);
    }

    SECTION("MESSAGES -> chat + history");
    for (size_t i = 0; i < N_MSGS; i++)
    {
        const SeedMessage *s = &g_msgs[i];
        record(seed_message(s), " %-7.7s%c %s", name_for(s->phone), dir_char(s->sent), s->text);
    }

    SECTION("HISTORY RING BUFFER");
    lcd_log_line(LOG_COLOR_DIM, " %u/%u sectors, head %u, seq %lu",
             g_history.occupancy, g_history.size, g_history.current_index,
             (unsigned long)g_history.seq);

    snprintf(text, sizeof(text), "WROTE %lu/%u", (unsigned long)g_ok, (unsigned)(N_PEOPLE + N_MSGS));
    lcd_log_footer(text, g_bad == 0 ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

#else /* PERSIST_DEMO_WRITE == 0 */
/* ================================================================== *
 *  Read
 * ================================================================== */

/* A fixed-size char field (maybe unterminated) as a C string. */
static void field_str(char *out, const char *field, size_t len, size_t cap)
{
    if (len > cap) { len = cap; }
    memcpy(out, field, len);
    out[len] = '\0';
}

static bool contact_matches(const char *name, const char *phone)
{
    for (size_t i = 0; i < N_PEOPLE; i++)
    {
        if (strcmp(g_people[i].phone, phone) == 0)
        {
            return strcmp(g_people[i].name, name) == 0;
        }
    }
    return false;
}

/* phone == NULL matches any contact (message history has no phone). */
static bool message_matches(const char *phone, const Message *m, const char *text)
{
    for (size_t i = 0; i < N_MSGS; i++)
    {
        const SeedMessage *s = &g_msgs[i];
        if (s->timestamp == msg_ts(m))
        {
            return (phone == NULL || strcmp(s->phone, phone) == 0) &&
                   s->sent == m->direction && strcmp(s->text, text) == 0;
        }
    }
    return false;
}

static void message_row(const char *phone, const Message *m)
{
    char text[SMS_MAX_MESSAGE_LENGTH + 1];

    field_str(text, m->str, strnlen(m->str, SMS_MAX_MESSAGE_LENGTH), SMS_MAX_MESSAGE_LENGTH);
    record(message_matches(phone, m, text), " %c t%-3u %s", dir_char(m->direction), (unsigned)msg_ts(m), text);
}

/* Boot-time rebuild from the card only: journal recovery, then the hash
 * table from the usage bitmap and contact sectors, then the chats. */
static bool rebuild_from_card(void)
{
    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    memset(g_entries, 0, sizeof(g_entries));
    if (!free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_ENTRIES))           { return false; }
    if (!free_list_empty_init(&g_message_fls, g_message_fls_mem, TOTAL_MESSAGE_SECTOR_SIZE)) { return false; }
    hash_init(&g_table, storage, &g_contact_fls, &g_message_fls, g_entries, HASH_TABLE_SIZE);

    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    if (!read_usage_bitmap(storage)) { return false; }

    return hash_reconstruct_contact(&g_table) && hash_reconstruct_message(&g_table, &g_journal);
}

static void show_contacts_and_chats(void)
{
    ContactBuffer contacts[DEMO_MAX_CONTACTS];
    char phones[DEMO_MAX_CONTACTS][MAX_PHONE_LEN + 1];
    char names[DEMO_MAX_CONTACTS][MAX_NAME_LEN + 1];
    Message chat[DEMO_MAX_CHAT];
    int n = (int)hash_size(&g_table);

    if (n > DEMO_MAX_CONTACTS) { n = DEMO_MAX_CONTACTS; }

    SECTION("CONTACTS (%d rebuilt)", (int)hash_size(&g_table));
    memset(contacts, 0, sizeof(contacts));
    size_t count = 0;
    if (hash_get_contact_list(&g_table, 0, n, &count, contacts) != STRG_OK)
    {
        record(false, " contact list read failed");
        return;
    }
    n = (int)count;

    for (int i = 0; i < n; i++)
    {
        field_str(names[i], contacts[i].contact.name, contacts[i].contact.name_len, MAX_NAME_LEN);
        field_str(phones[i], contacts[i].contact.phone, contacts[i].contact.phone_len, MAX_PHONE_LEN);
        record(contact_matches(names[i], phones[i]), " %-8.8s %s", names[i], phones[i]);
    }

    for (int i = 0; i < n; i++)
    {
        memset(chat, 0, sizeof(chat));
        int k = hash_find_n_message(&g_table, phones[i], 0, DEMO_MAX_CHAT, chat);
        if (k <= 0)
        {
            continue;
        }

        /* Newest first from the card; show the chat oldest first. */
        SECTION("CHAT %s (%d)", names[i], k);
        for (int j = k - 1; j >= 0; j--)
        {
            message_row(phones[i], &chat[j]);
        }
    }
}

/* The chat sector a history entry points at must belong to the message's
 * contact and hold that message. */
static bool history_sector_matches(const MessageHistoryEntry *e)
{
    MessageSectorBuffer sector;
    const char *phone = NULL;
    char sector_phone[MAX_PHONE_LEN + 1];

    for (size_t i = 0; i < N_MSGS; i++)
    {
        if (g_msgs[i].timestamp == msg_ts(&e->message))
        {
            phone = g_msgs[i].phone;
            break;
        }
    }
    if (phone == NULL || e->msgInd >= TOTAL_MESSAGE_SECTOR_SIZE ||
        read_message_sector(storage, e->msgInd, &sector) != STRG_OK)
    {
        return false;
    }

    /* The chat sector stores its contact's sector index, the phone is read from there */
    ContactBuffer owner;
    if (read_contact(storage, sector.var.header.contact_index, &owner) != STRG_OK)
    {
        return false;
    }
    field_str(sector_phone, owner.contact.phone, owner.contact.phone_len, MAX_PHONE_LEN);
    if (strcmp(sector_phone, phone) != 0)
    {
        return false;
    }
    for (uint16_t i = 0; i < sector.var.header.msg_count && i < MESSAGE_SECTOR_CAPACITY; i++)
    {
        if (memcmp(&sector.var.messages[i].msg, &e->message, sizeof(Message)) == 0)
        {
            return true;
        }
    }
    return false;
}

static void show_history(void)
{
    MessageHistoryEntry history[N_MSGS];
    size_t count = 0;

    if (!init_ring_buffer(storage, &g_history, DEMO_MH_SECTORS, DEMO_MH_RAW_START))
    {
        SECTION("HISTORY");
        record(false, " ring buffer rebuild failed");
        return;
    }

    memset(history, 0, sizeof(history));
    if (message_history_get_range(&g_history, storage, 0, N_MSGS, &count, history) != STRG_OK)
    {
        SECTION("HISTORY");
        record(false, " history read failed");
        return;
    }

    SECTION("HISTORY (%u, newest first)", (unsigned)count);
    for (size_t i = 0; i < count; i++)
    {
        message_row(NULL, &history[i].message);
        record(history_sector_matches(&history[i]), "   -> chat sector %u", history[i].msgInd);
    }
}

static void run_demo(void)
{
    /* Every contact, chat message and history entry (plus the chat sector
     * each entry points at) of the data set. */
    const uint32_t expected = N_PEOPLE + 3 * N_MSGS;
    char text[24];

    lcd_log_init("SD READ DEMO");
    lcd_log_footer("reading...", LOG_COLOR_RUN);

    if (!storage_bringup())
    {
        lcd_log_line(LOG_COLOR_FAIL, "SD card init failed");
        lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
        return;
    }

    if (!rebuild_from_card())
    {
        lcd_log_line(LOG_COLOR_FAIL, "rebuild from card failed");
        lcd_log_footer("REBUILD FAIL", LOG_COLOR_FAIL);
        return;
    }

    show_contacts_and_chats();
    show_history();

    bool pass = (g_bad == 0) && (g_ok == expected);
    snprintf(text, sizeof(text), "READ %lu/%lu %s", (unsigned long)g_ok, (unsigned long)expected,
             pass ? "OK" : "BAD");
    lcd_log_footer(text, pass ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

#endif /* PERSIST_DEMO_WRITE */

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

void PersistDemo_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "Persist Demo",
        .cb_mem = &g_demo_task_cb,
        .cb_size = sizeof(g_demo_task_cb),
        .stack_mem = g_demo_task_stack,
        .stack_size = sizeof(g_demo_task_stack),
        .priority = DEMO_TASK_PRIORITY};

    osThreadNew(demoTask, NULL, &task_attr);
}
