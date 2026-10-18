#ifndef CHAT_ORDER_H
#define CHAT_ORDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem_layout.h"
#include "storage.h"

// At most one chat per contact
#define CHAT_ORDER_ENTRIES HASH_TABLE_ENTRIES

// Chat Order: hash slots of every contact with a message chat, most recent message first
typedef struct {
    uint16_t occupancy;     // number of chats in the chat order
    uint16_t *mem;          // pointer to array memory (in RAM), each entry is a hash slot
} ChatOrder;

/**
 * @brief Find the rank of a hash slot in the chat order
 *
 * @param co chat order struct
 * @param hash_slot hash slot being searched for
 * @param rank pointer to rank of the hash slot (0 = most recent)
 * @retval true if found
 */
bool chat_order_find(ChatOrder *co, uint16_t hash_slot, size_t *rank);

/**
 * @brief Move a chat to the front of the chat order (most recent). If the hash
 * slot is not in the chat order it is inserted at the front
 *
 * @param co chat order struct
 * @param hash_slot hash slot of the chat with a new message
 * @retval STRG_OK if moved or inserted, STRG_FULL if a new chat doesn't fit
 */
STRG_RET chat_order_touch(ChatOrder *co, uint16_t hash_slot);

/**
 * @brief Remove a chat from the chat order, move all the memory greater than
 * its rank back one
 *
 * @param co chat order struct
 * @param hash_slot hash slot of the removed chat
 * @retval STRG_OK if removed, STRG_EMPTY if the hash slot has no chat
 */
STRG_RET chat_order_remove(ChatOrder *co, uint16_t hash_slot);

/**
 * @brief Replace the hash slot of a chat, keeping its rank. Used when a phone
 * number change moves the contact to a new hash slot
 *
 * @param co chat order struct
 * @param old_slot previous hash slot
 * @param new_slot new hash slot
 * @retval STRG_OK if replaced, STRG_EMPTY if the old hash slot has no chat
 */
STRG_RET chat_order_replace(ChatOrder *co, uint16_t old_slot, uint16_t new_slot);

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
bool chat_order_get_range(ChatOrder *co, size_t start, size_t n, size_t *out_count, uint16_t *out);

/**
 * @brief Append a hash slot to the end of the chat order (least recent), this is
 * only done on start up after the chats have been sorted by latest message
 *
 * @param co chat order struct
 * @param hash_slot hash slot being added
 * @retval false if the chat order is full
 */
bool chat_order_append(ChatOrder *co, uint16_t hash_slot);

/**
 * @brief Initialise an empty chat order structure
 *
 * @param co chat order struct
 * @param co_memory pre-allocated RAM_D1 memory pointer (CHAT_ORDER_ENTRIES long)
 * @retval false if a pointer is NULL
 */
bool chat_order_init(ChatOrder *co, uint16_t *co_memory);


#ifdef __cplusplus

}
#endif

#endif
