#include <string.h>
#include "storage.h"
#include "mem_layout.h"
#include "crc.h"

/**
 * @brief Calculate and store the CRC trailer of a sector
 *
 * @param sector SECTOR_SIZE byte buffer, last SECTOR_CRC_BYTES are overwritten
 */
void sector_crc_stamp(uint8_t *sector)
{
    uint32_t crc = crc32_calculate(sector, SECTOR_PAYLOAD_BYTES);

    // memcpy as the trailer is not guaranteed to be 4B aligned in the caller's buffer
    memcpy(&sector[SECTOR_PAYLOAD_BYTES], &crc, SECTOR_CRC_BYTES);
}

/**
 * @brief Check the CRC trailer of a sector
 *
 * @param sector SECTOR_SIZE byte buffer
 * @retval True if the stored CRC matches the payload else false.
 */
bool sector_crc_valid(const uint8_t *sector)
{
    uint32_t stored;
    memcpy(&stored, &sector[SECTOR_PAYLOAD_BYTES], SECTOR_CRC_BYTES);

    return stored == crc32_calculate(sector, SECTOR_PAYLOAD_BYTES);
}

/**
 * @brief Read sector from the sd card without checking its CRC trailer
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param out sector being read from the SD Card
 * @retval STRG_OK if successful read else STRG_FAIL.
 */
STRG_RET read_sector_raw(Storage *storage, uint16_t index, uint8_t *out)
{
    // read sector
    if (!storage->read_block( storage->context, index, out))
    {
        return STRG_FAIL;
    }

    return STRG_OK;
}

/**
 * @brief Read sector from the sd card and check its CRC trailer
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval STRG_OK if successful read, STRG_CORRUPT if the CRC trailer mismatches, else STRG_FAIL.
 */
STRG_RET read_sector(Storage *storage, uint16_t index, uint8_t *out)
{
    STRG_RET ret = read_sector_raw(storage, index, out);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // check the sector hasn't been corrupted since it was written
    if (!sector_crc_valid(out))
    {
        return STRG_CORRUPT;
    }

    return STRG_OK;
}

/**
 * @brief Stamp the CRC trailer and write the sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card (CRC trailer is overwritten)
 * @retval True if successful write else false.
 */
STRG_RET write_sector(Storage *storage, uint16_t index, uint8_t *in){
    // Protect the sector before it leaves RAM
    sector_crc_stamp(in);

    // Write contact sector
    if (!storage->write_block(storage->context, index, in))
    {
        return STRG_FAIL;
    }

    return STRG_OK;
}
