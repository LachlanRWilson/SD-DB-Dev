#ifndef ITERATOR_H
#define ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Iterator Iterator;

typedef bool (*iterator_next) (Iterator *it);
typedef bool (*iterator_prev) (Iterator *it);
typedef bool (*iterator_get) (Iterator *it, void *out);

typedef struct Iterator {
    void *context;
    iterator_next next;
    iterator_prev prev;
    iterator_get get;
} Iterator;


/**
 * @brief Move iterator to the next entity
 *
 * @param it pointer to iterator struct
 * @retval True if move successful, else false
 */
bool iterator_next_fn(Iterator *it);

/**
 * @brief Move iterator to the previous entity
 *
 * @param it pointer to iterator struct
 * @retval True if move successful, else false
 */
bool iterator_prev_fn(Iterator *it);

#ifdef __cplusplus
}
#endif

#endif
