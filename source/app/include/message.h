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

#define MESSAGE_SECTOR_SIZE 2 * HASH_TABLE_SIZE

#define SMS_MAX_MESSAGE_LENGTH 160
#define MAX_PHONE_LEN 15
#define MESSAGE_BYTES 164
#define MESSAGE_BLOCK_HEADER_BYTES 24
#define MESSAGE_BLOCK_BYTES SECTOR_SIZE

// Fit messages into a 512B block of memory
#define MESSAGE_BLOCK_CAPACITY \
    ((MESSAGE_BLOCK_BYTES - sizeof(MessageBlockHeader) - sizeof(SECTOR_TYPE) - sizeof(uint8_t)) / sizeof(Message))

// Ensure padding is accounted for
#define MESSAGE_BLOCK_PADDING \
    MESSAGE_BLOCK_BYTES - sizeof(MessageBlockHeader) - sizeof(SECTOR_TYPE) - sizeof(uint8_t) - sizeof(Message) * MESSAGE_BLOCK_CAPACITY

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


// Message Struct (24B)
typedef struct
{
    uint16_t next; // Next Extent (2B)
    uint16_t prev; // Previous Extent (2B)
    uint16_t msg_count; // Number of messages in the block (2B)
    char phone[MAX_PHONE_LEN]; // Phone number (15B)
    uint8_t phone_len;         // Length of phone number (1B)
    EXTENT_STATE state; // Extent State (1B) (This can be removed)
    uint8_t padding;
} MessageBlockHeader;


// Message Sector (512B)
typedef struct
{
    SECTOR_TYPE type; // (1B)
    uint8_t reserved; // (1B) for alignment
    MessageBlockHeader header; // Header (24B)
    Message messages[MESSAGE_BLOCK_CAPACITY]; // Array of chats
    uint8_t padding[MESSAGE_BLOCK_PADDING];
} MessageSector;

typedef union
{
    MessageSector var;
    uint8_t buffer[sizeof(MessageSector)];
} MessageSectorBuffer;


// Static checks to ensure the size of the struct are correct if they are changed
STATIC_ASSERT(MESSAGE_BLOCK_PADDING > 0, "0 or negative padding (remove padding from struct)");
STATIC_ASSERT(sizeof(Message) == MESSAGE_BYTES, "Unexpected MessageBlock size");
STATIC_ASSERT(sizeof(MessageBlockHeader) == MESSAGE_BLOCK_HEADER_BYTES, "Unexpected \
        MessageBlockHeader size");
STATIC_ASSERT(sizeof(MessageSector) == SECTOR_SIZE, "Unexpected MessageSector size");

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
bool read_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out);

/**
 * @brief Write message to the sd card
 *
 * @param storage Pointer to the storage abstraction.
 * @param index memory index of the contact.
 * @param in sector being written to the SD Card
 * @retval True if successful write else false.
 */
bool write_message_sector(Storage *storage, uint16_t index, MessageSectorBuffer *out);

/**
 * @brief Write the contact to the sd card in the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param journal pointer to rollback journal struct
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool write_message(Storage *storage, Journal *journal, uint16_t index, Message *in);

/**
 * @brief Read the contact to the sd card from the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool read_message(Storage *storage, uint16_t index, Message *out);

/**
 * @brief Remove the contact to the sd card from the appropriate contact sector
 *
 * @param table Pointer to the hash table.
 * @param index memory index of the contact.
 * @param in Contact Sector Buffer going into the SD card
 * @retval True if successful write else false.
 */
bool remove_message_chat(Storage *storage, Journal *journal, uint16_t index, Message *out);

#ifdef __cplusplus
}
#endif

#endif
