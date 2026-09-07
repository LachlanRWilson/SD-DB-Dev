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
    uint16_t id; // Phone number hash (hash_phone()), NOT a unique contact ID (2B)
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
    FreeList *free_stack; // Pointer to FLS struct (allocates in database struct)
    size_t num_elems; // amount of elements in table
    size_t size; // total space in table
#if defined (HOST_BUILD)
    size_t collision_count; // for benchmarking hash functions
#endif
} HashTable;

/**
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param  storage: Pointer to the storage struct
  * @param  fstack: Pointer to the FLS backing the table
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init(HashTable* table, Storage* storage, FreeList *fstack, HashEntry* entries, size_t size);

/**
  * @brief  Destroy a hash table and free all associated memory
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_destroy(HashTable *table);

/**
 * @brief Probe the hash table for a slot matching the given key.
 *
 * Returns the OCCUPIED entry if id matches, OR an EMPTY/DELETED slot
 * available for insertion — the caller must check entry->state to
 * know which case it got. Does not disambiguate 16-bit hash
 * collisions between different phone numbers; use find_hash_phone()
 * for that. Primarily intended for hash_reconstruct_contact() and
 * other callers that already have a pre-computed phone hash.
 *
 * @param table Pointer to the hash table.
 * @param id Key to search for (a phone hash from hash_phone()).
 * @param out Output: the entry the probe landed on.
 * @retval true  Landed on a matching entry or a free slot.
 * @retval false Table is completely full.
 */
bool hash_find_entry(HashTable *table, const char *phone, HashEntry** out);

/**
 * @brief Perform a double-hash search on the hash table using a phone
 *        number as the key, disambiguating hash collisions by reading
 *        back the stored phone number from storage.
 *
 * @param table Pointer to the hash table.
 * @param phone Phone number being searched for.
 * @param h1 Pre-calculated primary hash (of hash_phone(phone)).
 * @param h2 Pre-calculated secondary hash (of hash_phone(phone)).
 * @param entry Output: the empty/tombstoned slot to insert into, or the matching entry.
 * @retval true  An empty slot (for insertion) or a confirmed matching entry was found.
 * @retval false Table is full with no match found.
 */
bool find_hash_phone(HashTable *table, const char *phone, uint16_t h1, uint16_t h2, HashEntry **entry);

/**
 * @brief Insert or update a contact in the hash table using its phone
 *        number as the key, and persist it to storage.
 *
 * @param table Pointer to the hash table.
 * @param journal Pointer to the rollback journal.
 * @param contact Contact to insert or update (phone number read from here).
 * @retval Sector index on success, otherwise UINT16_MAX.
 */
uint16_t hash_insert_contact_by_phone(HashTable *table, Journal *journal, ContactBuffer *contact);

/**
 * @brief Find and read a contact by phone number.
 *
 * @param table Pointer to the hash table.
 * @param phone Phone number to search for.
 * @param out Pointer to output Contact.
 * @retval true if a matching contact was found and read successfully.
 * @retval false otherwise.
 */
bool hash_find_contact_by_phone(HashTable *table, const char *phone, ContactBuffer *out);

/**
  * @brief  Find a contact's sector index by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to search for
  * @retval Sector index, or UINT16_MAX if not found.
  */
uint16_t hash_find_sector_by_phone(HashTable *table, const char *phone);

/**
  * @brief  Find a contact's latest message extent by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to search for
  * @retval Latest message extent index, or UINT16_MAX if not found.
  */
uint16_t hash_find_message_by_phone(HashTable *table, const char *phone);

/**
  * @brief  Remove a contact from the hash table (RAM only) by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to remove
  * @param  removed: removed entry
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove_by_phone(HashTable *table, const char *phone, HashEntry **removed);

/**
 * @brief Remove a contact from the hash table and storage by phone number.
 *
 * @param table Pointer to the hash table.
 * @param journal Pointer to the rollback journal.
 * @param phone Phone number to remove.
 * @param out Removed contact's data.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact_by_phone(HashTable *table, Journal *journal, const char *phone, ContactBuffer *out);

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
  * @brief  Reconstruct the in-RAM hash table from persistent contact
  *         data after a restart, using the usage bitmap to avoid
  *         scanning unused sectors.
  * @param  table: Pointer to a freshly hash_init'd, empty hash table
  * @retval true if reconstruction completed successfully, false on a
  *         storage read failure or if the table filled up mid-reconstruction.
  */
bool hash_reconstruct_contact(HashTable *table);

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
