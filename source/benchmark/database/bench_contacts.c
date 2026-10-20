#include "database_bench.h"

#include <string.h>

#include "bench_common.h"

/**
 * Contacts benchmark. The database is filled with contact ids 0..N-1 in
 * checkpoints; at each checkpoint every operation runs against that state.
 * Operations that change the set are undone untimed (add -> remove, remove ->
 * re-add, update -> revert), so ids 0..N-1 are always stored.
 */

#define PAGE_LEN 20u
#define MISS_ID_BASE (BC_ID_SPARE + 100000u)  // never stored
#define RENAME_ID_BASE 7000000u               // name source for update_name

enum { OP_ADD = 1, OP_GET_HIT, OP_GET_MISS, OP_UPDATE_NAME, OP_UPDATE_PHONE, OP_REMOVE, OP_LIST, OP_SEEK };

#if DB_BENCH_QUICK
static const uint32_t k_checkpoints[] = {100, 500};
#else
// the last checkpoint is one below capacity so "add" still has a free slot
static const uint32_t k_checkpoints[] = {100, 500, 1000, 2500, 5000, 7500, HASH_TABLE_ENTRIES - 1u};
#endif
#define K_CHECKPOINT_COUNT (sizeof(k_checkpoints) / sizeof(k_checkpoints[0]))

#if DB_BENCH_QUICK
#define REPS 20u
#define WARM 2u
#elif DB_BENCH_TARGET
#define REPS 100u
#define WARM 10u
#else
#define REPS 1000u
#define WARM 10u
#endif

static ContactBuffer g_page[PAGE_LEN];

static BcPoint point(uint32_t n)
{
    BcPoint pt = {n, 0, 0, 0};
    return pt;
}

static void op_seed(uint32_t n, uint32_t op)
{
    bc_seed(0x0DB0C0DEu ^ (n << 8) ^ op);
}

static bool bench_add(Database *db, uint32_t n)
{
    char name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    bc_begin("add", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        uint32_t id = BC_ID_SPARE + r;
        bc_name(id, name);
        bc_phone(id, phone);
        BC_TIMED(ret = database_contact_add(db, name, phone));
        if (ret != STRG_OK) { bc_fail("add", id, ret); return false; }
        ret = database_contact_remove(db, phone);
        if (ret != STRG_OK) { bc_fail("add/undo", id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_get(Database *db, uint32_t n, bool hit)
{
    char phone[BC_PHONE_LEN];
    ContactBuffer out;
    STRG_RET want = hit ? STRG_OK : STRG_EMPTY;
    op_seed(n, hit ? OP_GET_HIT : OP_GET_MISS);
    bc_begin(hit ? "get_hit" : "get_miss", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        uint32_t id = hit ? bc_rand_below(n) : MISS_ID_BASE + bc_rand_below(1000000u);
        bc_phone(id, phone);
        BC_TIMED(ret = database_contact_get(db, phone, &out));
        if (ret != want) { bc_fail(hit ? "get_hit" : "get_miss", id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_update_name(Database *db, uint32_t n)
{
    char name[BC_NAME_MAX];
    char new_name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    op_seed(n, OP_UPDATE_NAME);
    bc_begin("update_name", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        uint32_t id = bc_rand_below(n);
        bc_name(id, name);
        bc_name(RENAME_ID_BASE + r, new_name);
        bc_phone(id, phone);
        BC_TIMED(ret = database_contact_update(db, phone, new_name, phone));
        if (ret != STRG_OK) { bc_fail("update_name", id, ret); return false; }
        ret = database_contact_update(db, phone, name, phone);
        if (ret != STRG_OK) { bc_fail("update_name/undo", id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_update_phone(Database *db, uint32_t n)
{
    char name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    char new_phone[BC_PHONE_LEN];
    op_seed(n, OP_UPDATE_PHONE);
    bc_begin("update_phone", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        uint32_t id = bc_rand_below(n);
        bc_name(id, name);
        bc_phone(id, phone);
        bc_phone(BC_ID_SPARE + r, new_phone);
        BC_TIMED(ret = database_contact_update(db, phone, name, new_phone));
        if (ret != STRG_OK) { bc_fail("update_phone", id, ret); return false; }
        ret = database_contact_update(db, new_phone, name, phone);
        if (ret != STRG_OK) { bc_fail("update_phone/undo", id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_remove(Database *db, uint32_t n)
{
    char name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    op_seed(n, OP_REMOVE);
    bc_begin("remove", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        uint32_t id = bc_rand_below(n);
        bc_name(id, name);
        bc_phone(id, phone);
        BC_TIMED(ret = database_contact_remove(db, phone));
        if (ret != STRG_OK) { bc_fail("remove", id, ret); return false; }
        ret = database_contact_add(db, name, phone);
        if (ret != STRG_OK) { bc_fail("remove/undo", id, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_list(Database *db, uint32_t n)
{
    op_seed(n, OP_LIST);
    bc_begin("list20", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t count = 0;
        uint32_t start = bc_rand_below(n - PAGE_LEN + 1u);
        BC_TIMED(ret = database_contact_list(db, start, PAGE_LEN, &count, g_page));
        if (ret != STRG_OK || count != PAGE_LEN) { bc_fail("list20", start, ret); return false; }
    }
    bc_end();
    return true;
}

static bool bench_seek(Database *db, uint32_t n)
{
    char name[BC_NAME_MAX];
    op_seed(n, OP_SEEK);
    bc_begin("seek_prefix3", point(n), WARM, 1);
    for (uint32_t r = 0; r < WARM + REPS; r++)
    {
        STRG_RET ret;
        size_t rank = 0;
        bc_name(bc_rand_below(n), name);
        name[3] = '\0';
        BC_TIMED(ret = database_contact_seek(db, name, &rank));
        if (ret != STRG_OK) { bc_fail("seek_prefix3", r, ret); return false; }
    }
    bc_end();
    return true;
}

bool db_bench_contacts(Database *db)
{
    if (!db_bench_fresh(db)) { return false; }

    uint32_t stored = 0;
    for (uint32_t c = 0; c < K_CHECKPOINT_COUNT; c++)
    {
        uint32_t n = k_checkpoints[c];
        if (!bc_fill_contacts(db, stored, n)) { return false; }
        stored = n;
        if (database_contact_count(db) != n) { bc_fail("count", n, (int)database_contact_count(db)); return false; }
        bc_log("[bench] contacts N=%lu\n", (unsigned long)n);

        if (!bench_add(db, n)) { return false; }
        if (!bench_get(db, n, true)) { return false; }
        if (!bench_get(db, n, false)) { return false; }
        if (!bench_update_name(db, n)) { return false; }
        if (!bench_update_phone(db, n)) { return false; }
        if (!bench_remove(db, n)) { return false; }
        if (!bench_list(db, n)) { return false; }
        if (!bench_seek(db, n)) { return false; }
    }
    return true;
}
