#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "message_extent.h"
#include "hash_table.h"
#include "storage.h"
#include "sdmmc.h"

/*
 * SD storage context.
 * This structure is passed through MessageStorage.context.
 */
typedef struct
{
    SD_HandleTypeDef *hsd;

    uint32_t start_sector;

    size_t block_size;
    uint32_t capacity_blocks;

} SDStorageContext;

/*
 * Initializes the SD storage context.
 */
bool SDStorage_Init( SDStorageContext *context, uint32_t start_sector, size_t block_size);

/*
 * MessageStorage callbacks.
 */
bool SDStorage_ReadBlock( void *context, uint32_t index, uint8_t *out);

bool SDStorage_WriteBlock( void *context, uint32_t index, uint8_t *in);

uint32_t SDStorage_Capacity( void *context);

#ifdef __cplusplus
}
#endif

#endif
