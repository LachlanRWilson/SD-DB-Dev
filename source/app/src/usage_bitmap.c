#include "usage_bitmap.h"
#include <string.h>

#ifndef HOST_BUILD
// Read entire usage bitmap into RAM (5.6KB)
__attribute__((section(".ram_d1")))
uint32_t usage_bitmap[USAGE_BITMAP_STORAGE_SIZE];
#else
uint32_t usage_bitmap[USAGE_BITMAP_STORAGE_SIZE];
#endif



/**
 * @brief Write zero-initialised bitmap to storage
 *
 * @param storage storage access struct 
 * @retval True if successful write else false.
 */
bool init_usage_bitmap(Storage *storage)
{
    // Zero-set entire bitmap
    memset(usage_bitmap, 0, sizeof(usage_bitmap));

    return storage->write_multiblock(storage->context, USAGE_BITMAP_START_SECTOR,
            USAGE_BITMAP_SECTOR_SIZE, (uint8_t *) usage_bitmap);
}

/**
 * @brief Read the usage bitmap to RAM
 *
 * @param storage storage access struct 
 * @param out read out usage bitmap sector
 * @retval True if successful read else false.
 */
bool read_usage_bitmap(Storage* storage)
{
    return storage->read_multiblock(storage->context, USAGE_BITMAP_START_SECTOR,
            USAGE_BITMAP_SECTOR_SIZE, (uint8_t *)usage_bitmap);
}


/**
 * @brief Check the usage bit in RAM
 *
 * @param index 
 * @retval True if used else false
 */
bool check_usage_bit(uint16_t index)
{
    return (usage_bitmap[USAGE_BITMAP_FIND_ELEMENT(index) + USAGE_BITMAP_FIND_SECTOR(index) *
            ELEMENTS_PER_SECTOR] >> USAGE_BITMAP_FIND_BIT(index)) & 0x1;
}


/**
 * @brief Do a blind write to usage bitmap based on in RAM bitmap
 *
 * @param index 
 * @retval True is successful write, else false
 */
bool update_usage_bit(Storage *storage, uint16_t index, bool used_state)
{
    // Bit map sector
    uint32_t bitmap_sector = USAGE_BITMAP_FIND_SECTOR(index);

    // Sector start pointer
    uint32_t *write_sector = &usage_bitmap[bitmap_sector * ELEMENTS_PER_SECTOR];

    // uint32_t element index in sector
    uint32_t element = USAGE_BITMAP_FIND_ELEMENT(index);

    // bit in uint32_t element
    uint32_t bit = USAGE_BITMAP_FIND_BIT(index);


    if (used_state) {
        // Set bit
        write_sector[element] |= (1U << bit);
    } else {
        // Unset bit
        write_sector[element] &= ~(1U << bit);
    }

    return storage->write_block( storage->context, USAGE_BITMAP_START_SECTOR + bitmap_sector,
            (uint8_t *)write_sector
    );
}
