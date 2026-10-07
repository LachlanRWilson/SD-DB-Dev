#include "ring_buffer.h"
#include "iterator.h"

/**
 * @brief Read the ring buffer sector from the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param out Contact Sector Buffer with the desired contact position
 * @retval True if successful read else false.
 */
STRG_RET read_ring_buffer_sector(Storage *storage, uint16_t raw_sector_start, uint16_t index, U_RingBufferSector *out)
{
  uint16_t raw_sector_index = (index) + raw_sector_start;

  return read_sector(storage, raw_sector_index, out->buffer);
}

/**
 * @brief Check to see if the head of the target is to the right or the left of the middle
 *
 * @param seq_start starting number in the ring buffer sequence
 * @param seq_i sequnce number that is being checked
 * @retval True if seq_i is greater than seq_start. This holds for normal sequence and if the number wraps around.
 * i.e for uint8, seq_start = 0xFF and seq_i = 3 than (seq_i - seq_start) = 3 therefore is greater
 */
static inline bool binary_search_target_right(RB_SECTOR_STATE state_i, uint32_t seq_start, uint32_t seq_i, uint16_t mid)
{
    return (state_i == RB_OCCUPIED) && (seq_i - seq_start) == mid;
}

STRG_RET seq_and_state_at(Storage *storage, uint16_t raw_sector_start,
                          uint16_t index, uint32_t *seq,
                          RB_SECTOR_STATE *state) {
  U_RingBufferSector rbSector;
  STRG_RET ret;

  ret = read_ring_buffer_sector(storage, raw_sector_start, index, &rbSector);
  if (ret != STRG_OK) {
    return ret;
  }

  // get sequence from sector
  *seq = rbSector.sector.header.seq;
  *state = rbSector.sector.header.state;
  return STRG_OK;
}

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
STRG_RET is_ring_buffer_wrapped(Storage *storage, uint16_t raw_sector_start, uint16_t head)
{
    STRG_RET ret;
    uint32_t seq;
    RB_SECTOR_STATE state;
    ret = seq_and_state_at(storage, raw_sector_start, head + 1, &seq, &state);
    if (ret != STRG_OK)
    {
        return STRG_FAIL;
    }


    // if the next sector is unoccupied then the ring buffer has not wrapped
    if (state != RB_OCCUPIED)
    {
        return STRG_EMPTY;
    }

    return STRG_OK;
}


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
                            uint16_t mem_size, uint16_t *head, uint32_t *seq)
{
    STRG_RET ret;
    uint32_t seq_start;
    uint32_t seq_i;
    RB_SECTOR_STATE state;


    // Set lo, hi and mid index for binary search
    uint16_t hi = mem_size;
    uint16_t lo = 0;
    // Get the mid point
    uint16_t mid = lo + (hi - lo) / 2;

    // If the first sector is empty than nothing as been added to the ring buffer
    ret = seq_and_state_at(storage, raw_sector_start, 0, &seq_start, &state);
    if (ret != STRG_OK) {
        return STRG_FAIL;
    }

    if (state == RB_EMPTY) {
        return STRG_EMPTY;
    }

    // Iterate until hi and lo indexes are next to each other
    while (hi - lo > 1) {

        ret = seq_and_state_at(storage, raw_sector_start, mid, &seq_i, &state);
        if (ret != STRG_OK)
        {
            return STRG_FAIL;
        }

        // If binary target is to right, move lo to mid, else high to mid
        if (binary_search_target_right(state, seq_start, seq_i, mid))
        {
            lo = mid;
        } else {
            hi = mid;
        }

        mid = lo + (hi - lo) / 2;
    }


    // make sure to get the sequence and state (these values wont be head if the final mid is turned to hi)
    ret = seq_and_state_at(storage, raw_sector_start, lo, &seq_i, &state);
    if (ret != STRG_OK)
    {
        return STRG_FAIL;
    }

    *head = lo;
    *seq = seq_i;

    return STRG_OK;
}

/**
 * @brief Using binary search to find the head of the ring buffer. Ring buffer sector indexes are
 * (raw_sector_start, sector_size - 1).
 *
 * @param storage pointer to storage abstraction struct
 * @param rb Pointer to RingBuffer struct that is going to be populated
 * @retval True if successful write else false.
 */
bool reconstruct_ring_buffer(Storage* storage, RingBuffer *rb, uint16_t raw_sector_start)
{
    STRG_RET ret;
    uint16_t head;
    uint32_t seq;


    // search for the head of the ring buffer
    ret = binary_search_head(storage, raw_sector_start, rb->size, &head, &seq);
    if (ret == STRG_EMPTY)
    {
        rb->current_index = 0;
        rb->seq = 0;
        rb->occupancy = 0;
        return true;
    } else if (ret == STRG_FAIL) {
        return false;
    }

    rb->seq = seq;
    rb->current_index = head;

    // if head is at the end than ring buffer is also full
    if (head == rb->size - 1)
    {
        rb->occupancy = rb->size;
        return true;
    }

    ret = is_ring_buffer_wrapped(storage, raw_sector_start, head);

    // Check if the ring buffer is wrapped
    switch (ret)
    {
        case STRG_OK:       // buffer is wrapped
            rb->occupancy = rb->size;
            break;

        case STRG_EMPTY:    // buffer is not wrapped
            rb->occupancy = head + 1;
            break;

        default:            // STRG_FAIL or other
            return false;
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
bool init_ring_buffer(Storage *storage, RingBuffer *rb, uint16_t size, uint16_t raw_sector_start)
{
    if ((rb == NULL) || (size == 0))
    {
        return false;
    }

    rb->size = size;

    return reconstruct_ring_buffer(storage, rb, raw_sector_start);
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

    // Wrap at rb->occurpancy - 1 index
    if (rb->current_index < (rb->occupancy - 1))
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
        rb->current_index = rb->size - 1;
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

    if (rb->occupancy < rb->size)
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

    rb->seq++;
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
