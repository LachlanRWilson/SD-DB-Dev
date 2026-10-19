/*
 * phone_ui.c - the `run` command: a terminal rendering of the UniQOS phone
 * menus (../UniQOS/ui/pages) driven by the real database.
 *
 *   Menu ─┬─ Phone ──── Call / Call History ──► contact details
 *         ├─ SMS ────── New Message / All Messages ──► chat
 *         └─ Contacts ─ name ordered list ──► contact details ──► chat
 *
 * Every screen is fetched with requests to the database task (db_task.h), and
 * the storage cost of the last key press (sector reads/writes) is shown under
 * the screen.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "phone_ui.h"
#include "card_image.h"
#include "contact.h"
#include "message.h"
#include "db_task.h"
#include "platform_os.h"

#define W         34  // screen inner width (chars)
#define ROWS      10  // body rows
#define INFO_ROWS 5   // name/phone lines above the contact details options
#define BUBBLE_W  24  // chat bubble width
#define CHUNK     20  // entries per database range call
#define MAX_DEPTH 8
#define INPUT_LEN 256

#define RESET "\x1b[0m"
#define BOLD  "\x1b[1m"
#define DIM   "\x1b[2m"
#define REV   "\x1b[7m"
#define B_OUT "\x1b[44;97m"
#define B_IN  "\x1b[100;97m"

typedef enum { PG_MENU, PG_PHONE, PG_CALLS, PG_SMS, PG_INBOX, PG_CONTACTS, PG_DETAILS, PG_CHAT } PageId;
typedef enum { ST_NONE, ST_DIM, ST_OUT, ST_IN } Style;
typedef enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SELECT, K_BACK, K_QUIT, K_OTHER } Key;

// One list row, or one chat line
typedef struct
{
    char label[W + 1];
    char right[16];
    char phone[MAX_PHONE_LEN + 1];
    uint32_t ts;
    uint8_t style;
    uint8_t pad;
} Item;

typedef struct
{
    PageId id;
    char title[W + 1];
    char phone[MAX_PHONE_LEN + 1];  // details and chat key
    bool known;                     // details: phone has a contact
    bool stale;                     // reload items before the next draw
    size_t cursor;
    size_t offset;                  // first row shown (chat: lines scrolled up from the newest)
    Item *items;
    size_t n_items;
    size_t cap;
    size_t base;                    // index of items[0] (contacts only hold the visible rows)
    size_t total;
} Page;

static DatabaseTaskContext *g_task;
static PlatformQueue *g_reply_q;
static uint32_t g_tag;
static Page g_stack[MAX_DEPTH];
static int g_depth;
static bool g_quit;
static char g_flash[128];
static CardStats g_before;
static struct termios g_saved;
static bool g_raw;

/* ------------------------------------------------------------------ *
 *  Terminal
 * ------------------------------------------------------------------ */

static void term_raw(bool on)
{
    if (on)
    {
        struct termios t = g_saved;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    }
    g_raw = on;
}

// Leave the terminal usable if the process exits mid render
static void term_restore(void)
{
    if (g_raw)
    {
        term_raw(false);
        printf("\x1b[?25h\x1b[?1049l");
        fflush(stdout);
    }
}

static void set_vtime(cc_t vmin, cc_t vtime)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_cc[VMIN] = vmin;
    t.c_cc[VTIME] = vtime;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

/**
 * @brief Read one key press. Arrows, vi keys (hjkl) and WASD all move
 *
 * @param ch raw character for K_OTHER
 * @retval Key
 */
static Key read_key(int *ch)
{
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
    {
        return K_QUIT;
    }
    *ch = c;

    if (c == 27)
    {
        unsigned char seq[2];

        // a lone ESC is back, arrows arrive as ESC [ A..D
        set_vtime(0, 1);
        ssize_t n = read(STDIN_FILENO, seq, sizeof(seq));
        set_vtime(1, 0);
        if (n <= 0)
        {
            return K_BACK;
        }
        if ((n == 2) && ((seq[0] == '[') || (seq[0] == 'O')))
        {
            switch (seq[1])
            {
                case 'A': return K_UP;
                case 'B': return K_DOWN;
                case 'C': return K_RIGHT;
                case 'D': return K_LEFT;
                default:  break;
            }
        }
        return K_NONE;
    }

    switch (c)
    {
        case 'k': case 'w':             return K_UP;
        case 'j': case 's':             return K_DOWN;
        case 'l': case ' ': case '\r':
        case '\n':                      return K_SELECT;
        case 'h': case 127: case 8:     return K_BACK;
        case 'q': case 3:               return K_QUIT;
        default:                        return K_OTHER;
    }
}

