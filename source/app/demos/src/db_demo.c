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
 *   7. CHATS      chat order: most recent chat first, new chats at the top
 *   8. CALLS      call history, known and unknown callers
 *   9. UPDATE     rename (moves in the list), new number (chat follows)
 *  10. DELETE     remove a contact (chat goes too), remove a chat
 *  11. REBOOT     wipe RAM, mount from the card, everything is rebuilt
 *  12. POWER CUT  cut power part way through a write, journal rollback
 *  13. SCALE      bulk add, timing, rebuild time
 *  14. SUMMARY
 *
 * Every check is printed green (OK) or red (FAIL); the last screen shows the
 * totals. The card is reset to an empty database at the start (usage bitmap,
 * journal and the start of each history ring), so it wipes any other demo or
 * test data set.
 *
 * Once mounted, every database call goes through the DB task (db_task.c) as a
 * request and waits for its response, the same as the CLI's phone UI
 * (phone_ui.c). Only the mount (boot) and the card reset run here directly,
 * while the DB task is idle.
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
#include "db_task.h"
#include "platform_os.h"
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

#define DEMO_SECTIONS 14

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
#define DANA         "0411000007"

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
static ChatSummary         g_chats[8];

/* DB task: started once the card is mounted, owns g_db from then on */
static DatabaseTaskContext *g_task;
static PlatformQueue       *g_reply_q;
static uint32_t             g_tag;
static uint32_t             g_requests;     // requests answered by the DB task
static uint32_t             g_stack_free;   // DB task's smallest free stack (bytes), set by the summary

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
 *  Database task requests (same as phone_ui.c)
 * ------------------------------------------------------------------ */

/**
 * @brief Post a request to the database task and wait for its response. The
 * wait has no timeout: list requests write into the caller's buffer, which
 * must stay valid until the task has replied
 *
 * @param req request (reply_q and tag are set here)
 * @param rsp response, may be NULL
 * @retval STRG_RET of the request
 */
static STRG_RET db_call(DbRequest *req, DbResponse *rsp)
{
    DbResponse local;

    rsp = (rsp != NULL) ? rsp : &local;
    req->reply_q = g_reply_q;
    req->tag = ++g_tag;
    if (!db_task_post(g_task, req, PLATFORM_OS_WAIT_FOREVER))
    {
        return STRG_FAIL;
    }
    do
    {
        platform_os_QueueReceive(g_reply_q, rsp, PLATFORM_OS_WAIT_FOREVER);
    } while (rsp->tag != req->tag);
    g_requests++;
    return rsp->ret;
}

static STRG_RET db_phone_req(DbRequestType type, const char *phone, DbResponse *rsp)
{
    DbRequest req = {.type = type};

    snprintf(req.args.phone.phone, sizeof(req.args.phone.phone), "%s", phone);
    return db_call(&req, rsp);
}

static STRG_RET db_range_req(DbRequestType type, size_t start, size_t n, void *out, size_t *count)
{
    DbRequest req = {.type = type, .args.range = {.start = start, .n = n, .out = out}};
    DbResponse rsp;

    STRG_RET ret = db_call(&req, &rsp);
    *count = (ret == STRG_OK) ? rsp.result.range.count : 0;
    return ret;
}

static STRG_RET db_contact_add(const char *name, const char *phone)
{
    DbRequest req = {.type = DB_REQ_CONTACT_ADD};

    // the request fields hold the max lengths: reject instead of truncating
    if ((strlen(name) > MAX_NAME_LEN) || (strlen(phone) > MAX_PHONE_LEN))
    {
        return STRG_FAIL;
    }

    snprintf(req.args.contact_add.name, sizeof(req.args.contact_add.name), "%s", name);
    snprintf(req.args.contact_add.phone, sizeof(req.args.contact_add.phone), "%s", phone);
    return db_call(&req, NULL);
}

static STRG_RET db_contact_get(const char *phone, ContactBuffer *out)
{
    DbResponse rsp;

    STRG_RET ret = db_phone_req(DB_REQ_CONTACT_GET, phone, &rsp);
    if (ret == STRG_OK)
    {
        *out = rsp.result.contact;
    }
    return ret;
}

