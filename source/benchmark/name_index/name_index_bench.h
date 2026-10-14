#ifndef NAME_INDEX_BENCH_H
#define NAME_INDEX_BENCH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Target: create the benchmark task (call before osKernelStart()).
 *        Results are written to the host over semihosting, so a debugger
 *        (OpenOCD with `arm semihosting enable`) must be attached.
 */
void NameIndexBench_Init(void);

/**
 * @brief Host: run the whole sweep, writing <out_prefix>.csv and
 *        <out_prefix>_meta.txt.
 *
 * @retval 0 on success.
 */
int name_index_bench_run(const char *out_prefix);

#ifdef __cplusplus
}
#endif

#endif