/**
 * @brief Read a line below the screen in normal (cooked) terminal mode
 *
 * @param label prompt text
 * @param out line read, without the newline
 * @param cap size of out
 * @retval true if a non-empty line was read
 */
static bool prompt(const char *label, char *out, size_t cap)
{
    term_raw(false);
    printf("\x1b[?25h  %s", label);
    fflush(stdout);
    bool ok = (fgets(out, (int)cap, stdin) != NULL);
    printf("\x1b[?25l");
    term_raw(true);

    if (!ok)
    {
        out[0] = '\0';
        return false;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */

static void field(char *out, const char *src, size_t len)
{
    memcpy(out, src, len);
    out[len] = '\0';
}

// Display name: the contact name, else the phone number
static void display_name(const Contact *c, char *out)
{
    if (c->name_len > 0)
    {
        field(out, c->name, c->name_len);
    } else {
        field(out, c->phone, c->phone_len);
    }
}

static uint32_t msg_time(const Message *m)
{
    return ((uint32_t)m->timestamp_hi << 16) | m->timestamp_lo;
}

// HH:MM for today, else dd/mm
static void short_time(uint32_t t, char out[16])
{
    time_t tt = (time_t)t;
    time_t now = time(NULL);
    struct tm tm;
    struct tm today;

    localtime_r(&tt, &tm);
    localtime_r(&now, &today);
    bool same_day = (tm.tm_year == today.tm_year) && (tm.tm_yday == today.tm_yday);
    strftime(out, 16, same_day ? "%H:%M" : "%d/%m", &tm);
}

static void begin_io(void)
{
    g_before = card_stats;
}

static void report(const char *what, STRG_RET ret)
{
    snprintf(g_flash, sizeof(g_flash), "%s: %s (%u reads, %u writes)", what, strg_name(ret),
             card_stats.reads - g_before.reads, card_stats.writes - g_before.writes);
}

static Item *add_item(Page *p)
{
    if (p->n_items == p->cap)
    {
        p->cap = (p->cap == 0) ? 32 : (p->cap * 2);
        p->items = realloc(p->items, p->cap * sizeof(Item));
        if (p->items == NULL)
        {
            abort();
        }
    }
    Item *it = &p->items[p->n_items++];
    memset(it, 0, sizeof(*it));
    return it;
}

static Item *add_row(Page *p, const char *label, const char *phone)
{
    Item *it = add_item(p);
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->phone, sizeof(it->phone), "%s", (phone != NULL) ? phone : "");
    return it;
}

/* ------------------------------------------------------------------ *
 *  Database task requests
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

/* ------------------------------------------------------------------ *
 *  Page loading
 * ------------------------------------------------------------------ */

static void load_static(Page *p, const char *const *labels, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        add_row(p, labels[i], NULL);
    }
}

static void load_calls(Page *p)
{
    CallHistoryEntry calls[CHUNK];
    size_t count = 0;
    char name[MAX_NAME_LEN + 1];

    for (size_t start = 0;; start += count)
    {
        if ((db_range_req(DB_REQ_CALL_RECENT, start, CHUNK, calls, &count) != STRG_OK) || (count == 0))
        {
            break;
        }
        for (size_t i = 0; i < count; i++)
        {
            char phone[MAX_PHONE_LEN + 1];

            display_name(&calls[i].contact, name);
            field(phone, calls[i].contact.phone, calls[i].contact.phone_len);
            Item *it = add_row(p, name, phone);
            if (calls[i].contact_index == UINT16_MAX)
            {
                strcpy(it->right, "unknown");
            }
        }
        if (count < CHUNK)
        {
            break;
        }
    }
}

// One row per chat, most recent message first (the task returns them in order)
static void load_inbox(Page *p)
{
    ChatSummary chats[CHUNK];
    size_t count = 0;

    for (size_t start = 0;; start += count)
    {
        if ((db_range_req(DB_REQ_CHAT_LIST, start, CHUNK, chats, &count) != STRG_OK) || (count == 0))
        {
            break;
        }
        for (size_t i = 0; i < count; i++)
        {
            char phone[MAX_PHONE_LEN + 1];
            char name[MAX_NAME_LEN + 1];

            field(phone, chats[i].contact.phone, chats[i].contact.phone_len);
            display_name(&chats[i].contact, name);
            Item *it = add_row(p, name, phone);
            it->ts = msg_time(&chats[i].latest);
            short_time(it->ts, it->right);
        }
        if (count < CHUNK)
        {
            break;
        }
    }
}

