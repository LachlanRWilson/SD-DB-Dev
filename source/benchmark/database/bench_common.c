#include "bench_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if DB_BENCH_TARGET
#include "stm32h7xx_hal.h"
#include "sd_storage.h"
#define BC_STORAGE sd_storage
#else
#include <time.h>
#include "heap_storage.h"
#define BC_STORAGE heap_storage
#endif

#ifndef DB_BENCH_C_FLAGS
#define DB_BENCH_C_FLAGS "unknown"
#endif
#ifndef DB_BENCH_OPT_LEVEL
#define DB_BENCH_OPT_LEVEL "unknown"
#endif

#define BARRIER() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ *
 *  Platform: clock and output
 * ------------------------------------------------------------------ */
#if DB_BENCH_TARGET

#define PLATFORM "target"

typedef struct {
    uint32_t cyc;
    uint32_t tick;
} BcStamp;

static inline BcStamp bc_now(void)
{
    BcStamp s = {DWT->CYCCNT, HAL_GetTick()};
    return s;
}

/**
 * @brief Elapsed ns. The cycle counter wraps every 2^32 / SYSCLK (8.3 s at
 *        520 MHz), so regions over 4 s use the ms tick instead.
 */
static uint64_t bc_elapsed_ns(BcStamp a, BcStamp b)
{
    uint32_t ms = b.tick - a.tick;
    if (ms >= 4000u) { return (uint64_t)ms * 1000000u; }
    return ((uint64_t)(uint32_t)(b.cyc - a.cyc) * 1000u) / (SystemCoreClock / 1000000u);
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

static void log_puts(const char *s)
{
    sh_call(0x04, (void *)s);
}

#else

#define PLATFORM "host"

typedef uint64_t BcStamp;

static inline BcStamp bc_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t bc_elapsed_ns(BcStamp a, BcStamp b)
{
    return b - a;
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
    fflush(*o);
}

static void out_close(Out *o)
{
    fclose(*o);
}

static void log_puts(const char *s)
{
    fputs(s, stderr);
}

#endif

static Out g_csv;
static Out g_meta;
static const char *g_bench;

static void out_vprintf(Out *o, const char *fmt, va_list ap)
{
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    out_puts(o, buf);
}

static void out_printf(Out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_vprintf(o, fmt, ap);
    va_end(ap);
}

void bc_meta(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_vprintf(&g_meta, fmt, ap);
    va_end(ap);
    out_puts(&g_meta, "\n");
}

void bc_log(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_puts(buf);
}

bool bc_open(const char *prefix, const char *bench)
{
    char path[160];
    g_bench = bench;
    snprintf(path, sizeof(path), "%s.csv", prefix);
    if (!out_open(&g_csv, path)) { return false; }
    snprintf(path, sizeof(path), "%s_meta.txt", prefix);
    if (!out_open(&g_meta, path)) { out_close(&g_csv); return false; }

    out_puts(&g_csv, "bench,platform,opt_level,n_contacts,n_chats,msgs_per_chat,history_len,operation,reps,"
                     "ns_min,ns_median,ns_p99,ns_max,ns_mean,sectors_read_avg,sectors_written_avg\n");
    bc_meta("bench,%s", bench);
    bc_meta("platform,%s", PLATFORM);
    bc_meta("opt_level,%s", DB_BENCH_OPT_LEVEL);
    bc_meta("c_flags,%s", DB_BENCH_C_FLAGS);
    bc_meta("quick,%d", DB_BENCH_QUICK);
    bc_meta("build,%s %s", __DATE__, __TIME__);
#if DB_BENCH_TARGET
    bc_meta("sysclk_hz,%lu", (unsigned long)SystemCoreClock);
    bc_meta("icache,%s", (SCB->CCR & SCB_CCR_IC_Msk) ? "on" : "off");
    bc_meta("dcache,%s", (SCB->CCR & SCB_CCR_DC_Msk) ? "on" : "off");
    bc_meta("storage,sd_storage (SDMMC1, 4-bit)");
    bc_meta("timer,DWT CYCCNT -> ns; HAL tick (1 ms) for regions >= 4 s; interrupts enabled");
#else
    bc_meta("storage,heap_storage (RAM)");
    bc_meta("timer,CLOCK_MONOTONIC ns");
#endif
    bc_meta("percentiles,over every stride-th sample (stride 1 except fill_add); min/max/mean over all samples");
    return true;
}

void bc_close(bool ok)
{
    bc_meta("failures,%lu", (unsigned long)bc_failures());
    bc_meta("done,%d", ok ? 1 : 0);
    out_close(&g_meta);
    out_close(&g_csv);
    bc_log("[bench] done (%s)\n", ok ? "ok" : "FAILED");
}

/* ------------------------------------------------------------------ *
 *  Sector I/O counting
 * ------------------------------------------------------------------ */
static Storage g_inner;
static bool g_global_wrapped;
static uint32_t g_reads;
static uint32_t g_writes;
static int32_t g_writes_left = -1;  // -1 = never fail

static bool shim_read_block(void *context, uint32_t index, uint8_t *out)
{
    g_reads++;
    return g_inner.read_block(context, index, out);
}

static bool shim_read_multiblock(void *context, uint32_t index, size_t num, uint8_t *out)
{
    g_reads += (uint32_t)num;
    return g_inner.read_multiblock(context, index, num, out);
}

/**
 * @brief Power cut simulation: false once the armed write budget is used up.
 */
static bool write_allowed(void)
{
    if (g_writes_left == 0) { return false; }
    if (g_writes_left > 0) { g_writes_left--; }
    return true;
}

static bool shim_write_block(void *context, uint32_t index, uint8_t *in)
{
    if (!write_allowed()) { return false; }
    g_writes++;
    return g_inner.write_block(context, index, in);
}

static bool shim_write_multiblock(void *context, uint32_t index, size_t num, uint8_t *in)
{
    if (g_writes_left < 0)
    {
        g_writes += (uint32_t)num;
        return g_inner.write_multiblock(context, index, num, in);
    }
    // a cut is armed: split so it can land inside the multi-block write
    for (size_t i = 0; i < num; i++)
    {
        if (!shim_write_block(context, index + (uint32_t)i, in + i * SECTOR_SIZE)) { return false; }
    }
    return true;
}

void bc_power_cut_after(int32_t writes)
{
    g_writes_left = writes;
}

void bc_wrap_storage(Database *db)
{
    // database_init() copies the global storage struct into db->storage, so
    // wrapping the global once also counts the I/O of every later init
    if (!g_global_wrapped)
    {
        g_inner = BC_STORAGE;
        BC_STORAGE.read_block = shim_read_block;
        BC_STORAGE.read_multiblock = shim_read_multiblock;
        BC_STORAGE.write_block = shim_write_block;
        BC_STORAGE.write_multiblock = shim_write_multiblock;
        g_global_wrapped = true;
    }
    db->storage.read_block = shim_read_block;
    db->storage.read_multiblock = shim_read_multiblock;
    db->storage.write_block = shim_write_block;
    db->storage.write_multiblock = shim_write_multiblock;
}

/* ------------------------------------------------------------------ *
 *  Measurement
 * ------------------------------------------------------------------ */
static uint64_t g_samples[BC_SAMPLES_MAX];

static const char *g_op;
static BcPoint g_pt;
static uint32_t g_warm;
static uint32_t g_stride;
static uint32_t g_calls;     // record() calls including warm-up
static uint32_t g_count;     // timed samples (after warm-up)
static uint32_t g_stored;
static uint64_t g_sum;
static uint64_t g_min;
static uint64_t g_max;
static uint64_t g_reads_sum;
static uint64_t g_writes_sum;

static BcStamp g_t0;
static uint32_t g_r0;
static uint32_t g_w0;

static uint32_t g_failures;

void bc_begin(const char *op, BcPoint pt, uint32_t warm, uint32_t stride)
{
    g_op = op;
    g_pt = pt;
    g_warm = warm;
    g_stride = (stride == 0u) ? 1u : stride;
    g_calls = 0;
    g_count = 0;
    g_stored = 0;
    g_sum = 0;
    g_min = UINT64_MAX;
    g_max = 0;
    g_reads_sum = 0;
    g_writes_sum = 0;
}

void bc_set_point(BcPoint pt)
{
    g_pt = pt;
}

void bc_timer_start(void)
{
    g_r0 = g_reads;
    g_w0 = g_writes;
    BARRIER();
    g_t0 = bc_now();
    BARRIER();
}

void bc_timer_stop(void)
{
    BARRIER();
    BcStamp t1 = bc_now();
    BARRIER();
    uint64_t ns = bc_elapsed_ns(g_t0, t1);
    uint32_t reads = g_reads - g_r0;
    uint32_t writes = g_writes - g_w0;

    if (g_calls++ < g_warm) { return; }
    if ((g_count % g_stride) == 0u && g_stored < BC_SAMPLES_MAX) { g_samples[g_stored++] = ns; }
    g_count++;
    g_sum += ns;
    g_reads_sum += reads;
    g_writes_sum += writes;
    if (ns < g_min) { g_min = ns; }
    if (ns > g_max) { g_max = ns; }
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

/**
 * @brief "x.yy" of sum / count without floating point printf.
 */
static const char *avg_str(char buf[32], uint64_t sum, uint32_t count)
{
    char tmp[24];
    uint64_t x100 = (sum * 100u + count / 2u) / count;
    snprintf(buf, 32, "%s.%02u", u64_str(tmp, x100 / 100u), (unsigned)(x100 % 100u));
    return buf;
}

void bc_end(void)
{
    if (g_count == 0u) { return; }
    sort_u64(g_samples, g_stored);
    uint32_t p99 = (99u * g_stored + 99u) / 100u - 1u;
    char v0[24], v1[24], v2[24], v3[24], v4[24], a0[32], a1[32];
    out_printf(&g_csv, "%s,%s,%s,%lu,%lu,%lu,%lu,%s,%lu,%s,%s,%s,%s,%s,%s,%s\n",
               g_bench, PLATFORM, DB_BENCH_OPT_LEVEL, (unsigned long)g_pt.contacts, (unsigned long)g_pt.chats,
               (unsigned long)g_pt.msgs, (unsigned long)g_pt.history, g_op,
               (unsigned long)g_count, u64_str(v0, g_min), u64_str(v1, g_samples[(g_stored - 1u) / 2u]),
               u64_str(v2, g_samples[p99]), u64_str(v3, g_max), u64_str(v4, g_sum / g_count),
               avg_str(a0, g_reads_sum, g_count), avg_str(a1, g_writes_sum, g_count));
}

void bc_fail(const char *what, uint32_t id, int ret)
{
    if (g_failures++ == 0u)
    {
        bc_log("[bench] FAIL %s id=%lu ret=%d (contacts=%lu)\n", what, (unsigned long)id, ret, (unsigned long)g_pt.contacts);
        bc_meta("first_failure,%s,id=%lu,ret=%d,contacts=%lu", what, (unsigned long)id, ret, (unsigned long)g_pt.contacts);
    }
}

uint32_t bc_failures(void)
{
    return g_failures;
}

/* ------------------------------------------------------------------ *
 *  Deterministic data
 * ------------------------------------------------------------------ */
static uint32_t g_rng = 1u;

void bc_seed(uint32_t seed)
{
    g_rng = (seed == 0u) ? 1u : seed;
}

static uint32_t bc_rand(void)
{
    // xorshift32
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

uint32_t bc_rand_below(uint32_t n)
{
    return (uint32_t)(((uint64_t)bc_rand() * n) >> 32);
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void bc_phone(uint32_t id, char out[BC_PHONE_LEN])
{
    // 48271 is coprime with 10^8, so ids below 10^8 map to distinct numbers
    uint32_t digits = (uint32_t)(((uint64_t)id * 48271u + 12345u) % 100000000u);
    snprintf(out, BC_PHONE_LEN, "04%08lu", (unsigned long)digits);
}

static const char *const k_first[] = {
    "Alice", "Ben", "Charlotte", "Daniel", "Emily", "Finn", "Grace", "Harrison", "Isla", "Jack",
    "Kate", "Liam", "Mia", "Noah", "Olivia", "Patrick", "Quinn", "Ruby", "Samuel", "Tessa",
    "Uma", "Victor", "Willow", "Xavier", "Yasmin", "Zachary", "Amelia", "Lachlan", "Sophie", "Thomas",
    "Chloe", "Oscar"};

static const char *const k_last[] = {
    "Anderson", "Brown", "Chen", "Davies", "Evans", "Fraser", "Green", "Harris", "Ivanov", "Jones",
    "Kelly", "Lee", "Martin", "Nguyen", "OBrien", "Patel", "Quinlan", "Robinson", "Smith", "Taylor",
    "Underwood", "Vance", "Wilson", "Xu", "Young", "Zhang", "Campbell", "Mitchell", "Stewart", "Walker",
    "White", "Wright", "Thompson", "Hughes", "Edwards", "Clarke", "Scott", "Murphy", "Bennett", "Russo"};

#define K_FIRST_COUNT (sizeof(k_first) / sizeof(k_first[0]))
#define K_LAST_COUNT (sizeof(k_last) / sizeof(k_last[0]))

void bc_name(uint32_t id, char out[BC_NAME_MAX])
{
    uint32_t h = mix(id + 0x9e3779b9u);
    snprintf(out, BC_NAME_MAX, "%s %s", k_first[h % K_FIRST_COUNT], k_last[(h >> 8) % K_LAST_COUNT]);
}

bool bc_fill_contacts(Database *db, uint32_t first, uint32_t last)
{
    char name[BC_NAME_MAX];
    char phone[BC_PHONE_LEN];
    uint32_t count = last - first;
    uint32_t stride = (count + BC_SAMPLES_MAX - 1u) / BC_SAMPLES_MAX;

    BcPoint pt = {last, 0, 0, 0};
    bc_begin("fill_add", pt, 0, stride);
    for (uint32_t id = first; id < last; id++)
    {
        STRG_RET ret;
        bc_name(id, name);
        bc_phone(id, phone);
        BC_TIMED(ret = database_contact_add(db, name, phone));
        if (ret != STRG_OK) { bc_fail("fill_add", id, ret); return false; }
    }
    bc_end();
    return true;
}

static uint32_t g_timestamp = 1700000000u;

uint32_t bc_timestamp(void)
{
    return ++g_timestamp;
}

void bc_message_text(uint32_t seq, char out[SMS_MAX_MESSAGE_LENGTH])
{
    static const char k_words[] = "see you at the station around six then we can grab dinner and catch up on everything "
                                  "that happened this week with the project and the new flat ";
    uint32_t h = mix(seq + 0x51ed27u);
    uint32_t len = 20u + h % 101u;            // 20..120
    uint32_t off = (h >> 8) % (sizeof(k_words) - 1u - 120u + 1u);
    memcpy(out, &k_words[off], len);
    out[len] = '\0';
}
