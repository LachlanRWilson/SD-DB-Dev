#include "name_index_bench.h"

#ifndef NI_TARGET
#define NI_TARGET 0
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "avl_tree.h"
#include "bench_names.h"
#include "sorted_array.h"

#if NI_TARGET
#include "stm32h7xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "hash_table.h"
#include "sd_baseline.h"
#else
#include <time.h>
#endif

#ifndef NI_C_FLAGS
#define NI_C_FLAGS "unknown"
#endif
#ifndef NI_OPT_LEVEL
#define NI_OPT_LEVEL "unknown"
#endif

#define REPS_SINGLE 1000u
#define REPS_SINGLE_HOST_COLD 50u
#define REPS_MAX 1000u
#define PAGE_LEN 20

enum { WHERE_RANDOM = 0, WHERE_FRONT, WHERE_END };

enum {
    OP_BULK_INSERT = 1, OP_BULK_REBUILD, OP_INSERT_RANDOM, OP_INSERT_FRONT, OP_INSERT_END, OP_REMOVE_RANDOM,
    OP_REMOVE_FIRST, OP_REMOVE_LAST, OP_FIND_HIT, OP_FIND_MISS, OP_UPDATE_MSG, OP_TRAVERSE, OP_PAGE_RANK,
    OP_PAGE_NAME, OP_TIMER
};

static const uint16_t k_sizes[] = {100, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000};
#define K_SIZES_COUNT (sizeof(k_sizes) / sizeof(k_sizes[0]))

/* ------------------------------------------------------------------ *
 *  Index storage: both candidates share one arena in the same region
 * ------------------------------------------------------------------ */
typedef union {
    NameArray a;
    AvlTree t;
} Arena;

NI_PLACE static Arena g_arena;

static uint16_t g_order[NI_MAX_CONTACTS];  // insertion order of sectors 0..g_n-1
static uint64_t g_samples[REPS_MAX];
static uint32_t g_calls[REPS_MAX];

static uint16_t g_n;
static NbDist g_dist;
static const char *g_impl;
static const char *g_dcache;
static bool g_cold;
static uint32_t g_bytes;

static uint32_t g_reps;
static uint32_t g_warm;
static uint32_t g_total;
static uint32_t g_iter;
static uint32_t g_stored;

volatile uint32_t g_sink;

/* ------------------------------------------------------------------ *
 *  Platform: clock, interrupt masking, cache eviction, output
 * ------------------------------------------------------------------ */
#if NI_TARGET

#define MEM_REGION "AXI_SRAM"

