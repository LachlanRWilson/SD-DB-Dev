#include <string.h>
#include "message.h"
#include "free_list_stack.h"
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
    return read_sector(storage, DATA_SECTOR_TO_RAW(MESSAGE_DATA_SECTOR(index)), out->buffer);
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
    return write_sector(storage, DATA_SECTOR_TO_RAW(MESSAGE_DATA_SECTOR(index)), in->buffer);
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
    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(index));


    // If the sector has not been used than don't need to read
    if (is_used)
    {

        // read message sector out off the SD card
        STRG_RET read_ret = read_message_sector(storage, index, &mSector);
        // Read failure or corrupt sector
        if (read_ret != STRG_OK)
        {
            return read_ret;
        }

    } else {
        return STRG_EMPTY;
    }

    // get the message count to know where to put the message
    uint16_t msg_count = mSector.var.header.msg_count;

    // if sector full then need to allocate a new sector (checked before journalling
    // so a full sector doesn't leave an active journal behind)
    if (msg_count >= MESSAGE_BLOCK_CAPACITY)
    {
        return STRG_FULL;
    }

    // Add message sector to journal
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, MESSAGE_DATA_SECTOR(index), mSector.buffer);

    if (!journal_add_success)
    {
        return STRG_FAIL;
    }

    // Update the sector bit in bitmap to used (ram and sd)
    bool update_success = update_usage_bit(storage, MESSAGE_DATA_SECTOR(index), true);
    if (update_success == STRG_FAIL)
    {
        return false;
    }

    // Write message to sector
    memcpy(mSector.var.messages[msg_count].buffer, in->buffer, sizeof(Message));

    // increment message count
    mSector.var.header.msg_count++;

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

STRG_RET write_new_message_sector(Storage *storage, Journal *journal, const char* phone, uint16_t index, MessageBuffer *in)
{
    MessageSectorBuffer mSector;

    // check if the sector is in use
    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(index));

    // If the sector has been used bad allocation
    if (is_used)
    {
        return STRG_FULL;
    }

    create_new_message_sector(&mSector, phone, UINT16_MAX);

    // Add to journal (storing usage_bitmap, mSector content is irrelivant)
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, MESSAGE_DATA_SECTOR(index), mSector.buffer);

    if (!journal_add_success)
    {
        return STRG_FAIL;
    }
    // Update the sector bit in bitmap to used (ram and sd)
    bool update_success = update_usage_bit(storage, MESSAGE_DATA_SECTOR(index), true);
    if (update_success == STRG_FAIL)
    {
        return false;
    }

    // Write message to sector
    memcpy(mSector.var.messages[0].buffer, in->buffer, sizeof(Message));

    // increment message count
    mSector.var.header.msg_count++;

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

