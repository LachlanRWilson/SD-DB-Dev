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
typedef struct HashEntry HashEntry;

#define KEY_LEN 8

typedef struct {
    char key[KEY_LEN];      // built once by sorted_array_make_key
    const char *name;       // contact name
    size_t name_len;
    const char *phone;      // NULL / 0 for seek
    size_t phone_len;
    bool name_only;         // seek: stop after the case-folded compare
} SortedArrayProbe;

// Sorted Array
typedef struct {
    char key[KEY_LEN];      // Comparison Key (8B)
    uint16_t hash_slot;     // hash_slot (2B)
} SortedArrayEntry;

typedef struct {
    uint16_t occupancy;     // number of entries in sorted array
    Storage *storage;       // Storage abstraciton pointer
    SortedArrayEntry *mem;  // pointer to array memory (in RAM)
    HashEntry *htable_mem;  // pointer to hash table memory (such that SortedArrayEntry hash_slot hash access easy)
    STRG_RET (*tiebreak) (Storage *storage, HashEntry *htable_mem, const SortedArrayProbe *probe, SortedArrayEntry *entry, int *order);
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

/**
 * @brief Find the lower index of the name of the given contact, then move all the memory greater than index by one 
 * and place index into lower index + 1
 *
 * @param sa sorted array struct
 * @param in contact that is being searched for
 * @param contact_index contact sector index
 * @param message_index message sector index
 * @retval STRG_RET
 */
STRG_RET sorted_array_insert(SortedArray *sa, const Contact *contact, uint16_t contact_index);

STRG_RET sorted_array_remove(SortedArray *sa, const Contact *contact);

bool sorted_array_get_range(SortedArray *sa, size_t start, size_t n, size_t *out_count, SortedArrayEntry *out);

bool sorted_array_append(SortedArray *sa, SortedArrayEntry *in);

STRG_RET sorted_array_sort(SortedArray *sa);

/**
 * @brief Initialise a sorted array structure
 *
 * @param sa sorted array struct
 * @param storage storage struct
 * @param sa_memory pre-allocated RAM_D1 memory pointer
 * @param rank pointer to rank which is <= prefix
 * @param match_found pointer to bool which stores if the contact is in the sorted array
 * @retval STRG_RET
 */
bool sorted_array_init(SortedArray *sa, HashEntry *htable_mem, Storage *storage, SortedArrayEntry* sa_memory);

/**
 * @brief Initialise and rebuild the sorted array from the hash table. Every
 * contact is appended in hash slot order then sorted once
 *
 * @param sa sorted array struct
 * @param htable_mem hash table memory
 * @param storage storage struct
 * @param sa_memory pre-allocated RAM_D1 memory pointer (SORTED_ARRAY_ENTRIES long)
 * @retval STRG_RET
 */
STRG_RET sorted_array_build(SortedArray *sa, HashEntry *htable_mem, Storage *storage, SortedArrayEntry *sa_memory);

/**
 * @brief create a key from contact name, ignore case
 *
 * @param name contact name
 * @param name_len length of contact name
 * @param out pointer to key array
 */
void sorted_array_make_key(const char *name, size_t name_len, char out[KEY_LEN]);


#ifdef __cplusplus

}
#endif

#endif