static STRG_RET db_contact_update(const char *phone, const char *new_name, const char *new_phone)
{
    DbRequest req = {.type = DB_REQ_CONTACT_UPDATE};

    snprintf(req.args.contact_update.phone, sizeof(req.args.contact_update.phone), "%s", phone);
    snprintf(req.args.contact_update.new_name, sizeof(req.args.contact_update.new_name), "%s", new_name);
    snprintf(req.args.contact_update.new_phone, sizeof(req.args.contact_update.new_phone), "%s", new_phone);
    return db_call(&req, NULL);
}

static STRG_RET db_contact_seek(const char *prefix, size_t *rank)
{
    DbRequest req = {.type = DB_REQ_CONTACT_SEEK};
    DbResponse rsp;

    snprintf(req.args.contact_seek.prefix, sizeof(req.args.contact_seek.prefix), "%s", prefix);
    STRG_RET ret = db_call(&req, &rsp);
    *rank = (ret == STRG_OK) ? rsp.result.seek.rank : 0;
    return ret;
}

static size_t db_contact_count(void)
{
    DbRequest req = {.type = DB_REQ_CONTACT_COUNT};
    DbResponse rsp;

    return (db_call(&req, &rsp) == STRG_OK) ? rsp.result.range.count : 0;
}

static STRG_RET db_message_add(const char *phone, uint32_t timestamp, bool direction, const char *str)
{
    DbRequest req = {.type = DB_REQ_MESSAGE_ADD,
                     .args.message_add = {.timestamp = timestamp, .direction = direction}};

    snprintf(req.args.message_add.phone, sizeof(req.args.message_add.phone), "%s", phone);
    snprintf(req.args.message_add.str, sizeof(req.args.message_add.str), "%s", str);
    return db_call(&req, NULL);
}

static STRG_RET db_message_get_chat(const char *phone, size_t start, size_t n, size_t *count, Message *out)
{
    DbRequest req = {.type = DB_REQ_MESSAGE_GET_CHAT, .args.chat = {.start = start, .n = n, .out = out}};
    DbResponse rsp;

    snprintf(req.args.chat.phone, sizeof(req.args.chat.phone), "%s", phone);
    STRG_RET ret = db_call(&req, &rsp);
    *count = (ret == STRG_OK) ? rsp.result.range.count : 0;
    return ret;
}

