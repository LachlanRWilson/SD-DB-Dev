#ifndef MESSAGE_EXTENT_H
#define MESSAGE_EXTENT_H

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
#include "free_list_stack.h"

#define EXTENT_SIZE_BYTES 4096
#define SMS_MAX_MESSAGE_LENGTH 160

#define MESSAGE_BYTES 164
#define MESSAGE_BLOCK_HEADER_BYTES 8
#define MESSAGE_BLOCK_BYTES 4096

// Fit messages into a 4KB block of memory
#define MESSAGE_BLOCK_CAPACITY \
    ((EXTENT_SIZE_BYTES - sizeof(MessageBlockHeader)) / sizeof(Message))

// Ensure padding is accounted for
#define MESSAGE_BLOCK_PADDING \
    EXTENT_SIZE_BYTES - sizeof(MessageBlockHeader) - sizeof(Message) * MESSAGE_BLOCK_CAPACITY

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


// Message Struct (8B)
typedef struct
{
    uint32_t prev; // Previous Extent (4B)
    uint16_t msg_count; // Number of messages in the block (2B)
    EXTENT_STATE state; // Extent State (1B)
    uint8_t padding; // 1B

} MessageBlockHeader;


// Message Extent Block (4KB)
typedef struct
{
    MessageBlockHeader header; // Header
    Message messages[MESSAGE_BLOCK_CAPACITY]; // Array of chats
    uint8_t padding[MESSAGE_BLOCK_PADDING]; // Padding
} MessageBlock;

typedef union
{
    MessageBlock block;
    uint8_t bytes[sizeof(MessageBlock)];
} MessageBlockBuffer;


// Static checks to ensure the size of the struct are correct
STATIC_ASSERT(sizeof(Message) == MESSAGE_BYTES, "Unexpected MessageBlockHeader size");
STATIC_ASSERT(sizeof(MessageBlockHeader) == MESSAGE_BLOCK_HEADER_BYTES, "Unexpected MessageBlockHeader size");
STATIC_ASSERT(sizeof(MessageBlock) == MESSAGE_BLOCK_BYTES, "Unexpected MessageBlockHeader size");


// Storage Abstraction Struct
typedef struct 
{
    void * context;  // Storage context
    bool (*read_block)(void *context, uint32_t index, MessageBlock *out);
    bool (*write_block)(void *context, uint32_t index, const MessageBlock *in);
    uint32_t (*capacity)(void *context);
} MessageStorage;

typedef struct
{
    MessageStorage *storage;
    FreeList *free_stack; // Free List Stack

    uint32_t bottom_extent; // furthest current extent from top of stack
    uint32_t total_extents; // total number of extents allocated
    uint32_t capacity; // number of used extents
} MessageExtent;

/**
 * @brief Initialise the message extent manager.
 *
 * @param self Message extent manager.
 * @param free_list Free list allocator.
 * @param blocks Storage array.
 * @param total_extents Number of extents.
 */
bool message_extent_init( MessageExtent *self, MessageStorage *storage, FreeList *free_list, 
        uint32_t total_extents);

/**
 * @brief Allocate the first extent for a new conversation.
 *
 * @return Extent index or INVALID_EXTENT.
 */
uint32_t message_extent_get(MessageExtent *self, uint32_t prev_extent);

/**
 * @brief Delete an entire conversation.
 *
 * @param last_extent Last extent in the chain.
 */
bool message_extent_delete( MessageExtent *self, uint32_t last_extent);

/**
 * @brief Append a message to a conversation.
 *
 * @param self Extent manager.
 * @param last_extent Pointer to current last extent.
 *        Updated if a new extent is allocated.
 * @param message Message to append.
 *
 * @retval true Success.
 */
bool message_extent_append( MessageExtent *self, uint32_t *last_extent, const Message *message);

/**
 * @brief Read a message by logical index.
 *
 * @param last_extent Last extent.
 * @param index Logical message index.
 * @param out Output message.
 */
// bool message_extent_get(MessageExtent *self, uint32_t first_extent, uint32_t index, Message
//         *out);


/**
 * @brief Count messages in a conversation.
 */
uint32_t message_extent_count(MessageExtent *self, uint32_t last_extent);

/**
 * @brief Reset all extents.
 */
void message_extent_reset( MessageExtent *self);

#ifdef __cplusplus
}
#endif

#endif
