#ifndef SORTED_ARRAY_H
#define SORTED_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif


#include "mem_layout.h"
#include "storage.h"

// Opaque Declarations
typedef struct Contact Contact;

#define KEY_LEN 8

typedef struct {
    char key[KEY_LEN];      // built once by make_key
    const char *name;       // contact name
    size_t name_len;
    const char *phone;      // NULL / 0 for seek
    size_t phone_len;
    bool name_only;         // seek: stop after the case-folded compare
} SortedArrayProbe;

// Sorted Array
typedef struct {
    char key[KEY_LEN];   // Comparison Key (8B)
    uint16_t contact_index; // Contact sector index (2B)
    uint16_t message_index; // Message sector index (2B)
} SortedArrayEntry;

typedef struct {
    uint16_t occupancy;     // number of entries in sorted array
    Storage *storage;       // Storage abstraciton pointer
    SortedArrayEntry *mem;  // pointer to array memory (in RAM)
    STRG_RET (*tiebreak) (Storage *storage, const SortedArrayProbe *probe, SortedArrayEntry *entry, int *order);
} SortedArray;


STATIC_ASSERT(sizeof(SortedArrayEntry) == SORTED_ARRAY_ENTRY_BYTES, "Unkown Size for Sorted Array Entry");

/**
 * @brief Using binary search find the first rank <= the prefix
 *
 * @param key key to search
 * @param prefix string that is being searched
 * @param rank pointer to rank which is <= prefix
 * @retval STRG_RET
 */
STRG_RET sorted_array_seek(SortedArray *sa, const char *prefix, size_t *rank);

/**
 * @brief Using binary search find the contact in the sorted array
 *
 * @param key key to search
 * @param in contact that is being searched for
 * @param rank pointer to rank which is <= prefix
 * @param match_found pointer to bool which stores if the contact is in the sorted array
 * @retval STRG_RET
 */
STRG_RET sorted_array_find(SortedArray *sa, const Contact *in, size_t *rank, bool *match_found);

bool sorted_array_insert(SortedArray *sa, const Contact *contact, uint16_t contact_index, uint16_t message_index);

bool sorted_array_remove(SortedArray *sa, const Contact *contact);

bool sorted_array_get_range(SortedArray *sa, size_t start, size_t n, SortedArrayEntry *out);

bool sorted_array_append(SortedArray *sa, SortedArrayEntry in);

bool sorted_array_sort_qsort(SortedArray *sa);

bool sorted_array_init(SortedArray *sa, Storage *storage, SortedArrayEntry *sa_memory);


#ifdef __cplusplus

}
#endif

#endif
