/*
 * db_cli.c - interactive host demo of the database API (database.c).
 *
 * Runs the real database code over heap storage (HOST_BUILD), with the
 * "SD card" kept in an image file so data survives quitting, `reboot` and
 * simulated power cuts. Every command prints the STRG_RET it got back and
 * the number of sector reads/writes it cost.
 *
 *   db_cli [--image card.img] [--script file] [--interactive] [--echo]
 *
 * Type `help` for the commands, or `run` for the phone menus (phone_ui.c). See README.md for an example session.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "database.h"
#include "contact.h"
#include "message.h"
#include "usage_bitmap.h"
#include "free_list_stack.h"
#include "card_image.h"
#include "phone_ui.h"

#define MAX_TOKENS 32
#define LINE_LEN   512
#define PAGE_SIZE  20

static Database g_db;
static bool     g_mounted;
static bool     g_echo;
static uint32_t g_last_ts;

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */

/**
 * @brief Name of a STRG_RET value
 *
 * @param ret storage return code
 * @retval constant string
 */
const char *strg_name(STRG_RET ret)
{
    switch (ret)
    {
        case STRG_FAIL:    return "STRG_FAIL";
        case STRG_OK:      return "STRG_OK";
        case STRG_EMPTY:   return "STRG_EMPTY";
        case STRG_FULL:    return "STRG_FULL";
        case STRG_NEXT:    return "STRG_NEXT";
        case STRG_CORRUPT: return "STRG_CORRUPT";
        case STRG_EXISTS:  return "STRG_EXISTS";
        default:           return "STRG_?";
    }
}

/**
 * @brief Milliseconds from a monotonic clock
 *
 * @retval time in ms
 */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000.0) + (ts.tv_nsec / 1e6);
}

/**
 * @brief Timestamp for a new message, strictly increasing so chats keep their order
 *
 * @retval unix time (s)
 */
uint32_t next_ts(void)
{
    uint32_t t = (uint32_t)time(NULL);
    g_last_ts = (t > g_last_ts) ? t : (g_last_ts + 1);
    return g_last_ts;
}

static uint32_t msg_time(const Message *m)
{
    return ((uint32_t)m->timestamp_hi << 16) | m->timestamp_lo;
}

static void fmt_time(uint32_t t, char out[20])
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, 20, "%d/%m %H:%M:%S", &tm);
}

/* Copy a length-prefixed contact field into a printable string. */
static void field(char *out, const char *src, size_t len)
{
    memcpy(out, src, len);
    out[len] = '\0';
}

static void print_contact_row(size_t rank, const Contact *c)
{
    char name[MAX_NAME_LEN + 1];
    char phone[MAX_PHONE_LEN + 1];

    field(name, c->name, c->name_len);
    field(phone, c->phone, c->phone_len);
    printf("  %5zu  %-32s %s\n", rank, (c->name_len > 0) ? name : "(unnamed)", phone);
}

/* Contact name/phone for a message chat sector, via the chat header's contact index. */
static void chat_owner(uint16_t msg_sector, char *name, char *phone)
{
    MessageSectorBuffer sector;
    ContactBuffer contact;

    strcpy(name, "?");
    strcpy(phone, "?");
    if (read_message_sector(&g_db.storage, msg_sector, &sector) != STRG_OK)
    {
        return;
    }
    if (read_contact(&g_db.storage, sector.var.header.contact_index, &contact) != STRG_OK)
    {
        return;
    }
    field(name, contact.contact.name, contact.contact.name_len);
    field(phone, contact.contact.phone, contact.contact.phone_len);
    if (name[0] == '\0')
    {
        strcpy(name, "(unnamed)");
    }
}

static size_t arg_size(int argc, char **argv, int i, size_t def)
{
    return (i < argc) ? (size_t)strtoul(argv[i], NULL, 10) : def;
}

/* Join argv[from..] with spaces (for unquoted names and message text). */
static void join_args(int argc, char **argv, int from, char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = from; i < argc; i++)
    {
        if (i > from)
        {
            strncat(out, " ", cap - strlen(out) - 1);
        }
        strncat(out, argv[i], cap - strlen(out) - 1);
    }
}

static bool need_mount(void)
{
    if (!g_mounted)
    {
        printf("  database not mounted (run `format` or `reboot`)\n");
    }
    return g_mounted;
}

/**
 * @brief Mount the card image like a power cycle: wipe RAM state, run
 * database_init, then reinstall the storage wrapper
 *
 * @retval STRG_RET from database_init
 */
