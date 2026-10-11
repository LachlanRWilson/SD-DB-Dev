#include "call_history.h"
#include "journal.h"
#include "string.h"

/**
 * @brief Read the message history sector to the appropriate sector index
 * offset by the message history data region start
 *
 * @param storage pointer to storage abstraction struct
 * @param sector_index message history sector index (not offset)
 * @param out output sector buffer
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET read_call_history_sector(Storage *storage, uint16_t sector_index, CallHistorySectorB *out)
{
    uint16_t raw_sector_index = sector_index + CALL_HISTORY_DATA_START_SECTOR + DATA_REGION_START_SECTOR;
    return read_sector(storage, raw_sector_index, out->buffer);
}

/**
 * @brief Write the message history sector to the appropriate sector index
 * offset by the message history data region start
 *
 * @param storage pointer to storage abstraction struct
 * @param sector_index message history sector index (not offset)
 * @param in input sector buffer
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET write_call_history_sector(Storage *storage, uint16_t sector_index, CallHistorySectorB *in)
{
    uint16_t raw_sector_index = sector_index + CALL_HISTORY_DATA_START_SECTOR + DATA_REGION_START_SECTOR;
    return write_sector(storage, raw_sector_index, in->buffer);
}

/**
 * @brief Initialise CallHistory and RingBuffer
 *
 * @param rb Ring Buffer struct pointer
 * @retval True if successful else false.
 */
bool call_history_init(RingBuffer *rb)
{
    if (rb == NULL)
    {
        return false;
    }
    rb->current_index = 0;
    rb->size = HASH_TABLE_ENTRIES;
    rb->occupancy = 0;
    return true;
}

/**
 * @brief Set the current sector to full and write back to the sd card. Then increment the ring buffer and rerun message add on new sector
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
static STRG_RET set_sector_full_move_next(Journal *journal, Storage *storage, RingBuffer* rb,
                                     CallHistorySectorB *mhSector, CallHistoryEntry *entry)
{
    STRG_RET ret;
    RingBufferHeader *rb_header = &(mhSector->sector.header);

    // Set the sector to full and write
    rb_header->state = RB_FULL;

    ret = write_call_history_sector(storage, rb->current_index, mhSector);
    if (ret != STRG_OK)
    {
        return ret;
    }
    ret = journal_free(journal);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // add entry and move ring buffer, then recall message history add
    add_ring_buffer(rb);
    move_next_ring_buffer(rb);
    inc_seq_ring_buffer(rb);
    return call_history_add(rb, journal, storage, entry);

}

/**
 * @brief Read all the message history entries from pos. Note that if the number of read message meets n than stop 
 * reading
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET read_call_history(Storage *storage, uint16_t sector_index, uint8_t pos, size_t n, size_t *out_count, 
                              CallHistoryEntry *out_list)
{
    STRG_RET ret;
    CallHistorySectorB chSector;
    RingBufferHeader *rbHeader;
    CallHistoryData *chData;

    ret = read_call_history_sector(storage, sector_index, &chSector);
    if (ret != STRG_OK)
    {
        return ret;

    }

    rbHeader = &(chSector.sector.header);
    chData = &(chSector.sector.data);

    // if the sector is empty stop getting list
    if (rbHeader->state == RB_EMPTY)
    {
        return STRG_OK;
    }

    // get message out of sector
    for (int slots = pos; slots >= 0 && *out_count < n; slots--)
    {
        memcpy(&out_list[*out_count], &(chData->chEntry[slots]), sizeof(CallHistoryEntry));
        (*out_count)++;
    }

    return STRG_OK;
}

static STRG_RET read_first_sector(Storage *storage, Iterator *it, uint8_t start_head, size_t n, size_t *out_count, CallHistoryEntry *out_list)
{
    STRG_RET ret;
    uint16_t sector_index;
    // Get the start sector and read the 
    if (!ring_iterator_get(it, (void *) &sector_index))
    {
        return STRG_FAIL;
    }

    ret = read_call_history(storage, sector_index, start_head, n, out_count, out_list);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (!ring_iterator_prev(it))
    {
        return STRG_FAIL;
    }
    return STRG_OK;
}

/**
 * @brief Get a range of call history
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET call_history_get_range(RingBuffer *rb, Storage *storage, size_t start, size_t n, size_t *out_count, CallHistoryEntry *out_list)
{
    if (rb == NULL || storage == NULL || out_count == NULL || out_list == NULL)
    {
        return STRG_FAIL;
    }

    STRG_RET ret;

    CallHistorySectorB chSector;
    RingBufferHeader *rbHeader;
    uint16_t sector_index;
    RBIteratorCtx rb_it_ctx;
    Iterator it;
    size_t sector_visited = 0;
    uint8_t start_head = 0;
    *out_count = 0;

    // look at the current sector to find how many sectors to skip
    ret = read_call_history_sector(storage, rb->current_index, &chSector);
    if (ret != STRG_OK)
    {
        return ret;

    }

    rbHeader = &(chSector.sector.header);

    // if the sector is empty stop getting list
    if (rbHeader->state == RB_EMPTY)
    {
        return STRG_OK;
    }

    // number of messages in the current sector (may be partially filled)
    size_t cur_cnt = rbHeader->head + 1;
    int num_sectors_to_start;

    if (start < cur_cnt)
    {
        // start is inside the current sector
        num_sectors_to_start = 0;
        start_head = rbHeader->head - start;
    }
    else
    {
        // messages past the current sector, older sectors are always full
        size_t rem = start - cur_cnt;
        num_sectors_to_start = 1 + rem / CALL_HISTORY_SECTOR_CAPACITY;
        // start head is the remainder inverted. Since latest to oldest is 2->0
        start_head = (CALL_HISTORY_SECTOR_CAPACITY - 1) - (rem % CALL_HISTORY_SECTOR_CAPACITY);
    }

    // start exceeds number of sectors in the ring buffer
    if (num_sectors_to_start >= rb->occupancy)
    {
        return STRG_FAIL;
    }

    int start_rb_index = rb->current_index - num_sectors_to_start;

    // handling for wrap around for negative index
    if (start_rb_index < 0)
    {
        start_rb_index = (rb->occupancy) + start_rb_index;
    }


    it = ring_buffer_iterator_init(&rb_it_ctx, rb, start_rb_index);

    // read the first sector
    ret = read_first_sector(storage, &it, start_head, n, out_count, out_list);
    if (ret != STRG_OK)
    {
        return ret;
    }

    sector_visited++;

    // read sectors until n messages are read or entire ring buffer
    while ((*out_count < n) && (sector_visited < rb->occupancy - num_sectors_to_start))
    {
        // get the sector index from iterator
        if (!ring_iterator_get(&it, (void *) &sector_index))
        {
            return STRG_FAIL;
        }

        ret = read_call_history(storage, sector_index, CALL_HISTORY_SECTOR_CAPACITY - 1, n, out_count, out_list);
        if (ret != STRG_OK)
        {
            return ret;
        }
        sector_visited++;
        if (!ring_iterator_prev(&it))
        {
            return STRG_FAIL;
        }
    }

    return STRG_OK;

}

/**
 * @brief Add new latest contact and index to the ring buffer
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET call_history_add(RingBuffer *rb, Journal *journal, Storage *storage, CallHistoryEntry *message)
{
    CallHistorySectorB chSector;
    STRG_RET ret;

    // if ring buffer hasn't been added to yet add entry
    if (rb->occupancy == 0) {
        add_ring_buffer(rb);
    }

    ret = read_call_history_sector(storage, rb->current_index, &chSector);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (!journal_add(journal, JRNL_CALL_HIST, CALL_HIST_DATA_SECTOR(rb->current_index), chSector.buffer))
    {
        return STRG_FAIL;
    }

    // Retrieve header and data pointer from sector
    CallHistoryData* ch_data = &chSector.sector.data;
    RingBufferHeader* rb_header = &chSector.sector.header;

    // check the ring buffer sector state
    switch (rb_header->state)
    {
        case RB_EMPTY:
            rb_header->state = RB_OCCUPIED;
            rb_header->head = 0;
            rb_header->seq = rb->seq;
            break;
        case RB_FULL:
            rb_header->seq = rb->seq;
            rb_header->head = 0;
            rb_header->state = RB_OCCUPIED;
            break;
        case RB_OCCUPIED:
            // check if current sector is full, if so move to next sector
            if (rb_header->head == CALL_HISTORY_SECTOR_CAPACITY - 1) {
                return set_sector_full_move_next(journal, storage, rb, &chSector, message);
            }
            // if sector not full increment the header
            rb_header->head++;
            break;

        default:
            return STRG_FAIL;
    }

    // Adding message to sector
    memcpy(&(ch_data->chEntry[rb_header->head]), message, sizeof(CallHistoryEntry));

    ret = write_call_history_sector(storage, rb->current_index, &chSector);
    if (ret != STRG_OK)
    {
        return ret;
    }

    ret = journal_free(journal);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Increment the sequence number after successful write
    return STRG_OK;

}
