/**
 * @file raw_sd.c
 * @brief Raw SD card block device driver.
 *
 * This driver provides synchronous sector-based access to an SD card using
 * the STM32 HAL SD driver. It is intended for use by a single database thread.
 */

#include "raw_sd.h"

#include <stdint.h>
#include <stdbool.h>

#include "sdmmc.h"
#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_sd.h"

/*--------------------------------------------------------------------------*/
/* Configuration                                                            */
/*--------------------------------------------------------------------------*/

#define SD_TIMEOUT_MS    5000U

/*--------------------------------------------------------------------------*/
/* Private Data                                                             */
/*--------------------------------------------------------------------------*/

static HAL_SD_CardInfoTypeDef card_info;
static bool initialized = false;

/*--------------------------------------------------------------------------*/
/* Private Functions                                                        */
/*--------------------------------------------------------------------------*/

static int wait_ready(void)
{
    uint32_t start = HAL_GetTick();

    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - start) > SD_TIMEOUT_MS)
        {
            return -1;
        }
    }

    return 0;
}

/*--------------------------------------------------------------------------*/
/* Public Functions                                                         */
/*--------------------------------------------------------------------------*/

int sd_raw_init(void)
{
    HAL_StatusTypeDef status;

    if (initialized)
    {
        return 0;
    }

    /*
     * If CubeMX already calls MX_SDMMC1_SD_Init(),
     * HAL_SD_Init() can be omitted.
     */
    status = HAL_SD_Init(&hsd1);
    if (status != HAL_OK)
    {
        return -1;
    }

    status = HAL_SD_GetCardInfo(&hsd1, &card_info);
    if (status != HAL_OK)
    {
        HAL_SD_DeInit(&hsd1);
        return -1;
    }

    initialized = true;

    return 0;
}

int sd_raw_deinit(void)
{
    if (!initialized)
    {
        return 0;
    }

    if (HAL_SD_DeInit(&hsd1) != HAL_OK)
    {
        return -1;
    }

    initialized = false;

    return 0;
}

int sd_raw_read(uint32_t sector, void *buffer, uint32_t sector_count)
{
    if (!initialized)
    {
        return -1;
    }

    if (HAL_SD_ReadBlocks(&hsd1,
                          (uint8_t *)buffer,
                          sector,
                          sector_count,
                          SD_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }

    return wait_ready();
}

int sd_raw_write(uint32_t sector, const void *buffer, uint32_t sector_count)
{
    if (!initialized)
    {
        return -1;
    }

    if (HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buffer, sector, sector_count,
    SD_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }

    return wait_ready();
}

int sd_raw_sync(void)
{
    if (!initialized)
    {
        return -1;
    }

    return wait_ready();
}

uint32_t sd_raw_sector_size(void)
{
    return card_info.BlockSize;
}

uint32_t sd_raw_sector_count(void)
{
    return card_info.LogBlockNbr;
}