static STRG_RET boot(void)
{
    double t0 = now_ms();

    card_image_cut_after(-1);
    memset(usage_bitmap, 0, sizeof(usage_bitmap));
    STRG_RET ret = database_init(&g_db);
    card_image_wrap(&g_db);

    // init may format or roll back the journal before the wrapper is installed
    card_image_flush();

    g_mounted = (ret == STRG_OK);
    printf("  boot: %s, %zu contacts (%.1f ms)\n", strg_name(ret), database_contact_count(&g_db), now_ms() - t0);
    if (ret == STRG_CORRUPT)
    {
        printf("  superheader corrupted or outdated: run `format` to wipe the card\n");
    }
    return ret;
}

/* ------------------------------------------------------------------ *
 *  Commands
 * ------------------------------------------------------------------ */
typedef void (*CmdFn)(int argc, char **argv);

typedef struct
{
    const char *name;
    const char *usage;
    const char *help;
    int min_args;
    CmdFn fn;
} Command;

static void dispatch(int argc, char **argv);

static void cmd_add(int argc, char **argv)
{
    char name[LINE_LEN];
    join_args(argc, argv, 2, name, sizeof(name));
    printf("  %s\n", strg_name(database_contact_add(&g_db, name, argv[1])));
}

static void cmd_get(int argc, char **argv)
{
    ContactBuffer c;
    (void)argc;
    STRG_RET ret = database_contact_get(&g_db, argv[1], &c);
    if (ret == STRG_OK)
    {
        print_contact_row(0, &c.contact);
    }
    printf("  %s\n", strg_name(ret));
}

static void cmd_update(int argc, char **argv)
{
    char name[LINE_LEN];
    join_args(argc, argv, 3, name, sizeof(name));
    printf("  %s\n", strg_name(database_contact_update(&g_db, argv[1], name, argv[2])));
}

static void cmd_rm(int argc, char **argv)
{
    (void)argc;
    printf("  %s\n", strg_name(database_contact_remove(&g_db, argv[1])));
}

/* Print n contacts from rank start in name order. */
static void list_from(size_t start, size_t n)
{
    ContactBuffer page[PAGE_SIZE];
    size_t shown = 0;
    size_t count;
    STRG_RET ret = STRG_OK;

    while (shown < n)
    {
        size_t want = ((n - shown) < PAGE_SIZE) ? (n - shown) : PAGE_SIZE;
        ret = database_contact_list(&g_db, start + shown, want, &count, page);
        if ((ret != STRG_OK) || (count == 0))
        {
            break;
        }
        for (size_t i = 0; i < count; i++)
        {
            print_contact_row(start + shown + i, &page[i].contact);
        }
        shown += count;
    }

    printf("  %zu shown of %zu (%s)\n", shown, database_contact_count(&g_db),
           (shown == 0) ? strg_name(ret) : "STRG_OK");
}

static void cmd_list(int argc, char **argv)
{
    list_from(arg_size(argc, argv, 1, 0), arg_size(argc, argv, 2, PAGE_SIZE));
}

static void cmd_find(int argc, char **argv)
{
    char prefix[LINE_LEN];
    size_t rank;

    join_args(argc, argv, 1, prefix, sizeof(prefix));
    STRG_RET ret = database_contact_seek(&g_db, prefix, &rank);
    if (ret != STRG_OK)
    {
        printf("  %s\n", strg_name(ret));
        return;
    }
    printf("  first name >= \"%s\" is rank %zu\n", prefix, rank);
    list_from(rank, 10);
}

static void cmd_count(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("  %zu contacts\n", database_contact_count(&g_db));
}

static void cmd_msg(int argc, char **argv)
{
    char text[LINE_LEN];
    bool out;

    if (strcmp(argv[2], "out") == 0)
    {
        out = true;
    } else if (strcmp(argv[2], "in") == 0) {
        out = false;
    } else {
        printf("  direction must be `in` or `out`\n");
        return;
    }

    join_args(argc, argv, 3, text, sizeof(text));
    printf("  %s\n", strg_name(database_message_add(&g_db, argv[1], next_ts(), out, text)));
}

