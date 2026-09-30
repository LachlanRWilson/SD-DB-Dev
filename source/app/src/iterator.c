#include "iterator.h"
/**
 * @brief Move iterator to the next entity
 *
 * @param it pointer to iterator struct
 * @retval True if move successful, else false
 */
bool iterator_next_fn(Iterator *it)
{
    return it->next(it);
}

/**
 * @brief Move iterator to the previous entity
 *
 * @param it pointer to iterator struct
 * @retval True if move successful, else false
 */
bool iterator_prev_fn(Iterator *it)
{
    return it->prev(it);
}

/**
 * @brief Get current value iterator is on
 *
 * @param it pointer to iterator struct
 * @retval True if move successful, else false
 */
bool iterator_get_fn(Iterator *it, void *out)
{
    return it->get(it, out);
}
