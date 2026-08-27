#include "superheader.h"
#include "crc.h"


#define USAGE_BITMAP_NUM_SECTORS TOTAL_DATA_SECTOR_SIZE / SECTOR_SIZE
#define USAGE_BITMAP_SECTOR SUPERHEADER_SECTOR + 1

#ifndef HOST_BUILD
// Read entire usage bitmap into RAM (5.6KB)
__attribute__((section(".ram_d1")))
uint32_t superheader_usage_bitmap[USAGE_BITMAP_SIZE];
#else
uint32_t superheader_usage_bitmap[USAGE_BITMAP_SIZE];
#endif


/**
 * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
bool read_usage_bitmap(Storage* storage, uint32_t *usage_bitmap_storage)
{
    return storage->read_multiblock(storage->context, USAGE_BITMAP_SECTOR,
            USAGE_BITMAP_NUM_SECTORS, (uint8_t *)usage_bitmap_storage);

}

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
bool read_superheader(Storage* storage, SuperHeaderBuffer* superHeaderBuf)
{
    return storage->read_block(storage->context, SUPERHEADER_SECTOR, superHeaderBuf->buffer);
}

/**
  * @brief  Determine the status of the super header
  * @param  superHeaderBuf superheader to get the status of
  * @retval SUPR_HEAD_STATUS status code of the superheader
  */
SUPR_HEAD_STATUS get_superheader_status(SuperHeaderBuffer* superHeaderBuf)
{

    SuperHeader superheader = superHeaderBuf->var;
    uint8_t* buffer = superHeaderBuf->buffer;

    // Check magic
    if (superheader.data.var.magic != SUPR_HEAD_MAGIC)
    {
        return SUPR_UNINITIALISED;
    }

    // Calculate CRC
    uint32_t crc = crc32_calculate(buffer, sizeof(SUPR_HEAD_DATA));

    // Check CRC
    if (crc != superheader.superheader_crc)
    {
        // Database if fucked, good luck
        return SUPR_CORRUPTED;
    }

    // Version Check
    if (superheader.data.var.version != DB_CURRENT_VERSION)
    {
        return SUPR_OUTDATED;
    }

    return SUPR_GOOD;

}

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
bool superheader_init(Storage* storage, SuperHeaderBuffer* superheader)
{

    // read the super header from the sd card
    if(!read_superheader(storage, superheader))
    {
        return false;
    }

    switch (get_superheader_status(superheader))
    {
        case SUPR_OUTDATED:
        case SUPR_CORRUPTED:
           return false; 

        case SUPR_UNINITIALISED:
            // initialise database
        case SUPR_GOOD:
           return true;
    }

    return false;
}


