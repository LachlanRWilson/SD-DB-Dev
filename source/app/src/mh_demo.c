/*
 * mh_demo.c - on-hardware demonstration of the message history ring buffer,
 * shown on the LCD.
 *
 * Not a test suite (that's mh_main.c): the write half fills a small ring
 * buffer past its capacity so it wraps, and the read half rebuilds the ring
 * buffer from the card alone and pages through it with
 * message_history_get_range(), newest first.
 *
 * ------------------------------------------------------------------------
 *  MH_DEMO_WRITE - which half runs
 * ------------------------------------------------------------------------
 *   1 (default) : format a DEMO_SECTORS ring buffer on the card and add
 *                 every message in g_msgs, showing each one and the ring
 *                 buffer state as it goes. DEMO_SECTORS * capacity is less
 *                 than N_MSGS, so the oldest sector gets overwritten.
 *   0           : write nothing (beyond any journal rollback); rebuild the
 *                 ring buffer from the card and read it back DEMO_PAGE
 *                 messages at a time until get_range() reports the end.
 *                 Each row is green if it is the message expected at that
 *                 position, red if not.
 *
 *   Flash with 1, power-cycle, flash with 0 (or power-cycle a 0 build).
 *
 *   cmake -S . -B build -DMH_DEMO=ON -DMH_DEMO_WRITE=ON
 *   cmake -S . -B build -DMH_DEMO=ON -DMH_DEMO_WRITE=OFF
 *
 * Uses the same sectors as persist_demo.c's history, so each demo's write
 * half wipes the other's data.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "message_history.h"
#include "ring_buffer.h"
#include "journal.h"
#include "storage.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "lcd_log.h"
#include "mh_demo.h"

#ifndef MH_DEMO_WRITE
#define MH_DEMO_WRITE 1
#endif

#define DEMO_TASK_STACK_SIZE (8 * 1024)
#define DEMO_TASK_PRIORITY   osPriorityNormal

/* Pause after each row so the screen can be followed live. */
#if MH_DEMO_WRITE
#define DEMO_STEP_MS 300
#else
#define DEMO_STEP_MS 250
#endif
#define DEMO_PAGE_MS 1200

/* 4 sectors of MESSAGE_HISTORY_SECTOR_CAPACITY (3) messages = 12 slots. */
#define DEMO_SECTORS   4
#define DEMO_CAP       MESSAGE_HISTORY_SECTOR_CAPACITY
#define DEMO_RAW_START (DATA_REGION_START_SECTOR + MESSAGE_HISTORY_DATA_START_SECTOR)

/* Messages per get_range() call in the read half. */
#define DEMO_PAGE 4

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Data set: message i has timestamp i + 1 (the read build checks
 *  against the same table)
 * ------------------------------------------------------------------ */
typedef struct
{
    bool        sent;
    const char *text;
} SeedMessage;

static const SeedMessage g_msgs[] = {
    { true,  "Hey, you free tonight?" },
    { false, "Yeah what's up" },
    { true,  "Thesis demo practice?" },
    { false, "Sure, where?" },
    { true,  "Lab 4, 6pm" },
    { false, "Bring the dev board" },
    { true,  "Already packed" },
    { false, "Does the screen work" },
    { true,  "It does now" },
    { false, "Nice!" },
    { true,  "SD card too" },
    { false, "Even after a reboot?" },
    { true,  "That's the demo" },
    { false, "Ha ok" },
    { true,  "See you there" },
    { false, "On my way" },
};
#define N_MSGS ((uint16_t)(sizeof(g_msgs) / sizeof(g_msgs[0])))

_Static_assert(N_MSGS > DEMO_SECTORS * DEMO_CAP, "the demo data set should wrap the ring buffer");

static Storage *const storage = &sd_storage;

static SDStorageContext g_sd_ctx;
static Journal          g_journal;
static RingBuffer       g_history;

