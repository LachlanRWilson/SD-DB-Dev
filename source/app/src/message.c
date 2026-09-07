#include "message.h"

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



