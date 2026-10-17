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
    if (msg_count >= MESSAGE_SECTOR_CAPACITY)
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

/**
 * @brief Initialise a new message sector with message and write to SD card
 *
 * @param storage Pointer to the storage abstraction.
 * @param journal pointer to rollback journal
 * @param phone phone number associated with message
 * @param index sector index the new sector is being written to
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_new_message_sector(Storage *storage, Journal *journal, uint16_t contact_index, uint16_t index, MessageBuffer *in)
{
    MessageSectorBuffer mSector;

    // check if the sector is in use
    bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(index));

    // If the sector has been used bad allocation
    if (is_used)
    {
        return STRG_FULL;
    }

    create_new_message_sector(&mSector, contact_index, UINT16_MAX);

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
STRG_RET write_next_message_sector(Storage *storage, Journal *journal, uint16_t contact_index, uint16_t prev, uint16_t next,
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

    create_new_message_sector(&mSector, contact_index, prev);

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
    if (pos > MESSAGE_SECTOR_CAPACITY) {
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
    if (msg_count > MESSAGE_SECTOR_CAPACITY)
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
 * @brief Find the sector index where the remainder of messages have been passed
 *
 * @param storage Pointer to the storage abstraction.
 * @param startIndex memory index of the latest message sector
 * @param rem remainding number of message to pass to get to the sector
 * @param target_index pointer to the target index where all the messages have been passed
 * @retval STRG_RET
 */
static STRG_RET find_message_n_index(Storage *storage, uint16_t startIndex, size_t startMsg, size_t *num_hops, uint8_t *startOffset, uint16_t *target_index, MessageSectorBuffer *mSector)
{
    STRG_RET ret;
    MessageSectorHeader *mHeader;
    size_t msg_passed = 0;
    bool is_used = false;

    // preset previous index as target/current index such that the while loop
    // move to previous at start of every iteration
    uint16_t prev_index = startIndex;

    // iterate until read passed or equal to the start message count
    while ((msg_passed <= startMsg))
    {

        // Start index is passed the end of the chat
        if (prev_index == UINT16_MAX)
        {
            return STRG_FAIL;
        }

        if (*num_hops >= TOTAL_MESSAGE_SECTOR_SIZE)
        {
            return STRG_FAIL;
        }

        *target_index = prev_index;

        is_used = check_usage_bit(MESSAGE_DATA_SECTOR(*target_index));
        if (!is_used)
        {
            return STRG_EMPTY;
        }

        // Read the message sector
        ret = read_message_sector(storage, *target_index, mSector);

        if (ret != STRG_OK)
        {
            return ret;
        }

        mHeader = &(mSector->var.header);
        msg_passed += mHeader->msg_count;
        (*num_hops)++;
        prev_index = mHeader->prev;
    }

    // the start offset for the current sector to start reading messages from
    *startOffset = (msg_passed - 1) - startMsg;
    return STRG_OK;
}

static bool read_messages_from_p_in_sector(size_t n, size_t p, size_t *out_count, MessageSector *mSector, Message* out)
{
    if ((out_count == NULL) || (out == NULL) || (mSector == NULL) || (n == 0))
    {
        return false;
    }

    MessageSectorHeader *mHeader = &(mSector->header);

    // ensure p is bounded by number of positions in sector (0 -> msg_count - 1)
    if ((p >= mHeader->msg_count) || (mHeader->msg_count > MESSAGE_SECTOR_CAPACITY))
    {
        return false;
    }

    for (int slots = p; (slots >= 0) && (*out_count < n); slots--)
    {
        memcpy(&(out[*out_count]), &(mSector->messages[slots].msg), sizeof(Message));
        (*out_count)++;
    }
    return true;
}

static bool read_messages_from_sector(size_t n, size_t *out_count, MessageSector *mSector, Message* out)
{
    if ((out_count == NULL) || (out == NULL) || (mSector == NULL) || (n == 0))
    {
        return false;
    }

    MessageSectorHeader *mHeader = &(mSector->header);

    if (mHeader->msg_count > MESSAGE_SECTOR_CAPACITY)
    {
        return false;
    }

    for (int slots = mHeader->msg_count - 1; (slots >= 0) && (*out_count < n); slots--)
    {
        memcpy(&(out[*out_count]), &(mSector->messages[slots].msg), sizeof(Message));
        (*out_count)++;
    }
    return true;
}

/**
 * @brief Read n number of messages from the sd card of the associated phone number
 *
 * @param storage Pointer to the storage abstraction.
 * @param startIndex memory index of the latest message sector
 * @param startMsg start of the message range
 * @param n number of message to be read to the output message buffer array
 * @param out_count pointer to message read count
 * @param out pointer array of messages
 * @retval STRG_RET
 */
