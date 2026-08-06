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
    Storage contact_storage;

    /** Contact allocator. */
    FreeList contact_allocator;

    /** Message extent allocator. */
    FreeList message_allocator;

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
bool database_init(Database *db, Storage storage, HashEntry *entries, uint16_t *contact_fls_mem,
        uint16_t *message_fls_mem);

/**
 * @brief Create a new contact.
 *
 * @param db Database.
 * @param contact Contact information.
 *
 * @return Assigned contact ID, or INVALID_ID.
 */
bool database_contact_create(Database *db, ContactBuffer *contact);

/**
 * @brief Find a contact.
 */
bool database_contact_get(Database *db, uint16_t id, ContactBuffer *out);

/**
 * @brief Update an existing contact.
 */
bool database_contact_update(Database *db, uint16_t id, ContactBuffer *contact);

/**
 * @brief Read latest message block from contact message
 */
bool database_message_read_latest(Database *db, uint16_t id, MessageBlock messageBlock);

/**
 * @brief Append a message to a contact.
 */
bool database_message_append(Database *db, uint16_t contact_id, const Message *message);

/**
 * @brief Count messages belonging to a contact.
 */
uint16_t database_message_count(Database *db, uint16_t contact_id);

/**
 * @brief Delete all messages belonging to a contact.
 */
bool database_message_delete(Database *db, uint16_t contact_id);

/**
 * @brief Reset the database to an empty state.
 *
 * Frees all allocated contacts and message extents and reinitialises
 * the internal data structures.
 *
 * @param[in,out] db Database instance.
 */
void database_clear(Database *db);

/**
 * @brief Flush pending changes to storage.
 */
bool database_sync(Database *db);

#ifdef __cplusplus
}
#endif

#endif /* DATABASE_H */