static void cmd_chat(int argc, char **argv)
{
    Message msgs[PAGE_SIZE];
    size_t count = 0;
    char when[20];
    size_t start = arg_size(argc, argv, 2, 0);
    size_t n = arg_size(argc, argv, 3, PAGE_SIZE);

    n = (n > PAGE_SIZE) ? PAGE_SIZE : n;
    STRG_RET ret = database_message_get_chat(&g_db, argv[1], start, n, &count, msgs);

    // newest first from the API: print oldest at the top like a chat window
    for (size_t i = count; i-- > 0;)
    {
        fmt_time(msg_time(&msgs[i]), when);
        printf("  %s  %s %.*s\n", when, msgs[i].direction ? "->" : "<-", SMS_MAX_MESSAGE_LENGTH, msgs[i].str);
    }
    printf("  %zu messages (%s)\n", count, strg_name(ret));
}

static void cmd_rmchat(int argc, char **argv)
{
    (void)argc;
    printf("  %s\n", strg_name(database_message_remove_chat(&g_db, argv[1])));
}

static void cmd_recent(int argc, char **argv)
{
    MessageHistoryEntry hist[PAGE_SIZE];
    size_t count = 0;
    char when[20];
    char name[MAX_NAME_LEN + 2];
    char phone[MAX_PHONE_LEN + 2];
    size_t n = arg_size(argc, argv, 1, 10);

    n = (n > PAGE_SIZE) ? PAGE_SIZE : n;
    STRG_RET ret = database_message_recent(&g_db, 0, n, &count, hist);
    for (size_t i = 0; i < count; i++)
    {
        fmt_time(msg_time(&hist[i].message), when);
        chat_owner(hist[i].msgInd, name, phone);
        printf("  %s  %-20.20s %-12s %s %.*s\n", when, name, phone, hist[i].message.direction ? "->" : "<-",
               SMS_MAX_MESSAGE_LENGTH, hist[i].message.str);
    }
    printf("  %zu messages (%s)\n", count, strg_name(ret));
}

static void cmd_call(int argc, char **argv)
{
    (void)argc;
    printf("  %s\n", strg_name(database_call_add(&g_db, argv[1])));
}

static void cmd_calls(int argc, char **argv)
{
    CallHistoryEntry calls[PAGE_SIZE];
    size_t count = 0;
    size_t n = arg_size(argc, argv, 1, 10);

    n = (n > PAGE_SIZE) ? PAGE_SIZE : n;
    STRG_RET ret = database_call_recent(&g_db, 0, n, &count, calls);
    for (size_t i = 0; i < count; i++)
    {
        print_contact_row(i, &calls[i].contact);
    }
    printf("  %zu calls (%s)\n", count, strg_name(ret));
}

static void cmd_seed(int argc, char **argv)
{
    static const char *const first[] = {
        "Olivia", "Jack", "Charlotte", "Noah", "Amelia", "William", "Isla", "Oliver", "Mia", "Leo",
        "Ava", "Henry", "Grace", "Thomas", "Chloe", "Lucas", "Zoe", "James", "Ruby", "Ethan",
        "Harper", "Max", "Sophie", "Liam", "Matilda", "Archie", "Ella", "Hudson", "Willow", "Oscar"};
    static const char *const last[] = {
        "Smith", "Jones", "Williams", "Brown", "Wilson", "Taylor", "Nguyen", "Johnson", "Martin", "White",
        "Anderson", "Walker", "Thompson", "Harris", "Lee", "Ryan", "Robinson", "Kelly", "King", "Davis"};
    const size_t n_first = sizeof(first) / sizeof(first[0]);
    const size_t n_last = sizeof(last) / sizeof(last[0]);
    size_t n = arg_size(argc, argv, 1, 100);
    size_t added = 0;
    size_t exists = 0;
    double t0 = now_ms();
    char name[64];
    char phone[16];

    for (size_t i = 0; i < n; i++)
    {
        // 7919 is coprime with 10^8, so phones don't repeat
        snprintf(name, sizeof(name), "%s %s", first[(i * 7) % n_first], last[(i * 13 + i / n_first) % n_last]);
        snprintf(phone, sizeof(phone), "04%08lu", (unsigned long)((12345678UL + i * 7919UL) % 100000000UL));

        STRG_RET ret = database_contact_add(&g_db, name, phone);
        if (ret == STRG_EXISTS)
        {
            exists++;
            continue;
        }
        if (ret != STRG_OK)
        {
            printf("  stopped at %zu: %s\n", i, strg_name(ret));
            break;
        }
        added++;

        // a short chat for every 5th contact
        if ((i % 5) == 0)
        {
            database_message_add(&g_db, phone, next_ts(), false, (char *)"Hey, are you free later?");
            database_message_add(&g_db, phone, next_ts(), true, (char *)"Yep, after 5");
        }

        if (((i + 1) % 1000) == 0)
        {
            printf("  ... %zu (%.0f ms)\n", i + 1, now_ms() - t0);
            fflush(stdout);
        }
    }

    printf("  added %zu, already existed %zu, total %zu contacts\n", added, exists, database_contact_count(&g_db));
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    boot();
}

