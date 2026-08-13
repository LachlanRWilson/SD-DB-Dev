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
#include "free_list_stack.h"
#include "storage.h"
#include "contact.h"



// Struct Sizes
#define HASH_ENTRY_BYTES 8


// Prime number that allows a hash table of 10000 entries to have a load factor of 70%
#define HASH_TABLE_SIZE 14293


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
    uint16_t id; // Contact ID (2B)
    uint16_t sector; // SD Sector (2B)
    uint16_t latest_msg_extent; // Latest Message Extent offset (2B)
    ENTRY_STATE state;  // Entry occupation state (1B)
    uint8_t padding; // (1B)
} HashEntry;

// Static checks to ensure the size of the structs are correct
STATIC_ASSERT(sizeof(HashEntry) == HASH_ENTRY_BYTES, "Unexpected HashEntry size");
STATIC_ASSERT(sizeof(ContactSectorHeader) == CONTACT_HEADER_BYTES, "Unexpected ContactSector size");
STATIC_ASSERT(sizeof(ContactSector) == CONTACT_SECTOR_BYTES, "Unexpected ContactSector size");


// Information Struct about hash table
typedef struct 
{
    HashEntry *htable; // In RAM hash table
    Storage *storage;
    FreeList *free_stack; // List of free list stack pointers
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

void hash_init( HashTable* table, Storage* storage, FreeList *fstacks,  HashEntry* entries, size_t size);

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
 * @retval Sector index if insertion successful, otherwise UINT16_MAX.
 */
uint16_t hash_insert_contact(HashTable *table, uint16_t id, ContactBuffer *contact);

/**
  * @brief  Insert a contact into the hash table using phone number of PK
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval if insertion successful return sector index, else UINT16_MAX
  */
uint16_t hash_insert_phone(HashTable *table, char *phone);

/**
  * @brief  Find a contact by its unique ID
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching contact, or NULL if not found
  */
uint16_t hash_find_sector(HashTable *table, uint16_t id);

/**
  * @brief  Find a contact by its unique ID
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @param  out: Output HashEntry pointer
  * @retval True if entry found else false
  */
bool hash_find_entry(HashTable *table, uint16_t id, HashEntry** out);

/**
 * @brief Find a contact by its unique ID. This will pull the contact from the SD Card in one
 * go
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to search for.
 * @param out Pointer to output Contact.
 * @retval true if contact found, otherwise false.
 */
bool hash_find_contact(HashTable *table, uint16_t id, ContactBuffer *out);

/**
  * @brief  Find a contacts message extent offset
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching message extent, or NULL if not found
  */
uint16_t hash_find_message(HashTable *table, uint16_t id);

/**
  * @brief  Remove a contact from the hash table
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to remove
  * @param removed: removed entry
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove(HashTable *table, uint16_t id, HashEntry **removed);

/**
 * @brief Remove a contact from the hash table.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to remove.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact(HashTable *table, uint16_t id, ContactBuffer *out);

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
