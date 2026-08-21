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

typedef struct 
{
    void * context;  // Storage context
    bool (*read_block)(void *context, uint32_t index, uint8_t *outBuf);
    bool (*write_block)(void *context, uint32_t index, uint8_t *inBuf);
    uint32_t (*capacity)(void *context);
} Storage;

#ifdef __cplusplus
}
#endif

#endif
