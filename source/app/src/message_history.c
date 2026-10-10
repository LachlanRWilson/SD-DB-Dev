#include "message_history.h"
#include "ring_buffer.h"
#include "iterator.h"
#include "journal.h"
#include <string.h>

/**
 * @brief Initialise MessageHistory and RingBuffer
 *
 * @param rb Ring Buffer struct pointer
 * @retval True if successful else false.
 */
bool message_history_init(RingBuffer *rb)
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
 * @brief Read the message history sector to the appropriate sector index
 * offset by the message history data region start
 *
 * @param storage pointer to storage abstraction struct
 * @param sector_index message history sector index (not offset)
 * @param out output sector buffer
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET read_message_history_sector(Storage *storage, uint16_t sector_index, MessageHistorySectorB *out)
{
    uint16_t raw_sector_index = sector_index + MESSAGE_HISTORY_DATA_START_SECTOR + DATA_REGION_START_SECTOR;
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
STRG_RET write_message_history_sector(Storage *storage, uint16_t sector_index, MessageHistorySectorB *in)
{
    uint16_t raw_sector_index = sector_index + MESSAGE_HISTORY_DATA_START_SECTOR + DATA_REGION_START_SECTOR;
    return write_sector(storage, raw_sector_index, in->buffer);
}

/**
 * @brief Get a range of message history
 *
 * @param rb message history ring buffer struct
 * @param start starting message in message history (from latest where 0 = latest message)
 * @param n number of latest message to be read
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_get_range(RingBuffer *rb, Storage *storage, size_t start, size_t n, Message *out)
{
    RBIteratorCtx ctx;
    STRG_RET ret;
    MessageHistorySectorB mhSector;
    int msg_history_cnt = 0;
    uint16_t messageHistIndex = 0;
    uint16_t messageIndex = 0;
    Iterator it = ring_buffer_iterator_init(&ctx, rb);

    // Use iterator to search over ring buffer
    while ((msg_history_cnt < n) && iterator_next_fn(&it))
    {
        if (!iterator_get_fn(&it, (void*) &messageHistIndex))
        {
            return STRG_FAIL;
        }

        ret = read_message_history_sector(storage, messageHistIndex / MESSAGE_HISTORY_SECTOR_CAPACITY, &mhSector);
        if (ret != STRG_OK)
        {
            return ret;
        }


        msg_history_cnt++;
    }

    return STRG_OK;
}


/**
 * @brief Set the current sector to full and write back to the sd card. Then increment the ring buffer and rerun message add on new sector
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET set_sector_full_move_next(Journal *journal, Storage *storage, RingBuffer* rb,
                                     MessageHistorySectorB *mhSector, Message *message)
{
    STRG_RET ret;
    RingBufferHeader *rb_header = &(mhSector->sector.header);

    // Set the sector to full and write
    rb_header->state = RB_FULL;

    ret = write_message_history_sector(storage, rb->current_index, mhSector);
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
    return message_history_add(rb, journal, storage, message);

}

/**
 * @brief Add new message to latest sector in the ring buffer. If sector is filled, move to next sector.
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_add(RingBuffer *rb, Journal *journal, Storage *storage, Message *message)
{
    MessageHistorySectorB mhSector;
    STRG_RET ret;

    // if ring buffer hasn't been added to yet add entry
    if (rb->occupancy == 0) {
        add_ring_buffer(rb);
    }

    ret = read_message_history_sector(storage, rb->current_index, &mhSector);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (!journal_add(journal, JRNL_MSG_HIST, MESSAGE_HIST_DATA_SECTOR(rb->current_index), mhSector.buffer))
    {
        return STRG_FAIL;
    }

    // Retrieve header and data pointer from sector
    MessageHistoryData* mh_data = &mhSector.sector.data;
    RingBufferHeader* rb_header = &mhSector.sector.header;

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
            if (rb_header->head == MESSAGE_HISTORY_SECTOR_CAPACITY - 1) {
                return set_sector_full_move_next(journal, storage, rb, &mhSector, message);
            }
            // if sector not full increment the header
            rb_header->head++;
            break;

        default:
            return STRG_FAIL;
    }

    // Adding message to sector
    memcpy(&(mh_data->mh_entry[rb_header->head]), message, sizeof(Message));

    ret = write_message_history_sector(storage, rb->current_index, &mhSector);
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



/**
 * @brief Get a list of message history entries from latest to oldest
 *
 * @param rb Ring Buffer struct pointer
 * @param storage Storage abstraction struct
 * @param n number of messages to get
 * @param out_count pointer to memory which stores number of messages read
 * @param out_list pointer to array of n messages
 * @retval STRG_OK if successful
 */
STRG_RET message_history_get_list(RingBuffer *rb, Storage *storage, size_t n, size_t *out_count, Message *out_list)
{

    if (rb == NULL || storage == NULL || out_count == NULL || out_list == NULL)
    {
        return STRG_FAIL;
    }

    STRG_RET ret;

    MessageHistorySectorB mhSector;
    RingBufferHeader *rbHeader;
    MessageHistoryData *mhData;

    uint16_t sector_index;
    RBIteratorCtx rb_it_ctx;
    Iterator it = ring_buffer_iterator_init(&rb_it_ctx, rb);
    *out_count = 0;
    size_t sector_visited = 0;

    // read sectors until n messages are read or entire ring buffer
    while (*out_count < n && sector_visited < rb->occupancy)
    {
        // get the sector index from iterator
        if (!ring_iterator_get(&it, (void *) &sector_index))
        {
            return STRG_FAIL;
        }

        ret = read_message_history_sector(storage, sector_index, &mhSector);
        if (ret != STRG_OK)
        {
            return ret;

        }

        sector_visited++;
        rbHeader = &(mhSector.sector.header);
        mhData = &(mhSector.sector.data);

        // if the sector is empty stop getting list
        if (rbHeader->state == RB_EMPTY)
        {
            return STRG_OK;
        }

        // get message out of sector
        for (int slots = rbHeader->head; slots >= 0 && *out_count < n; slots--)
        {
            memcpy(&out_list[*out_count], &(mhData->mh_entry[slots]), sizeof(Message));
            (*out_count)++;
        }

        if (!ring_iterator_prev(&it))
        {
            return STRG_FAIL;
        }
    }

    return STRG_OK;
}
