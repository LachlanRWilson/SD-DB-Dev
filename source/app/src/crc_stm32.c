#include "crc_stm32.h"
#include "main.h"          /* CRC_HandleTypeDef, hcrc */

extern CRC_HandleTypeDef hcrc;

uint32_t crc32_stm32_calculate(const uint8_t *data, size_t length)
{
    /* HAL_CRC_Calculate() reloads the init value each call; with BYTES
     * format, BufferLength is a byte count. The peripheral has no XorOut,
     * so apply it here to match crc.c's software CRC-32/ISO-HDLC. */
    uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)(uintptr_t)data, (uint32_t)length);
    return crc ^ 0xFFFFFFFFU;
}
