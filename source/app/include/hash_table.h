#ifndef HASH_TABLE_H
#define HASH_TABLE_H



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
#include "contact.h"
#include "message.h"

// Struct Sizes
#define HASH_ENTRY_BYTES 8

#define HASH_TABLE_SIZE 14293


// Opaque Declarations
typedef struct FreeList FreeList;
typedef struct Journal Journal;


// Entry State  (Pack enum to 1 byte)
typedef uint8_t ENTRY_STATE;
enum
{
    ENTRY_EMPTY = 0,
    ENTRY_OCCUPIED,
    ENTRY_DELETED
};

// Hash Entry that points to SD Card sector (8B)
typedef struct
{
    uint16_t id; // Contact ID (2B) PHONE NUMBER
    uint16_t sector; // SD Sector (2B)
    uint16_t latest_msg_extent; // Latest Message Extent offset (2B)
    ENTRY_STATE state;  // Entry occupation state (1B)
    uint8_t padding; // (1B)
} HashEntry;

// Static checks to ensure the size of the structs are correct
STATIC_ASSERT(sizeof(HashEntry) == HASH_ENTRY_BYTES, "Unexpected HashEntry size");


// Information Struct about hash table
typedef struct
{
    HashEntry *htable; // In RAM hash table (allocated in RAM D1)
    Storage *storage; // Pointer to storage struct (allocated in database struct)
    FreeList *contact_allocator; // Pointer to contat FLS struct (allocates in database struct)
    FreeList *message_allocator; // Pointer to message FLS struct (allocated in database struct)
    size_t num_elems; // amount of elements in table
    size_t size; // total space in table
#if defined (HOST_BUILD)
    size_t collision_count; // for benchmarking hash functions
#endif
} HashTable;


/**
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param storage:
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs)
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init(HashTable *table, Storage *storage, FreeList *contact_fstack, FreeList *message_fstack,
        HashEntry *entries, size_t size);

/**
  * @brief  Destroy a hash table and free all associated memory
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_destroy(HashTable *table);

/**
  * @brief  Create a new contact in the hash table in RAM
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval if insertion successful return sector index, else UINT16_MAX
  */
uint16_t hash_insert(HashTable *table, uint16_t id);

/**
 * @brief Insert a new contact into the hash table and on SD
 *
 * @param table Pointer to the hash table.
 * @param contact Contact to insert.
 * @retval true if insertion successful, otherwise false.
 */
bool hash_insert_contact(HashTable *table, Journal *journal, ContactBuffer *contact);

/**
  * @brief  Insert a contact into the hash table using phone number of PK
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval if insertion successful return sector index, else UINT16_MAX
  */
bool hash_insert_message(HashTable *table, Journal *journal, const char* phone, MessageBuffer *in);

/**
  * @brief  Find hash table entry to associated phone number
  * @param  table: Pointer to the hash table
  * @param  phone: phone number contact in being inserted with
  * @param  h1: phone number hash 1
  * @param  h2: phone number hash 2
  * @param entry: output entry from from the hash table
  * @retval True if entry found else false
  */
bool hash_find_entry(HashTable *table, const char *phone, HashEntry **entry);

/**
 * @brief Find a contact by its unique ID. This will pull the contact from the SD Card in one
 * go
 *
 * @param table Pointer to the hash table.
 * @param phone: phone number the contact is stored under
 * @param out Pointer to output Contact.
 * @retval true if contact found, otherwise false.
 */
bool hash_find_contact(HashTable *table, const char *phone, ContactBuffer *out);

/**
  * @brief  Find a contacts message
  * @param  table: Pointer to the hash table
  * @param phone: phone number the message is associated with
  * @retval Pointer to the matching message extent, or NULL if not found
  */
bool hash_find_message(HashTable *table, const char *phone, MessageBuffer *out);


/**
 * @brief Get the sector index of the nth contact in the hash table
 *
 * @param table Pointer to the hash table.
 * @param n number of contacts to be read
 * @retval contact index number
 * @retval UINT16_MAX if error.
 */
uint16_t get_nth_contact_index(HashTable *table, int n);

