#include "message_extent.h"
#include <string.h>
#include <stdlib.h>

#define INVALID_EXTENT UINT16_MAX

/**
 * Initialise the message extent manager.
 */
bool message_extent_init(MessageExtent *self, MessageStorage *storage, FreeList *free_list, uint32_t
        total_extents)
{
    if (self == NULL || storage == NULL || free_list == NULL || total_extents == 0)
    {
        return false;
    }

    self->storage = storage;
    self->free_stack = free_list;
    self->total_extents = total_extents;
    self->bottom_extent = total_extents;
    self->capacity = 0;

    return true;
}

/**
 * Allocate a new message extent.
 */
uint32_t message_extent_get(MessageExtent *self, uint32_t prev_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return INVALID_EXTENT;
    }

    uint32_t idx = free_list_allocate(self->free_stack);

    if (idx == INVALID_EXTENT)
    {
        return INVALID_EXTENT;
    }

    if (idx < self->bottom_extent)
    {
        self->bottom_extent = idx;
    }

    self->capacity++;

    MessageBlock block = {0};

    block.header = (MessageBlockHeader){
        .prev = prev_extent,
        .msg_count = 0,
        .state = EXTENT_EMPTY,
        .padding = 0
    };

    // Initialise message block
    self->storage->write_block(self->storage->context, idx, &block);

    return idx;
}

/**
 * Delete an entire message chain.
 */
bool message_extent_delete(MessageExtent *self, uint32_t last_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return false;
    }

    uint32_t current = last_extent;
    MessageBlock block;

    while (current != INVALID_EXTENT)
    {
        // get message block from storage
        if (!self->storage->read_block(self->storage->context, current, &block))
        {
            return false;
        }

        // get previous extent
        uint32_t prev = block.header.prev;

        // push current extent back onto the free list stack
        free_list_free(self->free_stack, current);

        // move to previous extent and decrease capacity
        current = prev;
        self->capacity--;
    }

    return true;
}

/**
 * Append a message to a conversation.
 */
bool message_extent_append(MessageExtent *self, uint32_t *last_extent, const Message *message)
{
    if (self == NULL || self->storage == NULL || last_extent == NULL || message == NULL)
    {
        return false;
    }

    MessageBlock block;

    if (!self->storage->read_block(self->storage->context, *last_extent, &block))
    {
        return false;
    }

    // If message block is not full
    if (block.header.msg_count < MESSAGE_BLOCK_CAPACITY)
    {
        block.messages[block.header.msg_count++] = *message;

        return self->storage->write_block(self->storage->context, *last_extent, &block);
    }

    // message block is full therefore need to create a new extent
    uint32_t new_idx = message_extent_get(self, *last_extent);

    if (new_idx == INVALID_EXTENT)
    {
        return false;
    }

    *last_extent = new_idx;
    // TEMP FIX
    message_extent_append(self, last_extent, message);

    return true;
}

/**
 * Count messages in a conversation chain.
 */
uint32_t message_extent_count(MessageExtent *self, uint32_t last_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return 0;
    }

    uint32_t count = 0;
    uint32_t current = last_extent;
    MessageBlock block;

    while (current != INVALID_EXTENT)
    {
        if (!self->storage->read_block(self->storage->context, current, &block))
        {
            break;
        }

        count += block.header.msg_count;
        current = block.header.prev;
    }

    return count;
}

/**
 * Reset extent manager state.
 */
void message_extent_reset(MessageExtent *self)
{
    if (self == NULL)
    {
        return;
    }

    self->capacity = 0;
    self->bottom_extent = self->total_extents;
}
