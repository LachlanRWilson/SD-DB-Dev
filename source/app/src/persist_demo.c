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
#include "fmc_func.h"
#include "lcd_font.h"
#include "persist_demo.h"

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

/* Charlie's 3 messages need two linked chat sectors (MESSAGE_BLOCK_CAPACITY
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
static uint16_t g_contact_fls_mem[HASH_TABLE_SIZE];

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
 *  LCD text view
 * ================================================================== */
#define SMALL_W 8
#define SMALL_H 12

#define BAR_H     28
#define ROW_H     (SMALL_H + 1)
#define LIST_Y    BAR_H
#define LIST_ROWS ((LCD_HEIGHT - 2 * BAR_H) / ROW_H)
#define ROW_CHARS (LCD_WIDTH / SMALL_W)
#define MAX_LINES 48

#define COLOR_BG      LCD_RGB565(16, 24, 48)
#define COLOR_BAR     LCD_RGB565(32, 44, 80)
#define COLOR_TEXT    LCD_COLOR_WHITE
#define COLOR_DIM     LCD_RGB565(150, 160, 190)
#define COLOR_SECTION LCD_COLOR_CYAN
#define COLOR_RUN     LCD_COLOR_YELLOW
#define COLOR_PASS    LCD_RGB565(60, 220, 90)
#define COLOR_FAIL    LCD_RGB565(255, 70, 70)

typedef struct
{
    char     text[ROW_CHARS + 1];
    uint16_t color;
} Line;

static Line     g_lines[MAX_LINES];
static uint32_t g_n_lines;

/* One full-width band, rendered then blitted with lcd_draw_image(). */
static uint16_t g_band[LCD_WIDTH * BAR_H];

static void band_fill(uint16_t h, uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * h; i++)
    {
        g_band[i] = color;
    }
}

static uint16_t glyph_index(char c)
{
    if (c < LCD_FONT_FIRST || c > LCD_FONT_LAST)
    {
        c = '?';
    }
    return (uint16_t)(c - LCD_FONT_FIRST);
}

/* Full-size (16x24) text at (x, y) inside the band. */
static void band_text(uint16_t x, uint16_t y, const char *s, uint16_t color)
{
    for (; *s != '\0' && x + LCD_FONT_WIDTH <= LCD_WIDTH; s++, x += LCD_FONT_WIDTH)
    {
        const uint16_t *g = lcd_font[glyph_index(*s)];
        for (uint16_t gy = 0; gy < LCD_FONT_HEIGHT; gy++)
        {
            for (uint16_t gx = 0; gx < LCD_FONT_WIDTH; gx++)
            {
                if (g[gy] & (0x8000U >> gx))
                {
                    g_band[(uint32_t)(y + gy) * LCD_WIDTH + x + gx] = color;
                }
            }
        }
    }
}

/* Half-size (8x12) text: a pixel is set when 2+ of the 2x2 font pixels under
 * it are (same scaling as htest_ui.c). */
static void band_text_small(uint16_t x, uint16_t y, const char *s, uint16_t color)
{
    for (; *s != '\0' && x + SMALL_W <= LCD_WIDTH; s++, x += SMALL_W)
    {
        const uint16_t *g = lcd_font[glyph_index(*s)];
        for (uint16_t sy = 0; sy < SMALL_H; sy++)
        {
            uint16_t r0 = g[2 * sy];
            uint16_t r1 = g[2 * sy + 1];
            for (uint16_t sx = 0; sx < SMALL_W; sx++)
            {
                uint16_t m = (uint16_t)(0xC000U >> (2 * sx));
                int n = __builtin_popcount(r0 & m) + __builtin_popcount(r1 & m);
                if (n >= 2)
                {
                    g_band[(uint32_t)(y + sy) * LCD_WIDTH + x + sx] = color;
                }
            }
        }
    }
}

static void draw_bar(uint16_t y, const char *text, uint16_t color)
{
    band_fill(BAR_H, COLOR_BAR);
    band_text(8, (BAR_H - LCD_FONT_HEIGHT) / 2, text, color);
    lcd_draw_image(0, y, LCD_WIDTH, BAR_H, g_band);
}

static void draw_footer(const char *text, uint16_t color)
{
    draw_bar(LCD_HEIGHT - BAR_H, text, color);
}

/* Draw list slot `slot` (0 = top) showing line `index`, or blank. */
static void draw_row(uint32_t slot, uint32_t index)
{
    band_fill(ROW_H, COLOR_BG);
    if (index < g_n_lines)
    {
        band_text_small(0, 0, g_lines[index].text, g_lines[index].color);
    }
    lcd_draw_image(0, (uint16_t)(LIST_Y + slot * ROW_H), LCD_WIDTH, ROW_H, g_band);
}

/* Append a row; once the list is longer than the screen it scrolls so the
 * newest row is at the bottom. */
static void add_line(uint16_t color, const char *fmt, ...)
{
    va_list args;

    if (g_n_lines >= MAX_LINES)
    {
        memmove(&g_lines[0], &g_lines[1], sizeof(Line) * (MAX_LINES - 1));
        g_n_lines--;
    }

    va_start(args, fmt);
    vsnprintf(g_lines[g_n_lines].text, sizeof(g_lines[0].text), fmt, args);
    va_end(args);
    g_lines[g_n_lines].color = color;
    g_n_lines++;

    if (g_n_lines <= LIST_ROWS)
    {
        draw_row(g_n_lines - 1, g_n_lines - 1);
        return;
    }

    for (uint32_t slot = 0; slot < LIST_ROWS; slot++)
    {
        draw_row(slot, g_n_lines - LIST_ROWS + slot);
    }
}

