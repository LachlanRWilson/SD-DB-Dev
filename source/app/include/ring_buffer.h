#ifndef RING_BUFFER_H
#define RING_BUFFER_H


#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "mem_layout.h"
#include "storage.h"
#include "iterator.h"

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif
typedef struct RingBuffer {
    uint32_t seq;           // Current Sequence number
    uint16_t size;          // Size of the ring buffer
    uint16_t current_index; // current index of the buffer
    uint16_t occupancy;     // current fill level of ring buffer
} RingBuffer;


//
typedef uint8_t RB_SECTOR_STATE;
enum {
    RB_EMPTY,   // Nothing in RB sector
    RB_FULL,    // RB Sector Full
    RB_PARTIAL, // RB Sector Partially filled
};

// Ring Buffer Iterator Context
typedef struct
{
    uint16_t current;
    uint16_t upper_lim;
    uint16_t lower_lim;
} RBIteratorCtx;

// Ring Buffer Sector Header (8B) - 4 Byte aligned
typedef struct
{
    uint32_t seq;           // Sequence number of sector (4B)
    uint8_t head;           // Head entry in sector (1B)
    RB_SECTOR_STATE state;  // Sector State (1B)
                            // padding (2B)
} RingBufferHeader;

// Generic Ring Buffer Sector
typedef struct {
    RingBufferHeader header;                 // Header
    uint8_t data[RING_BUFFER_PAYLOAD_BYTES]; // Data payload
    uint32_t crc;                            // CRC-32 trailer
} RingBufferSector;

STATIC_ASSERT(sizeof(RingBufferHeader) == RING_BUFFER_HEADER_BYTES, "Ring Buffer Header size if unknown");
STATIC_ASSERT(sizeof(RingBufferSector) == RING_BUFFER_SECTOR_BYTES, "Ring Buffer Sector struct is not the size of a sector");

/**
 * @brief Iniitialise a ring buffer on the SD card. Only the state, current index and occupancy is stored in RAM
 *
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @param n number of
 * @retval True if successful write else false.
 */
bool init_ring_buffer(RingBuffer *rb, uint16_t size, uint16_t startIndex);

/**
 * @brief Initialise an iterator over a ring buffer's occupied index range
 *        [0, occupancy], independent of the ring buffer's own read/write
 *        cursor (rb->current_index).
 *
 * @param ctx context storage owned by the caller, populated by this call
 * @param rb ring buffer to iterate over
 * @retval Iterator ready to be driven with iterator_next_fn/iterator_prev_fn/iterator_get_fn
 */
Iterator ring_buffer_iterator_init(RBIteratorCtx *ctx, RingBuffer *rb);

/**
 * @brief move to the next index in the ring buffer
 *
 * @param curInd current index to move from
 * @retval True if successful else false
 */
bool move_next_ring_buffer(RingBuffer *rb);

/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool move_prev_ring_buffer(RingBuffer *rb);

/**
 * @brief Increment the number of occupants in the ring buffer if not filled yet
 *
 * @param rb pointer to ring buffer struct
 * @retval true if successful else false
 */
bool add_ring_buffer(RingBuffer *rb);

/**
 * @brief Increment the sequence number of the ring buffer. Wrap on UINT32_MAX
 *
 * @param rb pointer to ring buffer struct
 * @retval true if successful else false
 */
bool inc_seq_ring_buffer(RingBuffer *rb);

/**
 * @brief get the index at the
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_get(Iterator *it, void* out);
/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_next(Iterator *it);
/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_prev(Iterator *it);

/**
 * @brief get the entry stored at the current index of the ring buffer
 *
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @param storage Storage abstraction struct
 * @param out pointer to output memory
 * @param memSize size of the memory being read from the ring buffer
 * @retval STRG_OK if successful
 */
STRG_RET get_ring_buffer(RingBuffer *rb, Storage *storage, void* out, size_t memSize);

/**
 * @brief put the next entry in to the ring buffer
 *
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @param storage Storage abstraction struct
 * @param out pointer to output memory
 * @param memSize size of the memory being read from the ring buffer
 * @retval STRG_OK if successful
 */
STRG_RET put_ring_buffer(RingBuffer *rb, Storage *storage, void* in, size_t memSize);
#ifdef __cplusplus
}
#endif

#endif