static void cmd_powercut(int argc, char **argv)
{
    int writes = atoi(argv[1]);
    CardStats before = card_stats;

    printf("  power cut after %d sector writes, running: ", writes);
    for (int i = 2; i < argc; i++)
    {
        printf("%s ", argv[i]);
    }
    printf("\n");

    card_image_cut_after(writes);
    dispatch(argc - 2, &argv[2]);
    printf("  -- power lost: %u writes landed, %u dropped --\n",
           card_stats.writes - before.writes, card_stats.failed - before.failed);

    boot();
}

static void cmd_format(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    double t0 = now_ms();
    STRG_RET ret = database_format(&g_db);
    card_image_wrap(&g_db);
    card_image_flush();
    g_mounted = (ret == STRG_OK);
    printf("  %s (%.1f ms)\n", strg_name(ret), now_ms() - t0);
}

static void cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("  contacts          %zu / %d\n", database_contact_count(&g_db), HASH_TABLE_ENTRIES);
    printf("  name index        %u entries\n", g_db.name_index.occupancy);
    printf("  contact slots     %zu used, %zu free\n", free_list_used(&g_db.contact_allocator),
           free_list_available(&g_db.contact_allocator));
    printf("  message sectors   %zu used, %zu free\n", free_list_used(&g_db.message_allocator),
           free_list_available(&g_db.message_allocator));
    printf("  message history   %u / %u sectors\n", g_db.message_history.occupancy, g_db.message_history.size);
    printf("  call history      %u / %u sectors\n", g_db.call_history.occupancy, g_db.call_history.size);
    printf("  card              %zu sectors (%.1f MB)\n", CARD_RAW_SECTORS, CARD_BYTES / 1e6);
}

static void cmd_run(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    phone_ui_run(&g_db);
}

static void cmd_help(int argc, char **argv);
static void cmd_quit(int argc, char **argv);

static const Command g_cmds[] = {
    {"add",      "<phone> <name...>",               "add a contact", 3, cmd_add},
    {"get",      "<phone>",                         "look up a contact", 2, cmd_get},
    {"update",   "<phone> <new_phone> <name...>",   "rename and/or change number", 4, cmd_update},
    {"rm",       "<phone>",                         "remove a contact and its chat", 2, cmd_rm},
    {"list",     "[start] [n]",                     "contacts in name order", 1, cmd_list},
    {"find",     "<prefix>",                        "jump to the first name >= prefix", 2, cmd_find},
    {"count",    "",                                "number of contacts", 1, cmd_count},
    {"msg",      "<phone> in|out <text...>",        "add a message (creates an unnamed contact if new)", 4, cmd_msg},
    {"chat",     "<phone> [start] [n]",             "show a chat", 2, cmd_chat},
    {"rmchat",   "<phone>",                         "delete a chat, keep the contact", 2, cmd_rmchat},
    {"recent",   "[n]",                             "latest messages across all chats", 1, cmd_recent},
    {"call",     "<phone>",                         "log a call", 2, cmd_call},
    {"calls",    "[n]",                             "latest calls", 1, cmd_calls},
    {"seed",     "[n]",                             "bulk add n generated contacts (+ some chats)", 1, cmd_seed},
    {"reboot",   "",                                "wipe RAM and mount from the card", 1, cmd_reboot},
    {"powercut", "<writes> <command...>",           "cut power after N sector writes, then reboot", 3, cmd_powercut},
    {"format",   "",                                "wipe the card to an empty database", 1, cmd_format},
    {"stats",    "",                                "allocator and ring buffer usage", 1, cmd_stats},
    {"run",      "",                                "phone UI: contacts, messages, call history", 1, cmd_run},
    {"help",     "",                                "this list", 1, cmd_help},
    {"quit",     "",                                "exit (data is already on the card)", 1, cmd_quit},
};

#define N_CMDS (sizeof(g_cmds) / sizeof(g_cmds[0]))

static bool g_quit;

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < N_CMDS; i++)
    {
        char usage[64];
        snprintf(usage, sizeof(usage), "%s %s", g_cmds[i].name, g_cmds[i].usage);
        printf("  %-40s %s\n", usage, g_cmds[i].help);
    }
}

