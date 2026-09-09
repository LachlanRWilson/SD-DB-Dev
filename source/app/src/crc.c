#include "crc.h"

#if defined(HOST_BUILD)

#define CRC32_POLYNOMIAL    0xEDB88320U
#define CRC32_INITIAL_VALUE 0xFFFFFFFFU
#define CRC32_FINAL_XOR     0xFFFFFFFFU

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
uint32_t crc32_calculate(const uint8_t *data, size_t length)
{
    uint32_t crc = CRC32_INITIAL_VALUE;

    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 1U)
            {
                crc = (crc >> 1) ^ CRC32_POLYNOMIAL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc ^ CRC32_FINAL_XOR;
}

#else

#include "crc_stm32.h"

/**
 * @brief Calculate CRC-32 using the STM32 CRC peripheral.
 * NEED TO ENSURE THE CRC CALC IS THE SAME AS SOFTARE
 *
 * @param data      Pointer to data.
 * @param length    Number of bytes.
 *
 * @return CRC-32 value.
 */
uint32_t crc32_calculate(const uint8_t *data, size_t length)
{
    return crc32_stm32_calculate(data, length);
}

#endif
