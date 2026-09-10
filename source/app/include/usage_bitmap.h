#ifndef USAGE_BITMAP_H
#define USAGE_BITMAP_H

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"
#include "mem_layout.h"

#ifdef __cplusplus
extern "C" {
#endif


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
#ifdef __cplusplus
}
#endif

#endif