static uint32_t g_ok;
static uint32_t g_bad;

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
#define SECTION(...) lcd_log_line(LOG_COLOR_SECTION, __VA_ARGS__)

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

/* Journal recovery: may roll back an add interrupted by a power cut. */
static bool journal_bringup(void)
{
    memset(&g_journal, 0, sizeof(g_journal));
    return journal_init(&g_journal, storage);
}

static void show_state(void)
{
    lcd_log_line(LOG_COLOR_DIM, "  ring %u/%u sect, head %u, seq %lu",
                 g_history.occupancy, g_history.size, g_history.current_index,
                 (unsigned long)g_history.seq);
}

/* Messages held after `added` adds (once wrapped, the current sector holds
 * 1..cap and the rest are full). */
static uint16_t stored_after(uint16_t added)
{
    if (added <= DEMO_SECTORS * DEMO_CAP)
    {
        return added;
    }
    return (uint16_t)(((added - 1) % DEMO_CAP) + 1 + (DEMO_SECTORS - 1) * DEMO_CAP);
}

#if MH_DEMO_WRITE
/* ================================================================== *
 *  Write
 * ================================================================== */

/* Every ring buffer sector marked empty, then an empty ring buffer. */
static bool format_history(void)
{
    MessageHistorySectorB empty;

    memset(&empty, 0, sizeof(empty));
    empty.sector.header.state = RB_EMPTY;
    for (uint16_t i = 0; i < DEMO_SECTORS; i++)
    {
        if (write_sector(storage, DEMO_RAW_START + i, empty.buffer) != STRG_OK) { return false; }
    }

    return init_ring_buffer(storage, &g_history, DEMO_SECTORS, DEMO_RAW_START);
}

static bool history_add(uint16_t i)
{
    Message m;

    memset(&m, 0, sizeof(m));
    m.timestamp = (uint16_t)(i + 1);
    m.direction = g_msgs[i].sent;
    strncpy(m.str, g_msgs[i].text, sizeof(m.str) - 1);

    return message_history_add(&g_history, &g_journal, storage, &m) == STRG_OK;
}

