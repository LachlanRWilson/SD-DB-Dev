#ifndef NAME_INDEX_COMMON_H
#define NAME_INDEX_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define NI_STATIC_ASSERT static_assert
#else
    #define NI_STATIC_ASSERT _Static_assert
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/**
 * Shared definitions for the two ordered name index candidates
 * (sorted_array.h and avl_tree.h). Both store the same logical entry and
 * order it the same way so the benchmark compares like with like.
 */

#define NI_KEY_LEN 8
#define NO_MESSAGE 0xFFFF

// Returned by find() when no entry matches
#define NI_NOT_FOUND 0xFFFF

/*
 * Sector value passed to the tie-break callback by find()/seek(): the
 * callback must resolve it to the name being searched for and, when the full
 * names are equal, return 0 instead of comparing sector numbers.
 */
#define NI_PROBE_SECTOR 0xFFFE

#ifndef NI_MAX_CONTACTS
#define NI_MAX_CONTACTS 10000
#endif

NI_STATIC_ASSERT(NI_MAX_CONTACTS < NI_PROBE_SECTOR, "NI_MAX_CONTACTS collides with the reserved sector values");

/**
 * @brief Tie-break callback, called only when two 8 byte keys are equal.
 *
 * Compares the full names of the contacts in sector_a and sector_b, then the
 * sector numbers. Returns <0, 0 or >0 like memcmp. The indexes never touch
 * Storage themselves; the caller decides where the full names come from.
 */
typedef int (*ni_tiebreak_fn)(uint16_t sector_a, uint16_t sector_b);

// 12B
typedef struct __attribute__((aligned(4))) {
    uint8_t key[NI_KEY_LEN];    // case-folded, zero-padded name prefix (8B)
    uint16_t contact_index;     // contact sector (2B)
    uint16_t message_index;     // first message sector, NO_MESSAGE if none (2B)
} SortedEntry;

NI_STATIC_ASSERT(sizeof(SortedEntry) == 12, "Unexpected SortedEntry size");

/**
 * @brief Build the comparison key of a name: the first NI_KEY_LEN bytes,
 *        ASCII lower-cased, zero padded (shorter names sort first).
 *
 * @param name NUL terminated contact name
 * @param key output key
 */
static inline void ni_make_key(const char *name, uint8_t key[NI_KEY_LEN])
{
    size_t i = 0;
    for (; i < NI_KEY_LEN && name[i] != '\0'; i++)
    {
        uint8_t c = (uint8_t)name[i];
        key[i] = (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
    }
    for (; i < NI_KEY_LEN; i++)
    {
        key[i] = 0;
    }
}

/**
 * @brief Total order shared by both indexes: key bytes, then the tie-break.
 */
static inline int ni_cmp(const uint8_t *key_a, uint16_t sector_a, const uint8_t *key_b, uint16_t sector_b, ni_tiebreak_fn tiebreak)
{
    int c = memcmp(key_a, key_b, NI_KEY_LEN);
    return (c != 0) ? c : tiebreak(sector_a, sector_b);
}

#ifdef __cplusplus
}
#endif

#endif
