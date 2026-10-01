#include "message_history.h"
#include "ring_buffer.h"
#include "iterator.h"

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

STRG_RET read_message_history_sector(Storage *storage, uint16_t sector_index, MessageHistorySectorB *out)
{

    uint16_t raw_sector_index = sector_index + MESSAGE_HISTORY_DATA_START_SECTOR + DATA_REGION_START_SECTOR;
    return read_sector(storage, raw_sector_index, out->buffer);
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


        messageIndex = mhSector.sector.sector_crc



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
STRG_RET message_history_add(RingBuffer *rb, uint16_t messageInd)
{

}