STRG_RET point_message_sector_to_next(Storage *storage, Journal *journal, uint16_t next, uint16_t prev)
{

    MessageSectorBuffer mSector;
    STRG_RET ret;
    // update previous sector to point to next
    ret = read_message_sector(storage, prev, &mSector);

    if (ret != STRG_OK)
    {
        return STRG_FAIL;
    }

    // Add to journal (storing the old prev-sector content for rollback)
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, MESSAGE_DATA_SECTOR(prev), mSector.buffer);

    if (!journal_add_success)
    {
        return STRG_FAIL;
    }

    mSector.var.header.next = next;

    bool write_success = write_message_sector(storage, prev, &mSector);

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
 * @brief Initialise a new message sector with message and write to SD card
 *
 * @param storage Pointer to the storage abstraction.
 * @param journal pointer to rollback journal
 * @param phone phone number associated with message
 * @param prev sector index the current full sector
 * @param next sector index the next sector given from allocator
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_next_message_sector(Storage *storage, Journal *journal, const char* phone, uint16_t prev, uint16_t next,
        MessageBuffer *in)
{
    MessageSectorBuffer mSector;
    STRG_RET ret;

    // check if the sector is in use
    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(next));

    // If the sector has been used bad allocation
    if (is_used)
    {
        return STRG_FULL;
    }

    // check the current full index is actually being used
    is_used = check_usage_bit(MESSAGE_DATA_SECTOR(prev));

    if (!is_used)
    {
        return STRG_EMPTY;
    }

    create_new_message_sector(&mSector, phone, prev);

    // Add to journal (storing usage_bitmap, mSector content is irrelivant)
    bool journal_add_success = journal_add(journal, JRNL_MESSAGE, MESSAGE_DATA_SECTOR(next), mSector.buffer);

    if (!journal_add_success)
    {
        return STRG_FAIL;
    }
    // Update the sector bit in bitmap to used (ram and sd)
    bool update_success = update_usage_bit(storage, MESSAGE_DATA_SECTOR(next), true);
    if (update_success == STRG_FAIL)
    {
        return false;
    }

    // Write message to sector
    memcpy(mSector.var.messages[0].buffer, in->buffer, sizeof(Message));

    // increment message count
    mSector.var.header.msg_count++;

    bool write_success = write_message_sector(storage, next, &mSector);

    if (write_success == STRG_FAIL)
    {
        return STRG_FAIL;
    }

    // free journal
    bool jrnl_free_success = journal_free(journal);
    if (!jrnl_free_success) {
        return STRG_FAIL;
    }

    // ensure the current sector (prev) is now pointing as the next sector (next)
    ret = point_message_sector_to_next(storage, journal, next, prev);

    if (ret != STRG_OK)
    {
        return ret;
    }

    return STRG_OK;
}

/**
 * @brief Read message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param jounral ponter to the journal struct to jounral sector for rollback
 * @param index memory index of the contact.
 * @param pos last message position in the sector
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

    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(index));
    // If sector is not used than it is an empty sector
    if (!is_used){
        return STRG_EMPTY;
    }

    // Read message sector
    ret = read_message_sector(storage, index, &mSector);

    // exit if read failure or corrupt sector
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Get message count from header
    uint16_t msg_count = mSector.var.header.msg_count;

    // If message count exceeds the capacity of the sector there is an error with the sector header or sector
    if (msg_count > MESSAGE_BLOCK_CAPACITY)
    {
        return STRG_FAIL;
    }

    // pos indexes back from the newest message; anything at or past msg_count doesn't exist
    if (pos >= msg_count)
    {
        return STRG_FAIL;
    }

    // write message from sd card to output
    memcpy(out->buffer, mSector.var.messages[msg_count - pos - 1].buffer, sizeof(Message));

    return STRG_OK;
}


/**
 * @brief Read n number of messages from the sd card of the associated phone number
 *
 * @param storage Pointer to the storage abstraction.
 * @param startIndex memory index of the latest message sector
 * @param n number of message to be read to the output message buffer array
 * @param out array of messages
 * @retval number of messages read, if failure return negative number. Follows STRG_RET enum except -1 if failure
 */
int read_n_messages(Storage *storage, uint16_t startIndex, int n, MessageBuffer *out)
{
    MessageSectorBuffer curMSector;
    STRG_RET ret;
    int msg_count;
    int msg_read = 0;

    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(startIndex));

    if (!is_used)
    {
        return -1 * STRG_EMPTY;
    }

    ret = read_message_sector(storage, startIndex, &curMSector);

    if (ret != STRG_OK)
    {
        return -1 * ret;
    }

    // Get message count from header
    msg_count = curMSector.var.header.msg_count;

    // iterate over the first message sector (could be partially full)
    for (; (msg_count > 0) && (msg_read < n ); msg_read++, msg_count--)
    {
        // Copy message to array
        memcpy(out[msg_read].buffer,
            curMSector.var.messages[msg_count - 1].buffer,
            sizeof(Message));
    }


    // iterate over n messages
    while (msg_read < n)
    {
        // When all messages are read from message sector move to the next sector
        if (msg_count <= 0)
        {
            // Reached the end of the messages
            if (curMSector.var.header.prev == UINT16_MAX)
            {
                return msg_read;
            }

            // read previous message sector
            ret = read_message_sector(storage, curMSector.var.header.prev, &curMSector);

            if (ret != STRG_OK)
            {
                return STRG_FAIL - 1; // return -1 for failue (need to do this because normally fail is 0)
            }

            // set message count to number of messages in new sector (should always be MESSAGE_BLOCK_CAPACITY)
            msg_count = curMSector.var.header.msg_count;
        }

        // Copy message to array
        memcpy(out[msg_read].buffer,
            curMSector.var.messages[msg_count - 1].buffer,
            sizeof(Message));

        // decrement message sector count and increment number of messages read
        msg_count--;
        msg_read++;
    }

    return msg_read;
}


