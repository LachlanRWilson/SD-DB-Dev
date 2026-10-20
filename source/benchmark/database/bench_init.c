#include "database_bench.h"

#include <string.h>

#include "bench_common.h"

/**
 * Init / recovery benchmark. Times database_format() on an empty card, then
 * database_init() (journal recovery + hash table, chat order, name index and
 * ring buffer rebuild) at growing database states, and finally database_init()
 * after a simulated power cut part way through a message add (journal rollback).
 * The global storage struct is wrapped, so the I/O of every init is counted.
 */

#define CUT_AFTER_WRITES 6  // a message add makes ~12-14 writes, so this cuts mid-transaction

#if DB_BENCH_QUICK
static const uint32_t k_contacts[] = {100, 500};
#define CHATS 50u
#define MESSAGES 200u
#define CALLS 20u
#define FORMAT_REPS 2u
#define MOUNT_REPS 2u
#define CUT_REPS 2u
#elif DB_BENCH_TARGET
static const uint32_t k_contacts[] = {1000, 2500, 5000, HASH_TABLE_ENTRIES};
#define CHATS 1000u
#define MESSAGES 5000u
#define CALLS 100u
#define FORMAT_REPS 3u
#define MOUNT_REPS 5u
#define CUT_REPS 5u
#else
static const uint32_t k_contacts[] = {1000, 2500, 5000, HASH_TABLE_ENTRIES};
#define CHATS 1000u
#define MESSAGES 5000u
#define CALLS 100u
#define FORMAT_REPS 10u
#define MOUNT_REPS 50u
#define CUT_REPS 20u
#endif

#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

static uint32_t g_messages;
static uint32_t g_calls;

static BcPoint point(Database *db)
{
    BcPoint pt = {(uint32_t)database_contact_count(db), (uint32_t)database_chat_count(db), 0, g_messages};
    return pt;
}

static bool init_add_message(Database *db, uint32_t id)
{
    char phone[BC_PHONE_LEN];
    char text[SMS_MAX_MESSAGE_LENGTH];
    bc_phone(id, phone);
    bc_message_text(g_messages, text);
    STRG_RET ret = database_message_add(db, phone, bc_timestamp(), (g_messages & 1u) != 0u, text);
    if (ret != STRG_OK) { bc_fail("message_add", id, ret); return false; }
    g_messages++;
    return true;
}

/**
 * @brief Time database_init() on the current card contents, checking that the
 *        rebuilt counts match.
 */
static bool bench_mount(Database *db, const char *op)
{
    BcPoint pt = point(db);
    bc_log("[bench] init %s contacts=%lu chats=%lu messages=%lu\n", op, (unsigned long)pt.contacts,
           (unsigned long)pt.chats, (unsigned long)pt.history);
    bc_begin(op, pt, 0, 1);
    for (uint32_t r = 0; r < MOUNT_REPS; r++)
    {
        STRG_RET ret;
        BC_TIMED(ret = database_init(db));
        if (ret != STRG_OK) { bc_fail(op, r, ret); return false; }
        bc_wrap_storage(db);
        if (database_contact_count(db) != pt.contacts || database_chat_count(db) != pt.chats)
        {
            bc_fail("mount/count", r, (int)database_contact_count(db));
            return false;
        }
    }
    bc_end();
    return true;
}

static bool bench_format(Database *db)
{
    bc_log("[bench] init format\n");
    bc_begin("format", point(db), 0, 1);
    for (uint32_t r = 0; r < FORMAT_REPS; r++)
    {
        STRG_RET ret;
        BC_TIMED(ret = database_format(db));
        if (ret != STRG_OK) { bc_fail("format", r, ret); return false; }
    }
    bc_end();
    // format leaves the in-RAM structures to the next init
    STRG_RET ret = database_init(db);
    if (ret != STRG_OK) { bc_fail("format/init", 0, ret); return false; }
    bc_wrap_storage(db);
    return true;
}

/**
 * @brief Cut the power part way through a message add, then time the init that
 *        rolls the journal back. The interrupted message must not appear.
 */
static bool bench_power_cut(Database *db)
{
    BcPoint pt = point(db);
    bc_log("[bench] init after power cut\n");
    bc_begin("mount_after_power_cut", pt, 0, 1);
    for (uint32_t r = 0; r < CUT_REPS; r++)
    {
        char phone[BC_PHONE_LEN];
        char text[SMS_MAX_MESSAGE_LENGTH];
        STRG_RET ret;
        bc_phone(r % CHATS, phone);
        bc_message_text(r, text);
        bc_power_cut_after(CUT_AFTER_WRITES);
        ret = database_message_add(db, phone, bc_timestamp(), false, text);
        bc_power_cut_after(-1);
        if (ret == STRG_OK) { bc_fail("power_cut/not_cut", r, ret); return false; }

        BC_TIMED(ret = database_init(db));
        if (ret != STRG_OK) { bc_fail("mount_after_power_cut", r, ret); return false; }
        bc_wrap_storage(db);
        if (database_contact_count(db) != pt.contacts || database_chat_count(db) != pt.chats)
        {
            bc_fail("power_cut/count", r, (int)database_contact_count(db));
            return false;
        }
    }
    bc_end();
    return true;
}

bool db_bench_init(Database *db)
{
    if (!db_bench_fresh(db)) { return false; }
    g_messages = 0;
    g_calls = 0;
    bc_meta("history_len,messages added since format (message history entries)");
    bc_meta("calls,%u added before the last mounts (call ring found by binary search)", CALLS);
    bc_meta("power_cut,writes fail after %d sector writes of a message add", CUT_AFTER_WRITES);

    if (!bench_format(db)) { return false; }
    if (!bench_mount(db, "mount")) { return false; }

    uint32_t stored = 0;
    for (uint32_t c = 0; c < COUNT_OF(k_contacts); c++)
    {
        if (!bc_fill_contacts(db, stored, k_contacts[c])) { return false; }
        stored = k_contacts[c];
        if (!bench_mount(db, "mount")) { return false; }
    }

    // one message opens a chat on contacts 0..CHATS-1, then round robin to MESSAGES
    for (uint32_t i = 0; i < CHATS; i++) { if (!init_add_message(db, i)) { return false; } }
    if (!bench_mount(db, "mount")) { return false; }
    while (g_messages < MESSAGES) { if (!init_add_message(db, g_messages % CHATS)) { return false; } }
    if (!bench_mount(db, "mount")) { return false; }

    for (; g_calls < CALLS; g_calls++)
    {
        char phone[BC_PHONE_LEN];
        bc_phone(g_calls % CHATS, phone);
        STRG_RET ret = database_call_add(db, phone);
        if (ret != STRG_OK) { bc_fail("call_add", g_calls, ret); return false; }
    }
    if (!bench_mount(db, "mount_with_calls")) { return false; }

    return bench_power_cut(db);
}
