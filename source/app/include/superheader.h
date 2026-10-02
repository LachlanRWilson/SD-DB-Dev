#ifndef SUPERHEADER_H
#define SUPERHEADER_H

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
#include "mem_layout.h"
#include "storage.h"





// Superheader magic to check if initialised
#define SUPR_HEAD_MAGIC 0x53444D42u

// Superheader magic is blank
#define SUPR_HEAD_BLANK_EMPTY 0x00000000u
#define SUPR_HEAD_BLANK_FULL 0xFFFFFFFFu

#define DB_CURRENT_VERSION 1

// Superheader status code
typedef enum {
    SUPR_GOOD = 0,
    SUPR_FAIL,
    SUPR_UNINITIALISED,
    SUPR_CORRUPTED,
    SUPR_OUTDATED
} SUPR_HEAD_STATUS;


// SuperHeaderData
typedef struct
{
    uint32_t magic; // superheader magic (4B)
    uint32_t version; // version (4B)
    uint32_t db_start; // start of database (4B)
    uint32_t db_end; // end of database (4B)
} SuperHeaderData;

// Super header data protected by CRC
typedef union
{
    SuperHeaderData var;
    uint8_t buffer[sizeof(SuperHeaderData)];
} SuperHeaderDataB;


// Superheader Struct (512B)
// (usage bitmap sectors carry their own CRC trailers, see usage_bitmap.c)
typedef struct
{
    SuperHeaderDataB data; // crc protected data
    uint8_t padding[SUPERHEADER_PADDING];
    uint32_t superheader_crc; // CRC-32 Trailer to protect all data in sector
} SuperHeader;

// SuperHeaderBuffer
typedef union
{
    SuperHeader var;
    uint8_t buffer[sizeof(SuperHeader)];
} SuperHeaderBuffer;

STATIC_ASSERT(offsetof(SuperHeader, superheader_crc) == SECTOR_PAYLOAD_BYTES,
              "SuperHeader CRC is the not the trailer word");
STATIC_ASSERT(sizeof(SuperHeader) == SECTOR_SIZE, "Unexpected SuperHeader size");
STATIC_ASSERT(sizeof(SuperHeaderData) == SUPERHEADER_DATA_BYTES, "Unexpected SuperHeaderData size");

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
STRG_RET read_superheader(Storage* storage, SuperHeaderBuffer* superHeaderBuf);

/**
  * @brief  Calculate the superheader CRC and write it to the SD Card
  * @param  storage storage access struct
  * @param  superHeaderBuf superheader to write (superheader_crc is overwritten)
  * @retval True if successful write else false.
  */
 STRG_RET write_superheader(Storage* storage, SuperHeaderBuffer* superHeaderBuf);

/**
  * @brief  Determine the status of the super header
  * @param  superHeaderBuf superheader to get the status of
  * @retval SUPR_HEAD_STATUS status code of the superheader
  */
SUPR_HEAD_STATUS get_superheader_status(SuperHeaderBuffer* superHeaderBuf);

/**
  * @brief  Read the superheader from the SD Card and determine it's validity
  * @param  table: Pointer to the hash table
  * @retval None
  */
 SUPR_HEAD_STATUS superheader_check(Storage* storage, SuperHeaderBuffer* superheader);


#ifdef __cplusplus
}
#endif

#endif
