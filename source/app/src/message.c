#include <string.h>
#include "message.h"
#include "journal.h"
#include "usage_bitmap.h"

/**
 * @brief Read message sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param out sector being read from the SD Card
 * @retval True if successful read else false.
 */
STRG_RET read_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out)
{
    return read_sector(storage, index, out->buffer);
}

/**
 * @brief write message sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *in)
{
    return write_sector(storage, index, in->buffer);
}


/**
 * @brief Write message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param jounral ponter to the journal struct to jounral sector for rollback
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card
 * @retval STRG_RET return enum which describes the storage after running function
 */
STRG_RET write_message(Storage *storage, Journal *journal, uint16_t index, MessageBuffer *in)
{
    MessageSectorBuffer mSector;

    // check if the sector is in use
    bool is_used = check_usage_bit(index);


    // If the sector has not been used than don't need to read
    if (is_used)
    {

        // read message sector out off the SD card
        bool read_success = read_message_sector(storage, index, &mSector);
        // Read failure
        if (!read_success)
        {
            return STRG_FAIL;
        }

    } else {
        memset(&mSector, 0, sizeof(mSector));
    }

    // Add message sector to journal
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, index, mSector.buffer);

    if (!journal_add_success)
    {
        return STRG_FAIL;
    }

    // Update the sector bit in bitmap to used (ram and sd)
    bool update_success = update_usage_bit(storage, index, true);
    if (update_success == STRG_FAIL)
    {
        return false;
    }

    // get the message count to know where to put the message
    uint16_t msg_count = mSector.var.header.msg_count;

    // if sector full then need to allocate a new sector
    if (msg_count >= MESSAGE_BLOCK_CAPACITY)
    {
        // allocate new message
        return STRG_FULL;
    }

    // Write message to sector
    memcpy(mSector.var.messages[msg_count].buffer, in->buffer, sizeof(Message));

    bool write_success = write_message_sector(storage, index, &mSector);

    if (write_success == STRG_FAIL)
    {
        return STRG_FAIL;
    }

    // free journal
    bool jrnl_free_success = journal_free(journal);
    if (!jrnl_free_success) {
        return STRG_FAIL;
    }

    return STRG_OK;
}

/**
 * @brief Read message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param jounral ponter to the journal struct to jounral sector for rollback
 * @param index memory index of the contact.
 * @param pos message position in sector
 * @param out sector being written to the SD Card
 * @retval STRG_RET return enum which describes the storage after running function
 */
STRG_RET read_message(Storage *storage, uint16_t index, uint8_t pos, MessageBuffer *out)
{
    MessageSectorBuffer mSector;
    // return value from each storage interaction
    STRG_RET ret;

    // If position is invalid
    if (pos > MESSAGE_BLOCK_CAPACITY) {
        return STRG_FAIL;
    }

    bool is_used = check_usage_bit(index);
    // If sector is not used than it is an empty sector
    if (!is_used){
        return STRG_EMPTY;
    }

    // Read message sector
    ret = read_message_sector(storage, index, &mSector);

    // exit if read failure
    if (ret == STRG_FAIL)
    {
        return STRG_FAIL;
    }

    // Get message count from header
    uint16_t msg_count = mSector.var.header.msg_count;

    // If message count exceeds the capacity of the sector there is an error with the sector header or sector
    if (msg_count > MESSAGE_BLOCK_CAPACITY)
    {
        return STRG_FAIL;
    }


    // write message from sd card to output
    memcpy(out->buffer, mSector.var.messages[pos].buffer, sizeof(Message));

    return STRG_OK;
}


/**
 * @brief Read all the messages in a message sector
 *
 * @param storage Pointer to the storage abstraction.
 * @param jounral ponter to the journal struct to jounral sector for rollback
 * @param index memory index of the contact.
 * @param out pointer to a message array of at least TWO free elements
 * @retval STRG_RET return enum which describes the storage after running function
 */
STRG_RET read_messages(Storage *storage, uint16_t index, MessageBuffer *out)
{
    // Iterate over message in message sector
    for (int i = 0; i < MESSAGE_BLOCK_CAPACITY; i++)
    {
        read_message(storage, index, i, out + i);
    }
    return STRG_OK;
}
