#include "database_bench.h"

#include <string.h>

#include "bench_common.h"

/**
 * Messages and chats benchmark, in four phases on one database:
 *  A. chat count: contacts 0..CONTACTS-1 are stored, chats are opened on
 *     contacts 0..C-1 (one message each) for each checkpoint C
 *  B. chat depth: one dedicated chat grown to M messages for each checkpoint M
 *  C. remove_chat: fresh chats of M messages are built untimed and removed timed
 *  D. history wrap: messages are added round robin until the message history
 *     ring buffer has wrapped, then the history reads are timed again
 * Messages are never undone, so the message history only grows; every row
 * records the counts it ran against.
 */

#define PAGE_LEN 20u
#define DEEP_ID (BC_ID_SPARE + 90000u)       // phase B chat owner (named contact)
#define NEW_CONTACT_ID (BC_ID_SPARE + 100000u)  // unknown phones for add_msg_new_contact
#define REMOVE_ID (BC_ID_SPARE + 200000u)    // phase C chat owners (removed again)
#define HISTORY_CAPACITY (TOTAL_MESSAGE_HISTORY_SECTOR_SIZE * MESSAGE_HISTORY_SECTOR_CAPACITY)

enum { OP_ADD = 1, OP_ADD_NEW, OP_GET_CHAT, OP_CHAT_LIST, OP_RECENT, OP_DEEP, OP_REMOVE, OP_WRAP };

#if DB_BENCH_QUICK
#define CONTACTS 100u
static const uint32_t k_chat_checkpoints[] = {10, 50};
static const uint32_t k_depth_checkpoints[] = {20, 60};
static const uint32_t k_remove_sizes[] = {1, 10};
#define REPS 20u
#define WARM 2u
#define REMOVE_REPS 3u
#define WRAP_TARGET 300u
#else
#define CONTACTS 1000u
static const uint32_t k_chat_checkpoints[] = {10, 100, 500, 1000};
static const uint32_t k_depth_checkpoints[] = {20, 100, 500, 1000};
static const uint32_t k_remove_sizes[] = {1, 10, 100};
#if DB_BENCH_TARGET
#define REPS 100u
#define REMOVE_REPS 5u
#else
#define REPS 1000u
#define REMOVE_REPS 50u
#endif
#define WARM 10u
#define WRAP_TARGET (HISTORY_CAPACITY + 1000u)  // total messages added before phase D
#endif

// deep_add_msg grows the deep chat, so keep it below the gap between depth checkpoints
#if DB_BENCH_QUICK
#define DEEP_ADD_REPS 10u
#else
#define DEEP_ADD_REPS 50u
#endif

#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

static Message g_msgs[PAGE_LEN];
static ChatSummary g_chats[PAGE_LEN];
static MessageHistoryEntry g_history[PAGE_LEN];

static uint32_t g_added;     // messages added since the format (history entries written)
static uint32_t g_new_contacts;

static void op_seed(uint32_t x, uint32_t op)
{
    bc_seed(0x0BADCAFEu ^ (x << 8) ^ op);
}

static uint32_t history_len(void)
{
    return (g_added < HISTORY_CAPACITY) ? g_added : HISTORY_CAPACITY;
}

static BcPoint point(Database *db, uint32_t msgs)
{
    BcPoint pt = {(uint32_t)database_contact_count(db), (uint32_t)database_chat_count(db), msgs, history_len()};
    return pt;
}

/**
 * @brief Add one message to the chat of contact id, timed when `timed`.
 */
static bool add_msg(Database *db, uint32_t id, bool timed, const char *what)
{
    char phone[BC_PHONE_LEN];
    char text[SMS_MAX_MESSAGE_LENGTH];
    STRG_RET ret;
    bc_phone(id, phone);
    bc_message_text(g_added, text);
    uint32_t ts = bc_timestamp();
    bool dir = (g_added & 1u) != 0u;
    if (timed) { BC_TIMED(ret = database_message_add(db, phone, ts, dir, text)); }
    else { ret = database_message_add(db, phone, ts, dir, text); }
    if (ret != STRG_OK) { bc_fail(what, id, ret); return false; }
    g_added++;
    return true;
}

/**
 * @brief Add `count` messages as one strided row (every add timed).
 */