static void ui_init(const char *title)
{
    g_n_lines = 0;
    lcd_init();
    lcd_fill_color(COLOR_BG);
    draw_bar(0, title, COLOR_TEXT);
}

/* ================================================================== *
 *  Record rows: green = ok, red = failed / doesn't match
 * ================================================================== */
static uint32_t g_ok;
static uint32_t g_bad;

#define SECTION(...) add_line(COLOR_SECTION, __VA_ARGS__)

static void record(bool ok, const char *fmt, ...)
{
    va_list args;
    char text[ROW_CHARS + 1];

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    add_line(ok ? COLOR_PASS : COLOR_FAIL, "%s", text);
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

    if (!free_list_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))           { return false; }
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
    return hash_insert_contact(&g_table, &g_journal, &c);
}

/* One message goes to two places: its contact's chat and the message
 * history ring buffer. */
static bool seed_message(const SeedMessage *s)
{
    MessageBuffer m = create_message(s->timestamp, s->sent, (char *)s->text);

    return hash_insert_message(&g_table, &g_journal, s->phone, &m) &&
           message_history_add(&g_history, &g_journal, storage, &m.msg) == STRG_OK;
}

static void run_demo(void)
{
    char text[24];

    ui_init("SD WRITE DEMO");
    draw_footer("writing...", COLOR_RUN);

    if (!storage_bringup() || !format_card())
    {
        add_line(COLOR_FAIL, "SD card init/format failed");
        draw_footer("SD FAIL", COLOR_FAIL);
        return;
    }
    add_line(COLOR_DIM, "card formatted (empty DB)");

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
    add_line(COLOR_DIM, " %u/%u sectors, head %u, seq %lu",
             g_history.occupancy, g_history.size, g_history.current_index,
             (unsigned long)g_history.seq);

    snprintf(text, sizeof(text), "WROTE %lu/%u", (unsigned long)g_ok, (unsigned)(N_PEOPLE + N_MSGS));
    draw_footer(text, g_bad == 0 ? COLOR_PASS : COLOR_FAIL);
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
        if (s->timestamp == m->timestamp)
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
    record(message_matches(phone, m, text), " %c t%-3u %s", dir_char(m->direction), m->timestamp, text);
}

/* Boot-time rebuild from the card only: journal recovery, then the hash
 * table from the usage bitmap and contact sectors, then the chats. */
static bool rebuild_from_card(void)
{
    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    memset(g_entries, 0, sizeof(g_entries));
    if (!free_list_empty_init(&g_contact_fls, g_contact_fls_mem, HASH_TABLE_SIZE))           { return false; }
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
    MessageBuffer chat[DEMO_MAX_CHAT];
    int n = (int)hash_size(&g_table);

    if (n > DEMO_MAX_CONTACTS) { n = DEMO_MAX_CONTACTS; }

    SECTION("CONTACTS (%d rebuilt)", (int)hash_size(&g_table));
    memset(contacts, 0, sizeof(contacts));
    if (hash_get_contact_list(&g_table, 0, n, contacts) != STRG_OK)
    {
        record(false, " contact list read failed");
        return;
    }

    for (int i = 0; i < n; i++)
    {
        field_str(names[i], contacts[i].contact.name, contacts[i].contact.name_len, MAX_NAME_LEN);
        field_str(phones[i], contacts[i].contact.phone, contacts[i].contact.phone_len, MAX_PHONE_LEN);
        record(contact_matches(names[i], phones[i]), " %-8.8s %s", names[i], phones[i]);
    }

    for (int i = 0; i < n; i++)
    {
        memset(chat, 0, sizeof(chat));
        int k = hash_find_n_message(&g_table, phones[i], DEMO_MAX_CHAT, chat);
        if (k <= 0)
        {
            continue;
        }

        /* Newest first from the card; show the chat oldest first. */
        SECTION("CHAT %s (%d)", names[i], k);
        for (int j = k - 1; j >= 0; j--)
        {
            message_row(phones[i], &chat[j].msg);
        }
    }
}

static void show_history(void)
{
    Message history[N_MSGS];
    size_t count = 0;

    if (!init_ring_buffer(storage, &g_history, DEMO_MH_SECTORS, DEMO_MH_RAW_START))
    {
        SECTION("HISTORY");
        record(false, " ring buffer rebuild failed");
        return;
    }

    memset(history, 0, sizeof(history));
    if (message_history_get_list(&g_history, storage, N_MSGS, &count, history) != STRG_OK)
    {
        SECTION("HISTORY");
        record(false, " history read failed");
        return;
    }

    SECTION("HISTORY (%u, newest first)", (unsigned)count);
    for (size_t i = 0; i < count; i++)
    {
        message_row(NULL, &history[i]);
    }
}

static void run_demo(void)
{
    /* Every contact, chat message and history entry of the data set. */
    const uint32_t expected = N_PEOPLE + 2 * N_MSGS;
    char text[24];

    ui_init("SD READ DEMO");
    draw_footer("reading...", COLOR_RUN);

    if (!storage_bringup())
    {
        add_line(COLOR_FAIL, "SD card init failed");
        draw_footer("SD FAIL", COLOR_FAIL);
        return;
    }

    if (!rebuild_from_card())
    {
        add_line(COLOR_FAIL, "rebuild from card failed");
        draw_footer("REBUILD FAIL", COLOR_FAIL);
        return;
    }

    show_contacts_and_chats();
    show_history();

    bool pass = (g_bad == 0) && (g_ok == expected);
    snprintf(text, sizeof(text), "READ %lu/%lu %s", (unsigned long)g_ok, (unsigned long)expected,
             pass ? "OK" : "BAD");
    draw_footer(text, pass ? COLOR_PASS : COLOR_FAIL);
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
