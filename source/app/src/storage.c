#include "storage.h"

/**
 * @brief Write the contact sector that the contact in stored in on the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval True if successful read else false.
 */
STRG_RET read_sector(Storage *storage, uint16_t index, uint8_t *out)
{

    // read sector
    if (!storage->read_block( storage->context, index, out))
    {
        return STRG_FAIL;
    }

    return STRG_OK;
}
/**
 * @brief Write the contact sector that the contact in stored in on the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_sector(Storage *storage, uint16_t index, uint8_t *in){
    // Write contact sector
    if (!storage->write_block(storage->context, index, in))
    {
        return STRG_FAIL;
}

    return STRG_OK;
}
