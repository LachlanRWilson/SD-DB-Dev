#ifndef NI_BENCH_NAMES_H
#define NI_BENCH_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "name_index_common.h"

/**
 * Benchmark name table. Every contact sector maps to a "First Last" name
 * built from two const word lists (4B per contact instead of 64B), plus its
 * precomputed key. The tie-break callback compares these in RAM so SD
 * latency never reaches the index timings, and counts its calls.
 */

#if defined(NI_TARGET) && NI_TARGET
#define NI_PLACE __attribute__((section(".ram_d1")))
#else
#define NI_PLACE
#endif

typedef struct {
    uint16_t first;  // index into nb_first_words
    uint16_t last;   // index into nb_last_words
} NiName;

typedef enum {
    NB_DIST_RANDOM = 0,  // uniform over all first and last names
    NB_DIST_ASCENDING,   // random names, inserted in index order
    NB_DIST_DESCENDING,  // random names, inserted in reverse index order
    NB_DIST_PREFIX,      // ~30% long first names (>= 8 letters) that share keys
    NB_DIST_COUNT
} NbDist;

// Reserved words: never produced by a distribution
#define NB_FIRST_AAA 0  // "Aaa": with NB_LAST_AAA sorts before every name
#define NB_FIRST_ZZZ 1  // "Zzz": with NB_LAST_ZZZ sorts after every name
#define NB_LAST_AAA 0
#define NB_LAST_ZZZ 1
#define NB_LAST_MISS 2  // "Qzx": no stored name uses it, so lookups miss

#define NB_SEED 20261014u
#define NB_NAME_MAX 40

extern NiName nb_names[NI_MAX_CONTACTS];
extern uint8_t nb_keys[NI_MAX_CONTACTS][NI_KEY_LEN];
extern volatile uint32_t nb_tiebreak_calls;

const char *nb_dist_name(NbDist dist);

void nb_seed(uint32_t seed);

uint32_t nb_rand(void);

uint32_t nb_rand_below(uint32_t n);

void nb_draw(NbDist dist, NiName *out);

void nb_draw_miss(NiName *out);

void nb_set(uint16_t sector, NiName name);

void nb_set_probe(NiName name);

void nb_compose(NiName name, char *buf);

void nb_generate(NbDist dist, uint16_t n);

int nb_tiebreak(uint16_t sector_a, uint16_t sector_b);

#ifdef __cplusplus
}
#endif

#endif