/* Start the DB task on the mounted g_db (once; later mounts reuse it) */
static bool start_task(void)
{
    if (g_reply_q == NULL)
    {
        // one request outstanding at a time
        g_reply_q = platform_os_QueueCreate(1, sizeof(DbResponse));
    }
    g_task = db_init(&g_db);
    return (g_reply_q != NULL) && (g_task != NULL);
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
        if (db_range_req(DB_REQ_CONTACT_LIST, start, want, g_page, &count) != STRG_OK || count == 0)
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

    if (db_contact_count() != n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (db_range_req(DB_REQ_CONTACT_LIST, i, 1, g_page, &count) != STRG_OK || count != 1 ||
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

    while (db_range_req(DB_REQ_CONTACT_LIST, i, 1, g_page, &count) == STRG_OK && count == 1)
    {
        memcpy(phone, g_page[0].contact.phone, g_page[0].contact.phone_len);
        phone[g_page[0].contact.phone_len] = '\0';
        if (db_contact_get(phone, &found) != STRG_OK)
        {
            return false;
        }
        i++;
    }
    return i == db_contact_count();
}

static size_t chat_len(const char *phone)
{
    size_t count = 0;
    if (db_message_get_chat(phone, 0, 8, &count, g_msgs) != STRG_OK)
    {
        return 0;
    }
    return count;
}

/* Print the chat list from the chat order: "rank name latest message" */
static void show_chats(void)
{
    char name[MAX_NAME_LEN + 1];
    size_t count = 0;

    db_range_req(DB_REQ_CHAT_LIST, 0, 8, g_chats, &count);
    for (size_t i = 0; i < count; i++)
    {
        const ChatSummary *c = &g_chats[i];
        lcd_log_line(LOG_COLOR_TEXT, "%u %-10.10s %s %.13s", (unsigned)i, name_of(&c->contact, name),
                     c->latest.direction ? "->" : "<-", c->latest.str);
        osDelay(DEMO_ROW_MS);
    }
}

/* The whole chat list is exactly the chats of phones[] in order */
static bool chats_are(const char *const *phones, size_t n)
{
    size_t count = 0;

    if ((db_range_req(DB_REQ_CHAT_LIST, 0, 8, g_chats, &count) != STRG_OK) || (count != n))
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        size_t pl = strlen(phones[i]);
        if ((g_chats[i].contact.phone_len != pl) || (memcmp(g_chats[i].contact.phone, phones[i], pl) != 0))
        {
            return false;
        }
    }
    return true;
}

/* Mount like a power cycle: wipe RAM state, database_init, re-wrap storage.
 * Runs here, not as a request: every request is waited on, so the DB task is
 * blocked on its queue and not using g_db */
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

    // from here every database call is a request to the DB task
    if (!check((ret == STRG_OK) && start_task(), "DB task started"))
    {
        lcd_log_footer("TASK FAIL", LOG_COLOR_FAIL);
        return false;
    }
    if (!check(db_contact_count() == 0, "empty database ready"))
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
        STRG_RET ret = db_contact_add(g_people[i].name, g_people[i].phone);
        lcd_log_line((ret == STRG_OK) ? LOG_COLOR_TEXT : LOG_COLOR_FAIL, "+ %-15.15s %s %luw",
                     g_people[i].name, strg_name(ret), (unsigned long)(g_writes - w0));
        osDelay(DEMO_ROW_MS);
        if (ret == STRG_OK) { g_ok++; } else { g_bad++; }
    }
    lcd_log_line(LOG_COLOR_DIM, "w = sector writes: journal,");
    lcd_log_line(LOG_COLOR_DIM, "contact sector, usage bitmap");
    snprintf(line, sizeof(line), "%u contacts stored", (unsigned)N_PEOPLE);
    check(db_contact_count() == N_PEOPLE, line);
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

    STRG_RET ret = db_contact_seek(prefix, &rank);
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
    STRG_RET ret = db_contact_add("Someone Else", BOB);
    lcd_log_line(LOG_COLOR_TEXT, "add dup %s -> %s", BOB, strg_name(ret));
    check(ret == STRG_EXISTS, "duplicate phone rejected");

    ret = db_contact_add("No Number", "");
    lcd_log_line(LOG_COLOR_TEXT, "add empty phone -> %s", strg_name(ret));
    check(ret == STRG_FAIL, "empty phone rejected");

    ret = db_contact_add(long_name, "0411000099");
    lcd_log_line(LOG_COLOR_TEXT, "add 65 char name -> %s", strg_name(ret));
    lcd_log_line(LOG_COLOR_DIM, " (too long for the request)");
    check(ret == STRG_FAIL, "name > 64 chars rejected");

    ret = db_contact_get("0400000000", &out);
    lcd_log_line(LOG_COLOR_TEXT, "get 0400000000 -> %s", strg_name(ret));
    check(ret == STRG_EMPTY, "unknown number not found");

    check(db_contact_count() == N_PEOPLE, "nothing was changed");
}

static void show_chat(const char *phone)
{
    size_t count = 0;

    db_message_get_chat(phone, 0, 8, &count, g_msgs);
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
    db_message_add(ALICE, ++g_ts, true, "Lunch tomorrow?");
    db_message_add(ALICE, ++g_ts, false, "Sure, 12:30?");
    db_message_add(BOB, ++g_ts, false, "Did you push the fix?");
    db_message_add(ALICE, ++g_ts, true, "Perfect, see you then");
    db_message_add(ALICE, ++g_ts, false, "Bring the slides");
    show_chat(ALICE);
    count = chat_len(ALICE);
    snprintf(what, sizeof(what), "alice: %u msgs, 2 sectors", (unsigned)count);
    check(count == 4, what);
    check(chat_len(BOB) == 1, "bob's chat kept separate");

    lcd_log_line(LOG_COLOR_DIM, "SMS from an unknown number:");
    db_message_add(UNKNOWN_SMS, ++g_ts, false, "Your parcel is on its way");
    show_chat(UNKNOWN_SMS);
    check((db_contact_get(UNKNOWN_SMS, &out) == STRG_OK) && (out.contact.name_len == 0),
          "unnamed contact created");
    check(db_contact_count() == N_PEOPLE + 1, "listed first (no name)");
}

