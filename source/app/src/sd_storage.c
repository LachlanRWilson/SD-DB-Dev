#include "sd_storage.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include <string.h>

extern SD_HandleTypeDef hsd1;

/** Number of 512-byte SD sectors in one MessageBlock. */
// (x + y - 1) / y provides the ceiling functionality
#define SD_SECTORS_PER_BLOCK(size) (((size) + 512U - 1) / 512U)
/**
 * @brief Initializes the SD storage context.
 *
 * Retrieves the SD card capacity and configures the storage context.
 *
 * @param[out] context Pointer to the storage context.
 * @param[in] hsd Pointer to the initialized HAL SD handle.
 * @param[in] start_sector First SD sector reserved for database storage.
 *
 * @retval true  Initialization successful.
 * @retval false Failed to retrieve SD card information.
 */
bool SDStorage_Init( SDStorageContext *context, uint32_t start_sector, size_t block_size)
{
    HAL_SD_CardInfoTypeDef cardInfo;

    if ((context == NULL))
    {
        return false;
    }



    if (HAL_SD_GetCardInfo(&hsd1, &cardInfo) != HAL_OK)
    {
        return false;
    }

    context->hsd = &hsd1;
    context->start_sector = start_sector;
    context->block_size = block_size;

    context->capacity_blocks = (cardInfo.LogBlockNbr - start_sector) /
        SD_SECTORS_PER_BLOCK(block_size);

    return true;
}

/**
 * @brief Reads a database block from the SD card.
 *
 * @param[in] context Pointer to the SDStorageContext.
 * @param[in] index raw sector index of read
 * @param[out] out Pointer to a buffer of MESSAGE_BLOCK_SIZE bytes.
 *
 * @retval true Block read successfully.
 * @retval false Read failed.
 */
bool SDStorage_ReadBlock( void *context, uint32_t index, uint8_t *out) 
{
    SDStorageContext *ctx = (SDStorageContext *)context;

    // Read sector
//     uint32_t sector = ctx->start_sector + (index * SD_SECTORS_PER_BLOCK(ctx->block_size));
    
    // number of sectors to read
    uint32_t readSize = SD_SECTORS_PER_BLOCK(ctx->block_size);

    // Allocate raw array
    uint8_t raw[readSize * 512U];


    // read to raw array
    if (HAL_SD_ReadBlocks( ctx->hsd, raw, index, readSize, HAL_MAX_DELAY) != HAL_OK)
    {
        return false;
    }

    while (HAL_SD_GetCardState(ctx->hsd) != HAL_SD_CARD_TRANSFER)
    {
        osDelay(1);
    }

    // copy memory to output block
    memcpy(out, raw, sizeof(uint8_t) * ctx->block_size);

    return true;
}

/**
 * @brief Reads multiple database blocks from the SD card.
 *
 * @param[in] context Pointer to the SDStorageContext.
 * @param[in] index raw sector index of read start
 * @param[in] readNum number of database block to read
 * @param[out] out Pointer to a buffer of readNum * ctx->BLOCK_SIZE
 *
 * @retval true Block read successfully.
 * @retval false Read failed.
 */
bool SDStorage_ReadMultiBlock( void *context, uint32_t index, size_t readNum, uint8_t *out)
{
    SDStorageContext *ctx = (SDStorageContext *)context;

    // Total 512B sectors spanned by readNum logical blocks
    uint32_t sectors = SD_SECTORS_PER_BLOCK(ctx->block_size) * (uint32_t)readNum;

    // Read straight into the caller's buffer (must hold readNum * block_size
    // bytes and be 32-bit aligned for the SDMMC transfer).
    if (HAL_SD_ReadBlocks(ctx->hsd, out, index, sectors, HAL_MAX_DELAY) != HAL_OK)
    {
        return false;
    }

    while (HAL_SD_GetCardState(ctx->hsd) != HAL_SD_CARD_TRANSFER)
    {
        osDelay(1);
    }

    return true;
}


bool SDStorage_WriteMultiBlock( void *context, uint32_t index, size_t writeNum, uint8_t *in) 
{
    SDStorageContext *ctx = (SDStorageContext *)context;

    // Read sector
//     uint32_t sector = ctx->start_sector + (index * SD_SECTORS_PER_BLOCK(ctx->block_size));
    
    // number of sectors to read
    uint32_t writeSize = SD_SECTORS_PER_BLOCK(ctx->block_size);

    // read to raw array
    if (HAL_SD_WriteBlocks( ctx->hsd, in, index, writeSize * writeNum, HAL_MAX_DELAY) != HAL_OK)
    {
        return false;
    }

    while (HAL_SD_GetCardState(ctx->hsd) != HAL_SD_CARD_TRANSFER)
    {
        osDelay(1);
    }

    return true;
}


/**
 * @brief Writes a database block to the SD card.
 *
 * @param[in] context Pointer to the SDStorageContext.
 * @param[in] index Logical block index.
 * @param[in] in Pointer to a buffer of bytes.
 *
 * @retval true Block written successfully.
 * @retval false Write failed.
 */
bool SDStorage_WriteBlock( void *context, uint32_t index, uint8_t *in)
{
    SDStorageContext *ctx = (SDStorageContext *)context;

    // Get sector position
//     uint32_t sector = ctx->start_sector + (index * SD_SECTORS_PER_BLOCK(ctx->block_size));

    // get write size (ceiling)
    uint32_t writeSize = SD_SECTORS_PER_BLOCK(ctx->block_size);



    // write to SD Card
    if (HAL_SD_WriteBlocks( ctx->hsd, in, index, writeSize, HAL_MAX_DELAY) != HAL_OK)
    {
        return false;
    }

    while (HAL_SD_GetCardState(ctx->hsd) != HAL_SD_CARD_TRANSFER)
    {
        osDelay(1);
    }

    return true;
}

/**
 * @brief Returns the number of database blocks available.
 *
 * @param[in] context Pointer to the SDStorageContext.
 *
 * @return Number of logical storage blocks.
 */
uint32_t SDStorage_Capacity(void *context)
{
    SDStorageContext *ctx = (SDStorageContext *)context;

    return ctx->capacity_blocks;
}

/**
 * @brief SD card implementation of the Storage interface.
 */
Storage sd_storage =
{
    .context = NULL,
    .read_block = SDStorage_ReadBlock,
    .read_multiblock = SDStorage_ReadMultiBlock,
    .write_block = SDStorage_WriteBlock,
    .write_multiblock = SDStorage_WriteMultiBlock,
    .capacity = SDStorage_Capacity
};
