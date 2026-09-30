#ifndef USAGE_BITMAP_H
#define USAGE_BITMAP_H

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"
#include "mem_layout.h"
#include "iterator.h"

#ifdef __cplusplus
extern "C" {
#endif

// Usage Bitmap Iterator Structure
typedef struct {
    uint16_t currBit;       // Current Bit the Iterator is on
    uint16_t total_bits;    // Total Bits in the usage map
    uint16_t total_elems;   // total number of elements usage bitmap array
} UsageBitmapIteratorCtx;

// Give other files access to usage bitmap vector
extern uint32_t usage_bitmap[USAGE_BITMAP_STORAGE_SIZE];

/**
 * @brief Zero the whole usage bitmap region, in RAM and on storage.
 *
 * Used at database bring-up so reconstruction never walks stale bits.
 *
 * @param storage storage access struct
 * @retval True if successful write else false.
 */
bool init_usage_bitmap(Storage *storage);

/**
 * @brief Read the usage bitmap to RAM
 *
 * @param storage storage access struct
 * @param out read out usage bitmap sector
 * @retval True if successful read else false.
 */
bool read_usage_bitmap(Storage* storage);

/**
 * @brief Check the usage bit in RAM
 *
 * @param index
 * @retval True if used else false
 */
bool check_usage_bit(uint16_t index);

/**
 * @brief Do a blind write to usage bitmap based on in RAM bitmap
 *
 * @param index
 * @retval True is successful write, else false
 */
STRG_RET update_usage_bit(Storage *storage, uint16_t index, bool used_state);

/**
 * @brief return the index of the next bit set in the bitmap
 *
 * @param curInd the current bit index
 * @retval UINT16_MAX if fail, else the bit index
 */
uint16_t get_next_bit(uint16_t curInd, uint16_t limInd);

/**
 * @brief return the index of the next bit set in the bitmap
 *
 * @param curInd the current bit index
 * @retval UINT16_MAX if fail, else the bit index
 */
uint16_t get_prev_bit(uint16_t curInd, uint16_t limInd);

/**
 * @brief Find the nth set bit in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 * @retval UINT16_MAX if fail, else bit index
 */
uint16_t get_nth_set_bit(uint32_t bitmap, int n);

/**
 * @brief Initialise a usage bitmap iterator over [0, total_bits)
 *
 * @param it_ctx context storage owned by the caller, populated by this call
 * @param total_bits number of valid bit indices the iterator may visit
 * @param total_elems number of uint32_t elements backing the bitmap
 * @retval Iterator ready to be driven with iterator_next_fn/iterator_prev_fn/iterator_get_fn
 */
Iterator usage_bitmap_iterator_init(UsageBitmapIteratorCtx *it_ctx, uint16_t total_bits, uint16_t total_elems);

/**
 * @brief Advance a usage bitmap iterator to the next set bit
 *
 * @param it pointer to iterator struct
 * @retval True if a next set bit was found, else false
 */
bool usage_bitmap_iterator_next(Iterator *it);

/**
 * @brief Move a usage bitmap iterator to the previous set bit
 *
 * @param it pointer to iterator struct
 * @retval True if a previous set bit was found, else false
 */
bool usage_bitmap_iterator_prev(Iterator *it);

/**
 * @brief Get the bit index a usage bitmap iterator currently points to
 *
 * @param it pointer to iterator struct
 * @param out receives the current bit index (uint16_t)
 * @retval True if the iterator has a current position, else false
 */
bool usage_bitmap_iterator_get(Iterator *it, void *out);

/**
 * @brief Clear the first n bits in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 */
static inline void clear_bits_to_n_u32(uint32_t *bitmap, uint32_t n)
{
    if (n > 0 && n < BITS_PER_ELEMENT)
    {
        *bitmap &= (~0u) << (n);
    }
}

/**
 * @brief Clear the first n bits in a bitmap
 *
 * @param bitmap bitmap being searched
 * @param n the nth bit to find
 */
static inline void clear_bits_to_n_u8(uint8_t *bitmap, uint32_t n)
{
    if (n > 0 && n < 8)
        *bitmap &= (uint8_t)(~0u << n);
}
#ifdef __cplusplus
}
#endif

#endif
