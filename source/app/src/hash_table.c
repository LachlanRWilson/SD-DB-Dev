#include <stdlib.h>
#include <string.h>

#include "hash_table.h"
#include "contact.h"
#include "storage.h"
#include "usage_bitmap.h"
#include "free_list_stack.h"
#include "journal.h"

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
  * @brief  Phone Number Hash Function (DJB2) NOTE: non-numeric characters are ignored
  * @param  phone: phone number
  * @retval uint16_t: Hash Code
  */
uint16_t hash_phone(const char *phone)
{
    uint32_t hash = 5381u;

    // Iterate over phone number until null character is reached
    while (*phone)
    {
        if (*phone >= '0' && *phone <= '9')
        {
            hash = ((hash << 5) + hash) + (uint8_t)*phone;
            // Equivalent to: hash = hash * 33 + *phone;
        }

        phone++;
    }

    return (uint16_t)hash ^ (hash >> 16);
}

/**
 * @brief Perform a double-hash search on the hash table using a phone
 *        number as the key.
 *
 * Because the table is indexed by a 16-bit DJB2 hash of the phone
 * number rather than a unique numeric ID, two different phone numbers
 * can land on the same hash. Any OCCUPIED slot whose stored hash
 * matches is therefore verified against the actual phone number
 * persisted in the contact sector before being treated as a match.
 *
 * @param table Pointer to the hash table.
 * @param phone Phone number being searched for.
 * @param h1 Pre-calculated primary hash (of hash_phone(phone)).
 * @param h2 Pre-calculated secondary hash (of hash_phone(phone)).
 * @param entry Output: the empty/tombstoned slot to insert into, or the matching entry.
 * @retval true  An empty slot (for insertion) or a confirmed matching entry was found.
 * @retval false Table is full with no match found.
 */
bool find_hash_phone(HashTable *table, const char *phone, uint16_t h1, uint16_t h2, HashEntry **entry)
{
    uint16_t target_hash = hash_phone(phone);
    ContactBuffer contact;

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;

        *entry = &table->htable[index];

        // Empty or tombstoned slot - nothing stored here, safe to insert
        if ((*entry)->state == ENTRY_EMPTY || (*entry)->state == ENTRY_DELETED)
        {
#if defined(HOST_BUILD)
            table->collision_count = i;
#endif
            return true;
        }

        // Occupied slot with the same hash - could be the same phone
        // number, or a genuine hash collision between two different
        // numbers. Disambiguate by reading the actual stored phone.
        if ((*entry)->state == ENTRY_OCCUPIED && (*entry)->id == target_hash)
        {
            if (!read_contact(table->storage, (*entry)->sector, &contact))
            {
                // Couldn't verify - treat as a miss and keep probing
                continue;
            }

            if (strcmp(contact.contact.phone, phone) == 0)
            {
#if defined(HOST_BUILD)
                table->collision_count = i;
#endif
                return true;
            }
            // Same hash, different phone - keep probing past this slot
        }
    }

    return false; // table full, no match
}

/**
 * @brief Insert or update a contact in the hash table using its phone
 *        number as the key, and persist it to storage.
 *
 * @param table Pointer to the hash table.
 * @param journal Pointer to the rollback journal.
 * @param contact Contact to insert or update (phone number read from here).
 * @retval Sector index on success, otherwise UINT16_MAX.
 */
uint16_t hash_insert_contact_by_phone(HashTable *table, Journal *journal, ContactBuffer *contact)
{
    HashEntry *entry;
    uint16_t sector;
    bool is_new_entry;

    if (table == NULL || table->storage == NULL || table->free_stack == NULL || contact == NULL)
    {
        return UINT16_MAX;
    }

    const char *phone = contact->contact.phone;
    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry) || entry == NULL)
    {
        return UINT16_MAX; // table full
    }

    is_new_entry = (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED);

    if (is_new_entry)
    {
        sector = free_list_allocate(table->free_stack);

        if (sector == UINT16_MAX)
        {
            return UINT16_MAX; // allocator exhausted
        }

        entry->sector = sector;
        entry->state = ENTRY_OCCUPIED;
        entry->id = hash; // stored hash used for fast re-probing, not a unique key
        table->num_elems++;
    }
    else
    {
        // find_hash_phone already confirmed this is the same phone number
        sector = entry->sector;
    }

    if (!write_contact(table->storage, journal, entry->sector, contact))
    {
        // Only unwind hash-table state for a brand-new entry. If this was
        // an update to an existing contact, the entry was valid before
        // this call, and a failed write here must not tear it down.
        if (is_new_entry)
        {
            entry->state = ENTRY_EMPTY;
            entry->id = 0;
            entry->sector = UINT16_MAX;
            free_list_free(table->free_stack, sector);
            table->num_elems--;
        }

        return UINT16_MAX;
    }

    return sector;
}

/**
 * @brief Find and read a contact by phone number.
 *
 * @param table Pointer to the hash table.
 * @param phone Phone number to search for.
 * @param out Pointer to output Contact.
 * @retval true if a matching contact was found and read successfully.
 * @retval false otherwise.
 */
