#ifndef STORAGE_H
#define STORAGE_H



#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>



/**
 * Database Layout
 */

// Superheader

// TODO: FIX DUPLICATE FROM HASH_TABLE.H
#define HASH_TABLE_SIZE 14293

// Round Up Division
#define SECTORS_REQUIRED(bytes) \
    (((bytes) + SECTOR_SIZE - 1) / SECTOR_SIZE)

// Type of sector stored
typedef uint8_t SECTOR_TYPE;

// Sector Types
enum {
    CONTACT_SECTOR = 0,
    MESSAGE_SECTOR,
    CALL_HISTORY_SECTOR
};

// Storage access return values
typedef enum {
    STRG_FAIL = 0,
    STRG_OK,
    STRG_EMPTY,
    STRG_FULL
} STRG_RET;


typedef struct
{
    void * context;  // Storage context
    bool (*read_block)(void *context, uint32_t index, uint8_t *outBuf);
    bool (*read_multiblock)(void *contect, uint32_t startIndex, size_t readNum, uint8_t *outBuf);
    bool (*write_block)(void *context, uint32_t index, uint8_t *inBuf);
    bool (*write_multiblock)(void *contect, uint32_t startIndex, size_t writeNum, uint8_t *inBuf);
    uint32_t (*capacity)(void *context);
} Storage;



/**
 * @brief Read sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval True if successful read else false.
 */
STRG_RET read_sector(Storage *storage, uint16_t index, uint8_t *out);

/**
 * @brief Write to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_sector(Storage *storage, uint16_t index, uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif
