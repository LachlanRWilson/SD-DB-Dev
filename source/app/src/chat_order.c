#include <string.h>
#include "chat_order.h"
#include "hash_table.h"
#include "message.h"

// Chat order rebuild scratch entry (6B)
typedef struct
{
    uint16_t timestamp_hi;  // latest message timestamp (as Message)
    uint16_t timestamp_lo;
    uint16_t hash_slot;
} ChatOrderScratch;

STATIC_ASSERT(sizeof(ChatOrderScratch) == CHAT_ORDER_SCRATCH_ENTRY_BYTES, "Unknown Size for Chat Order Scratch Entry");



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

/**
 * @brief Compare two chat order rebuild entries: latest message first, then
 * hash slot so the order is the same every boot
 *
 * @param a first entry
 * @param b second entry
 * @retval < 0 if a comes before b, > 0 if a comes after b, 0 if equal
 */
static int chat_scratch_cmp(const ChatOrderScratch *a, const ChatOrderScratch *b)
{
    uint32_t ta = ((uint32_t)a->timestamp_hi << 16) | a->timestamp_lo;
    uint32_t tb = ((uint32_t)b->timestamp_hi << 16) | b->timestamp_lo;

    if (ta != tb)
    {
        return (ta > tb) ? -1 : 1;
    }
    return (int)a->hash_slot - (int)b->hash_slot;
}

/**
 * @brief Swap two chat order rebuild entries
 *
 * @param a first entry
 * @param b second entry
 */
static void chat_scratch_swap(ChatOrderScratch *a, ChatOrderScratch *b)
{
    ChatOrderScratch temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * @brief Move the entry at root down the max heap [0, end) until both of its
 * children are smaller than it
 *
 * @param scratch chat order rebuild entries
 * @param root index of the entry being moved down
 * @param end number of entries in the heap
 */
static void chat_scratch_sift_down(ChatOrderScratch *scratch, size_t root, size_t end)
{
    size_t child;

    while ((child = (2 * root) + 1) < end)
    {
        // pick the larger of the two children
        if (((child + 1) < end) && (chat_scratch_cmp(&scratch[child], &scratch[child + 1]) < 0))
        {
            child++;
        }

        // heap property holds once root is not smaller than its larger child
        if (chat_scratch_cmp(&scratch[root], &scratch[child]) >= 0)
        {
            break;
        }

        chat_scratch_swap(&scratch[root], &scratch[child]);
        root = child;
    }
}

/**
 * @brief Sort the chat order rebuild entries in place using heapsort (same as
 * sorted_array_sort). No recursion and no extra memory
 *
 * @param scratch chat order rebuild entries
 * @param n number of entries
 */
static void chat_scratch_sort(ChatOrderScratch *scratch, size_t n)
{
    if (n < 2)
    {
        return;
    }

    // build a max heap, starting from the last entry that has a child
    for (size_t i = n / 2; i-- > 0;)
    {
        chat_scratch_sift_down(scratch, i, n);
    }

    // move the largest entry to the end of the array, then repair the heap
    for (size_t end = n - 1; end > 0; end--)
    {
        chat_scratch_swap(&scratch[0], &scratch[end]);
        chat_scratch_sift_down(scratch, 0, end);
    }
}

/**
 * @brief Initialise and rebuild the chat order from the hash table. The latest message of every
 * chat is read, then the chats are sorted by its timestamp
 *
 * @param co chat order struct
 * @param htable_mem hash table memory
 * @param storage storage struct
 * @param co_memory pre-allocated RAM_D1 memory pointer (CHAT_ORDER_ENTRIES long)
 * @param scratch_memory scratch memory (at least CHAT_ORDER_SCRATCH_BYTES), free until the build returns
 * @retval STRG_RET
 */
STRG_RET chat_order_build(ChatOrder *co, HashEntry *htable_mem, Storage *storage, uint16_t *co_memory, void *scratch_memory)
{
    ChatOrderScratch *scratch = (ChatOrderScratch *)scratch_memory;
    size_t chats = 0;
    Message latest;
    size_t count;
    STRG_RET ret;

    if (!chat_order_init(co, co_memory))
    {
        return STRG_FAIL;
    }

    for (uint16_t slot = 0; slot < HASH_TABLE_SIZE; slot++)
    {
        if ((htable_mem[slot].state != ENTRY_OCCUPIED) || (htable_mem[slot].latest_msg_extent == UINT16_MAX))
        {
            continue;
        }

        if (chats >= CHAT_ORDER_ENTRIES)
        {
            return STRG_FULL;
        }

        ret = read_n_messages(storage, htable_mem[slot].latest_msg_extent, 0, 1, &count, &latest);
        if (ret != STRG_OK)
        {
            return ret;
        }

        scratch[chats].timestamp_hi = latest.timestamp_hi;
        scratch[chats].timestamp_lo = latest.timestamp_lo;
        scratch[chats].hash_slot = slot;
        chats++;
    }

    chat_scratch_sort(scratch, chats);

    for (size_t i = 0; i < chats; i++)
    {
        chat_order_append(co, scratch[i].hash_slot);
    }

    return STRG_OK;
}
