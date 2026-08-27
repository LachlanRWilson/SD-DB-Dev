#ifndef STORAGE_H
#define STORAGE_H



#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Storage sector size
#define SECTOR_SIZE 512

// Round Up Division
#define SECTORS_REQUIRED(bytes) \
    (((bytes) + SECTOR_SIZE - 1) / SECTOR_SIZE)


typedef struct 
{
    void * context;  // Storage context
    bool (*read_block)(void *context, uint32_t index, uint8_t *outBuf);
    bool (*read_multiblock)(void *contect, uint32_t startIndex, size_t readNum, uint8_t *outBuf); 
    bool (*write_block)(void *context, uint32_t index, uint8_t *inBuf);
    uint32_t (*capacity)(void *context);
} Storage;

#ifdef __cplusplus
}
#endif

#endif
