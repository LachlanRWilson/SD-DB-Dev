#include "ring_buffer.h"
#include "iterator.h"
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
 * @brief get the index at the
 *
 * @param curind current index to move from
 * @retval true if successful else false
 */
bool ring_iterator_get(Iterator *it, void* out)
{
    if (it == NULL)
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
