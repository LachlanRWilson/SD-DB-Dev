#ifndef DATABASE_H
#define DATABASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "storage.h"
#include "journal.h"
#include "contact.h"
#include "message.h"
#include "hash_table.h"
#include "sorted_array.h"
#include "free_list_stack.h"
#include "ring_buffer.h"
#include "message_history.h"
#include "call_history.h"
#include "mem_layout.h"

typedef struct
{
    Storage storage;
    Journal journal;
    HashTable htable;
    FreeList contact_allocator;
    FreeList message_allocator;
    RingBuffer message_history;
    RingBuffer call_history;
    SortedArray name_index;
} Database;


/**
 * @brief Initialise the storage and mount the database. If the superheader is
 * uninitialised the storage is formatted, otherwise the journal is recovered and
 * the in RAM structures are rebuilt from storage.
 *
 * @param db database struct pointer
 * @retval STRG_OK database mounted or formatted
 * @retval STRG_CORRUPT superheader corrupted or outdated (call database_format to wipe)
 * @retval STRG_FAIL storage or rebuild failure
 */
STRG_RET database_init(Database *db);

/**
 * @brief Format the storage to an empty database. Writes the journal, usage
 * bitmap, empty ring buffers and then the superheader.
 *
 * @param db database struct pointer (storage must be initialised)
 * @retval STRG_RET
 */
STRG_RET database_format(Database *db);

/**
 * @brief Add a new contact to the hash table, storage and name index
 *
 * @param db database struct pointer
 * @param name contact name
 * @param phone contact phone number
 * @retval STRG_OK if added, STRG_EXISTS if phone number already exists, else STRG_* error code
 */
STRG_RET database_contact_add(Database *db, const char *name, const char *phone);

/**
 * @brief Find a contact by phone number
 *
 * @param db database struct pointer
 * @param phone contact phone number
 * @param out found contact
 * @retval STRG_OK if found, STRG_EMPTY if not found, else STRG_* error code
 */
STRG_RET database_contact_get(Database *db, const char *phone, ContactBuffer *out);

/**
 * @brief Update the name and/or phone number of a contact
 *
 * @param db database struct pointer
 * @param phone current contact phone number
 * @param new_name new contact name
 * @param new_phone new contact phone number
 * @retval STRG_OK if updated, STRG_EMPTY if not found, STRG_EXISTS if new phone already exists
 */
STRG_RET database_contact_update(Database *db, const char *phone, const char *new_name, const char *new_phone);

/**
 * @brief Remove a contact and its message chat
 *
 * @param db database struct pointer
 * @param phone contact phone number
 * @retval STRG_OK if removed, STRG_EMPTY if not found, else STRG_* error code
 */
STRG_RET database_contact_remove(Database *db, const char *phone);

/**
 * @brief Get a range of contacts in alphabetical order
 *
 * @param db database struct pointer
 * @param start rank of the first contact
 * @param n max number of contacts to read
 * @param out_count number of contacts read
 * @param out contact array (at least n long)
 * @retval STRG_OK if read, STRG_EMPTY if start is past the last contact, else STRG_* error code
 */
STRG_RET database_contact_list(Database *db, size_t start, size_t n, size_t *out_count, ContactBuffer *out);

/**
 * @brief Find the rank of the first contact whose name is >= prefix. Use with
 * database_contact_list for search
 *
 * @param db database struct pointer
 * @param prefix name prefix
 * @param rank rank of the first contact >= prefix
 * @retval STRG_RET
 */
STRG_RET database_contact_seek(Database *db, const char *prefix, size_t *rank);

/**
 * @brief Get the number of contacts in the database
 *
 * @param db database struct pointer
 * @retval number of contacts
 */
size_t database_contact_count(Database *db);

/**
 * @brief Add a message to a contacts chat and the message history. If the
 * phone number has no contact an unnamed contact is created
 *
 * @param db database struct pointer
 * @param phone phone number the message is associated with
 * @param timestamp message timestamp (must not be 0)
 * @param direction sending or receiving
 * @param str message string
 * @retval STRG_RET
 */
STRG_RET database_message_add(Database *db, const char *phone, uint32_t timestamp, bool direction, char *str);

/**
 * @brief Read a range of messages from a contacts chat, from latest backwards
 *
 * @param db database struct pointer
 * @param phone phone number the chat is associated with
 * @param start start message (0 = latest)
 * @param n max number of messages to read
 * @param out_count number of messages read
 * @param out message array (at least n long)
 * @retval STRG_OK if read, STRG_EMPTY if no contact or chat, else STRG_* error code
 */
STRG_RET database_message_get_chat(Database *db, const char *phone, size_t start, size_t n, size_t *out_count,
        Message *out);

/**
 * @brief Remove a contacts entire message chat. The contact is kept
 *
 * @param db database struct pointer
 * @param phone phone number the chat is associated with
 * @retval STRG_OK if removed, STRG_EMPTY if no contact or chat, else STRG_* error code
 */
STRG_RET database_message_remove_chat(Database *db, const char *phone);

/**
 * @brief Get a range of the latest messages across all chats
 *
 * @param db database struct pointer
 * @param start start message (0 = latest)
 * @param n max number of messages to read
 * @param out_count number of messages read
 * @param out message history array (at least n long)
 * @retval STRG_RET
 */
STRG_RET database_message_recent(Database *db, size_t start, size_t n, size_t *out_count, MessageHistoryEntry *out);

/**
 * @brief Add a call to the call history. If the phone number has no contact
 * the entry is stored with only the phone number
 *
 * @param db database struct pointer
 * @param phone phone number of the call
 * @retval STRG_RET
 */
STRG_RET database_call_add(Database *db, const char *phone);

/**
 * @brief Get a range of the latest calls
 *
 * @param db database struct pointer
 * @param start start call (0 = latest)
 * @param n max number of calls to read
 * @param out_count number of calls read
 * @param out call history array (at least n long)
 * @retval STRG_RET
 */
STRG_RET database_call_recent(Database *db, size_t start, size_t n, size_t *out_count, CallHistoryEntry *out);


#ifdef __cplusplus
}
#endif

#endif /* DATABASE_H */