// Only the visible window is fetched, through the name index
static void load_contacts(Page *p)
{
    ContactBuffer page[ROWS];
    size_t count = 0;
    char name[MAX_NAME_LEN + 1];
    char phone[MAX_PHONE_LEN + 1];

    p->base = p->offset;
    if (db_range_req(DB_REQ_CONTACT_LIST, p->offset, ROWS, page, &count) != STRG_OK)
    {
        return;
    }
    for (size_t i = 0; i < count; i++)
    {
        display_name(&page[i].contact, name);
        field(phone, page[i].contact.phone, page[i].contact.phone_len);
        add_row(p, name, phone);
    }
}

static void load_details(Page *p)
{
    static const char *const known[] = {"Messages", "Call", "Send Message", "Edit Contact", "Delete Contact"};
    static const char *const unknown[] = {"Messages", "Call", "Send Message", "Add Contact"};
    ContactBuffer c;

    p->known = (db_contact_get(p->phone, &c) == STRG_OK);
    if (p->known)
    {
        char name[MAX_NAME_LEN + 1];
        field(name, c.contact.name, c.contact.name_len);
        snprintf(p->title, sizeof(p->title), "%.*s", W, (name[0] != '\0') ? name : "(unnamed)");
        load_static(p, known, sizeof(known) / sizeof(known[0]));
    } else {
        snprintf(p->title, sizeof(p->title), "Unknown");
        load_static(p, unknown, sizeof(unknown) / sizeof(unknown[0]));
    }
}

/**
 * @brief Word wrap text into lines of at most width chars
 *
 * @retval number of lines
 */
static size_t wrap(const char *text, size_t width, char lines[][BUBBLE_W + 1], size_t max_lines)
{
    size_t n = 0;
    const char *s = text;

    while ((*s != '\0') && (n < max_lines))
    {
        while (*s == ' ')
        {
            s++;
        }
        size_t len = strlen(s);
        if (len == 0)
        {
            break;
        }
        size_t take = (len < width) ? len : width;
        if (take < len)
        {
            // break at the last space if the word would be cut
            size_t sp = take;
            while ((sp > 0) && (s[sp] != ' '))
            {
                sp--;
            }
            take = (sp > 0) ? sp : take;
        }
        field(lines[n++], s, take);
        s += take;
    }
    return n;
}

static void add_bubble(Page *p, const Message *m)
{
    char text[SMS_MAX_MESSAGE_LENGTH + 1];
    char lines[8][BUBBLE_W + 1];
    char when[16];
    size_t widest = 0;

    field(text, m->str, strnlen(m->str, SMS_MAX_MESSAGE_LENGTH));
    for (char *c = text; *c != '\0'; c++)
    {
        *c = ((*c < 32) || (*c == 127)) ? ' ' : *c;
    }

    size_t n = wrap(text, BUBBLE_W - 2, lines, 8);
    for (size_t i = 0; i < n; i++)
    {
        size_t len = strlen(lines[i]);
        widest = (len > widest) ? len : widest;
    }

    // received bubbles sit on the left, sent on the right
    for (size_t i = 0; i < n; i++)
    {
        Item *it = add_item(p);
        snprintf(it->label, sizeof(it->label), " %-*s ", (int)widest, lines[i]);
        it->style = m->direction ? ST_OUT : ST_IN;
        it->pad = m->direction ? (uint8_t)(W - 1 - (widest + 2)) : 1;
    }

    Item *t = add_item(p);
    short_time(msg_time(m), when);
    snprintf(t->label, sizeof(t->label), "%s", when);
    t->style = ST_DIM;
    t->pad = m->direction ? (uint8_t)(W - 1 - strlen(when)) : 1;
}

