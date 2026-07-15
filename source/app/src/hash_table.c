#include "hash_table.h"
#include <stdlib.h>

/**
  * @brief  Primary Hash Function
  * @param  key: entry key
  * @param  capacity: Number of entries 
  * @retval uint32_t: Hash Code
  */
static inline uint32_t hash_primary(uint32_t key, uint32_t capacity)
{
    return key % capacity;
}

/**
  * @brief  Seconary Hash Function
  * @param  key: entry key
  * @param  capacity: Number of entries 
  * @retval uint32_t: Hash Code
  */
static inline uint32_t hash_secondary(uint32_t key, uint32_t capacity)
{
    // must never be 0
    return 1 + (key % (capacity - 1));
}


/**
  * @brief  Double Hash Function
  * @param  key: entry key
  * @param attemptNum: Hashing attempt number
  * @param  capacity: Number of entries 
  * @retval uint32_t: Hash Code
  */
static inline uint32_t hash_double(uint32_t key, uint32_t attemptNum, uint32_t capacity)
{
    uint32_t h1 = hash_primary(key, capacity);
    uint32_t h2 = hash_secondary(key, capacity);

    return (h1 + attemptNum * h2) % capacity;
}

/**
  * @brief  Create a hash table
  * @param  buckets: Number of buckets (hash table capacity)
  * @retval HashTable*: Pointer to the newly created hash table, or NULL on failure
  */
void hash_create( HashTable* table, FreeList *fstack,  HashEntry* entries, size_t
        size)
{
    table->htable = entries;
    table->free_stack = fstack;
    table->capacity = size;
    table->size = 0;
}

/**
  * @brief  Create a hash table
  * @param  buckets: Number of buckets (hash table capacity)
  * @retval HashTable*: Pointer to the newly created hash table, or NULL on failure
  */
void hash_init( HashTable* table, FreeList *fstack,  HashEntry* entries, size_t
        size, Storage *storage)
{
    table->htable = entries;
    table->free_stack = fstack;
    table->capacity = size;
    table->storage = storage;
    table->size = 0;
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
bool hash_insert(HashTable *table, uint32_t id)
{
    // Pre calculate double hash
    uint32_t h1 = hash_primary(id, table->capacity);
    uint32_t h2 = hash_secondary(id, table->capacity);

    // Iterate until no collision (shouldn't be too many as table is limited to 71Contact contact% table->capacity)
    for (uint32_t i = 0; i < table->capacity; i++)
    {
        // Calculate hash code based on step
        uint32_t index = (h1 + i * h2) % table->capacity;

         HashEntry *entry = &table->htable[index];

        // Check empty or tombstoned
        if (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED)
        {
            entry->state = ENTRY_OCCUPIED;
            entry->id = id;
            entry->sector = free_list_allocate(table->free_stack);
            table->size++;

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
  * @retval Pointer to the matching contact, or NULL if not found
  */
uint32_t hash_find(HashTable *table, uint32_t id)
{
    if (table == NULL || table->htable == NULL || table->capacity == 0)
    {
        return UINT32_MAX;
    }

    uint32_t h1 = hash_primary(id, table->capacity);
    uint32_t h2 = hash_secondary(id, table->capacity);

    for (uint32_t i = 0; i < table->capacity; i++)
    {
        uint32_t index = (h1 + i * h2) % table->capacity;

        HashEntry *entry = &table->htable[index];

        // If we hit an empty slot, key was never inserted
        if (entry->state == ENTRY_EMPTY)
        {
            return UINT32_MAX;
        }

        // If occupied and match found
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
            // NOTE:
            // currently do NOT store Contact in RAM,
            // only sector pointer.
            //
            // So this must be reconstructed or loaded from SD.

            return entry->sector; // placeholder until SD read layer exists
        }

        // ENTRY_DELETED → continue probing
    }

    return UINT32_MAX;
}


uint32_t hash_find_message(HashTable *table, uint32_t id)
{
    if (table == NULL || table->htable == NULL || table->capacity == 0)
    {
        return UINT32_MAX;
    }

    uint32_t h1 = hash_primary(id, table->capacity);
    uint32_t h2 = hash_secondary(id, table->capacity);

    for (uint32_t i = 0; i < table->capacity; i++)
    {
        uint32_t index = (h1 + i * h2) % table->capacity;

        HashEntry *entry = &table->htable[index];

        // If we hit an empty slot, key was never inserted
        if (entry->state == ENTRY_EMPTY)
        {
            return UINT32_MAX;
        }

        // If occupied and match found
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
            return entry->latest_msg_extent; // placeholder until SD read layer exists
        }

        // ENTRY_DELETED → continue probing
    }

    return UINT32_MAX;
}





/**
  * @brief  Remove a contact from the hash table
  * @param  table: Pointer to the hash table
  * @param  id: Contact ID to remove
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove(HashTable *table, uint32_t id)
{
    // Pre calculate double hash
    uint32_t h1 = hash_primary(id, table->capacity);
    uint32_t h2 = hash_secondary(id, table->capacity);

    // Iterate until no collision (shouldn't be too many as table is limited to 70% capacity)
    for (uint32_t i = 0; i < table->capacity; i++)
    {
        // Calculate hash code based on step
        uint32_t index = (h1 + i * h2) % table->capacity;

         HashEntry *entry = &table->htable[index];

        // If same id then set to deleted
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
            entry->state = ENTRY_DELETED;
            free_list_free(table->free_stack, table->htable[index].sector);
            return true;
        }
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
    return table->size;

}

/**
  * @brief  Remove all contacts from the hash table
  * @param  table: Pointer to the hash table
  * @retval None
  */
void hash_clear(HashTable *table)
{
    // Just set the values as empty. Written data will simple be overwritten
    for (int i = 0; i < table->capacity; i++) {
        table->htable[i].state = ENTRY_EMPTY; 
        // Clear the free list stack
        free_list_free(table->free_stack, table->htable[i].sector);
    }
        table->size = 0;

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