static bool fill_msgs(Database *db, const char *op, uint32_t first_id, uint32_t count, bool round_robin, uint32_t ids)
{
    if (count == 0u) { return true; }
    bc_begin(op, point(db, 0), 0, (count + BC_SAMPLES_MAX - 1u) / BC_SAMPLES_MAX);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = round_robin ? (first_id + i) % ids : first_id + i;
        if (!add_msg(db, id, true, op)) { return false; }
    }
    bc_set_point(point(db, 0));
    bc_end();
    return true;
}

static bool bench_add(Database *db, uint32_t chats, const char *op)
{
    op_seed(chats, OP_ADD);
    bc_begin(op, point(db, 0), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        if (!add_msg(db, bc_rand_below(chats), true, op)) { return false; }
    }
    bc_end();
    return true;
}

/**
 * @brief Message to a phone with no contact (creates an unnamed contact and a chat).
 */
static bool bench_add_new_contact(Database *db)
{
    bc_begin("add_msg_new_contact", point(db, 0), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        if (!add_msg(db, NEW_CONTACT_ID + g_new_contacts++, true, "add_msg_new_contact")) { return false; }
    }
    bc_end();
    return true;
}

static bool bench_get_chat(Database *db, const char *op, uint32_t id_range, uint32_t fixed_id, uint32_t msgs, uint32_t start)
{
    char phone[BC_PHONE_LEN];
    op_seed(msgs ^ start, OP_GET_CHAT);
    bc_begin(op, point(db, msgs), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t count = 0;
        uint32_t id = (id_range != 0u) ? bc_rand_below(id_range) : fixed_id;
        bc_phone(id, phone);
        BC_TIMED(ret = database_message_get_chat(db, phone, start, PAGE_LEN, &count, g_msgs));
        if (ret != STRG_OK || count == 0u) { bc_fail(op, id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_chat_list(Database *db, const char *op, bool top)
{
    uint32_t chats = (uint32_t)database_chat_count(db);
    op_seed(chats, OP_CHAT_LIST + (top ? 100u : 0u));
    bc_begin(op, point(db, 0), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t count = 0;
        uint32_t start = (top || chats <= PAGE_LEN) ? 0u : bc_rand_below(chats - PAGE_LEN + 1u);
        BC_TIMED(ret = database_chat_list(db, start, PAGE_LEN, &count, g_chats));
        if (ret != STRG_OK || count == 0u) { bc_fail(op, start, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_recent(Database *db, const char *op, bool top)
{
    // stay a few sectors clear of the oldest entries, which a wrap may drop
    uint32_t len = history_len();
    uint32_t span = (len > HISTORY_CAPACITY - 16u) ? HISTORY_CAPACITY - 16u : len;
    op_seed(len, OP_RECENT + (top ? 100u : 0u));
    bc_begin(op, point(db, 0), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t count = 0;
        uint32_t start = (top || span <= PAGE_LEN) ? 0u : bc_rand_below(span - PAGE_LEN + 1u);
        BC_TIMED(ret = database_message_recent(db, start, PAGE_LEN, &count, g_history));
        if (ret != STRG_OK || count == 0u) { bc_fail(op, start, ret); return false; }
    }
    bc_end();
    return true;
}

static bool phase_chats(Database *db)
{
    uint32_t opened = 0;
    for (uint32_t c = 0; c < COUNT_OF(k_chat_checkpoints); c++)
    {
        uint32_t chats = k_chat_checkpoints[c];
        if (!fill_msgs(db, "fill_first_msg", opened, chats - opened, false, 0)) { return false; }
        opened = chats;
        bc_log("[bench] messages chats=%lu\n", (unsigned long)database_chat_count(db));

        if (!bench_get_chat(db, "get_chat20_newest", chats, 0, 0, 0)) { return false; }
        if (!bench_chat_list(db, "chat_list20_top", true)) { return false; }
        if (!bench_chat_list(db, "chat_list20", false)) { return false; }
        if (!bench_recent(db, "recent20_top", true)) { return false; }
        if (!bench_recent(db, "recent20", false)) { return false; }
        if (!bench_add(db, chats, "add_msg")) { return false; }
        if (!bench_add_new_contact(db)) { return false; }
    }
    return true;
}

static bool phase_depth(Database *db)
{
    char name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    bc_name(DEEP_ID, name);
    bc_phone(DEEP_ID, phone);
    STRG_RET ret = database_contact_add(db, name, phone);
    if (ret != STRG_OK) { bc_fail("deep/contact_add", DEEP_ID, ret); return false; }

    uint32_t msgs = 0;
    for (uint32_t c = 0; c < COUNT_OF(k_depth_checkpoints); c++)
    {
        uint32_t m = k_depth_checkpoints[c];
        if (m > msgs)
        {
            bc_begin("fill_deep_chat", point(db, msgs), 0, (m - msgs + BC_SAMPLES_MAX - 1u) / BC_SAMPLES_MAX);
            for (; msgs < m; msgs++) { if (!add_msg(db, DEEP_ID, true, "fill_deep_chat")) { return false; } }
            bc_set_point(point(db, msgs));
            bc_end();
        }
        bc_log("[bench] messages deep chat M=%lu\n", (unsigned long)msgs);

        if (!bench_get_chat(db, "deep_get_chat20_newest", 0, DEEP_ID, msgs, 0)) { return false; }
        if (!bench_get_chat(db, "deep_get_chat20_middle", 0, DEEP_ID, msgs, msgs / 2u - PAGE_LEN / 2u)) { return false; }
        if (!bench_get_chat(db, "deep_get_chat20_oldest", 0, DEEP_ID, msgs, msgs - PAGE_LEN)) { return false; }

        bc_begin("deep_add_msg", point(db, msgs), WARM, 1);
        for (uint32_t r = 0; r < WARM + DEEP_ADD_REPS; r++)
        {
            if (!add_msg(db, DEEP_ID, true, "deep_add_msg")) { return false; }
        }
        bc_end();
        msgs += WARM + DEEP_ADD_REPS;
    }
    return true;
}

static bool phase_remove(Database *db)
{
    char phone[BC_PHONE_LEN];
    uint32_t id = REMOVE_ID;
    for (uint32_t s = 0; s < COUNT_OF(k_remove_sizes); s++)
    {
        uint32_t m = k_remove_sizes[s];
        bc_log("[bench] messages remove_chat M=%lu\n", (unsigned long)m);
        // every repetition removes a chat of exactly m messages, so no warm-up
        bc_begin("remove_chat", point(db, m), 0, 1);
        for (uint32_t r = 0; r < REMOVE_REPS; r++, id++)
        {
            STRG_RET ret;
            for (uint32_t i = 0; i < m; i++) { if (!add_msg(db, id, false, "remove_chat/build")) { return false; } }
            bc_phone(id, phone);
            BC_TIMED(ret = database_message_remove_chat(db, phone));
            if (ret != STRG_OK) { bc_fail("remove_chat", id, ret); return false; }
            ret = database_contact_remove(db, phone);
            if (ret != STRG_OK) { bc_fail("remove_chat/contact_remove", id, ret); return false; }
        }
        bc_end();
    }
    return true;
}

static bool phase_wrap(Database *db)
{
    uint32_t chats = k_chat_checkpoints[COUNT_OF(k_chat_checkpoints) - 1u];
    uint32_t need = (g_added < WRAP_TARGET) ? WRAP_TARGET - g_added : 0u;
    bc_log("[bench] messages history wrap (+%lu messages)\n", (unsigned long)need);
    if (!fill_msgs(db, "fill_history", 0, need, true, chats)) { return false; }

    if (!bench_recent(db, "wrapped_recent20_top", true)) { return false; }
    if (!bench_recent(db, "wrapped_recent20", false)) { return false; }
    if (!bench_chat_list(db, "wrapped_chat_list20_top", true)) { return false; }
    if (!bench_add(db, chats, "wrapped_add_msg")) { return false; }
    return true;
}

bool db_bench_messages(Database *db)
{
    if (!db_bench_fresh(db)) { return false; }
    g_added = 0;
    g_new_contacts = 0;
    bc_meta("contacts,%u", CONTACTS);
    bc_meta("history_capacity,%u", (unsigned)HISTORY_CAPACITY);
    bc_meta("history_len,messages added since format capped at history_capacity");

    if (!bc_fill_contacts(db, 0, CONTACTS)) { return false; }
    return phase_chats(db) && phase_depth(db) && phase_remove(db) && phase_wrap(db);
}