static void load_chat(Page *p)
{
    Message *msgs = NULL;
    size_t n = 0;
    size_t count = 0;
    ContactBuffer c;

    if (db_contact_get(p->phone, &c) == STRG_OK)
    {
        char name[MAX_NAME_LEN + 1];
        display_name(&c.contact, name);
        snprintf(p->title, sizeof(p->title), "%.*s", W, name);
    } else {
        snprintf(p->title, sizeof(p->title), "%s", p->phone);
    }

    // the API reads from the newest backwards
    for (size_t start = 0;; start += count)
    {
        msgs = realloc(msgs, (n + CHUNK) * sizeof(Message));
        if (msgs == NULL)
        {
            abort();
        }
        if ((db_message_get_chat(p->phone, start, CHUNK, &count, &msgs[n]) != STRG_OK) || (count == 0))
        {
            break;
        }
        n += count;
        if (count < CHUNK)
        {
            break;
        }
    }

    for (size_t i = n; i-- > 0;)
    {
        add_bubble(p, &msgs[i]);
        if (i > 0)
        {
            add_item(p);
        }
    }
    free(msgs);
}

static void refresh(Page *p)
{
    static const char *const menu[] = {"Phone", "SMS", "Contacts"};
    static const char *const phone[] = {"Call", "Call History"};
    static const char *const sms[] = {"New Message", "All Messages"};

    p->n_items = 0;
    p->base = 0;
    switch (p->id)
    {
        case PG_MENU:     load_static(p, menu, 3); break;
        case PG_PHONE:    load_static(p, phone, 2); break;
        case PG_SMS:      load_static(p, sms, 2); break;
        case PG_CALLS:    load_calls(p); break;
        case PG_INBOX:    load_inbox(p); break;
        case PG_CONTACTS: load_contacts(p); break;
        case PG_DETAILS:  load_details(p); break;
        case PG_CHAT:     load_chat(p); break;
    }
    if (p->id != PG_CONTACTS)
    {
        p->total = p->n_items;
    }
}

// Keep the cursor in range and on screen
static void clamp(Page *p)
{
    size_t rows = (p->id == PG_DETAILS) ? (ROWS - INFO_ROWS) : ROWS;

    if (p->id == PG_CONTACTS)
    {
        p->total = db_contact_count();
    }
    if (p->id == PG_CHAT)
    {
        size_t max = (p->total > ROWS) ? (p->total - ROWS) : 0;
        p->offset = (p->offset > max) ? max : p->offset;
        return;
    }

    if (p->cursor >= p->total)
    {
        p->cursor = (p->total > 0) ? (p->total - 1) : 0;
    }
    if (p->cursor < p->offset)
    {
        p->offset = p->cursor;
    }
    if (p->cursor >= (p->offset + rows))
    {
        p->offset = p->cursor - rows + 1;
    }
}

/* ------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------ */

static const char *style_code(uint8_t style)
{
    switch (style)
    {
        case ST_DIM: return DIM;
        case ST_OUT: return B_OUT;
        case ST_IN:  return B_IN;
        default:     return "";
    }
}

static void hrule(const char *l, const char *r)
{
    printf("%s", l);
    for (int i = 0; i < W; i++)
    {
        printf("─");
    }
    printf("%s\n", r);
}

// One screen line: pad spaces, then text in style, filled to W
static void put_line(size_t pad, const char *text, const char *style)
{
    size_t len = strlen(text);

    pad = (pad > W) ? W : pad;
    len = (len > (W - pad)) ? (W - pad) : len;
    printf("│%*s%s%.*s%s%*s│\n", (int)pad, "", style, (int)len, text, (style[0] != '\0') ? RESET : "",
           (int)(W - pad - len), "");
}

static void put_centred(const char *text, const char *style)
{
    size_t len = strlen(text);
    put_line((len < W) ? ((W - len) / 2) : 0, text, style);
}

// " label .......... right "
static void row_text(char out[W + 1], const char *label, const char *right)
{
    int rlen = (int)strlen(right);
    int lw = W - 2 - ((rlen > 0) ? (rlen + 1) : 0);
    snprintf(out, W + 1, " %-*.*s%s%s ", lw, lw, label, (rlen > 0) ? " " : "", right);
}

static void draw_list(const Page *p, size_t first_row, size_t rows, const char *empty)
{
    char buf[W + 1];

    if (p->total == 0)
    {
        for (size_t i = 0; i < rows; i++)
        {
            if (i == (rows / 2))
            {
                put_centred(empty, DIM);
            } else {
                put_line(0, "", "");
            }
        }
        return;
    }

    for (size_t i = 0; i < rows; i++)
    {
        size_t idx = first_row + i;
        if ((idx < p->total) && (idx >= p->base) && ((idx - p->base) < p->n_items))
        {
            const Item *it = &p->items[idx - p->base];
            row_text(buf, it->label, it->right);
            put_line(0, buf, (idx == p->cursor) ? REV : "");
        } else {
            put_line(0, "", "");
        }
    }
}