bool hash_find_contact_by_phone(HashTable *table, const char *phone, ContactBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL || out == NULL || 
            phone == NULL || table->size == 0)
    {
        return false;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry))
    {
        return false; // table full, no match
    }

    // find_hash_phone returns true both for a confirmed match AND for an
    // empty/tombstoned slot (the "safe to insert here" case used by the
    // insert path). Only an OCCUPIED slot is an actual find.
    if (entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    return read_contact(table->storage, entry->sector, out);
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
 * @brief Perform a double hash search on the hash table
 *
 * @param table Pointer to the hash table.
 * @param h1 primary hash.
 * @param h2 secondary hash.
 * @param entry entry found in hash table or NULL
 * @retval True if hash entry found else false
 */
uint16_t find_hash(HashTable *table, uint16_t id, uint16_t h1, uint16_t h2, HashEntry** entry)
{
    // Iterate until no collision
    for (uint16_t i = 0; i <= table->size; i++)
    {
        
        // Table full
        if (i == table->size) {
            return UINT16_MAX;
        }

        // Calculate hash index based on probe step
        uint16_t index = (h1 + i * h2) % table->size;

        *entry = &table->htable[index];

        // Empty or tombstoned entry
        if ((*entry)->state == ENTRY_EMPTY)
        {

#if defined(HOST_BUILD)
            table->collision_count = i;
#endif
            return true;
        }

        // Contact already exists and the same id
        if ((*entry)->state == ENTRY_OCCUPIED && (*entry)->id == id)
        {
#if defined(HOST_BUILD)
            table->collision_count = i;
#endif
            return true;
        }
    }

    return false;

}


/**
 * @brief Insert a contact into the hash table and write / update to SD Card.
 *
 * @param table Pointer to the hash table.
 * @param contact Contact to insert.
 * @retval Sector index if insertion successful, otherwise UINT16_MAX.
 */
uint16_t hash_insert_contact(HashTable *table, Journal *journal, uint16_t id, ContactBuffer *contact)
{
    // HashEntry pointer that contact will be inserted into
    HashEntry *entry;

    // Catch null pointers
    if (table == NULL || table->storage == NULL || table->free_stack == NULL || contact == NULL)
    {
        return UINT16_MAX;
    }

    // Pre-calculate double hash
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);

    // If has cannot be found throw error
    if (!find_hash(table, id, h1, h2, &entry) || entry == NULL)
    {
        return UINT16_MAX;
    }

    // Variable for new sector
    uint16_t sector; 

    // Check to see if this is a new contact
    if (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED) {
        sector = free_list_allocate(table->free_stack);

        // If invalid sector throw error (stack empty)
        if (sector == UINT16_MAX)
        {
            return UINT16_MAX;
        }    

        // allocate sector to new contact
        entry->sector = sector;
        entry->state = ENTRY_OCCUPIED;
        entry->id = id;
        entry->sector = sector;
        table->num_elems++;
    }

    // Update Contact in ContactSector
    if (!write_contact(table->storage, journal, entry->sector, contact))
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

/**
 * @brief Find and read a contact by its unique ID on the SD Card.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to search for.
 * @param out Pointer to output Contact.
 * @retval true if contact found, otherwise false.
 */
bool hash_find_contact(HashTable *table, uint16_t id, ContactBuffer *out)
{
    // HashEntry that will be pulled from the table
    HashEntry* entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL || out == NULL ||
            table->size == 0)
    {
        return false;
    }

    // Hash calculations
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);
    
    // if hash cannot be found in table
    if (!find_hash(table, id, h1, h2, &entry) && entry == NULL) {
        return false;
    }

    // read contact from sd card
    if(!read_contact(table->storage, entry->sector, out))
    {
        return false;
    }

    return true;
}

/**
  * @brief  Find an entry in the table
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
 * @brief Remove a contact from the hash table.
 *
 * @param table Pointer to the hash table.
 * @param id Contact ID to remove.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact(HashTable *table, Journal *journal, uint16_t id, ContactBuffer *out)
{
    // HashEntry that will be pulled from the table
    HashEntry* entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL || out == NULL ||
            table->size == 0)
    {
        return false;
    }

    // Hash calculations
    uint16_t h1 = hash_primary(id, table->size);
    uint16_t h2 = hash_secondary(id, table->size);
    
    // if hash cannot be found in table
    if (!find_hash(table, id, h1, h2, &entry) && entry == NULL) {
        return false;
    }

    if (entry->state != ENTRY_OCCUPIED || entry->id != id)
    {
        return false;
    }
    
    // remove contact from SD card
    if(!remove_contact(table->storage, journal, entry->sector, out))
    {
        return false;
    }

    /* NOTE: IF FAILURE HERE SD CARD AND RAM OUT OF SYNC */

    // Set state to deleted
    entry->state = ENTRY_DELETED;

    // return memory address back to free stack to be recycled
    free_list_free(table->free_stack, entry->sector);

    // decrease number of elements
    table->num_elems--;

    return true;
    {
        return false;
    }
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
  * @brief  Reconstruct the HashTable in RAM from the information store in the data on the SD Card
  * @param  table: Pointer to the hash table
  * @retval None
  */
bool hash_reconstruct_contact(HashTable *table) 
{
    
    uint32_t used, last_bit;
    ContactSectorBuffer cSector;
    // Iterate over usage map in ram
    for (uint32_t j = 0; j < USAGE_BITMAP_STORAGE_SIZE; j++)
    {
        used = usage_bitmap[j];
        last_bit = 0;

        while (used != 0)
        {
            uint32_t bit = __builtin_ctz(used);
            uint16_t sector = (uint16_t)(j * 32 + bit);

            // Free sectors before this used sector
            free_list_free_range(table->free_stack, (uint16_t) (j * 32 + last_bit), sector);

            // Read ContactSector
            read_contact_sector(table->storage, sector, &cSector);

            uint8_t cSector_used = cSector.sector.header.used_bitmap;
            // Read every contact in sector
            while (cSector_used != 0)
            {
                uint32_t contact_bit = __builtin_ctz(cSector_used);
                cSector_used--;
            }
            // Reconstruct hash entry

            used &= used - 1;
            last_bit = bit + 1;
        }

        // Free sectors after the final used sector
        free_list_free_range(table->free_stack, (uint16_t) (j * BITS_PER_ELEMENT + last_bit), 
                j * BITS_PER_ELEMENT + BITS_PER_ELEMENT);
    }
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
