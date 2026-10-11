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

// Opaque Declaration
typedef struct Journal Journal;

typedef uint16_t MessageIndex;

typedef struct
{
    MessageIndex msgInd;    // (2B)
    Message message;        // (164B)
} MessageHistoryEntry;

// Message History Data in RingBufferSector
typedef struct
{
    MessageHistoryEntry mh_entry[MESSAGE_HISTORY_SECTOR_CAPACITY];

// Only include delaration of member if padding is not 0
#if MESSAGE_HISTORY_SECTOR_PADDING != 0
    uint8_t padding [MESSAGE_HISTORY_SECTOR_PADDING];
#endif
} MessageHistoryData;

// Message History Sector (Refer to RingBufferSector)
typedef struct {
    RingBufferHeader header; // header
    MessageHistoryData data; // data
    uint32_t sector_crc;     // CRC32 trailer
} MessageHistorySector;

typedef union {
    MessageHistorySector sector;
    uint8_t buffer[sizeof(MessageHistorySector)];
} MessageHistorySectorB;

STATIC_ASSERT(sizeof(MessageHistoryEntry) == MESSAGE_HISTORY_ENTRY_BYTES, "Uknown MessageHistoryEntry size");
STATIC_ASSERT(sizeof(MessageHistoryData) == RING_BUFFER_PAYLOAD_BYTES, "MessageHistoryData doesn't fit in ring buffer payload");
STATIC_ASSERT(sizeof(MessageHistorySector) == RING_BUFFER_SECTOR_BYTES, "Unexpected MessageHistorySector size");
STATIC_ASSERT(offsetof(MessageHistorySector, sector_crc) == RING_BUFFER_PAYLOAD_BYTES + RING_BUFFER_HEADER_BYTES, "MessageHistorySector CRC is not the sector trailer");
STATIC_ASSERT(0 != MESSAGE_HISTORY_SECTOR_PADDING, "Array size of 0 in padding delaration");

/**
 * @brief Initialise MessageHistory and RingBuffer
 *
 * @param rb Ring Buffer struct pointer
 * @retval True if successful else false.
 */
bool message_history_init(RingBuffer *rb);

/**
 * @brief Add new latest message index to the ring buffer
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_add(RingBuffer *rb, Journal *journal, Storage *storage, MessageHistoryEntry *message);

/**
 * @brief Get a range of message history from start -> start + n
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_get_range(RingBuffer *rb, Storage *storage, size_t start, size_t n, size_t *out_count, MessageHistoryEntry *out_list);



#ifdef __cplusplus
}
#endif

#endif
