#include <string.h>
#include "chat_order.h"



/**
 * @brief Find the rank of a hash slot in the chat order. Linear search, the
 * chat order is in recency order not slot order
 *
 * @param co chat order struct
 * @param hash_slot hash slot being searched for
 * @param rank pointer to rank of the hash slot (0 = most recent)
 * @retval true if found
 */
bool chat_order_find(ChatOrder *co, uint16_t hash_slot, size_t *rank)
{
    if ((co == NULL) || (rank == NULL))
    {
        return false;
    }

    for (size_t i = 0; i < co->occupancy; i++)
    {
        if (co->mem[i] == hash_slot)
        {
            *rank = i;
            return true;
        }
    }

    return false;
}

/**
 * @brief Move a chat to the front of the chat order (most recent). If the hash
 * slot is not in the chat order it is inserted at the front
 *
 * @param co chat order struct
 * @param hash_slot hash slot of the chat with a new message
 * @retval STRG_OK if moved or inserted, STRG_FULL if a new chat doesn't fit
 */
STRG_RET chat_order_touch(ChatOrder *co, uint16_t hash_slot)
{
    size_t rank;

    if (co == NULL)
    {
        return STRG_FAIL;
    }

    if (!chat_order_find(co, hash_slot, &rank))
    {
        if (co->occupancy >= CHAT_ORDER_ENTRIES)
        {
            return STRG_FULL;
        }

        // new chat: shift every chat back by one
        rank = co->occupancy;
        co->occupancy++;
    }

    // move to front: shift [0, rank) right by one
    memmove(&co->mem[1], &co->mem[0], rank * sizeof(uint16_t));
    co->mem[0] = hash_slot;

    return STRG_OK;
}

/**
 * @brief Remove a chat from the chat order, move all the memory greater than
 * its rank back one
 *
 * @param co chat order struct
 * @param hash_slot hash slot of the removed chat
 * @retval STRG_OK if removed, STRG_EMPTY if the hash slot has no chat
 */
STRG_RET chat_order_remove(ChatOrder *co, uint16_t hash_slot)
{
    size_t rank;

    if (co == NULL)
    {
        return STRG_FAIL;
    }

    // if the hash slot has no chat do not delete anything
    if (!chat_order_find(co, hash_slot, &rank))
    {
        return STRG_EMPTY;
    }

    // remove at rank: shift (rank, occupancy) left by one
    memmove(&co->mem[rank], &co->mem[rank + 1], (co->occupancy - rank - 1) * sizeof(uint16_t));

    co->occupancy--;
    return STRG_OK;
}

/**
 * @brief Replace the hash slot of a chat, keeping its rank. Used when a phone
 * number change moves the contact to a new hash slot
 *
 * @param co chat order struct
 * @param old_slot previous hash slot
 * @param new_slot new hash slot
 * @retval STRG_OK if replaced, STRG_EMPTY if the old hash slot has no chat
 */
STRG_RET chat_order_replace(ChatOrder *co, uint16_t old_slot, uint16_t new_slot)
{
    size_t rank;

    if (co == NULL)
    {
        return STRG_FAIL;
    }

    if (!chat_order_find(co, old_slot, &rank))
    {
        return STRG_EMPTY;
    }

    co->mem[rank] = new_slot;
    return STRG_OK;
}

/**
 * @brief Get a range of hash slots from the chat order
 *
 * @param co chat order struct
 * @param start rank of the first chat
 * @param n max number of hash slots to read
 * @param out_count number of hash slots read
 * @param out hash slot array (at least n long)
 * @retval false if start is past the last chat
 */
bool chat_order_get_range(ChatOrder *co, size_t start, size_t n, size_t *out_count, uint16_t *out)
{
    if ((co == NULL) || (out == NULL) || (out_count == NULL) || (start >= co->occupancy))
    {
        return false;
    }

    if (n > co->occupancy - start)
    {
        n = co->occupancy - start;
    }

    memcpy(out, &co->mem[start], n * sizeof(uint16_t));
    *out_count = n;
    return true;
}

/**
 * @brief Append a hash slot to the end of the chat order (least recent), this is
 * only done on start up after the chats have been sorted by latest message
 *
 * @param co chat order struct
 * @param hash_slot hash slot being added
 * @retval false if the chat order is full
 */
bool chat_order_append(ChatOrder *co, uint16_t hash_slot)
{
    if ((co == NULL) || (co->occupancy >= CHAT_ORDER_ENTRIES))
    {
        return false;
    }

    co->mem[co->occupancy] = hash_slot;
    co->occupancy++;
    return true;
}

/**
 * @brief Initialise an empty chat order structure
 *
 * @param co chat order struct
 * @param co_memory pre-allocated RAM_D1 memory pointer (CHAT_ORDER_ENTRIES long)
 * @retval false if a pointer is NULL
 */
bool chat_order_init(ChatOrder *co, uint16_t *co_memory)
{
    if ((co == NULL) || (co_memory == NULL))
    {
        return false;
    }
    co->mem = co_memory;
    co->occupancy = 0;

    return true;
}