static inline uint32_t bench_now(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t bench_irq_off(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void bench_irq_restore(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static void bench_evict(void)
{
    if (SCB->CCR & SCB_CCR_DC_Msk) { SCB_CleanInvalidateDCache(); }
}

static int sh_call(int op, void *arg)
{
    register int r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

typedef int Out;

static bool out_open(Out *o, const char *path)
{
    uint32_t args[3] = {(uint32_t)path, 4u, (uint32_t)strlen(path)};
    *o = sh_call(0x01, args);
    return *o >= 0;
}

static void out_puts(Out *o, const char *s)
{
    uint32_t args[3] = {(uint32_t)*o, (uint32_t)s, (uint32_t)strlen(s)};
    sh_call(0x05, args);
}

static void out_close(Out *o)
{
    uint32_t args[1] = {(uint32_t)*o};
    sh_call(0x02, args);
}

static void log_line(const char *s)
{
    sh_call(0x04, (void *)s);
}

#else

#define MEM_REGION "host"

static inline uint32_t bench_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

static inline uint32_t bench_irq_off(void)
{
    return 0;
}

static inline void bench_irq_restore(uint32_t primask)
{
    (void)primask;
}

#define EVICT_BYTES (32u * 1024u * 1024u)
static uint8_t g_evict[EVICT_BYTES];

static void bench_evict(void)
{
    for (uint32_t i = 0; i < EVICT_BYTES; i += 64u) { g_evict[i]++; }
}

typedef FILE *Out;

static bool out_open(Out *o, const char *path)
{
    *o = fopen(path, "w");
    return *o != NULL;
}

static void out_puts(Out *o, const char *s)
{
    fputs(s, *o);
}

static void out_close(Out *o)
{
    fclose(*o);
}

static void log_line(const char *s)
{
    fputs(s, stderr);
}

#endif

static Out g_csv;
static Out g_meta;

static void out_printf(Out *o, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_puts(o, buf);
}

/* ------------------------------------------------------------------ *
 *  Measurement
 * ------------------------------------------------------------------ */
#define BARRIER() __asm__ volatile("" ::: "memory")

/*
 * Time one statement block with interrupts masked. Only repetitions after
 * the warm-up are stored; the tie-break call count is taken per repetition.
 */
#define TIMED(...) do { \
        uint32_t c0_ = nb_tiebreak_calls; \
        uint32_t irq_ = bench_irq_off(); \
        BARRIER(); \
        uint32_t t0_ = bench_now(); \
        BARRIER(); \
        __VA_ARGS__; \
        BARRIER(); \
        uint32_t t1_ = bench_now(); \
        BARRIER(); \
        bench_irq_restore(irq_); \
        record(t1_ - t0_, nb_tiebreak_calls - c0_); \
    } while (0)

/*
 * Time `count` steps of one statement block (step index `i`) as a single region. The counter
 * is read after every step and the 32-bit deltas are summed in 64 bits, so a
 * region longer than one counter period (2^32 cycles) is still measured
 * correctly as long as every single step is shorter than that period. The
 * host keeps one region: a clock read per step costs ~20 ns there, and its
 * 32-bit ns counter cannot wrap within these runtimes.
 */
#if NI_TARGET
#define TIMED_STEPS(i, count, ...) do { \
        uint32_t c0_ = nb_tiebreak_calls; \
        uint32_t irq_ = bench_irq_off(); \
        uint64_t acc_ = 0; \
        BARRIER(); \
        uint32_t prev_ = bench_now(); \
        BARRIER(); \
        for (uint32_t i = 0; i < (count); i++) { \
            __VA_ARGS__; \
            BARRIER(); \
            uint32_t now_ = bench_now(); \
            BARRIER(); \
            acc_ += (uint32_t)(now_ - prev_); \
            prev_ = now_; \
        } \
        bench_irq_restore(irq_); \
        record(acc_, nb_tiebreak_calls - c0_); \
    } while (0)
#else
#define TIMED_STEPS(i, count, ...) TIMED(for (uint32_t i = 0; i < (count); i++) { __VA_ARGS__; })
#endif

static void record(uint64_t ticks, uint32_t calls)
{
    if (g_iter++ < g_warm || g_stored >= g_reps) { return; }
    g_samples[g_stored] = ticks;
    g_calls[g_stored] = calls;
    g_stored++;
}

/**
 * @brief Cold mode: evict the caches before the timed region.
 */
static void bench_prepare(void)
{
    if (g_cold) { bench_evict(); }
}

/**
 * @brief Reset the sample buffers and seed the RNG so both implementations
 *        see the same targets for the same (distribution, N, op).
 */
static void begin_op(int op_id, uint32_t reps)
{
    if (reps > REPS_MAX) { reps = REPS_MAX; }
    g_reps = reps;
    g_warm = (reps >= 100u) ? 50u : 2u;
    g_total = g_warm + reps;
    g_iter = 0;
    g_stored = 0;
    nb_seed(NB_SEED ^ ((uint32_t)g_dist << 28) ^ ((uint32_t)g_n << 8) ^ (uint32_t)op_id);
}

static uint32_t single_reps(void)
{
#if !NI_TARGET
    if (g_cold) { return REPS_SINGLE_HOST_COLD; }
#endif
    return REPS_SINGLE;
}

static uint32_t traverse_reps(void)
{
    uint32_t reps = (g_n <= 1000u) ? REPS_SINGLE : 1000000u / g_n;
    return (reps < 50u) ? 50u : reps;
}

static uint32_t bulk_reps(void)
{
    if (g_n <= 1000u) { return 50u; }
    if (g_n <= 5000u) { return 10u; }
    return 5u;
}

static void sort_u64(uint64_t *v, uint32_t n)
{
    for (uint32_t gap = n / 2u; gap > 0u; gap /= 2u)
    {
        for (uint32_t i = gap; i < n; i++)
        {
            uint64_t x = v[i];
            uint32_t j = i;
            for (; j >= gap && v[j - gap] > x; j -= gap) { v[j] = v[j - gap]; }
            v[j] = x;
        }
    }
}

/**
 * @brief Decimal string of a 64-bit value (newlib-nano printf has no %llu).
 */
static const char *u64_str(char buf[24], uint64_t v)
{
    char *p = buf + 23;
    *p = '\0';
    do { *--p = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v != 0u);
    return p;
}

static void emit_row(const char *impl, uint16_t n, const char *dist, const char *op, const char *dcache, const char *mode, uint64_t *s, uint32_t reps, const uint32_t *calls, uint32_t divisor, uint32_t bytes)
{
    if (reps == 0u) { return; }
    sort_u64(s, reps);
    uint64_t call_sum = 0;
    uint32_t call_max = 0;
    for (uint32_t i = 0; i < reps && calls != NULL; i++)
    {
        call_sum += calls[i];
        if (calls[i] > call_max) { call_max = calls[i]; }
    }
    uint32_t avg100 = (uint32_t)((call_sum * 100u + reps / 2u) / reps);
    uint32_t p99 = (99u * reps + 99u) / 100u - 1u;
    char v0[24], v1[24], v2[24], v3[24];
    out_printf(&g_csv, "%s,%u,%s,%s,%s,%s,%s,%s,%lu,%s,%s,%s,%s,%lu.%02lu,%lu,%lu\n",
               impl, n, dist, op, MEM_REGION, dcache, mode, NI_OPT_LEVEL, (unsigned long)reps,
               u64_str(v0, s[0] / divisor), u64_str(v1, s[(reps - 1u) / 2u] / divisor),
               u64_str(v2, s[p99] / divisor), u64_str(v3, s[reps - 1u] / divisor),
               (unsigned long)(avg100 / 100u), (unsigned long)(avg100 % 100u), (unsigned long)call_max, (unsigned long)bytes);
}

/**
 * @brief Emit the stored samples of the current operation as one CSV row.
 */
static void end_op(const char *op, uint32_t divisor)
{
    emit_row(g_impl, g_n, nb_dist_name(g_dist), op, g_dcache, g_cold ? "cold" : "warm", g_samples, g_stored, g_calls, divisor, g_bytes);
}

static void pick_insert_name(int where, NiName *nm)
{
    if (where == WHERE_FRONT) { nm->first = NB_FIRST_AAA; nm->last = NB_LAST_AAA; }
    else if (where == WHERE_END) { nm->first = NB_FIRST_ZZZ; nm->last = NB_LAST_ZZZ; }
    else { nb_draw(g_dist, nm); }
}

/* ------------------------------------------------------------------ *
 *  Per-implementation operations (direct calls, see bench_ops.inc)
 * ------------------------------------------------------------------ */
static uint16_t array_last_sector(void)
{
    return g_arena.a.e[g_arena.a.count - 1u].contact_index;
}

static uint16_t avl_edge_sector(bool rightmost)
{
    const AvlTree *t = &g_arena.t;
    uint16_t i = t->root;
    for (;;)
    {
        uint16_t next = rightmost ? t->n[i].right : t->n[i].left;
        if (next == TREE_NIL) { return t->n[i].contact_index; }
        i = next;
    }
}

#define OPS(x) array_##x
#define IMPL_INIT() na_init(&g_arena.a, nb_tiebreak)
#define IMPL_INSERT(name, s) na_insert(&g_arena.a, (name), (s))
#define IMPL_INSERT_KEY(key, s) na_insert_key(&g_arena.a, (key), (s))
#define IMPL_REMOVE(name, s) na_remove(&g_arena.a, (name), (s))
#define IMPL_FIND(name) na_find(&g_arena.a, (name))
#define IMPL_SET_MSG(name, s, m) na_set_message(&g_arena.a, (name), (s), (m))
#define IMPL_REBUILD_BEGIN() na_rebuild_begin(&g_arena.a)
#define IMPL_REBUILD_ADD(key, s) na_rebuild_add(&g_arena.a, (key), (s))
#define IMPL_REBUILD_FINISH() na_rebuild_finish(&g_arena.a)
#define ITER_T NaIter
#define IMPL_BEGIN(it) na_begin((it), &g_arena.a)
#define IMPL_NEXT(it, e) na_next((it), (e))
#define IMPL_SEEK_RANK(it, k) na_seek_rank((it), &g_arena.a, (k))
#define IMPL_SEEK(it, name) na_seek((it), &g_arena.a, (name))
#define IMPL_FIRST_SECTOR() (g_arena.a.e[0].contact_index)
#define IMPL_LAST_SECTOR() array_last_sector()
#include "bench_ops.inc"
#undef OPS
#undef IMPL_INIT
#undef IMPL_INSERT
#undef IMPL_INSERT_KEY
#undef IMPL_REMOVE
#undef IMPL_FIND
#undef IMPL_SET_MSG
#undef IMPL_REBUILD_BEGIN
#undef IMPL_REBUILD_ADD
#undef IMPL_REBUILD_FINISH
#undef ITER_T
#undef IMPL_BEGIN
#undef IMPL_NEXT
#undef IMPL_SEEK_RANK
#undef IMPL_SEEK
#undef IMPL_FIRST_SECTOR
#undef IMPL_LAST_SECTOR

#define OPS(x) avl_ops_##x
#define IMPL_INIT() avl_init(&g_arena.t, nb_tiebreak)
#define IMPL_INSERT(name, s) avl_insert(&g_arena.t, (name), (s))
#define IMPL_INSERT_KEY(key, s) avl_insert_key(&g_arena.t, (key), (s))
#define IMPL_REMOVE(name, s) avl_remove(&g_arena.t, (name), (s))
#define IMPL_FIND(name) avl_find(&g_arena.t, (name))
#define IMPL_SET_MSG(name, s, m) avl_set_message(&g_arena.t, (name), (s), (m))
#define IMPL_REBUILD_BEGIN() avl_rebuild_begin(&g_arena.t)
#define IMPL_REBUILD_ADD(key, s) avl_rebuild_add(&g_arena.t, (key), (s))
#define IMPL_REBUILD_FINISH() avl_rebuild_finish(&g_arena.t)
#define ITER_T AvlIter
#define IMPL_BEGIN(it) avl_begin((it), &g_arena.t)
#define IMPL_NEXT(it, e) avl_next((it), (e))
#define IMPL_SEEK_RANK(it, k) avl_seek_rank((it), &g_arena.t, (k))
#define IMPL_SEEK(it, name) avl_seek((it), &g_arena.t, (name))
#define IMPL_FIRST_SECTOR() avl_edge_sector(false)
#define IMPL_LAST_SECTOR() avl_edge_sector(true)
#include "bench_ops.inc"

/* ------------------------------------------------------------------ *
 *  Sweep
 * ------------------------------------------------------------------ */

/**
 * @brief Set g_order for the distribution and log how many entries share
 *        their 8 byte key with another entry (uses the arena, untimed).
 */
static void prepare_order(bool log_keyshare)
{
    NameArray *a = &g_arena.a;
    na_init(a, nb_tiebreak);
    na_rebuild_begin(a);
    for (uint16_t s = 0; s < g_n; s++) { na_rebuild_add(a, nb_keys[s], s); }
    na_rebuild_finish(a);

    if (log_keyshare)
    {
        uint32_t shared = 0;
        for (uint16_t i = 0; i < g_n; i++)
        {
            bool prev = (i > 0) && memcmp(a->e[i].key, a->e[i - 1].key, NI_KEY_LEN) == 0;
            bool next = (i + 1 < g_n) && memcmp(a->e[i].key, a->e[i + 1].key, NI_KEY_LEN) == 0;
            if (prev || next) { shared++; }
        }
        out_printf(&g_meta, "keyshare,%s,%u,%lu,%lu.%lu%%\n", nb_dist_name(g_dist), g_n, (unsigned long)shared,
                   (unsigned long)(shared * 100u / g_n), (unsigned long)((shared * 1000u / g_n) % 10u));
    }

    for (uint16_t i = 0; i < g_n; i++)
    {
        uint16_t s = a->e[i].contact_index;
        if (g_dist == NB_DIST_ASCENDING) { g_order[i] = s; }
        else if (g_dist == NB_DIST_DESCENDING) { g_order[g_n - 1u - i] = s; }
        else { g_order[i] = i; }
    }
}

static uint32_t array_bytes(uint16_t n)
{
    return (uint32_t)(sizeof(NameArray) - sizeof(g_arena.a.e) + (size_t)n * sizeof(SortedEntry));
}

static uint32_t avl_bytes(uint16_t n)
{
    return (uint32_t)(sizeof(AvlTree) - sizeof(g_arena.t.n) + (size_t)n * sizeof(AvlNode));
}

static void timer_overhead(void)
{
    g_impl = "timer";
    g_n = 0;
    g_bytes = 0;
    begin_op(OP_TIMER, REPS_SINGLE);
    for (uint32_t r = 0; r < g_total; r++)
    {
        bench_prepare();
        TIMED((void)0);
    }
    emit_row("timer", 0, "n/a", "empty_timed_region", g_dcache, g_cold ? "cold" : "warm", g_samples, g_stored, NULL, 1, 0);
}

static void sweep(bool log_keyshare)
{
    char line[96];
    timer_overhead();
    for (int d = 0; d < NB_DIST_COUNT; d++)
    {
        g_dist = (NbDist)d;
        for (size_t k = 0; k < K_SIZES_COUNT; k++)
        {
            g_n = k_sizes[k];
            nb_generate(g_dist, g_n);
            prepare_order(log_keyshare);
            snprintf(line, sizeof(line), "[bench] dcache=%s %s %s N=%u\n", g_dcache, g_cold ? "cold" : "warm", nb_dist_name(g_dist), g_n);
            log_line(line);

            g_impl = "sorted_array";
            g_bytes = array_bytes(g_n);
            na_wordcopy = false;
            array_run_all(false);

            g_impl = "sorted_array_wordcopy";
            na_wordcopy = true;
            array_run_all(true);
            na_wordcopy = false;

            g_impl = "avl_tree";
            g_bytes = avl_bytes(g_n);
            avl_reset_max_depth();
            avl_ops_run_all(false);
            if (log_keyshare)
            {
                avl_ops_build();
                out_printf(&g_meta, "avl_depth,%s,%u,max_recursion=%u,height_after_build=%d\n", nb_dist_name(g_dist), g_n, avl_max_depth, avl_height(&g_arena.t));
            }
        }
    }
}

static void write_meta_static(void)
{
    out_printf(&g_meta, "platform,%s\n", NI_TARGET ? "target" : "host");
    out_printf(&g_meta, "compiler,%s\n", __VERSION__);
    out_printf(&g_meta, "c_flags,%s\n", NI_C_FLAGS);
    out_printf(&g_meta, "opt_level,%s\n", NI_OPT_LEVEL);
    out_printf(&g_meta, "seed,0x%08lx (xorshift32; per-op seed = seed ^ dist<<28 ^ N<<8 ^ op)\n", (unsigned long)NB_SEED);
    out_printf(&g_meta, "capacity,%u\n", NI_MAX_CONTACTS);
    out_printf(&g_meta, "sizeof,SortedEntry=%u,AvlNode=%u,NameArray=%u,AvlTree=%u,arena=%u\n", (unsigned)sizeof(SortedEntry), (unsigned)sizeof(AvlNode), (unsigned)sizeof(NameArray), (unsigned)sizeof(AvlTree), (unsigned)sizeof(Arena));
    out_printf(&g_meta, "name_table_bytes,%u\n", (unsigned)(sizeof(nb_names) + sizeof(nb_keys)));
    out_printf(&g_meta, "address,arena=%p,name_table=%p,array_base_align4=%d\n", (void *)&g_arena, (void *)nb_names, (int)(((uintptr_t)g_arena.a.e & 3u) == 0u));
#if NI_TARGET
    out_printf(&g_meta, "core_clock_hz,%lu\n", (unsigned long)SystemCoreClock);
    SCB->CSSELR = 0;
    __DSB();
    uint32_t ccsidr = SCB->CCSIDR;
    uint32_t line_bytes = 4u << ((ccsidr & 7u) + 2u);
    uint32_t ways = ((ccsidr >> 3) & 0x3FFu) + 1u;
    uint32_t sets = ((ccsidr >> 13) & 0x7FFFu) + 1u;
    out_printf(&g_meta, "dcache_geometry,bytes=%lu,ways=%lu,sets=%lu,line=%lu\n", (unsigned long)(line_bytes * ways * sets), (unsigned long)ways, (unsigned long)sets, (unsigned long)line_bytes);
    out_printf(&g_meta, "mpu_ctrl,0x%08lx\n", (unsigned long)MPU->CTRL);
    for (uint32_t r = 0; r < 16u; r++)
    {
        MPU->RNR = r;
        if (MPU->RASR & MPU_RASR_ENABLE_Msk)
        {
            out_printf(&g_meta, "mpu_region,%lu,rbar=0x%08lx,rasr=0x%08lx\n", (unsigned long)r, (unsigned long)MPU->RBAR, (unsigned long)MPU->RASR);
        }
    }
    out_printf(&g_meta, "mpu_note,no enabled region covers 0x24000000 unless listed above; the default memory map then applies (AXI SRAM: Normal, write-back write-allocate)\n");
    out_printf(&g_meta, "hash_table_bytes,%u (HashEntry %u B x HASH_TABLE_SIZE %u)\n", (unsigned)(sizeof(HashEntry) * HASH_TABLE_SIZE), (unsigned)sizeof(HashEntry), (unsigned)HASH_TABLE_SIZE);
    out_printf(&g_meta, "preemption,FreeRTOS running; PRIMASK set (all maskable IRQs off) around every timed region\n");
#else
    out_printf(&g_meta, "units,nanoseconds (cycles_* columns hold ns on host; non-authoritative)\n");
    out_printf(&g_meta, "cold_mode,touch a %u MiB buffer before each repetition; cold single ops use %u reps\n", (unsigned)(EVICT_BYTES >> 20), REPS_SINGLE_HOST_COLD);
#endif
}

#if NI_TARGET
static void sd_section(void)
{
    if (!sd_baseline_init())
    {
        out_printf(&g_meta, "sd_baseline,init failed (not measured)\n");
        return;
    }
    for (int op = 0; op < SDB_COUNT; op++)
    {
        uint32_t reps = (op == SDB_WRITE_CONTACT) ? 100u : 200u;
        uint32_t got = sd_baseline_measure((SdbOp)op, g_calls, reps);
        for (uint32_t i = 0; i < got; i++) { g_samples[i] = g_calls[i]; }
        emit_row("sd", 0, "n/a", sd_baseline_name((SdbOp)op), "off", "n/a", g_samples, got, NULL, 1, 0);
        out_printf(&g_meta, "sd_baseline,%s,ok=%lu/%lu,irqs enabled (driver polls with osDelay)\n", sd_baseline_name((SdbOp)op), (unsigned long)got, (unsigned long)reps);
    }
}
#endif

/**
 * @brief Run the full sweep, writing <out_prefix>.csv and <out_prefix>_meta.txt.
 */
int name_index_bench_run(const char *out_prefix)
{
    char path[160];
    snprintf(path, sizeof(path), "%s.csv", out_prefix);
    if (!out_open(&g_csv, path)) { return 1; }
    snprintf(path, sizeof(path), "%s_meta.txt", out_prefix);
    if (!out_open(&g_meta, path)) { out_close(&g_csv); return 1; }

    out_puts(&g_csv, "impl,N,distribution,operation,mem_region,dcache,cache_mode,opt_level,reps,cycles_min,cycles_median,cycles_p99,cycles_max,tiebreak_calls_avg,tiebreak_calls_max,bytes_total\n");
    write_meta_static();

#if NI_TARGET
    if (SCB->CCR & SCB_CCR_DC_Msk) { SCB_DisableDCache(); }
    g_dcache = "off";
    sd_section();

    SCB_EnableDCache();
    g_dcache = "on";
    g_cold = false;
    sweep(true);
    g_cold = true;
    sweep(false);

    SCB_DisableDCache();
    g_dcache = "off";
    g_cold = false;
    sweep(false);
#else
    g_dcache = "n/a";
    g_cold = false;
    sweep(true);
    g_cold = true;
    sweep(false);
#endif

    out_printf(&g_meta, "done,1\n");
    out_close(&g_meta);
    out_close(&g_csv);
    log_line("[bench] done\n");
    return 0;
}

#if NI_TARGET

#ifndef NI_BENCH_OUT_PREFIX
#define NI_BENCH_OUT_PREFIX "target"
#endif

static StaticTask_t g_bench_task_cb;
static uint32_t g_bench_task_stack[2048];

static void bench_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    SCB_EnableICache();

    name_index_bench_run(NI_BENCH_OUT_PREFIX);
    for (;;) { osDelay(1000); }
}

void NameIndexBench_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "NameIndexBench",
        .cb_mem = &g_bench_task_cb,
        .cb_size = sizeof(g_bench_task_cb),
        .stack_mem = g_bench_task_stack,
        .stack_size = sizeof(g_bench_task_stack),
        .priority = osPriorityHigh};
    osThreadNew(bench_task, NULL, &task_attr);
}

#else

int main(int argc, char **argv)
{
    const char *prefix = (argc > 1) ? argv[1] : "name_index_host";
    return name_index_bench_run(prefix);
}

#endif
