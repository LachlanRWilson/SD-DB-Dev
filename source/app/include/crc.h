#ifndef CRC_H
#define CRC_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Calculate a CRC-32 over a buffer.
 *
 * The implementation is selected at compile time.
 *
 * @param data      Pointer to the data.
 * @param length    Number of bytes to process.
 *
 * @return CRC-32 value.
 */
uint32_t crc32_calculate(const uint8_t *data, size_t length);

#endif /* CRC_H */
