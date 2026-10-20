#ifndef DB_BENCH_COMMON_H
#define DB_BENCH_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "database.h"

/**
 * Shared harness for the database API benchmarks: clock, per-op statistics,
 * CSV/meta output (files on the host, semihosting on the target), sector I/O
 * counting and the deterministic contact generator.
 *
 * Times are in nanoseconds on both platforms. The target derives them from the
 * DWT cycle counter (falling back to the 1 ms HAL tick for regions longer than
 * 4 s, where the 32-bit counter would wrap), the host from CLOCK_MONOTONIC.
 * Interrupts stay enabled in the timed region because the SD driver waits on
 * DMA completion with the scheduler.
 */

#ifndef DB_BENCH_TARGET
#define DB_BENCH_TARGET 0
#endif

#ifndef DB_BENCH_QUICK
#define DB_BENCH_QUICK 0
#endif

#define BC_SAMPLES_MAX 1000u
#define BC_PHONE_LEN 11          // "04" + 8 digits + NUL
#define BC_NAME_MAX 32
#define BC_ID_SPARE 50000u       // ids from here up are never stored by a fill

// Database state an operation runs against (CSV columns)
typedef struct {
    uint32_t contacts;   // contacts stored
    uint32_t chats;      // chats with at least one message
    uint32_t msgs;       // messages in the chat the op targets (0 = not applicable)
    uint32_t history;    // entries in the message history ring buffer
} BcPoint;

/**
 * @brief Open <prefix>.csv and <prefix>_meta.txt and write the CSV header and
 *        the static metadata.
 */
bool bc_open(const char *prefix, const char *bench);

/**
 * @brief Write "done,<ok>" to the meta file and close both outputs.
 */
void bc_close(bool ok);

/**
 * @brief Append one free-form line to the meta file (newline added).
 */
void bc_meta(const char *fmt, ...);

/**
 * @brief Progress line (stderr on the host, semihosting console on the target).
 */
void bc_log(const char *fmt, ...);

/**
 * @brief Route the global storage struct (sd_storage / heap_storage) and
 *        db->storage through the sector counting shims. database_init() copies
 *        the global, so later inits are counted too.
 */
void bc_wrap_storage(Database *db);

/**
 * @brief Fail every write after the next `writes` sector writes (-1 disarms).
 *        Multi-block writes are split so the cut can land inside one.
 */
void bc_power_cut_after(int32_t writes);

/**
 * @brief Start timing an operation.
 *
 * @param op operation name for the CSV
 * @param pt database state the op runs against
 * @param warm untimed warm-up repetitions (recorded calls that are discarded)
 * @param stride keep every stride-th sample for the percentiles (min, max and
 *        mean always use every sample)
 */
void bc_begin(const char *op, BcPoint pt, uint32_t warm, uint32_t stride);

/**
 * @brief Replace the state recorded for the current operation (fill rows report
 *        the state they end at).
 */
void bc_set_point(BcPoint pt);

/**
 * @brief Emit the current operation as one CSV row.
 */
void bc_end(void);

void bc_timer_start(void);
void bc_timer_stop(void);

/**
 * @brief Time one statement block and record it into the current operation.
 */
#define BC_TIMED(...) do { \
        bc_timer_start(); \
        __VA_ARGS__; \
        bc_timer_stop(); \
    } while (0)

/**
 * @brief Note a failed API call. The first failure is logged with its context.
 */
void bc_fail(const char *what, uint32_t id, int ret);

uint32_t bc_failures(void);

void bc_seed(uint32_t seed);
uint32_t bc_rand_below(uint32_t n);

/**
 * @brief Phone number of contact id (unique for ids below 10^8).
 */
void bc_phone(uint32_t id, char out[BC_PHONE_LEN]);

/**
 * @brief "First Last" name of contact id (not unique, like real names).
 */
void bc_name(uint32_t id, char out[BC_NAME_MAX]);

/**
 * @brief Add contacts first..last-1 as one timed operation row ("fill_add").
 */
bool bc_fill_contacts(Database *db, uint32_t first, uint32_t last);

/**
 * @brief Monotonic non-zero message timestamp.
 */
uint32_t bc_timestamp(void);

/**
 * @brief Deterministic message text of 20..120 characters.
 */
void bc_message_text(uint32_t seq, char out[SMS_MAX_MESSAGE_LENGTH]);

#ifdef __cplusplus
}
#endif

#endif
