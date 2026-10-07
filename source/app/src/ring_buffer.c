#include "ring_buffer.h"
#include "iterator.h"


/**
 * @brief Is seq_i greater than seq_start regardless of unsigned overflow
 *
 * @param seq_start starting number in the ring buffer sequence
 * @param seq_i sequnce number that is being checked
 * @retval True if seq_i is greater than seq_start. This holds for normal sequence and if the number wraps around. 
 * i.e for uint8, seq_start = 0xFF and seq_i = 3 than (seq_i - seq_start) = 3 therefore is greater
 */
static inline bool is_greater(uint32_t seq_start, uint32_t seq_i)
{
    return (seq_i - seq_start) > 0;
}

/**
 * @brief Using binary search to find the head of the ring buffer. Ring buffer sector indexes are 
 * (raw_sector_start, sector_size - 1).
 *
 * @param storage pointer to storage abstraction struct
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @retval True if successful write else false.
 */
bool reconstruct_ring_buffer(Storage* storage, RingBuffer *rb, uint16_t raw_sector_start, uint16_t sector_size)
{
    uint16_t hi = sector_size - 1;
    uint16_t lo = 0;
    // Get the mid point
    uint16_t mid = hi - (hi - lo) / 2;

    // iterate until hi and lo indexes are next to each other
    while (hi - lo > 1)
    {
        
    }

    return true;
}

/**
 * @brief Iniitialise a ring buffer on the SD card. Only the state, current index and occupancy is stored in RAM
 *
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @param n number of
 * @retval True if successful write else false.
 */
bool init_ring_buffer(RingBuffer *rb, uint16_t size, uint16_t startIndex)
{
    if (rb == NULL || size == 0 || startIndex < 0)
    {
        return false;
    }

    rb->size = size;
    rb->current_index = startIndex;
    rb->occupancy = 0;
    return true;
}

/**
 * @brief Initialise an iterator over a ring buffer's occupied index range
 *        [0, occupancy], independent of the ring buffer's own read/write
 *        cursor (rb->current_index).
 *
 * @param ctx context storage owned by the caller, populated by this call
 * @param rb ring buffer to iterate over
 * @retval Iterator ready to be driven with iterator_next_fn/iterator_prev_fn/iterator_get_fn
 */
Iterator ring_buffer_iterator_init(RBIteratorCtx *ctx, RingBuffer *rb)
{
    ctx->lower_lim = 0;
    ctx->upper_lim = rb->occupancy;
    ctx->current = rb->current_index;

    Iterator it = {0};
    it.context = (void *)ctx;
    it.next = ring_iterator_next;
    it.prev = ring_iterator_prev;
    it.get = ring_iterator_get;
    return it;
}

/**
 * @brief move to the next index in the ring buffer
 *
 * @param curInd current index to move from
 * @retval True if successful else false
 */
bool move_next_ring_buffer(RingBuffer *rb)
{
    if (rb == NULL)
    {
        return false;
    }

    if (rb->current_index < rb->occupancy)
    {
        rb->current_index++;
    } else {
        rb->current_index = 0;
    }

    return true;
}

/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool move_prev_ring_buffer(RingBuffer *rb)
{
    if (rb == NULL)
    {
        return false;
    }

    if (rb->current_index > 0)
    {
        rb->current_index--;
    } else {
        rb->current_index = rb->occupancy;
    }

    return true;
}

/**
 * @brief Increment the number of occupants in the ring buffer if not filled yet and increment sequence number
 *
 * @param rb pointer to ring buffer struct
 * @retval true if successful else false
 */
bool add_ring_buffer(RingBuffer *rb)
{
    if (rb == NULL)
    {
        return false;
    }

    if (rb->occupancy != rb->size)
    {
        rb->occupancy++;
    }


    return true;
}

/**
 * @brief Increment the sequence number of the ring buffer. Wrap on UINT32_MAX
 *
 * @param rb pointer to ring buffer struct
 * @retval true if successful else false
 */
bool inc_seq_ring_buffer(RingBuffer *rb)
{
    if (rb == NULL)
    {
        return false;
    }

    if (rb->seq <= UINT32_MAX)
    {
        rb->seq++;
    } else {
        rb->seq = 0;
    }
    return true;
}

/**
 * @brief get the index at the
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_get(Iterator *it, void* out)
{
    if (it == NULL || out == NULL)
    {
        return false;
    }

    RBIteratorCtx *ctx = it->context;

    if (ctx->current < ctx->lower_lim || ctx->current > ctx->upper_lim)
    {
        return false;
    }

    *(uint16_t *)out = ctx->current;

    return true;
}
/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_next(Iterator *it)
{
    if (it == NULL)
    {
        return false;
    }

    RBIteratorCtx *ctx = it->context;

    // if not at the end of the ring buffer increment else, move back to start
    if (ctx->current < ctx->upper_lim)
    {
        ctx->current++;
    } else {
        ctx->current = ctx->lower_lim;
    }

    return true;

}
/**
 * @brief move to the previous index in the ring buffer
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_prev(Iterator *it)
{
    if (it == NULL)
    {
        return false;
    }

    RBIteratorCtx *ctx = it->context;

    // if not at the start of the ring buffer increment else, move to end
    if (ctx->current > ctx->lower_lim)
    {
        ctx->current--;
    } else {
        ctx->current = ctx->upper_lim;
    }

    return true;
}

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