/**
 * @brief Remove a message sector from the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param journal pointer to journal to rollback removal if write error
 * @param msg_alloctor pointer to message allocator to allow sector to be reused of FLS
 * @param index memory index of message sector
 * @param out removed message
 * @retval STRG_RET, return code of storage interaction
 */
STRG_RET remove_message_sector(Storage *storage, Journal *journal, FreeList *msg_allocator, uint16_t index,
                               MessageSectorBuffer *out)
{
    STRG_RET ret;

    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(index));
    if (!is_used)
    {
        return STRG_EMPTY;
    }

    // Read the message sector
    ret = read_message_sector(storage, index, out);

    if (ret != STRG_OK)
    {
        return ret;
    }


    // Add contact to jounral
    ret = journal_add(journal, JRNL_MESSAGE, MESSAGE_DATA_SECTOR(index), out->buffer);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // Update usage bit vector to state the sector is no longer allocated
    ret = update_usage_bit(storage, MESSAGE_DATA_SECTOR(index), false);

    if (ret != STRG_OK)
    {
        return ret;
    }

    // free active journal now the changes have been made
    ret = journal_free(journal);

    if (ret != STRG_OK)
    {
        return ret;
    }

    // free the sector to be reallocated by FLS
    free_list_free(msg_allocator, index);

    return STRG_OK;
}

bool remove_message_chat(Storage *storage, Journal *journal, FreeList *msg_allocator, uint16_t startIndex)
{
    MessageSectorBuffer mSector;
    uint16_t curIndex = startIndex;
    STRG_RET ret;

    // Iterate until there are no more message sectors
    while (curIndex != UINT16_MAX)
    {
        // Remove the message sector
        ret = remove_message_sector(storage, journal, msg_allocator, curIndex, &mSector);
        if (ret != STRG_OK)
        {
            return false;
        }

        // Update current sector to previous sector
        curIndex = mSector.var.header.prev;
    }

    return true;
}

MessageBuffer create_message(uint16_t timestamp, bool direction, char *str)
{
    MessageBuffer newMsg = {0};

    if (timestamp == 0 || str == NULL)
    {
        return newMsg;
    }

    size_t len = strlen(str);
    if (len >= SMS_MAX_MESSAGE_LENGTH)
    {
        len = SMS_MAX_MESSAGE_LENGTH - 1;
    }

    newMsg.msg.timestamp= timestamp;
    newMsg.msg.direction = direction;
    memcpy(newMsg.msg.str, str, len);


    return newMsg;
}

void create_new_message_sector(MessageSectorBuffer *mSector, const char* phone, uint16_t prev)
{
    // start from a clean sector so no stack garbage is persisted
    memset(mSector, 0, sizeof(*mSector));

    mSector->var.type = MESSAGE_SECTOR;
    mSector->var.header.prev = prev;
    mSector->var.header.next = UINT16_MAX;
    mSector->var.header.msg_count = 0;
    mSector->var.header.state = EXTENT_OCCUPIED;

    // phone number is needed to link the chat back to its contact on reconstruction
    size_t phone_len = strlen(phone);
    if (phone_len > MAX_PHONE_LEN)
    {
        phone_len = MAX_PHONE_LEN;
    }
    memcpy(mSector->var.header.phone, phone, phone_len);
    mSector->var.header.phone_len = (uint8_t)phone_len;
}
