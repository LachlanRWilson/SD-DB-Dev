#include "database_bench.h"

#include <string.h>

#include "bench_common.h"

/**
 * Call history benchmark. Contacts 0..CONTACTS-1 are stored, then the call
 * history is filled to each checkpoint (80% known callers, 20% unknown) and
 * call_add / call_recent are timed. The last checkpoint is past the ring
 * capacity, so the ring buffer has wrapped (its rows are prefixed "wrapped_"). history_len in the CSV is the call
 * history here (calls added since the format, capped at the capacity).
 */

#define PAGE_LEN 20u
#define UNKNOWN_ID (BC_ID_SPARE + 300000u)  // never stored as contacts
#define CALL_CAPACITY (TOTAL_CALL_HISTORY_SECTOR_SIZE * CALL_HISTORY_SECTOR_CAPACITY)

enum { OP_FILL = 1, OP_KNOWN, OP_UNKNOWN, OP_RECENT };

#if DB_BENCH_QUICK
#define CONTACTS 100u
static const uint32_t k_checkpoints[] = {20, 200};
#define REPS 20u
#define WARM 2u
#else
#define CONTACTS 1000u
// the host's 1,000-rep adds (2,020 calls per checkpoint) can overshoot a checkpoint, so the wrap
// checkpoint leaves room for a fill on both platforms
static const uint32_t k_checkpoints[] = {100, 1000, 5000, CALL_CAPACITY, CALL_CAPACITY + 5000u};
#if DB_BENCH_TARGET
#define REPS 100u
#else
#define REPS 1000u
#endif
#define WARM 10u
#endif

#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

static CallHistoryEntry g_calls[PAGE_LEN];
static uint32_t g_added;
static uint32_t g_unknown;
static bool g_wrapped;     // ring has wrapped: op names get a "wrapped_" prefix

static uint32_t call_len(void)
{
    return (g_added < CALL_CAPACITY) ? g_added : CALL_CAPACITY;
}

static BcPoint point(Database *db)
{
    BcPoint pt = {(uint32_t)database_contact_count(db), (uint32_t)database_chat_count(db), 0, call_len()};
    return pt;
}

static bool add_call(Database *db, bool known, const char *what)
{
    char phone[BC_PHONE_LEN];
    STRG_RET ret;
    uint32_t id = known ? bc_rand_below(CONTACTS) : UNKNOWN_ID + g_unknown++;
    bc_phone(id, phone);
    BC_TIMED(ret = database_call_add(db, phone));
    if (ret != STRG_OK) { bc_fail(what, id, ret); return false; }
    g_added++;
    return true;
}

static bool fill_calls(Database *db, uint32_t count)
{
    if (count == 0u) { return true; }
    bc_seed(0xCA11u ^ g_added);
    bc_begin(g_wrapped ? "wrapped_fill_calls" : "fill_calls", point(db), 0, (count + BC_SAMPLES_MAX - 1u) / BC_SAMPLES_MAX);
    for (uint32_t i = 0; i < count; i++)
    {
        if (!add_call(db, bc_rand_below(10u) < 8u, "fill_calls")) { return false; }
    }
    bc_set_point(point(db));
    bc_end();
    return true;
}

static bool bench_add(Database *db, bool known)
{
    const char *op = g_wrapped ? (known ? "wrapped_call_add_known" : "wrapped_call_add_unknown")
                               : (known ? "call_add_known" : "call_add_unknown");
    bc_seed(0xCA11u ^ (call_len() << 8) ^ (known ? OP_KNOWN : OP_UNKNOWN));
    bc_begin(op, point(db), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        if (!add_call(db, known, op)) { return false; }
    }
    bc_end();
    return true;
}

static bool bench_recent(Database *db, bool top)
{
    const char *op = g_wrapped ? (top ? "wrapped_call_recent20_top" : "wrapped_call_recent20")
                               : (top ? "call_recent20_top" : "call_recent20");
    // stay a few sectors clear of the oldest entries, which a wrap may drop
    uint32_t len = call_len();
    uint32_t guard = 4u * CALL_HISTORY_SECTOR_CAPACITY;
    uint32_t span = (len > CALL_CAPACITY - guard) ? CALL_CAPACITY - guard : len;
    bc_seed(0xCA11u ^ (len << 8) ^ OP_RECENT ^ (top ? 0x100u : 0u));
    bc_begin(op, point(db), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t count = 0;
        uint32_t start = (top || span <= PAGE_LEN) ? 0u : bc_rand_below(span - PAGE_LEN + 1u);
        BC_TIMED(ret = database_call_recent(db, start, PAGE_LEN, &count, g_calls));
        if (ret != STRG_OK || count == 0u) { bc_fail(op, start, ret); return false; }
    }
    bc_end();
    return true;
}

bool db_bench_calls(Database *db)
{
    if (!db_bench_fresh(db)) { return false; }
    g_added = 0;
    g_unknown = 0;
    g_wrapped = false;
    bc_meta("contacts,%u", CONTACTS);
    bc_meta("call_capacity,%u", (unsigned)CALL_CAPACITY);
    bc_meta("history_len,calls added since format capped at call_capacity");

    if (!bc_fill_contacts(db, 0, CONTACTS)) { return false; }
    for (uint32_t c = 0; c < COUNT_OF(k_checkpoints); c++)
    {
        uint32_t target = k_checkpoints[c];
        g_wrapped = (target > CALL_CAPACITY);
        if (!fill_calls(db, (g_added < target) ? target - g_added : 0u)) { return false; }
        bc_log("[bench] calls added=%lu\n", (unsigned long)g_added);

        if (!bench_recent(db, true)) { return false; }
        if (!bench_recent(db, false)) { return false; }
        if (!bench_add(db, true)) { return false; }
        if (!bench_add(db, false)) { return false; }
    }
    return true;
}