static void draw_details(const Page *p)
{
    char buf[W + 1];

    put_line(1, "Name", DIM);
    snprintf(buf, sizeof(buf), "%s", p->title);
    put_line(2, buf, BOLD);
    put_line(1, "Phone", DIM);
    put_line(2, p->phone, BOLD);
    put_line(0, "", "");
    draw_list(p, p->offset, ROWS - INFO_ROWS, "");
}

static void draw_chat(const Page *p)
{
    if (p->total == 0)
    {
        draw_list(p, 0, ROWS, "No messages");
        return;
    }

    // newest at the bottom, offset scrolls back
    size_t end = p->total - p->offset;
    size_t start = (end > ROWS) ? (end - ROWS) : 0;
    for (size_t i = 0; i < (ROWS - (end - start)); i++)
    {
        put_line(0, "", "");
    }
    for (size_t i = start; i < end; i++)
    {
        put_line(p->items[i].pad, p->items[i].label, style_code(p->items[i].style));
    }
}

static void bottom_bar(const char *left, const char *centre, const char *right)
{
    char bar[W + 1];
    size_t cl = strlen(centre);
    size_t rl = strlen(right);

    memset(bar, ' ', W);
    bar[W] = '\0';
    memcpy(&bar[1], left, strlen(left));
    memcpy(&bar[(W - cl) / 2], centre, cl);
    memcpy(&bar[W - 1 - rl], right, rl);
    put_line(0, bar, BOLD);
}

static const char *hint(PageId id)
{
    switch (id)
    {
        case PG_CONTACTS: return "↑↓ move  ←→ page  / search  a add  ⏎ open  ⌫ back";
        case PG_CALLS:
        case PG_INBOX:    return "↑↓ move  ←→ page  ⏎ open  ⌫ back";
        case PG_CHAT:     return "↑↓ scroll  ⏎ reply  i incoming  ⌫ back";
        default:          return "↑↓ move  ⏎ select  ⌫ back  q quit";
    }
}

