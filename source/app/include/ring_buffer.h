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
    RB_EMPTY,       // Nothing in RB sector
    RB_OCCUPIED,    // RB sector occupied with something
    RB_FULL,        // RB sector filled
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

typedef union {
    RingBufferSector sector;
    uint8_t buffer[sizeof(RingBufferSector)];
} U_RingBufferSector;

STATIC_ASSERT(sizeof(RingBufferHeader) == RING_BUFFER_HEADER_BYTES, "Ring Buffer Header size if unknown");
STATIC_ASSERT(sizeof(RingBufferSector) == RING_BUFFER_SECTOR_BYTES, "Ring Buffer Sector struct is not the size of a sector");

/**
 * @brief Read the ring buffer sector from the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param out Contact Sector Buffer with the desired contact position
 * @retval True if successful read else false.
 */
STRG_RET read_ring_buffer_sector(Storage *storage, uint16_t raw_sector_start, uint16_t index, U_RingBufferSector *out);

/**
 * @brief Get the sequence and state of the ring buffer sector to perform binary search
 *
 * @param storage pointer to storage abstraction struct
 * @param raw_sector_start starting sector offset of ring buffer memory block
 * @param index index (in the ring buffer) that the sequence and state is coming from
 * @param seq pointer to sequence storage memory
 * @param state pointer to sector state storage memory
 * @retval STRG_RET return code
 */
STRG_RET seq_and_state_at(Storage *storage, uint16_t raw_sector_start,
                          uint16_t index, uint32_t *seq,
                          RB_SECTOR_STATE *state);

/**
 * @brief check is the ring buffer has wrapped by checking is the next sector from the head is occupied
 *
 * @param storage Pointer to the storage abstraction.
 * @param raw_sector_start starting sector of the ring buffer
 * @param head head of reign buffer
 * @retval  STRG_FAIL the is a read error,
 *          STRG_EMPTY ring buffer has not wrapped,
 *          STRG_OK ring buffer has wrapped
 */
STRG_RET is_ring_buffer_wrapped(Storage *storage, uint16_t raw_sector_start, uint16_t head);

/**
 * @brief Perform binary search on the ring buffer memory to find the head and sequence number
 *
 * @param storage Pointer to the storage abstraction.
 * @param raw_sector_start starting sector of the ring buffer
 * @param mem_size memory size of the ring buffer (in sectors) on the sd card
 * @param head pointer to head index for the ring buffer
 * @param seq pointer to current sequence number of the ring buffer
 * @retval  STRG_FAIL the is a read error,
 *          STRG_EMPTY ring buffer has not wrapped,
 *          STRG_OK ring buffer has wrapped
 */
STRG_RET binary_search_head(Storage *storage, uint16_t raw_sector_start,
                            uint16_t mem_size, uint16_t *head, uint32_t *seq);

/**
 * @brief Using binary search to find the head of the ring buffer. Ring buffer sector indexes are
 * (raw_sector_start, sector_size - 1).
 *
 * @param storage pointer to storage abstraction struct
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @retval True if successful write else false.
 */
bool reconstruct_ring_buffer(Storage* storage, RingBuffer *rb, uint16_t raw_sector_start);

/**
 * @brief Iniitialise a ring buffer on the SD card. Only the state, current index and occupancy is stored in RAM
 *
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @param n number of
 * @retval True if successful write else false.
 */
bool init_ring_buffer(Storage *storage, RingBuffer *rb, uint16_t size, uint16_t raw_sector_start);

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
