#include "heap_storage.h"

#include <string.h>


/**
 * @brief Initialise heap-backed storage.
 */
bool HeapStorage_Init( HeapStorageContext *context, uint8_t *memory, uint32_t block_size, uint32_t
        capacity_blocks)
{
    if(context == NULL || memory == NULL || block_size == 0 || capacity_blocks == 0)
    {
        return false;
    }


    context->memory = memory;

    context->block_size = block_size;

    context->capacity_blocks = capacity_blocks;


    return true;
}


/**
 * @brief Read a block from heap storage.
 */
bool HeapStorage_ReadBlock( void *context, uint32_t index, uint8_t *out)
{
    HeapStorageContext *ctx = (HeapStorageContext *)context;


    if(ctx == NULL || out == NULL || index > ctx->capacity_blocks)
    {
        return false;
    }


    memcpy( out, &ctx->memory[index * ctx->block_size], ctx->block_size);


    return true;
}


/**
 * @brief Write a block to heap storage.
 */
bool HeapStorage_WriteBlock( void *context, uint32_t index, uint8_t *in)
{
    HeapStorageContext *ctx = (HeapStorageContext *)context;


    if(ctx == NULL || in == NULL || index > ctx->capacity_blocks)
    {
        return false;
    }


    memcpy( &ctx->memory[index * ctx->block_size], in, ctx->block_size);


    return true;
}


/**
 * @brief Get heap storage capacity.
 */
uint32_t HeapStorage_Capacity(void *context)
{
    HeapStorageContext *ctx = (HeapStorageContext *)context;


    if(ctx == NULL)
    {
        return 0;
    }


    return ctx->capacity_blocks;
}


/**
 * @brief Heap storage implementation of Storage interface.
 */
Storage heap_storage =
{
    .context = NULL,

    .read_block = HeapStorage_ReadBlock,

    .write_block = HeapStorage_WriteBlock,

    .capacity = HeapStorage_Capacity
};
