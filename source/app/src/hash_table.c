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
  * @brief  Phone Number Hash Function (FNV-1a)
  * @param  phone: phone number
  * @retval uint16_t: Hash Code
  */
uint16_t hash_phone(const char *phone)
{
    uint32_t hash = 2166136261u;

    // Iterate over numbers in phone number until null character is reached
    while (*phone)
    {
        if (*phone >= '0' && *phone <= '9')
        {
            hash ^= (uint8_t)*phone;
            hash *= 16777619u;
        }

        phone++;
    }

    return (uint16_t)hash ^ (hash >> 16);
}

/**
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param storage: storage struct for Heap or SD Card storage
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs) 
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init(HashTable* table, Storage* storage, FreeList *fstack,  HashEntry* entries, size_t
        size)
{
    table->htable = entries;
    table->storage = storage;
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
  * @retval if insertion successful return sector index, else UINT16_MAX
  */
uint16_t hash_insert(HashTable *table, uint16_t id)
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
            return entry->sector;
        }

        // If same id then updating
        if (entry->state == ENTRY_OCCUPIED && entry->id == id)
        {
// Collission Debugging
#if defined (HOST_BUILD)
            table->collision_count = i;
#endif
            return entry->sector;
        }
    }

    return UINT16_MAX; // table full (should never happen)
}

/**
 * @brief Insert a contact into the hash table.
 *
 * @param table Pointer to the hash table.
 * @param contact Contact to insert.
 * @retval Sector index if insertion successful, otherwise UINT16_MAX.
 */
uint16_t hash_insert_contact(HashTable *table, uint16_t id, ContactBuffer *contact)
{
    if (table == NULL ||
        table->storage == NULL ||
        table->free_stack == NULL ||
        contact == NULL)
    {
        return UINT16_MAX;
    }

    // Pre-calculate double hash
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    // Iterate until no collision
    for (uint16_t i = 0; i < table->size; i++)
    {
        // Calculate hash index based on probe step
        uint16_t index = (h1 + i * h2) % table->size;

        HashEntry *entry = &table->htable[index];

        // Empty or tombstoned entry
        if (entry->state == ENTRY_EMPTY ||
            entry->state == ENTRY_DELETED)
        {
            uint16_t sector = free_list_allocate(table->free_stack);

            if (sector == UINT16_MAX)
            {
                return UINT16_MAX;
            }

            entry->state = ENTRY_OCCUPIED;
            entry->id = id;
            entry->sector = sector;

            table->num_elems++;

#if defined(HOST_BUILD)
            table->collision_count = i;
#endif

            // Store contact
            if (!table->storage->write_block( table->storage->context, sector,
            contact->buffer))
            {
                // Storage failed, undo hash table allocation
                entry->state = ENTRY_EMPTY;
                entry->id = 0;
                entry->sector = UINT16_MAX;

                free_list_free(table->free_stack, sector);
                table->num_elems--;

                return UINT16_MAX;
            }

            return sector;
        }

        // Contact already exists
        if (entry->state == ENTRY_OCCUPIED &&
            entry->id == id)
        {
#if defined(HOST_BUILD)
            table->collision_count = i;
#endif

            return entry->id;
        }
    }

    // Table full
    return UINT16_MAX;
}


/**
  * @brief  Insert a contact into the hash table using phone number of PK
  * @param  table: Pointer to the hash table
  * @param  contact: Contact to insert
  * @retval if insertion successful return sector index, else UINT16_MAX
  */
uint16_t hash_insert_phone(HashTable *table, char *phone)
{
    uint16_t hash = hash_phone(phone);

    // Iterate until an empty/deleted slot is found
    // (table should be limited to ~70% occupancy)
    for (uint16_t i = 0; i < table->size; i++)
    {
        // Linear probing
        uint16_t index = (hash + i) % table->size;

        HashEntry *entry = &table->htable[index];

        // Check empty or tombstoned entry
        if (entry->state == ENTRY_EMPTY ||
            entry->state == ENTRY_DELETED)
        {
            entry->state = ENTRY_OCCUPIED;
            //entry->hash = hash;
            entry->sector = free_list_allocate(table->free_stack);

            table->num_elems++;

#if defined(HOST_BUILD)
            table->collision_count = i;
#endif

            return entry->sector;
        }

        // Same hash — potentially the same phone number
        if (entry->state == ENTRY_OCCUPIED) //&&
            //entry->hash == hash)
        {
            /*
             * Hash collision or existing phone number.
             *
             * You should compare the actual phone number here
             * before treating this as an existing entry.
             */
        }
    }

    // Hash table is full
    return UINT16_MAX;
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
 * @brief Find a contact by its unique ID.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to search for.
 * @param out Pointer to output Contact.
 * @retval true if contact found, otherwise false.
 */
bool hash_find_contact(HashTable *table, uint16_t id, ContactBuffer *out)
{
    if (table == NULL ||
        table->htable == NULL ||
        table->storage == NULL ||
        out == NULL ||
        table->size == 0)
    {
        return false;
    }

    // Hash calculations
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;

        // Get entry from RAM
        HashEntry *entry = &table->htable[index];

        // If we hit an empty slot, key was never inserted
        if (entry->state == ENTRY_EMPTY)
        {
            return false;
        }

        // If occupied and ID matches
        if (entry->state == ENTRY_OCCUPIED &&
            entry->id == id)
        {
            // Read contact from storage
            if (!table->storage->read_block( table->storage->context,
            entry->sector, out->buffer))
            {
                return false;
            }

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
 * @brief Remove a contact from the hash table.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to remove.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact(HashTable *table, uint16_t id, ContactBuffer *out)
{
    if (table == NULL ||
        table->htable == NULL ||
        table->free_stack == NULL ||
        table->size == 0)
    {
        return false;
    }

    // Hash calculations
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;

        HashEntry *entry = &table->htable[index];

        // If we hit an empty slot, key was never inserted
        if (entry->state == ENTRY_EMPTY)
        {
            return false;
        }

        // If occupied and ID matches
        if (entry->state == ENTRY_OCCUPIED &&
            entry->id == id)
        {
            // Read contact from storage
            if (!table->storage->read_block( table->storage->context,
            entry->sector, out->buffer))
            {
                return false;
            }

            // Return sector to free list
            free_list_free(table->free_stack, entry->sector);

            // Mark hash entry as deleted
            entry->state = ENTRY_DELETED;

            // Decrease element count
            table->num_elems--;

            return true;
        }

        // ENTRY_DELETED -> continue probing
    }

    return false;
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
