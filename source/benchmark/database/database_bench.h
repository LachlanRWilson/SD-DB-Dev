#ifndef DATABASE_BENCH_H
#define DATABASE_BENCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "database.h"

/**
 * Database API benchmarks. Each one formats the database, fills it through the
 * public API and times the database_* calls at a series of fill levels.
 * Results: <prefix>.csv and <prefix>_meta.txt (see bench_common.h).
 */

typedef bool (*DbBenchFn)(Database *db);

bool db_bench_contacts(Database *db);
bool db_bench_messages(Database *db);
bool db_bench_calls(Database *db);
bool db_bench_init(Database *db);

/**
 * @brief Run one benchmark by name ("contacts", "messages", "calls", "init").
 *
 * @retval 0 on success, 1 on an output, unknown name or benchmark failure
 */
int database_bench_run(const char *bench, const char *out_prefix);

/**
 * @brief Format the database for a benchmark and wrap its storage for I/O counting.
 */
bool db_bench_fresh(Database *db);

#ifdef __cplusplus
}
#endif

#endif
