#include "crc_stm32.h"

#include "main.h"
#include "crc.h"

extern CRC_HandleTypeDef hcrc;

uint32_t crc32_stm32_calculate(const uint8_t *data, size_t length)
{
    return HAL_CRC_Calculate( &hcrc, (uint32_t *)data, length / sizeof(uint32_t));
}
