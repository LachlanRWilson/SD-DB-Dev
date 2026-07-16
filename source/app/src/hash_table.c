#include "hash_table.h"
#include <stdlib.h>

/**
  * @brief  Primary Hash Function
  * @param  key: entry key
  * @param  capacity: Number of entries 
  * @retval uint16_t: Hash Code
  */
static inline uint16_t hash_primary(uint16_t key, uint16_t capacity)
{
    return key % capacity;
}

/**
  * @brief  Seconary Hash Function
  * @param  key: entry key
  * @param  capacity: Number of entries 
  * @retval uint16_t: Hash Code
  */
static inline uint16_t hash_secondary(uint16_t key, uint16_t capacity)
{
    // must never be 0
    return 1 + (key % (capacity - 1));
}


/**
  * @brief  Double Hash Function
  * @param  key: entry key
  * @param attemptNum: Hashing attempt number
  * @param  capacity: Number of entries 
  * @retval uint16_t: Hash Code
  */
static inline uint16_t hash_double(uint16_t key, uint16_t attemptNum, uint16_t capacity)
{
    uint16_t h1 = hash_primary(key, capacity);
    uint16_t h2 = hash_secondary(key, capacity);

    return (h1 + attemptNum * h2) % capacity;
}

/**
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs) 
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init( HashTable* table, FreeList *fstack,  HashEntry* entries, size_t
        size)
{
    table->htable = entries;
    table->free_stack = fstack;
    table->size = size;
    table->num_elems = 0;
}

/**
  * @brief  Destroy a hash table and free all associated memory
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_destroy(HashTable *table)
{
    // For statically allocated on stm32 memory will not be freed
    hash_clear(table);         
}

/**
  * @brief  Insert a contact into the hash table
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval true if the contact was inserted successfully, false otherwise
  */
bool hash_insert(HashTable *table, uint16_t id)
{
    // Pre calculate double hash
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    // Iterate until no collision (shouldn't be too many as table is limited to 70% table->size)
    for (uint16_t i = 0; i < table->size; i++)
    {
        // Calculate hash code based on step
        uint16_t index = (h1 + i * h2) % table->size;

         HashEntry *entry = &table->htable[index];

        // Check empty or tombstoned
        if (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED)
        {
            entry->state = ENTRY_OCCUPIED;
            entry->id = id;
            entry->sector = free_list_allocate(table->free_stack);
            table->num_elems++;

// Collission Debugging
#if defined (HOST_BUILD)
            table->collision_count = i;
#endif
            return true;
        }

        // If same id then updating
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
// Collission Debugging
#if defined (HOST_BUILD)
            table->collision_count = i;
#endif
            return true;
        }
    }

    return false; // table full (should never happen)
}

/**
  * @brief  Find a contact by its unique ID
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @param  out: Output HashEntry pointer
  * @retval True if entry found else false
  */
bool hash_find_entry(HashTable *table, uint16_t id, HashEntry** out) 
{
    if (table == NULL || table->htable == NULL || table->size == 0)
    {
        return false;
    }

    // Hash Calculations
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;

        // Get Entry from RAM
        HashEntry *entry = &table->htable[index];

        // If we hit an empty slot, key was never inserted
        if (entry->state == ENTRY_EMPTY)
        {
            return false;
        }

        // If occupied and match found
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
            // set the out HashEntry pointer to the HashEntry in RAM
            *out = entry;
            return true;
        }

        // ENTRY_DELETED -> continue probing
    }

    return false;
}

/**
  * @brief  Find a contact by its unique ID
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching contact, or NULL if not found
  */
uint16_t hash_find_sector(HashTable *table, uint16_t id)
{
    if (table == NULL || table->htable == NULL || table->size == 0)
    {
        return UINT16_MAX;
    }

    HashEntry *entry;
    
    // Get the entry, if not found through error
    if (!hash_find_entry(table, id, &entry)) {
        return UINT16_MAX;
    }
    
    return entry->sector;
}

/**
  * @brief  Find a contacts latest message extent index
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to search for
  * @retval Pointer to the matching contact, or NULL if not found
  */
uint16_t hash_find_message(HashTable *table, uint16_t id)
{
    if (table == NULL || table->htable == NULL || table->size == 0)
    {
        return UINT16_MAX;
    }

    HashEntry *entry;
    
    // Get the entry, if not found through error
    if (!hash_find_entry(table, id, &entry)) {
        return UINT16_MAX;
    }
    
    return entry->latest_msg_extent;
}

/**
  * @brief  Remove a contact from the hash table
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to remove
  * @param removed: removed entry
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove(HashTable *table, uint16_t id, HashEntry **removed)
{
    HashEntry *entry = NULL;
    
    // Get the entry, if not found through error
    if (!hash_find_entry(table, id, &entry)) {
        return false;
    }

    // If same id then set to deleted
    if (entry->state == ENTRY_OCCUPIED && entry->id == id)
    {
        // Set state to deleted
        entry->state = ENTRY_DELETED;

        // return memory address back to free stack to be recycled
        free_list_free(table->free_stack, entry->sector);

        // decrease number of elements
        table->num_elems--;

        // Store removed entry
        *removed = entry;
        return true;
    }

    return false; // table full (should never happen)

}

/**
  * @brief  Get the number of contacts currently stored in the hash table
  * @param  table: Pointer to the hash table
  * @retval Number of contacts in the hash table
  */
size_t hash_size(const HashTable *table)
{
    return table->num_elems;

}

/**
  * @brief  Remove all contacts from the hash table
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_clear(HashTable *table)
{
    // Just set the values as empty. Written data will simple be overwritten
    for (int i = 0; i < table->size; i++) {
        table->htable[i].state = ENTRY_EMPTY; 
        // Clear the free list stack
        free_list_free(table->free_stack, table->htable[i].sector);
    }
        table->num_elems = 0;

}


/**
  * @brief  Print the contents of the hash table for debugging
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_print(const HashTable *table)
{

}

#if defined (HOST_BUILD)

/**
 * @brief SOFTWARE TESTING ONLY - Allocate memory on heap for RAM (statically allocated for STM32)
 *
 */
HashEntry* hash_create_software(void) {
    return  (HashEntry*)calloc(HASH_TABLE_SIZE, sizeof(HashEntry));
}

/**
 * @brief SOFTWARE TESTING ONLY - Allocate memory on heap for SD Card  
 */
uint8_t* hash_create_sd_mock(void) {
    return (uint8_t*)calloc(HASH_TABLE_SIZE, sizeof(Contact));
}

/**
 * @brief SOFTWARE TESTING ONLY - Free allocated memory for hash table testing
 */
void hash_destroy_software(HashTable* table, uint8_t* sd) {
    free(table->htable);
    free(sd);
}

#endif