static void run_demo(void)
{
    char text[24];
    uint16_t last_index;

    lcd_log_init("HISTORY WRITE");
    lcd_log_footer("writing...", LOG_COLOR_RUN);

    if (!storage_bringup() || !journal_bringup() || !format_history())
    {
        lcd_log_line(LOG_COLOR_FAIL, "SD card init/format failed");
        lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
        return;
    }
    lcd_log_line(LOG_COLOR_DIM, "%u sectors x %u msgs formatted", DEMO_SECTORS, DEMO_CAP);

    SECTION("ADD -> RING BUFFER");
    last_index = g_history.current_index;
    for (uint16_t i = 0; i < N_MSGS; i++)
    {
        bool ok = history_add(i);

        if (ok && g_history.current_index != last_index)
        {
            lcd_log_line(LOG_COLOR_SECTION, " -> sector %u%s", g_history.current_index,
                         (i + 1 > DEMO_SECTORS * DEMO_CAP) ? " (overwrite)" : "");
            last_index = g_history.current_index;
        }

        lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " t%-3u%c %s",
                     (unsigned)(i + 1), dir_char(g_msgs[i].sent), g_msgs[i].text);
        if (ok) { g_ok++; } else { g_bad++; }
        osDelay(DEMO_STEP_MS);
    }

    SECTION("RING BUFFER");
    show_state();
    lcd_log_line(LOG_COLOR_DIM, "  %u held, %u oldest overwritten",
                 stored_after(N_MSGS), (unsigned)(N_MSGS - stored_after(N_MSGS)));

    snprintf(text, sizeof(text), "WROTE %lu/%u", (unsigned long)g_ok, (unsigned)N_MSGS);
    lcd_log_footer(text, g_bad == 0 ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

#else /* MH_DEMO_WRITE == 0 */
/* ================================================================== *
 *  Read
 * ================================================================== */

/* Row `pos` (0 = newest) should be the message with timestamp N_MSGS - pos. */
static bool message_matches(size_t pos, const Message *m)
{
    uint16_t want_ts = (uint16_t)(N_MSGS - pos);
    const SeedMessage *s;

    if (pos >= N_MSGS || m->timestamp != want_ts)
    {
        return false;
    }
    s = &g_msgs[want_ts - 1];
    return m->direction == s->sent && strncmp(m->str, s->text, sizeof(m->str)) == 0;
}

static void run_demo(void)
{
    const uint16_t expected = stored_after(N_MSGS);
    Message page[DEMO_PAGE];
    size_t start = 0;
    char text[24];

    lcd_log_init("HISTORY READ");
    lcd_log_footer("reading...", LOG_COLOR_RUN);

    if (!storage_bringup() || !journal_bringup())
    {
        lcd_log_line(LOG_COLOR_FAIL, "SD card init failed");
        lcd_log_footer("SD FAIL", LOG_COLOR_FAIL);
        return;
    }

    /* Boot-time rebuild: binary search the card for the head sector. */
    SECTION("REBUILD FROM CARD");
    if (!init_ring_buffer(storage, &g_history, DEMO_SECTORS, DEMO_RAW_START))
    {
        lcd_log_line(LOG_COLOR_FAIL, " ring buffer rebuild failed");
        lcd_log_footer("REBUILD FAIL", LOG_COLOR_FAIL);
        return;
    }
    show_state();
    osDelay(DEMO_PAGE_MS);

    /* Page through newest -> oldest. A start past the oldest message fails,
     * which is how the reader knows it has reached the end. */
    for (uint32_t p = 1;; p++)
    {
        size_t count = 0;

        memset(page, 0, sizeof(page));
        STRG_RET ret = message_history_get_range(&g_history, storage, start, DEMO_PAGE, &count, page);
        if (ret != STRG_OK || count == 0)
        {
            lcd_log_line(LOG_COLOR_DIM, "get_range(%u, %u): end", (unsigned)start, DEMO_PAGE);
            break;
        }

        SECTION("get_range(%u, %u) -> %u", (unsigned)start, DEMO_PAGE, (unsigned)count);
        for (size_t i = 0; i < count; i++)
        {
            bool ok = message_matches(start + i, &page[i]);
            char body[SMS_MAX_MESSAGE_LENGTH + 1];

            memcpy(body, page[i].str, SMS_MAX_MESSAGE_LENGTH);
            body[SMS_MAX_MESSAGE_LENGTH] = '\0';
            lcd_log_line(ok ? LOG_COLOR_PASS : LOG_COLOR_FAIL, " t%-3u%c %s",
                         page[i].timestamp, dir_char(page[i].direction), body);
            if (ok) { g_ok++; } else { g_bad++; }
            osDelay(DEMO_STEP_MS);
        }

        start += count;
        snprintf(text, sizeof(text), "PAGE %lu  %u/%u", (unsigned long)p, (unsigned)start, expected);
        lcd_log_footer(text, LOG_COLOR_RUN);
        osDelay(DEMO_PAGE_MS);
    }

    bool pass = (g_bad == 0) && (g_ok == expected);
    snprintf(text, sizeof(text), "READ %lu/%u %s", (unsigned long)g_ok, expected, pass ? "OK" : "BAD");
    lcd_log_footer(text, pass ? LOG_COLOR_PASS : LOG_COLOR_FAIL);
}

#endif /* MH_DEMO_WRITE */

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

void MH_Demo_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "MH Demo",
        .cb_mem = &g_demo_task_cb,
        .cb_size = sizeof(g_demo_task_cb),
        .stack_mem = g_demo_task_stack,
        .stack_size = sizeof(g_demo_task_stack),
        .priority = DEMO_TASK_PRIORITY};

    osThreadNew(demoTask, NULL, &task_attr);
}
