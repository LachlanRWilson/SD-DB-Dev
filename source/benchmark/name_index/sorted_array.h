#ifndef NI_SORTED_ARRAY_H
#define NI_SORTED_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "name_index_common.h"

/**
 * Candidate 1: sorted array of fixed-size entries.
 * Binary search for lookup, memmove to open or close a gap on update.
 * (Benchmark copy; independent of source/app/include/sorted_array.h.)
 */

typedef struct __attribute__((aligned(4))) {
    SortedEntry e[NI_MAX_CONTACTS];  // entries in ascending order
    uint16_t count;                  // number of entries in use
    ni_tiebreak_fn tiebreak;         // full-name tie-break callback
} NameArray;

// In-order iterator (a position in the array)
typedef struct {
    const NameArray *a;
    uint16_t pos;
} NaIter;

// Use a hand-written word-copy loop instead of memmove (default false)
extern bool na_wordcopy;

void na_init(NameArray *a, ni_tiebreak_fn tiebreak);

bool na_insert(NameArray *a, const char *name, uint16_t contact_index);

bool na_insert_key(NameArray *a, const uint8_t key[NI_KEY_LEN], uint16_t contact_index);

bool na_remove(NameArray *a, const char *name, uint16_t contact_index);

uint16_t na_find(const NameArray *a, const char *name);

bool na_set_message(NameArray *a, const char *name, uint16_t contact_index, uint16_t message_index);

uint16_t na_count(const NameArray *a);

void na_begin(NaIter *it, const NameArray *a);

bool na_seek_rank(NaIter *it, const NameArray *a, uint16_t rank);

void na_seek(NaIter *it, const NameArray *a, const char *name);

bool na_next(NaIter *it, SortedEntry *out);

void na_rebuild_begin(NameArray *a);

bool na_rebuild_add(NameArray *a, const uint8_t key[NI_KEY_LEN], uint16_t contact_index);

void na_rebuild_finish(NameArray *a);

bool na_rebuild(NameArray *a, const char *const *names, const uint16_t *sectors, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif
