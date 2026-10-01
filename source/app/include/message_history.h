#ifndef MESSAGE_HISTORY_H
#define MESSAGE_HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "mem_layout.h"
#include "storage.h"
#include "message.h"
#include "ring_buffer.h"

typedef uint16_t MessageIndex;

typedef struct {
    MessageIndex messageIndex[MESSAGE_HISTORY_SECTOR_CAPACITY];
    uint32_t sector_crc;
} MessageHistorySector;

STATIC_ASSERT(sizeof(MessageHistorySector) == MESSAGE_HISTORY_SECTOR_BYTES, "Unexpected MessageHistorySector size");

/**
 * @brief Initialise MessageHistory and RingBuffer
 *
 * @param rb Ring Buffer struct pointer
 * @retval True if successful else false.
 */
bool message_history_init(RingBuffer *rb);

/**
 * @brief Get a range of message history
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_get_range(RingBuffer *rb, size_t start, size_t n, Message *out);

/**
 * @brief Add new latest message index to the ring buffer
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_add(RingBuffer *rb, uint16_t messageInd);



#ifdef __cplusplus
}
#endif

#endif
