#ifndef MESSAGE_H
#define MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
    #define STATIC_ASSERT static_assert
#else
    #define STATIC_ASSERT _Static_assert
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "mem_layout.h"
#include "storage.h"


// Ensure enum is 1 byte
typedef uint8_t EXTENT_STATE;

enum
{
    EXTENT_EMPTY = 0,
    EXTENT_OCCUPIED,
    EXTENT_TOMBSTONED
};

// Message struct (164B)
typedef struct {
    uint16_t timestamp; // Time of message (2B)
    bool direction; // Sending or receiving (1B)
    char str [SMS_MAX_MESSAGE_LENGTH]; // Message (160B)
    uint8_t padding; // 1B
} Message;


typedef union {
    Message msg;
    uint8_t buffer[sizeof(Message)];
} MessageBuffer;


// Message Struct (24B)
typedef struct
{
    uint16_t next;              // Next Extent (2B)
    uint16_t prev;              // Previous Extent (2B)
    uint16_t msg_count;         // Number of messages in the block (2B)
    char phone[MAX_PHONE_LEN];  // Phone number (15B)
    uint8_t phone_len;          // Length of phone number (1B)
    EXTENT_STATE state;         // Extent State (1B) (This can be removed)
    uint8_t padding;
} MessageSectorHeader;


// Message Sector (512B)
typedef struct
{
    MessageSectorHeader header; // Header (24B)
    MessageBuffer messages[MESSAGE_SECTOR_CAPACITY]; // Array of chats
#if MESSAGE_SECTOR_PADDING != 0
    uint8_t padding[MESSAGE_SECTOR_PADDING];
#endif
    uint32_t crc; // CRC-32 trailer, stamped by write_sector() (4B)
} MessageSector;

typedef union
{
    MessageSector var;
    uint8_t buffer[sizeof(MessageSector)];
} MessageSectorBuffer;



STATIC_ASSERT(sizeof(Message) == MESSAGE_BYTES, "Unexpected Message size");

STATIC_ASSERT(sizeof(MessageSectorHeader) == MESSAGE_SECTOR_HEADER_BYTES, "Unexpected \
        MessageSectorHeader size");

STATIC_ASSERT(sizeof(MessageSector) == SECTOR_SIZE, "Unexpected MessageSector size");

STATIC_ASSERT(offsetof(MessageSector, crc) == SECTOR_PAYLOAD_BYTES, "MessageSector CRC is not the sector trailer");

STATIC_ASSERT(MESSAGE_SECTOR_CAPACITY == 2, "CRC trailer changed the message sector capacity");

// Opaque Declaration of FreeList
typedef struct FreeList FreeList;
typedef struct Journal Journal;

typedef struct
{
    Storage *storage;
    FreeList *free_stack; // Free List Stack

    uint16_t bottom_extent; // furthest current extent from top of stack
    uint16_t total_extents; // total number of extents allocated
    uint16_t num_extents; // number of used extents
} MessageExtent;


/**
 * @brief Read message sector to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index sector index on the SD Card
 * @param in sector being read from the SD Card
 * @retval True if successful read else false.
 */
STRG_RET read_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out);

/**
 * @brief Write message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the message.
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
STRG_RET write_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out);


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
STRG_RET write_new_message_sector(Storage *storage, Journal *journal, const char* phone, uint16_t index, MessageBuffer *in);

/**
 * @brief Write the message to the sd card in the appropriate message sector
 *
 * @param table Pointer to the hash table.
 * @param journal pointer to rollback journal struct
 * @param index memory index of the message.
 * @param in message Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
STRG_RET write_message(Storage *storage, Journal *journal, uint16_t index, MessageBuffer *in);

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
        MessageBuffer *in);


/**
 * @brief Read the message to the sd card from the appropriate message sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the message.
 * @param in message Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
STRG_RET read_message(Storage *storage, uint16_t index, uint8_t pos, MessageBuffer *out);

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
STRG_RET read_n_messages(Storage *storage, uint16_t startIndex, size_t startMsg, size_t n, size_t *out_count, Message *out);

/**
 * @brief Remove the message to the sd card from the appropriate message sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the message.
 * @param out message Sector Buffer that has been deleted from the chat
 * @retval True if successful write else false.
 */
bool remove_message_chat(Storage *storage, Journal *journal, FreeList *msg_allocator, uint16_t index);

/**
 * @brief Add a new message to a chat. Shall be appended to the end of the chat
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the message.
 * @param in message being appended to the end of the chat
 * @retval True if message has successfully been added, else false
 */
bool add_message(Storage *storage, Journal *journal, uint16_t, MessageBuffer *in);

/**
 * @brief Remove a message from the chat. Shall remove the last message from the chat
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the message.
 * @param in message being appended to the end of the chat
 * @retval True if message has successfully been added, else false
 */
bool remove_message(Storage *storage, Journal *journal, uint16_t, MessageBuffer *out);


/**
 * @brief Create a new message struct
 *
 * @retval Empty MessageBuffer if failure
 */
MessageBuffer create_message(uint16_t timestamp, bool direction, char *str);

/**
 * @brief Create a new message sector struct
 */
void create_new_message_sector(MessageSectorBuffer *mSector, const char *phone, uint16_t prev);







#ifdef __cplusplus
}
#endif

#endif