/**
 * @brief Get a contiguous list of contacts from the hash table.
 *
 * @param table Pointer to the hash table.
 * @param start start contact number
 * @param n number of contacts to be read
 * @param out Pointer to array of contacts (must hold at least n entries)
 * @retval STRG_OK if the read completed successfully.
 */
STRG_RET hash_get_contact_list(HashTable *table, int start, int n, ContactBuffer *out);

/**
  * @brief  Find n number of messages from a contact
  * @param  table: Pointer to the hash table
  * @param  phone: phone number the message is associated with
  * @retval the number of messages read from the sd card, -1 if fault
  */
int hash_find_n_message(HashTable *table, const char *phone, int n, MessageBuffer *out);

/**
  * @brief  Remove an entry from the hash table. That include removed the contact and message chat from the SD card
  * @param  table: Pointer to the hash table
  * @param  phone: phone number of entry that is being removed
  * @param removed: output, set to the removed entry (may be NULL if the caller doesn't need it)
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove(HashTable *table, Journal *journal, const char *phone, HashEntry **removed);

/**
 * @brief Remove a contact from the hash table.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to remove.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact(HashTable *table, Journal *journal, const char *phone, ContactBuffer *out);

/**
 * @brief Remove a messages from the hash table
 *
 * @param table Pointer to the hash table.
 * @param phone: phone number with which the message is being removed from
 * @param message_num: message number being removed from the message chat
 * @param out: pointer to message buffer which is filled with removed message buffer
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_message(HashTable *table, Journal *journal, const char *phone, MessageBuffer *out);


/**
  * @brief  Reconstruct the in-RAM hash table from persistent contact
  *         data after a restart, using the usage bitmap to avoid
  *         scanning unused sectors.
  * @param  table: Pointer to a freshly hash_init'd, empty hash table
  * @retval true if reconstruction completed successfully, false on a
  *         storage read failure or if the table filled up mid-reconstruction.
  */
bool hash_reconstruct_contact(HashTable *table);


/**
  * @brief  Reconstruct the in-RAM hash table from persistent message
  *         data after a restart, using the usage bitmap to avoid
  *         scanning unused sectors.
  * @param  table: Pointer to a freshly hash_init'd, empty hash table
  * @retval true if reconstruction completed successfully, false on a
  *         storage read failure or if the table filled up mid-reconstruction.
  */
bool hash_reconstruct_message(HashTable *table, Journal *journal);

/**
  * @brief  Reconstruct the in-RAM hash table from persistent
  *         data after a restart, using the usage bitmap to avoid
  *         scanning unused sectors.
  * @param  table: Pointer to a freshly hash_init'd, empty hash table
  * @retval true if reconstruction completed successfully, false on a
  *         storage read failure or if the table filled up mid-reconstruction.
  */
bool hash_reconstruct(HashTable *table);

/**
 * @brief  Reconstruct the in-RAM hash table from persistent contact data to cleanup tombstoned entries.
 *         This shall be done if the effective load factor reaches 75% OR the number of collission /  probing hops
 *         exceeds PROBING_HOP_MAX
 * @param  table: Pointer to a freshly hash table
 * @retval true if reconstruction completed successfully, false on a
 *         storage read failure or if the table filled up mid-reconstruction.
 */
bool hash_cleanup(HashTable* table);


/**
  * @brief  Get the number of contacts currently stored in the hash table
  * @param  table: Pointer to the hash table
  * @retval Number of contacts in the hash table
  */
size_t hash_size(const HashTable *table);

/**
  * @brief  Remove all contacts from the hash table
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_clear(HashTable *table);

/**
  * @brief  Print the contents of the hash table for debugging
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_print(const HashTable *table);

#if defined (HOST_BUILD)

/**
 * @brief SOFTWARE TESTING ONLY - Allocate memory on heap for RAM (statically allocated for STM32)
 *
 */
HashEntry* hash_create_software(void);

/**
 * @brief SOFTWARE TESTING ONLY - Allocate memory on heap for SD Card
 */
uint8_t* hash_create_sd_mock(void);
/**
 * @brief SOFTWARE TESTING ONLY - Free allocated memory for hash table testing
 */
void hash_destroy_software(HashTable* table, uint8_t* sd);
#endif

#ifdef __cplusplus
}
#endif

#endif
