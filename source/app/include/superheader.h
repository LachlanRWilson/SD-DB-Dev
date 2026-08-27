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
#include "contact.h"
#include "message_extent.h"
#include "hash_table.h"


/*------------------------------------ USAGE BITMAP DEFINES --------------------------------------*/
#define USAGE_BITMAP_START_SECTOR 1

// Total Memory consumed by contacts and messages
#define TOTAL_CONTACT_SECTOR_SIZE CONTACT_MEMORY_SECTOR_SIZE
#define TOTAL_MESSAGE_SECTOR_SIZE (HASH_TABLE_SIZE * 2)
    
// Total number of data sectors allocated
#define TOTAL_DATA_SECTOR_SIZE (TOTAL_CONTACT_SECTOR_SIZE + TOTAL_MESSAGE_SECTOR_SIZE)

// 32 bits in word
#define BITS_PER_ELEMENT 32

// Define how many 4 byte bit maps in usage_bitmap array
#define USAGE_BITMAP_SIZE (TOTAL_DATA_SECTOR_SIZE + BITS_PER_ELEMENT - 1) / BITS_PER_ELEMENT

#define ELEMENTS_PER_SECTOR (512 / 32)

// Number of sectors allocated for usage bitmap
#define USAGE_BITMAP_SECTOR_SIZE (USAGE_BITMAP_SIZE + ELEMENTS_PER_SECTOR - 1) / ELEMENTS_PER_SECTOR


// Macros for getting BITMAP SECTOR, ELEMENT, BIT
#define USAGE_BITMAP_FIND_SECTOR(sectorInd) (sectorInd / (SECTOR_SIZE * 8))
#define USAGE_BITMAP_FIND_ELEMENT(sectorInd) ((sectorInd % (SECTOR_SIZE * 8)) / BITS_PER_ELEMENT)
#define USAGE_BITMAP_FIND_BIT(sectorInd) ((sectorInd % (SECTOR_SIZE * 8)) % BITS_PER_ELEMENT)

/*------------------------------------ SUPERHEADER DEFINES --------------------------------------*/

// Size of super header on sd card
#ifndef HOST_BUILD
#define SUPERHEADER_BYTES 24
#else 
#define SUPERHEADER_BYTES 20
#endif

// Superheader Sector on SD Card
#define SUPERHEADER_SECTOR 0

#define SUPERHEADER_SECTOR_SIZE 1

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
} SuperHeader;

// SuperHeaderBuffer
typedef union
{
    SuperHeader var;
    uint8_t buffer[sizeof(SuperHeader)];
} SuperHeaderBuffer;


STATIC_ASSERT(sizeof(SuperHeader) == SUPERHEADER_BYTES, "Unexpected SuperHeader size");
STATIC_ASSERT(sizeof(SuperHeaderData) == SUPR_HEAD_DATA, "Unexpected SuperHeaderData size");


#ifdef __cplusplus
}
#endif

#endif