static void cmd_quit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_quit = true;
}

/* Commands that work without a mounted database */
static bool no_mount_needed(CmdFn fn)
{
    return (fn == cmd_help) || (fn == cmd_quit) || (fn == cmd_format) || (fn == cmd_reboot);
}

/* Run one tokenised command */
static void dispatch(int argc, char **argv)
{
    if (argc == 0)
    {
        return;
    }

    for (size_t i = 0; i < N_CMDS; i++)
    {
        if (strcmp(argv[0], g_cmds[i].name) != 0)
        {
            continue;
        }
        if (argc < g_cmds[i].min_args)
        {
            printf("  usage: %s %s\n", g_cmds[i].name, g_cmds[i].usage);
            return;
        }
        if (!no_mount_needed(g_cmds[i].fn) && !need_mount())
        {
            return;
        }
        g_cmds[i].fn(argc, argv);
        return;
    }
    printf("  unknown command `%s` (try `help`)\n", argv[0]);
}

/**
 * @brief Split a line into tokens. "double quotes" group words; # starts a comment
 *
 * @param line input line (modified in place)
 * @param argv token pointers
 * @retval number of tokens
 */
static int tokenise(char *line, char **argv)
{
    int argc = 0;
    char *p = line;

    while ((*p != '\0') && (argc < MAX_TOKENS))
    {
        while (isspace((unsigned char)*p))
        {
            p++;
        }
        if ((*p == '\0') || (*p == '#'))
        {
            break;
        }
        if (*p == '"')
        {
            argv[argc++] = ++p;
            while ((*p != '\0') && (*p != '"'))
            {
                p++;
            }
        } else {
            argv[argc++] = p;
            while ((*p != '\0') && !isspace((unsigned char)*p))
            {
                p++;
            }
        }
        if (*p != '\0')
        {
            *p++ = '\0';
        }
    }
    return argc;
}

/* Run one input line and report its storage cost */
static void run_line(char *line, bool show)
{
    char *argv[MAX_TOKENS];
    char copy[LINE_LEN];

    // echo the line (script mode) before it is split in place
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    copy[strcspn(copy, "\r\n")] = '\0';

    int argc = tokenise(line, argv);
    if (argc == 0)
    {
        // keep comments in script output, they are the presentation notes
        if (show && (copy[0] == '#'))
        {
            printf("%s\n", copy);
        }
        return;
    }
    if (show)
    {
        printf("db> %s\n", copy);
    }

    CardStats before = card_stats;
    double t0 = now_ms();
    dispatch(argc, argv);
    double dt = now_ms() - t0;

    if (strcmp(argv[0], "help") != 0)
    {
        printf("  (%u reads, %u writes, %.2f ms)\n", card_stats.reads - before.reads,
               card_stats.writes - before.writes, dt);
    }
}

/* Read lines from a stream until EOF or `quit` */
static void run_stream(FILE *in, bool prompt, bool show)
{
    char line[LINE_LEN];

    while (!g_quit)
    {
        if (prompt)
        {
            printf("db> ");
            fflush(stdout);
        }
        if (fgets(line, sizeof(line), in) == NULL)
        {
            break;
        }
        run_line(line, show);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    const char *image = "card.img";
    const char *script = NULL;
    bool interactive = false;

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "--image") == 0) && (i + 1 < argc))
        {
            image = argv[++i];
        } else if ((strcmp(argv[i], "--script") == 0) && (i + 1 < argc)) {
            script = argv[++i];
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        } else if (strcmp(argv[i], "--echo") == 0) {
            g_echo = true;
        } else {
            fprintf(stderr, "usage: %s [--image card.img] [--script file] [--interactive] [--echo]\n", argv[0]);
            return 2;
        }
    }

    printf("UniQOS database demo: card image %s (%.1f MB). Type `help`.\n", image, CARD_BYTES / 1e6);
    if (!card_image_open(image))
    {
        return 1;
    }
    boot();

    if (script != NULL)
    {
        FILE *f = fopen(script, "r");
        if (f == NULL)
        {
            perror(script);
            card_image_close();
            return 1;
        }
        run_stream(f, false, true);
        fclose(f);
    }

    if ((script == NULL) || interactive)
    {
        bool tty = isatty(STDIN_FILENO);
        run_stream(stdin, tty, g_echo || !tty);
    }

    card_image_close();
    return 0;
}