static void draw(unsigned reads, unsigned writes)
{
    const Page *p = &g_stack[g_depth - 1];
    char buf[W + 1];
    char clock[16];
    char pos[24] = "";
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(clock, sizeof(clock), "%H:%M", &tm);

    printf("\x1b[H\x1b[2J");
    hrule("┌", "┐");
    row_text(buf, clock, "UniQOS");
    put_line(0, buf, DIM);
    hrule("├", "┤");
    put_centred(p->title, BOLD);
    hrule("├", "┤");

    switch (p->id)
    {
        case PG_DETAILS: draw_details(p); break;
        case PG_CHAT:    draw_chat(p); break;
        case PG_CALLS:   draw_list(p, p->offset, ROWS, "No calls"); break;
        case PG_INBOX:   draw_list(p, p->offset, ROWS, "No messages"); break;
        case PG_CONTACTS: draw_list(p, p->offset, ROWS, "No contacts"); break;
        default:         draw_list(p, p->offset, ROWS, ""); break;
    }

    hrule("├", "┤");
    if ((p->id == PG_CALLS) || (p->id == PG_INBOX) || (p->id == PG_CONTACTS))
    {
        if (p->total > 0)
        {
            snprintf(pos, sizeof(pos), "%zu/%zu", p->cursor + 1, p->total);
        }
    } else if ((p->id == PG_CHAT) && (p->total > ROWS) && (p->offset < (p->total - ROWS))) {
        strcpy(pos, "more ^");
    }
    bottom_bar((p->id == PG_CHAT) ? "Reply" : "Select", pos, (g_depth == 1) ? "Exit" : "Back");
    hrule("└", "┘");

    printf(DIM "  %s" RESET "\n", hint(p->id));
    printf(DIM "  last key: %u reads, %u writes" RESET "\n", reads, writes);
    if (g_flash[0] != '\0')
    {
        printf("  %s\n", g_flash);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 *  Navigation and actions
 * ------------------------------------------------------------------ */

static void push(PageId id, const char *title, const char *phone)
{
    if (g_depth == MAX_DEPTH)
    {
        return;
    }
    Page *p = &g_stack[g_depth++];
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->stale = true;
    snprintf(p->title, sizeof(p->title), "%s", (title != NULL) ? title : "");
    snprintf(p->phone, sizeof(p->phone), "%s", (phone != NULL) ? phone : "");
}

static void pop(void)
{
    free(g_stack[g_depth - 1].items);
    g_depth--;
    if (g_depth > 0)
    {
        g_stack[g_depth - 1].stale = true;
    }
}

static void move(Page *p, long delta)
{
    if (p->id == PG_CHAT)
    {
        // up scrolls back in time
        long off = (long)p->offset - delta;
        p->offset = (off < 0) ? 0 : (size_t)off;
        return;
    }
    if (p->total == 0)
    {
        return;
    }
    long cur = (long)p->cursor + delta;
    long last = (long)p->total - 1;
    p->cursor = (size_t)((cur < 0) ? 0 : ((cur > last) ? last : cur));
}

static const char *cursor_phone(const Page *p)
{
    if ((p->total == 0) || (p->cursor < p->base) || ((p->cursor - p->base) >= p->n_items))
    {
        return NULL;
    }
    return p->items[p->cursor - p->base].phone;
}

static bool send_message(const char *phone, bool out)
{
    char text[SMS_MAX_MESSAGE_LENGTH * 2];

    if (!prompt(out ? "Message: " : "Incoming message: ", text, sizeof(text)))
    {
        return false;
    }
    begin_io();
    STRG_RET ret = db_message_add(phone, next_ts(), out, text);
    report(out ? "Sent" : "Received", ret);
    return ret == STRG_OK;
}

static void call(const char *phone, const char *who)
{
    char what[W + 16];

    begin_io();
    STRG_RET ret = db_phone_req(DB_REQ_CALL_ADD, phone, NULL);
    snprintf(what, sizeof(what), "Called %s", who);
    report(what, ret);
}

static void details_action(Page *p)
{
    const char *opt = p->items[p->cursor].label;
    char name[INPUT_LEN];
    char phone[INPUT_LEN];
    char yn[8];

    if (strcmp(opt, "Messages") == 0)
    {
        push(PG_CHAT, NULL, p->phone);
    } else if (strcmp(opt, "Call") == 0) {
        call(p->phone, p->title);
    } else if (strcmp(opt, "Send Message") == 0) {
        if (send_message(p->phone, true))
        {
            push(PG_CHAT, NULL, p->phone);
        }
    } else if (strcmp(opt, "Add Contact") == 0) {
        if (prompt("Name: ", name, sizeof(name)))
        {
            begin_io();
            report("Add contact", db_contact_add(name, p->phone));
        }
    } else if (strcmp(opt, "Edit Contact") == 0) {
        // empty input keeps the current value
        prompt("Name (Enter keeps): ", name, sizeof(name));
        prompt("Phone (Enter keeps): ", phone, sizeof(phone));
        if (name[0] == '\0')
        {
            snprintf(name, sizeof(name), "%s", (strcmp(p->title, "(unnamed)") == 0) ? "" : p->title);
        }
        if (phone[0] == '\0')
        {
            snprintf(phone, sizeof(phone), "%s", p->phone);
        }
        begin_io();
        STRG_RET ret = db_contact_update(p->phone, name, phone);
        report("Edit contact", ret);
        if (ret == STRG_OK)
        {
            snprintf(p->phone, sizeof(p->phone), "%.*s", MAX_PHONE_LEN, phone);
        }
    } else if (strcmp(opt, "Delete Contact") == 0) {
        prompt("Delete contact and chat? (y/N): ", yn, sizeof(yn));
        if ((yn[0] == 'y') || (yn[0] == 'Y'))
        {
            begin_io();
            STRG_RET ret = db_phone_req(DB_REQ_CONTACT_REMOVE, p->phone, NULL);
            report("Delete contact", ret);
            if (ret == STRG_OK)
            {
                pop();
                return;
            }
        }
    }
    p->stale = true;
}

static void select_row(Page *p)
{
    char phone[INPUT_LEN];
    const char *row_phone = cursor_phone(p);

    switch (p->id)
    {
        case PG_MENU:
            push((PageId[]){PG_PHONE, PG_SMS, PG_CONTACTS}[p->cursor], p->items[p->cursor].label, NULL);
            break;
        case PG_PHONE:
            if (p->cursor == 1)
            {
                push(PG_CALLS, "Call History", NULL);
            } else if (prompt("Number: ", phone, sizeof(phone))) {
                call(phone, phone);
            }
            break;
        case PG_SMS:
            if (p->cursor == 1)
            {
                push(PG_INBOX, "All Messages", NULL);
            } else if (prompt("To (number): ", phone, sizeof(phone)) && send_message(phone, true)) {
                push(PG_CHAT, NULL, phone);
            }
            break;
        case PG_CALLS:
        case PG_CONTACTS:
            if (row_phone != NULL)
            {
                push(PG_DETAILS, NULL, row_phone);
            }
            break;
        case PG_INBOX:
            if (row_phone != NULL)
            {
                push(PG_CHAT, NULL, row_phone);
            }
            break;
        case PG_DETAILS:
            if (p->total > 0)
            {
                details_action(p);
            }
            break;
        case PG_CHAT:
            if (send_message(p->phone, true))
            {
                p->offset = 0;
                p->stale = true;
            }
            break;
    }
}

static void other_key(Page *p, int ch)
{
    char prefix[INPUT_LEN];
    char phone[INPUT_LEN];
    size_t rank;

    if ((p->id == PG_CONTACTS) && (ch == 'a'))
    {
        if (prompt("Number: ", phone, sizeof(phone)) && prompt("Name: ", prefix, sizeof(prefix)))
        {
            begin_io();
            STRG_RET ret = db_contact_add(prefix, phone);
            report("Add contact", ret);
            // move the cursor to the new contact
            if ((ret == STRG_OK) && (db_contact_seek(prefix, &rank) == STRG_OK))
            {
                p->cursor = rank;
            }
        }
    } else if ((p->id == PG_CONTACTS) && (ch == '/'))
    {
        if (prompt("Search: ", prefix, sizeof(prefix)))
        {
            begin_io();
            STRG_RET ret = db_contact_seek(prefix, &rank);
            report("Seek", ret);
            if (ret == STRG_OK)
            {
                // matching name at the top of the screen
                p->cursor = rank;
                p->offset = rank;
            }
        }
    } else if ((p->id == PG_CHAT) && ((ch == 'i') || (ch == 'r'))) {
        if (send_message(p->phone, ch == 'r'))
        {
            p->offset = 0;
            p->stale = true;
        }
    }
}

static void handle(Key key, int ch)
{
    Page *p = &g_stack[g_depth - 1];

    switch (key)
    {
        case K_QUIT:   g_quit = true; break;
        case K_BACK:   pop(); break;
        case K_UP:     move(p, -1); break;
        case K_DOWN:   move(p, 1); break;
        case K_LEFT:   move(p, -ROWS); break;
        case K_RIGHT:  move(p, ROWS); break;
        case K_SELECT: select_row(p); break;
        case K_OTHER:  other_key(p, ch); break;
        default:       break;
    }
}

void phone_ui_run(DatabaseTaskContext *task)
{
    static bool registered;
    int ch = 0;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        printf("  `run` needs an interactive terminal\n");
        return;
    }

    // one request outstanding at a time (platform_os has no queue delete, so it is kept)
    if (g_reply_q == NULL)
    {
        g_reply_q = platform_os_QueueCreate(1, sizeof(DbResponse));
        if (g_reply_q == NULL)
        {
            printf("  couldn't create the reply queue\n");
            return;
        }
    }

    g_task = task;
    g_depth = 0;
    g_quit = false;
    g_flash[0] = '\0';
    tcgetattr(STDIN_FILENO, &g_saved);
    if (!registered)
    {
        atexit(term_restore);
        registered = true;
    }

    printf("\x1b[?1049h\x1b[?25l");
    term_raw(true);
    push(PG_MENU, "Menu", NULL);

    CardStats key_start = card_stats;
    while (!g_quit && (g_depth > 0))
    {
        Page *p = &g_stack[g_depth - 1];

        // contacts reload every time because only the visible window is held
        clamp(p);
        if (p->stale || (p->id == PG_CONTACTS))
        {
            refresh(p);
            p->stale = false;
            clamp(p);
        }
        draw(card_stats.reads - key_start.reads, card_stats.writes - key_start.writes);

        Key key = read_key(&ch);
        g_flash[0] = '\0';
        key_start = card_stats;
        handle(key, ch);
    }

    while (g_depth > 0)
    {
        pop();
    }
    term_raw(false);
    printf("\x1b[?25h\x1b[?1049l");
    fflush(stdout);
}
