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
 * @brief Add new latest message index to the ring buffer
 *
 * @param rb message history ring buffer struct
 * @param messageInd message sector index of the latest message
 * @retval STRG_OK is storage read successful else STRG_* error code
 */
STRG_RET message_history_add(RingBuffer *rb, Journal *journal, Storage *storage, Message *message)
{
    MessageHistorySectorB mhSector;
    STRG_RET ret;

    // move the ring buffer to the next sector
    if (!move_next_ring_buffer(rb))
    {
        return STRG_FAIL;
    }

    if (!add_ring_buffer(rb))
    {
        return STRG_FAIL;
    }


    uint16_t sector_index = rb->current_index / MESSAGE_HISTORY_SECTOR_CAPACITY;

    ret = read_message_history_sector(storage, rb->current_index / MESSAGE_HISTORY_SECTOR_CAPACITY, &mhSector);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if (!journal_add(journal, JRNL_MSG_HIST, MESSAGE_HIST_DATA_SECTOR(rb->current_index), mhSector.buffer))
    {
        return STRG_FAIL;
    }

    MessageHistEntry* mh_entry = &mhSector.sector.mh_entry[rb->current_index % MESSAGE_HISTORY_SECTOR_CAPACITY];

    // Add the sequence number to the entry and copy the message
    mh_entry->seq = rb->seq;
    memcpy((void *) &mh_entry->message, (void*) message, sizeof(Message));


    ret = write_message_history_sector(storage, sector_index, &mhSector);

    if (ret != STRG_OK)
    {
        return ret;
    }

    ret = journal_free(journal);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Increment the sequence number on ring buffer
    if(!inc_seq_ring_buffer(rb))
    {
        return STRG_FAIL;
    }
    return STRG_OK;
}