STRG_RET read_n_messages(Storage *storage, uint16_t startIndex, size_t startMsg, size_t n, size_t *out_count, Message *out)
{
    MessageSectorBuffer curMSector;
    STRG_RET ret;
    uint8_t pos;

    if ((storage == NULL) || (out_count == NULL) || (out == NULL))
    {
        return STRG_FAIL;
    } else if (n == 0) {
        *out_count = 0;
        return STRG_OK;
    }

    *out_count = 0;

    // empty chat: no sectors, nothing to read
    if (startIndex == UINT16_MAX)
    {
        return STRG_OK;
    }

    uint16_t current_index = startIndex;
    uint16_t prev_index;
    size_t hops = 0;
    bool is_used = false;

    ret = find_message_n_index(storage, startIndex, startMsg, &hops, &pos, &current_index, &curMSector);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // don't need to re-read message sector since stored in curMSector pointer from find_message_n_index

    if (!read_messages_from_p_in_sector(n, pos, out_count, &curMSector.var, out))
    {
        return STRG_FAIL;
    }

    // set prev to the next message sector
    prev_index = curMSector.var.header.prev;

    // stop if the message count meets n OR number of sectors read has reached the cap
    while ((*out_count < n) )
    {

        // if the previous index is the end of the chat, return
        if (prev_index == UINT16_MAX)
        {
            return STRG_OK;
        }

        if (hops >= TOTAL_MESSAGE_SECTOR_SIZE)
        {
            return STRG_FAIL;
        }

        current_index = prev_index;

        is_used = check_usage_bit(MESSAGE_DATA_SECTOR(current_index));
        if (!is_used)
        {
            return STRG_EMPTY;
        }

        ret = read_message_sector(storage, current_index, &curMSector);
        if (ret != STRG_OK)
        {
            return ret;
        }

        if (!read_messages_from_sector(n, out_count, &(curMSector.var), out))
        {
            return STRG_FAIL;
        }

        prev_index = curMSector.var.header.prev;
        hops++;

    }

    return STRG_OK;
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

STRG_RET remove_message_bit(Storage *storage, Journal *journal, uint16_t index)
{
    STRG_RET ret;

    // Add contact to jounral
    if (!journal_add(journal, JRNL_REMOVE, MESSAGE_DATA_SECTOR(index), NULL))
    {
        return STRG_FAIL;
    }

    // Update usage bit vector to state the sector is no longer allocated
    ret = update_usage_bit(storage, MESSAGE_DATA_SECTOR(index), false);

    if (ret != STRG_OK)
    {
        return ret;
    }

    // free active journal now the changes have been made
    return journal_free(journal);
}

/**
 * @brief If a contact is empty, remove the entire message chat
 *
 * @param journal journal struct pointer (allocated in database struct)
 * @param storage storage abstraction struct pointer (allocated in database struct)
 *
 * @retval true Journal Init successful
 * @retval false journal init fail
 */
STRG_RET remove_orphaned_message_chat(Storage *storage, Journal *journal, FreeList *msg_allocator, uint16_t startIndex)
{
    MessageSectorBuffer mSector;
    uint16_t curr;
    uint16_t prev = startIndex;
    int jumps = 0;
    STRG_RET ret;

    // Iterate until there are no more message sectors
    while (prev != UINT16_MAX)
    {
        curr = prev;

        bool is_used = check_usage_bit(MESSAGE_DATA_SECTOR(curr));
        if (!is_used)
        {
            return STRG_EMPTY;
        }

        // Read the message sector
        ret = read_message_sector(storage, curr, &mSector);

        if (ret != STRG_OK)
        {
            return ret;
        }

        ret = remove_message_bit(storage, journal, curr);
        if (ret != STRG_OK)
        {
            return ret;
        }

        // if the current index is before the start index (hasn't been freed during reconstruction)
        if (curr <= startIndex)
        {
            free_list_free(msg_allocator, curr);

        }

        // Update current sector to previous sector
        prev = mSector.var.header.prev;

        // if number of jumps exceeds the total number of messages in sector (corrupted prev prevention)
        if (jumps++ >= TOTAL_MESSAGE_SECTOR_SIZE)

        {
            return STRG_FAIL;
        }
    }

    return STRG_OK;
}

MessageBuffer create_message(uint32_t timestamp, bool direction, char *str)
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

    newMsg.msg.timestamp_hi= (uint16_t)(timestamp >> 16);
    newMsg.msg.timestamp_lo= (uint16_t)(timestamp & 0xFFFF);
    newMsg.msg.direction = direction;
    memcpy(newMsg.msg.str, str, len);


    return newMsg;
}

void create_new_message_sector(MessageSectorBuffer *mSector, uint16_t contact_index, uint16_t prev)
{
    // start from a clean sector so no stack garbage is persisted
    memset(mSector, 0, sizeof(*mSector));

    mSector->var.header.prev = prev;
    mSector->var.header.next = UINT16_MAX;
    mSector->var.header.msg_count = 0;
    mSector->var.header.contact_index = contact_index;
}
