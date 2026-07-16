#ifndef HEAP_STORAGE_H
#define HEAP_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "storage.h"


/**
 * @brief Heap-backed storage context.
 *
 * Provides an in-memory implementation of the Storage interface
 * for unit testing database components without hardware.
 */
typedef struct
{
    uint8_t *memory;
    uint32_t block_size;
    uint32_t capacity_blocks;

} HeapStorageContext;


/**
 * @brief Initialise heap-backed storage.
 *
 * @param context Storage context.
 * @param memory Backing memory buffer.
 * @param block_size Size of each logical block.
 * @param capacity_blocks Number of available blocks.
 *
 * @return true if initialised successfully.
 */
bool HeapStorage_Init( HeapStorageContext *context, uint8_t *memory, uint32_t block_size, uint32_t
        capacity_blocks);


/**
 * @brief Read a block from heap storage.
 *
 * @param context Storage context.
 * @param index Block index.
 * @param out Destination buffer.
 *
 * @return true if successful.
 */
bool HeapStorage_ReadBlock( void *context, uint32_t index, uint8_t *out);


/**
 * @brief Write a block to heap storage.
 *
 * @param context Storage context.
 * @param index Block index.
 * @param in Source buffer.
 *
 * @return true if successful.
 */
bool HeapStorage_WriteBlock( void *context, uint32_t index, uint8_t *in);


/**
 * @brief Get storage capacity.
 *
 * @param context Storage context.
 *
 * @return Number of blocks available.
 */
uint32_t HeapStorage_Capacity( void *context);


/**
 * @brief Heap storage implementation of Storage interface.
 */
extern Storage heap_storage;


#ifdef __cplusplus
}
#endif

#endif
