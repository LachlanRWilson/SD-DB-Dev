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
#include "contact.h"
#include "message_extent.h"
#include "hash_table.h"
#include "usage_bitmap.h"





// Superheader magic to check if initialised
#define SUPR_HEAD_MAGIC 0x53444D42u

#define SUPR_HEAD_DATA 12

#define DB_CURRENT_VERSION 1

// Superheader status code
typedef enum {
    SUPR_GOOD = 0,
    SUPR_UNINITIALISED,
    SUPR_CORRUPTED,
    SUPR_OUTDATED
} SUPR_HEAD_STATUS;


// SuperHeaderData
typedef struct 
{
    uint32_t magic; // superheader magic (4B)
    uint32_t version; // version (4B)
    uint16_t db_start; // start of database (2B)
    uint16_t db_end; // end of database (2B)
} SuperHeaderData;

// Super header data protected by CRC
typedef union
{
    SuperHeaderData var;
    uint8_t buffer[sizeof(SuperHeaderData)];
} SuperHeaderDataB;


// Superheader Struct (20B)
typedef struct
{
    SuperHeaderDataB data; // crc protected data
    uint32_t superheader_crc; // CRC of superheader (4B)
    uint32_t usage_bitmap_crc; // CRC of bitmap (4B)
    uint8_t padding[SUPERHEADER_PADDING];
} SuperHeader;

// SuperHeaderBuffer
typedef union
{
    SuperHeader var;
    uint8_t buffer[sizeof(SuperHeader)];
} SuperHeaderBuffer;


STATIC_ASSERT(sizeof(SuperHeader) == SECTOR_SIZE, "Unexpected SuperHeader size");
STATIC_ASSERT(sizeof(SuperHeaderData) == SUPR_HEAD_DATA, "Unexpected SuperHeaderData size");


#ifdef __cplusplus
}
#endif

#endif
