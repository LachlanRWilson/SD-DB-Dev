#include "database_bench.h"

#include <string.h>

#include "bench_common.h"

#if DB_BENCH_TARGET
#include "stm32h7xx_hal.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#endif

typedef struct {
    const char *name;
    DbBenchFn fn;
} DbBench;

static const DbBench k_benches[] = {
    {"contacts", db_bench_contacts},
    {"messages", db_bench_messages},
    {"calls", db_bench_calls},
    {"init", db_bench_init},
};

#define K_BENCH_COUNT (sizeof(k_benches) / sizeof(k_benches[0]))

static Database g_db;

bool db_bench_fresh(Database *db)
{
    STRG_RET ret = database_init(db);
    if (ret != STRG_OK && ret != STRG_CORRUPT) { bc_fail("database_init", 0, ret); return false; }
    // the card may hold an older database: always start empty
    ret = database_format(db);
    if (ret != STRG_OK) { bc_fail("database_format", 0, ret); return false; }
    ret = database_init(db);
    if (ret != STRG_OK) { bc_fail("database_init", 0, ret); return false; }
    bc_wrap_storage(db);
    return database_contact_count(db) == 0u;
}

int database_bench_run(const char *bench, const char *out_prefix)
{
    const DbBench *b = NULL;
    for (uint32_t i = 0; i < K_BENCH_COUNT; i++)
    {
        if (strcmp(k_benches[i].name, bench) == 0) { b = &k_benches[i]; }
    }
    if (b == NULL) { bc_log("[bench] unknown benchmark '%s'\n", bench); return 1; }
    if (!bc_open(out_prefix, b->name)) { bc_log("[bench] cannot open %s\n", out_prefix); return 1; }

    bc_log("[bench] %s start\n", b->name);
    bool ok = b->fn(&g_db) && bc_failures() == 0u;
    bc_close(ok);
    return ok ? 0 : 1;
}

#if DB_BENCH_TARGET

#ifndef DB_BENCH_NAME
#define DB_BENCH_NAME "contacts"
#endif

static StaticTask_t g_bench_task_cb;
static uint32_t g_bench_task_stack[4096];

static void bench_task(void *argument)
{
    (void)argument;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    SCB_EnableICache();

    // fixed names: run_target.sh renames them, ocd_resume.py waits for target_meta.txt
    database_bench_run(DB_BENCH_NAME, "target");
    for (;;) { osDelay(1000); }
}

/**
 * @brief Replaces DB_Init() in the firmware when linked with -Wl,--wrap=DB_Init
 *        (see CMakeLists.txt), so main.c needs no benchmark hook.
 */
void __wrap_DB_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "DatabaseBench",
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
    const char *bench = (argc > 1) ? argv[1] : "contacts";
    const char *prefix = (argc > 2) ? argv[2] : "database_host";
    return database_bench_run(bench, prefix);
}

#endif
