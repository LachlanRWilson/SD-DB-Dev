#include "superheader.h"
#include "crc.h"
#include <string.h>

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
STRG_RET read_superheader(Storage* storage, SuperHeaderBuffer* superHeaderBuf)
{
    return read_sector_raw(storage, SUPERHEADER_SECTOR, superHeaderBuf->buffer);
}

/**
  * @brief  Calculate the superheader CRC and write it to the SD Card
  * @param  storage storage access struct
  * @param  superHeaderBuf superheader to write (superheader_crc is overwritten)
  * @retval True if successful write else false.
  */
STRG_RET write_superheader(Storage* storage, SuperHeaderBuffer* superHeaderBuf)
{
    return write_sector(storage, SUPERHEADER_SECTOR, superHeaderBuf->buffer);
}

/**
  * @brief  Determine the status of the super header
  * @param  superHeaderBuf superheader to get the status of
  * @retval SUPR_HEAD_STATUS status code of the superheader
  */
SUPR_HEAD_STATUS get_superheader_status(SuperHeaderBuffer* superHeaderBuf)
{

    SuperHeader* superheader = &superHeaderBuf->var;
    uint8_t* buffer = superHeaderBuf->buffer;
    uint32_t magic = superheader->data.var.magic;
    uint32_t version = superheader->data.var.version;


    // Check CRC32 Trailer
    if (!sector_crc_valid(superHeaderBuf->buffer))
    {
        // if the magic is blank than SD DB uninitialised
        if (magic == SUPR_HEAD_BLANK_EMPTY || magic == SUPR_HEAD_BLANK_FULL)
        {
            return SUPR_UNINITIALISED;
        }

        return SUPR_CORRUPTED;
    }

    // Check magic
    if (magic != SUPR_HEAD_MAGIC)
    {
        return SUPR_UNINITIALISED;
    }

    // Version Check
    if (version != DB_CURRENT_VERSION)
    {
        return SUPR_OUTDATED;
    }

    return SUPR_GOOD;

}

/**
  * @brief  Initialise superheader and write to the SD card if the superheader read is uninitialised. Only do this after successful journal and usage bitmap initialisations.
  * @param  storage pointer to storage struct
  * @param superheader sector buffer
  * @retval None
  */
STRG_RET superheader_init(Storage *storage, SuperHeaderBuffer *superheader)
{
    STRG_RET ret;
    // clear data read from sd card superheader
    memset(superheader, 0, sizeof(SuperHeader));

    // Set the values in the header
    SuperHeaderData *shData = &superheader->var.data.var;
    shData->magic = SUPR_HEAD_MAGIC;
    shData->db_start = DATA_REGION_START_SECTOR;
    shData->db_end = DATA_REGION_START_SECTOR + CALL_HISTORY_DATA_START_SECTOR + TOTAL_CALL_HISTORY_SECTOR_SIZE;
    shData->version = DB_CURRENT_VERSION;

    return write_superheader(storage, superheader);
}

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  storage pointer to storage abstraction struct
  * @param supderheader pointer to buffer storing super header buffer
  * @retval None
  */
SUPR_HEAD_STATUS superheader_check(Storage* storage, SuperHeaderBuffer* superheader)
{
    STRG_RET ret;
    // read the super header from the sd card
    ret = read_superheader(storage, superheader);
    if(ret == STRG_FAIL)
    {
        return SUPR_FAIL;
    }

    return get_superheader_status(superheader);
}


