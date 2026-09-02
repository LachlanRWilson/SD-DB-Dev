#include "message_extent.h"
#include <string.h>
#include <stdlib.h>

#define INVALID_EXTENT UINT16_MAX

/**
 * Initialise the message extent manager.
 */
bool message_extent_init(MessageExtent *self, Storage *storage, FreeList *free_list, uint16_t
        total_extents)
{
    if (self == NULL || storage == NULL || free_list == NULL || total_extents == 0)
    {
        return false;
    }

    self->free_stack = free_list;
    self->storage = storage;
    self->total_extents = total_extents;
    self->bottom_extent = total_extents - 1;
    self->num_extents = 0;

    return true;
}

/**
 * Allocate a new message extent.
 */
uint16_t message_extent_get(MessageExtent *self, uint16_t prev_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return INVALID_EXTENT;
    }

    uint16_t idx = free_list_allocate(self->free_stack);

    if (idx == INVALID_EXTENT)
    {
        return INVALID_EXTENT;
    }

    if (idx < self->bottom_extent)
    {
        self->bottom_extent = idx;
    }

    self->num_extents++;

    MessageBlockBuffer block = {0};

    block.var.header = (MessageBlockHeader){
        .prev = prev_extent,
        .msg_count = 0,
        .state = EXTENT_EMPTY,
    };

    // Initialise message block
    self->storage->write_block(self->storage->context, idx, block.buffer);

    return idx;
}

/**
 * Delete an entire message chain.
 */
bool message_extent_delete(MessageExtent *self, uint16_t last_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return false;
    }

    uint16_t current = last_extent;
    MessageBlockBuffer block;

    while (current != INVALID_EXTENT)
    {
        // get message block from storage
        if (!self->storage->read_block(self->storage->context, current, block.buffer))
        {
            return false;
        }

        // get previous extent
        uint16_t prev = block.var.header.prev;

        // push current extent back onto the free list stack
        free_list_free(self->free_stack, current);

        // move to previous extent and decrease capacity
        current = prev;
        self->num_extents--;
    }

    return true;
}

/**
 * Append a message to a conversation.
 */
bool message_extent_append(MessageExtent *self, uint16_t *last_extent, const Message *message)
{
    if (self == NULL || self->storage == NULL || last_extent == NULL || message == NULL)
    {
        return false;
    }

    MessageBlockBuffer block;

    if (!self->storage->read_block(self->storage->context, *last_extent, block.buffer))
    {
        return false;
    }

    // If message block is not full
    if (block.var.header.msg_count < MESSAGE_BLOCK_CAPACITY)
    {
        block.var.messages[block.var.header.msg_count++] = *message;

        return self->storage->write_block(self->storage->context, *last_extent, block.buffer);
    }

    // message block is full therefore need to create a new extent
    uint16_t new_idx = message_extent_get(self, *last_extent);

    if (new_idx == INVALID_EXTENT)
    {
        return false;
    }

    *last_extent = new_idx;

    // Append message to new extent
    message_extent_append(self, last_extent, message);

    return true;
}

/**
 * Count messages in a conversation chain.
 */
uint16_t message_extent_count(MessageExtent *self, uint16_t last_extent)
{
    if (self == NULL || self->storage == NULL)
    {
        return 0;
    }

    uint16_t count = 0;
    uint16_t current = last_extent;

    // use message block union for reading
    MessageBlockBuffer block;

    while (current != INVALID_EXTENT)
    {
        if (!self->storage->read_block(self->storage->context, current, block.buffer))
        {
            break;
        }

        count += block.var.header.msg_count;
        current = block.var.header.prev;
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

    self->num_extents = 0;
    self->bottom_extent = self->total_extents;
}