static void step_history(void)
{
    char name[MAX_NAME_LEN + 1];
    size_t count = 0;

    section(6, "HISTORY", "Latest messages, all chats:");
    STRG_RET ret = db_range_req(DB_REQ_MESSAGE_RECENT, 0, 6, g_hist, &count);
    for (size_t i = 0; i < count; i++)
    {
        // which chat: the message sector header holds the contact index (a
        // direct read of the card, the DB task is idle between requests)
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

static void step_chats(void)
{
    static const char *const after_msgs[] = { UNKNOWN_SMS, ALICE, BOB };
    static const char *const after_bob[] = { BOB, UNKNOWN_SMS, ALICE };
    static const char *const after_new[] = { CHARLIE, DANA, BOB, UNKNOWN_SMS, ALICE };

    section(7, "CHATS", "Chat order, newest first:");
    show_chats();
    check(chats_are(after_msgs, 3), "ordered by latest message");

    lcd_log_line(LOG_COLOR_DIM, "Bob replies (oldest chat):");
    db_message_add(BOB, ++g_ts, false, "Yep, pushed it");
    show_chats();
    check(chats_are(after_bob, 3), "bob moved to the top");

    lcd_log_line(LOG_COLOR_DIM, "Dana, then Charlie, new chats:");
    db_message_add(DANA, ++g_ts, true, "Coffee later?");
    db_message_add(CHARLIE, ++g_ts, false, "Running 5 min late");
    show_chats();
    check(chats_are(after_new, 5), "new chats go on top");
    lcd_log_line(LOG_COLOR_DIM, "hash slots in RAM, no card");
    lcd_log_line(LOG_COLOR_DIM, "writes to reorder");
}

static void step_calls(void)
{
    char name[MAX_NAME_LEN + 1];
    size_t count = 0;

    section(8, "CALLS", "Log 3 calls:");
    db_phone_req(DB_REQ_CALL_ADD, ZOE, NULL);
    db_phone_req(DB_REQ_CALL_ADD, UNKNOWN_CALL, NULL);
    db_phone_req(DB_REQ_CALL_ADD, CHARLIE, NULL);

    STRG_RET ret = db_range_req(DB_REQ_CALL_RECENT, 0, 8, g_calls, &count);
    for (size_t i = 0; i < count; i++)
    {
        lcd_log_line(LOG_COLOR_TEXT, "%u %-15.15s %.*s", (unsigned)i, name_of(&g_calls[i].contact, name),
                     g_calls[i].contact.phone_len, g_calls[i].contact.phone);
        osDelay(DEMO_ROW_MS);
    }
    check((ret == STRG_OK) && (count == 3), "3 calls, newest first");
    check((count == 3) && contact_is(&g_calls[0].contact, "Charlie Day", CHARLIE), "known caller has name");
    check((count == 3) && (g_calls[1].contact_index == UINT16_MAX), "unknown caller: no name");
    check(db_contact_count() == N_PEOPLE + 1, "unknown caller not added");
}

static void step_update(void)
{
    ContactBuffer out;

    section(9, "UPDATE", "Rename Zoe Park -> Aaron Park:");
    check(db_contact_update(ZOE, "Aaron Park", ZOE) == STRG_OK, "renamed");
    show_list(0, 3);
    lcd_log_line(LOG_COLOR_DIM, "moved from last to near top");

    lcd_log_line(LOG_COLOR_DIM, "alice gets a new number:");
    check(db_contact_update(ALICE, "alice Brown", ALICE_NEW) == STRG_OK, ALICE " -> " ALICE_NEW);
    check(db_contact_get(ALICE, &out) == STRG_EMPTY, "old number gone");
    check(chat_len(ALICE_NEW) == 4, "chat followed (4 msgs)");

    static const char *const chats[] = { CHARLIE, DANA, BOB, UNKNOWN_SMS, ALICE_NEW };
    check(chats_are(chats, 5), "chat kept its place (4th)");
}

static void step_delete(void)
{
    ContactBuffer out;
    size_t count = 0;

    section(10, "DELETE", "Remove first Bob Smith:");
    check(db_phone_req(DB_REQ_CONTACT_REMOVE, BOB, NULL) == STRG_OK, "removed " BOB);
    check(db_contact_get(BOB, &out) == STRG_EMPTY, "not found any more");
    check(db_message_get_chat(BOB, 0, 8, &count, g_msgs) == STRG_EMPTY, "his chat was deleted");
    check(db_contact_get("0411000008", &out) == STRG_OK, "other Bob Smith kept");

    lcd_log_line(LOG_COLOR_DIM, "Delete parcel chat only:");
    check(db_phone_req(DB_REQ_MESSAGE_REMOVE_CHAT, UNKNOWN_SMS, NULL) == STRG_OK, "chat removed");
    check(db_contact_get(UNKNOWN_SMS, &out) == STRG_OK, "contact kept");

    lcd_log_line(LOG_COLOR_DIM, "List now:");
    show_list(0, N_FINAL);
    check(list_is(g_final, N_FINAL), "list as expected");

    static const char *const chats[] = { CHARLIE, DANA, ALICE_NEW };
    lcd_log_line(LOG_COLOR_DIM, "Chats now:");
    show_chats();
    check(chats_are(chats, 3), "both chats left the order");
}

static void step_reboot(void)
{
    uint32_t ms;
    char what[40];

    section(11, "REBOOT", "Wipe RAM, mount from card:");
    STRG_RET ret = boot(&ms);
    snprintf(what, sizeof(what), "mounted in %lu ms", (unsigned long)ms);
    check(ret == STRG_OK, what);
    lcd_log_line(LOG_COLOR_DIM, "rebuilt from the card:");
    lcd_log_line(LOG_COLOR_DIM, " hash table, free lists,");
    lcd_log_line(LOG_COLOR_DIM, " name index, ring heads,");
    lcd_log_line(LOG_COLOR_DIM, " chat order (heapsort)");
    check(list_is(g_final, N_FINAL), "same contacts, same order");
    check(chat_len(ALICE_NEW) == 4, "alice's chat intact");
    check(chat_len(UNKNOWN_SMS) == 0, "deleted chat still gone");
    static const char *const chats[] = { CHARLIE, DANA, ALICE_NEW };
    check(chats_are(chats, 3), "chat order rebuilt");
    check(free_list_available(&g_db.contact_allocator) + free_list_used(&g_db.contact_allocator) ==
          HASH_TABLE_ENTRIES, "contact free list exact");

    size_t count = 0;
    db_range_req(DB_REQ_CALL_RECENT, 0, 8, g_calls, &count);
    check(count == 3, "call history intact");
}

static void step_powercut(void)
{
    uint32_t ms;
    uint32_t w0;
    char what[40];
    size_t before = db_contact_count();
    ContactBuffer out;

    section(12, "POWER CUT", "Every write is journalled:");
    w0 = g_writes;
    db_contact_add("Eve Normal", "0411000010");
    snprintf(what, sizeof(what), "normal add: %lu writes", (unsigned long)(g_writes - w0));
    lcd_log_line(LOG_COLOR_TEXT, "%s", what);
    before = db_contact_count();

    lcd_log_line(LOG_COLOR_RUN, "Cut power after 2 writes:");
    g_writes_left = 2;
    STRG_RET ret = db_contact_add("Half Written", "0411000011");
    lcd_log_line(LOG_COLOR_TEXT, "add -> %s (power lost)", strg_name(ret));
    ret = boot(&ms);
    snprintf(what, sizeof(what), "reboot + rollback %lu ms", (unsigned long)ms);
    check(ret == STRG_OK, what);
    check(db_contact_count() == before, "half add rolled back");
    check(db_contact_get("0411000011", &out) == STRG_EMPTY, "no trace of it");
    check(list_consistent(), "list consistent");

    lcd_log_line(LOG_COLOR_RUN, "Cut power after 6 writes:");
    g_writes_left = 6;
    ret = db_contact_add("Nearly Done", "0411000012");
    lcd_log_line(LOG_COLOR_TEXT, "add -> %s (power lost)", strg_name(ret));
    boot(&ms);
    bool landed = (db_contact_get("0411000012", &out) == STRG_OK);
    lcd_log_line(LOG_COLOR_TEXT, "after reboot: %s", landed ? "contact kept" : "rolled back");
    check(db_contact_count() == before + (landed ? 1 : 0), "old or new, never half");
    check(list_consistent(), "list consistent");
}

static void step_scale(void)
{
    char name[24];
    char phone[16];
    char what[40];
    uint32_t ms;
    size_t start = db_contact_count();
    uint32_t added = 0;

    section(13, "SCALE", "Bulk add contacts:");
    uint32_t t0 = osKernelGetTickCount();
    for (uint32_t i = 0; i < DEMO_SCALE_N; i++)
    {
        snprintf(name, sizeof(name), "Contact %03lu", (unsigned long)i);
        snprintf(phone, sizeof(phone), "0422%06lu", (unsigned long)i);
        if (db_contact_add(name, phone) == STRG_OK)
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
    snprintf(what, sizeof(what), "%u contacts mount %lu ms", (unsigned)db_contact_count(),
             (unsigned long)ms);
    check((ret == STRG_OK) && (db_contact_count() == start + DEMO_SCALE_N), what);

    size_t rank = 0;
    db_contact_seek("contact 1", &rank);
    lcd_log_line(LOG_COLOR_DIM, "search \"contact 1\":");
    show_list(rank, 2);
    check(list_consistent(), "consistent after reboot");
}

/* Smallest free stack the DB task has had (bytes), 0 if not found */
static uint32_t db_task_stack_free(void)
{
    osThreadId_t threads[8];
    uint32_t n = osThreadEnumerate(threads, 8);

    for (uint32_t i = 0; i < n; i++)
    {
        const char *name = osThreadGetName(threads[i]);
        if ((name != NULL) && (strcmp(name, "DB Task") == 0))
        {
            return osThreadGetStackSpace(threads[i]);
        }
    }
    return 0;
}

static void step_summary(uint32_t elapsed_ms)
{
    char footer[24];

    lcd_log_init("14/14 SUMMARY");
    lcd_log_line(LOG_COLOR_SECTION, "Shown on the real SD card:");
    lcd_log_line(LOG_COLOR_TEXT, " contacts: add/get/update/rm");
    lcd_log_line(LOG_COLOR_TEXT, " sorted list + prefix search");
    lcd_log_line(LOG_COLOR_TEXT, " chats across message sectors");
    lcd_log_line(LOG_COLOR_TEXT, " chat order, newest first");
    lcd_log_line(LOG_COLOR_TEXT, " message + call history rings");
    lcd_log_line(LOG_COLOR_TEXT, " reboot rebuild from card");
    lcd_log_line(LOG_COLOR_TEXT, " journal power-cut rollback");
    lcd_log_line(LOG_COLOR_DIM, " ");
    lcd_log_line(LOG_COLOR_TEXT, "%lu contacts on the card", (unsigned long)db_contact_count());
    lcd_log_line(LOG_COLOR_TEXT, "%lu reads, %lu writes", (unsigned long)g_reads, (unsigned long)g_writes);
    lcd_log_line(LOG_COLOR_TEXT, "%lu DB task requests", (unsigned long)g_requests);
    g_stack_free = db_task_stack_free();
    lcd_log_line(LOG_COLOR_TEXT, "DB task stack free %lu B", (unsigned long)g_stack_free);
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
        step_chats, step_calls, step_update, step_delete, step_reboot, step_powercut, step_scale,
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
