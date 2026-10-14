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

#include "contact.h"


#define KEY_LEN 8

// 12B
typedef struct {
    uint8_t key[KEY_LEN];   // Comparison Key (8B)
    uint16_t contact_index; // Contact sector index (2B)
    uint16_t message_index; // Message sector index (2B)
} SortedEntry;

typedef struct {
    uint16_t occupancy;     // number of entries in sorted array
    SortedEntry *mem;       // pointer to array memory (in RAM)
    bool (*tiebreak) (Storage *storage, uint32_t h1, uint32_t h2);    // tiebreaker function pointer
} SortedArray;

bool sorted_array_insert_contact(SortedArray *sa, Contact contact);

bool sorted_array_remove_contact(SortedArray *sa, Contact contact);

bool sorted_array_get_range(SortedArray *sa, size_t start, size_t n, SortedEntry *out);

bool sorted_array_append(SortedArray *sa, SortedEntry in);

bool sorted_array_sort_qsort(SortedArray *sa);


#ifdef __cplusplus

}
#endif

#endif
