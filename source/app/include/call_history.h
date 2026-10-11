#ifndef CALL_HISTORY_H
#define CALL_HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#include "ring_buffer.h"
#include "mem_layout.h"
#include "contact.h"


typedef struct
{
    uint16_t contact_index; // Sector index for contact (2B)
    Contact contact;        // Contact information (81B)
                            // padding for alignment (1B)
} CallHistoryEntry;

typedef struct
{
    CallHistoryEntry chEntry[CALL_HISTORY_SECTOR_CAPACITY];
#if CALL_HISTORY_SECTOR_PADDING != 0
    uint8_t padding [CALL_HISTORY_SECTOR_PADDING];
#endif
} CallHistoryData;

typedef struct
{
    RingBufferHeader header;    // Ring Buffer Header
    CallHistoryData data;       // Call History Data
    uint32_t crc;               // CRC-32 Trailer
} CallHistorySector;


typedef union
{
    CallHistorySector sector;
    uint8_t buffer[sizeof(CallHistorySector)];
} CallHistorySectorB;


STATIC_ASSERT(sizeof(CallHistoryEntry) == CALL_HISTORY_ENTRY_BYTES, "Unknown size of CallHistoryEntry");
STATIC_ASSERT(sizeof(CallHistoryData) == RING_BUFFER_PAYLOAD_BYTES, "MessageHistoryData doesn't fit in ring buffer payload");
STATIC_ASSERT(sizeof(CallHistorySector) == RING_BUFFER_SECTOR_BYTES, "Unexpected MessageHistorySector size");
STATIC_ASSERT(offsetof(CallHistorySector, crc) == RING_BUFFER_PAYLOAD_BYTES + RING_BUFFER_HEADER_BYTES, "MessageHistorySector CRC is not the sector trailer");

/**
 * @brief Initialise MessageHistory and RingBuffer
 *
 * @param rb Ring Buffer struct pointer
 * @retval True if successful else false.
 */
bool call_history_init(RingBuffer *rb);

/**
 * @brief Get a range of call history
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET call_history_get_range(RingBuffer *rb, Storage *storage, size_t start, size_t n, size_t *out_count, CallHistoryEntry *out);

/**
 * @brief Add new latest contact and index to the ring buffer
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET call_history_add(RingBuffer *rb, Journal *journal, Storage *storage, CallHistoryEntry *entry);
#ifdef __cplusplus
}
#endif

#endif
