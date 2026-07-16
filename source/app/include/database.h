#ifndef DATABASE_H
#define DATABASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "hash_table.h"
#include "message_extent.h"
#include "free_list_stack.h"
#include "storage.h"

/**
 * @brief Top-level database object.
 *
 * Owns all state required for the contact database, including:
 * - Contact hash table
 * - Message extent manager
 * - Storage backend
 * - Free-list allocators
 *
 * The database is responsible for coordinating interactions between
 * contacts and their associated message chains.
 */
typedef struct
{
    /** Contact hash table. */
    HashTable contacts;

    /** Message extent manager. */
    MessageExtent messages;

    /** Storage backend (SD card or heap). */
    Storage *storage;

    /** Contact allocator. */
    FreeList contact_allocator;

    /** Message extent allocator. */
    FreeList extent_allocator;

} Database;

/**
 * @brief Initialise the database.
 *
 * Initialises the contact hash table, message extent manager,
 * storage backend and allocators.
 *
 * @param[out] db Database instance.
 * @param[in] storage Storage backend.
 * @param[in] contact_entries Hash table backing array.
 * @param[in] contact_stack Memory for the contact free-list.
 * @param[in] contact_capacity Number of contacts.
 * @param[in] extent_stack Memory for the message extent free-list.
 * @param[in] extent_capacity Number of message extents.
 *
 * @retval true Initialisation successful.
 * @retval false Initialisation failed.
 */
bool database_init(Database *db, Storage *storage, HashEntry *contact_entries, uint32_t
        *contact_stack, uint32_t contact_capacity, uint32_t *extent_stack, uint32_t
        extent_capacity);

/**
 * @brief Reset the database to an empty state.
 *
 * Frees all allocated contacts and message extents and reinitialises
 * the internal data structures.
 *
 * @param[in,out] db Database instance.
 */
void database_clear(Database *db);

#ifdef __cplusplus
}
#endif

#endif /* DATABASE_H */
