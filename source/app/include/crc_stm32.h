#ifndef CRC_STM32_H
#define CRC_STM32_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Calculate CRC-32 using the STM32 CRC peripheral.
 *
 * @param data      Pointer to data.
 * @param length    Number of bytes.
 *
 * @return CRC-32 value.
 */
uint32_t crc32_stm32_calculate(const uint8_t *data, size_t length);

#endif /* CRC_STM32_H */
