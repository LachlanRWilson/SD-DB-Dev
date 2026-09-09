#include <string.h>
#include "message.h"
#include "journal.h"
#include "usage_bitmap.h"

/**
 * @brief Read message sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval True if successful read else false.
 */
bool read_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out)
{
    return read_sector(storage, index, out->buffer);
}

/**
 * @brief Write message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
bool write_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out)
{
    return write_sector(storage, index, out->buffer);
}


bool write_message(Storage *storage, Journal *journal, uint16_t index, MessageBuffer *in)
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
            return false;
        }

    } else {
        memset(&mSector, 0, sizeof(mSector));
    }

    // Add message sector to journal
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, index, mSector.buffer);

    if (!journal_add_success)
    {
        return false;
    }

    // Update the sector bit in bitmap to used (ram and sd)
    bool update_success = update_usage_bit(storage, index, true);
    if (!update_success)
    {
        return false;
    }

    // get the message count to know where to put the message
    uint16_t msg_count = mSector.var.header.msg_count;

    // if sector full then need to allocate a new sector
    if (msg_count >= MESSAGE_BLOCK_CAPACITY)
    {
        // allocate new message
    }

    // Write message to sector
    memcpy(mSector.var.messages[msg_count].buffer, in->buffer, sizeof(Message));

    bool write_success = write_message_sector(storage, index, &mSector);

    if (!write_success)
    {
        return false;
    }

    // free journal
    bool jrnl_free_success = journal_free(journal);
    if (!jrnl_free_success) {
        return false;
    }

    return true;
}


