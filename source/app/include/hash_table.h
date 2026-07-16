#ifndef HASH_TABLE_H
#define HASH_TABLE_H



#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "free_list_stack.h"
#include "storage.h"

#define MAX_NAME_LEN 64
#define MAX_PHONE_LEN 15

// Prime number that allows a hash table of 10000 entries to have a load factor of 70%
#define HASH_TABLE_SIZE 14293

// Entry State  (Pack enum to 1 byte)
typedef enum
{ 
    ENTRY_EMPTY = 0,
    ENTRY_OCCUPIED,
    ENTRY_DELETED
} EntryState;

// Will make a copy of contact typedef struct current in repo
typedef struct
{
    uint8_t name_len;          /**< Length of name string */
    char name[MAX_NAME_LEN];   /**< Contact name */
    uint8_t phone_len;         /**< Length of phone number */
    char phone[MAX_PHONE_LEN]; /**< Phone number */
    uint32_t offset_id;        /**< Unique offset identifier */
} Contact;

typedef union {
   Contact contact;
   uint8_t buffer[sizeof(Contact)];
} ContactBuffer;


// Hash Entry that points to SD Card sector
typedef struct
{
    EntryState state;  // Entry occupation state
    uint32_t id; // Contact ID
    uint32_t sector; // SD Sector
    uint32_t latest_msg_extent; // Latest Message Extent offset
} HashEntry;


// Information Struct about hash table
typedef struct 
{
    HashEntry *htable;
    Storage *storage;
    FreeList *free_stack;
    size_t capacity;
    size_t size;
#if defined (HOST_BUILD)
    size_t collision_count; // for benchmarking hash functions
#endif
} HashTable;


/**
  * @brief  Create a hash table
  * @param  buckets: Number of buckets (hash table capacity)
  * @retval HashTable*: Pointer to the newly created hash table, or NULL on failure
  */
void hash_init( HashTable* table, FreeList *fstack,  HashEntry* entries, size_t
        size, Storage *storage);

/**
  * @brief  Destroy a hash table and free all associated memory
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_destroy(HashTable *table);

/**
  * @brief  Insert a contact into the hash table
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval true if the contact was inserted successfully, false otherwise
  */
bool hash_insert(HashTable *table, uint32_t id);

/**
  * @brief  Find a contact by its unique ID
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching contact, or NULL if not found
  */
uint32_t hash_find(HashTable *table, uint32_t id);

/**
  * @brief  Find a contacts message extent offset
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching message extent, or NULL if not found
  */
uint32_t hash_find_message(HashTable *table, uint32_t id);

/**
  * @brief  Remove a contact from the hash table
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to remove
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove(HashTable *table, uint32_t id);

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
