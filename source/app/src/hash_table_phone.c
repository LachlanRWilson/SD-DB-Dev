#include <stdlib.h>
#include <string.h>

#include "hash_table_phone.h"
#include "contact.h"
#include "storage.h"
#include "usage_bitmap.h"
#include "free_list_stack.h"
#include "journal.h"

typedef enum {
    SAME_PHONE = 0,
    DIFF_PHONE,
    CONTACT_READ_ERROR
} PHONE_CHECK;

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
  * @brief  Secondary Hash Function
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

    while (*phone)
    {
        if (*phone >= '0' && *phone <= '9')
        {
            hash = ((hash << 5) + hash) + (uint8_t)*phone;
        }

        phone++;
    }

    return (uint16_t)hash ^ (hash >> 16);
}

/**
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param storage: storage struct for Heap or SD Card storage
  * @param  fstack: pointer to the FLS backing the table
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init(HashTable* table, Storage* storage, FreeList *fstack, HashEntry* entries, size_t size)
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
    hash_clear(table);
}

PHONE_CHECK check_contact_phone(Storage *storage, const char* phone, HashEntry *entry)
{
    ContactBuffer contact;

    /*
     * entry->id is the phone HASH, not a storage location. The contact
     * lives at entry->sector - read from there.
     */
    if (!read_contact(storage, entry->sector, &contact))
    {
       return CONTACT_READ_ERROR;
    }

    /*
     * contact.phone is a fixed 15-byte field that is NOT guaranteed to be
     * NUL-terminated, so compare against its stored length. Require the
     * lengths to match as well, otherwise "0412" would match "0412345678".
     */
    size_t query_len = strlen(phone);

    if (query_len == contact.contact.phone_len &&
        strncmp(phone, contact.contact.phone, contact.contact.phone_len) == 0)
    {
        return SAME_PHONE;
    }
    return DIFF_PHONE;
}

/**
 * @brief Probe the hash table for a slot matching the given key.
 *
 * This is the generic double-hash probe used by both the phone-based
 * lookup/insert path and by reconstruction. On an OCCUPIED slot whose
 * id matches, it returns that entry (a "found" result). On an EMPTY or
 * DELETED slot, it returns that slot as an insertion point. The two
 * callers decide what "empty" means for their use case - "not found"
 * for a pure lookup, or "free to claim" for an insert/reconstruct.
 *
 * @param table Pointer to the hash table.
 * @param id Key to search for (a phone hash from hash_phone()).
 * @param out Output: the entry the probe landed on.
 * @retval true  Landed on a matching OCCUPIED entry, or a free EMPTY/DELETED slot.
 * @retval false Table is completely full with no match and no free slot.
 */
bool hash_find_entry(HashTable *table, const char* phone, HashEntry** out)
{

    if (table == NULL || table->htable == NULL || table->size == 0 || out == NULL)
    {
        return false;
    }

    // hash phone number
    uint16_t hash = hash_phone(phone);

    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;
        HashEntry *entry = &table->htable[index];

        if (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED)
        {
            *out = entry;
            return true;
        }

        if (entry->state == ENTRY_OCCUPIED)
        {
            // check the contact phone number is the same as the one being inserted
            PHONE_CHECK is_contact_phone_same = check_contact_phone(table->storage, phone, entry);

            // If different pohone number continue double hashing, if read error cry
            if (is_contact_phone_same == DIFF_PHONE)
            {
                continue;
            } else if (is_contact_phone_same == CONTACT_READ_ERROR) {
                return false;
            }
            *out = entry;
            return true;
        }
    }

    return false; // table full
}

/**
 * @brief Perform a double-hash search on the hash table using a phone
 *        number as the key, disambiguating hash collisions by reading
 *        back the stored phone number.
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

    /*
     * A DELETED slot is a tombstone: it is a valid insertion point, but
     * it must NOT stop a lookup, because a matching entry may have been
     * probed past it before the deletion. So we remember the first
     * tombstone for the insert path and keep probing for a real match.
     * Only an EMPTY slot ends the probe chain (nothing was ever stored
     * beyond it on this chain).
     */
    HashEntry *first_free = NULL;

    for (uint16_t i = 0; i < table->size; i++)
    {
        uint16_t index = (h1 + i * h2) % table->size;
        HashEntry *cur = &table->htable[index];

        if (cur->state == ENTRY_EMPTY)
        {
#if defined(HOST_BUILD)
            table->collision_count = i;
#endif
            // End of chain: phone not present. Hand back the earlier
            // tombstone if we saw one, otherwise this empty slot.
            *entry = (first_free != NULL) ? first_free : cur;
            return true;
        }

        if (cur->state == ENTRY_DELETED)
        {
            if (first_free == NULL)
            {
                first_free = cur;
            }
            continue; // keep probing past the tombstone
        }

        // ENTRY_OCCUPIED
        if (cur->id == target_hash)
        {
            if (!read_contact(table->storage, cur->sector, &contact))
            {
                continue; // couldn't verify - treat as a miss and keep probing
            }

            if (strcmp(contact.contact.phone, phone) == 0)
            {
#if defined(HOST_BUILD)
                table->collision_count = i;
#endif
                *entry = cur;
                return true;
            }
            // same hash, different phone - keep probing past this slot
        }
    }

    // Walked the whole table with no EMPTY sentinel. A tombstone seen
    // along the way is still a usable insertion point.
    if (first_free != NULL)
    {
        *entry = first_free;
        return true;
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

    // If the entry at the hash index (therefore no sector allocated, allocate a new sector)
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
        sector = entry->sector; // find_hash_phone already confirmed same phone number
    }

    if (!write_contact(table->storage, journal, entry->sector, contact))
    {
        // Only unwind hash-table state for a brand-new entry. An update
        // to an existing contact must not tear down a previously valid
        // entry just because the write failed.
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

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry))
    {
        return false;
    }

    if (entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    return read_contact(table->storage, entry->sector, out);
}

/**
  * @brief  Find a contact's sector index by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to search for
  * @retval Sector index, or UINT16_MAX if not found.
  */
uint16_t hash_find_sector_by_phone(HashTable *table, const char *phone)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || phone == NULL || table->size == 0)
    {
        return UINT16_MAX;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return UINT16_MAX;
    }

    return entry->sector;
}

/**
  * @brief  Find a contact's latest message extent by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to search for
  * @retval Latest message extent index, or UINT16_MAX if not found.
  */
uint16_t hash_find_message_by_phone(HashTable *table, const char *phone)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || phone == NULL || table->size == 0)
    {
        return UINT16_MAX;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return UINT16_MAX;
    }

    return entry->latest_msg_extent;
}

/**
  * @brief  Remove a contact from the hash table (RAM only) by phone number.
  * @param  table: Pointer to the hash table
  * @param  phone: Phone number to remove
  * @param  removed: removed entry
  * @retval true if the contact was removed, false if it was not found
  */
bool hash_remove_by_phone(HashTable *table, const char *phone, HashEntry **removed)
{
    HashEntry *entry;

    if (table == NULL || phone == NULL || removed == NULL)
    {
        return false;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    entry->state = ENTRY_DELETED;
    free_list_free(table->free_stack, entry->sector);
    table->num_elems--;

    *removed = entry;
    return true;
}

/**
 * @brief Remove a contact from the hash table and storage by phone number.
 *
 * @param table Pointer to the hash table.
 * @param journal Pointer to the rollback journal.
 * @param phone Phone number to remove.
 * @param out Removed contact's data.
 * @retval true if the contact was removed, otherwise false.
 */
bool hash_remove_contact_by_phone(HashTable *table, Journal *journal, const char *phone, ContactBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    uint16_t hash = hash_phone(phone);
    uint16_t h1 = hash_primary(hash, table->size);
    uint16_t h2 = hash_secondary(hash, table->size);

    if (!find_hash_phone(table, phone, h1, h2, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    if (!remove_contact(table->storage, journal, entry->sector, out))
    {
        return false;
    }

    /* NOTE: IF FAILURE HERE SD CARD AND RAM OUT OF SYNC */

    entry->state = ENTRY_DELETED;
    free_list_free(table->free_stack, entry->sector);
    table->num_elems--;

    return true;
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
    for (int i = 0; i < table->size; i++)
    {
        table->htable[i].state = ENTRY_EMPTY;
        free_list_free(table->free_stack, table->htable[i].sector);
    }

    table->num_elems = 0;
}


/**
  * @brief insert all contact from the contact sector into the hash table.
  *
  * @param table Pointer to a freshly hash_init'd, empty hash table.
  * @param cSector contact sector read from the SD Card
  * @param contact_sector_base contact sector start conatct pointer (raw_sector_ptr * CONTACT_SECTOR_CAPACITY)
  * @retval true contacts read and inserted from sector successfully
  * @retval false if fail
  */
bool insert_contacts_from_sector(HashTable *table, ContactSector cSector, uint16_t contact_sector_base)
{
    // Get used bit map from header
    uint8_t used_bit_vec = cSector.header.used_bitmap;
    while (used_bit_vec != 0)
    {
        uint8_t pos_in_sector = __builtin_ctz(used_bit_vec);
        uint16_t slot_index = (uint16_t)(contact_sector_base + pos_in_sector);

        const char *phone = cSector.contacts[pos_in_sector].contact.phone;
        uint16_t hash = hash_phone(phone);

        HashEntry *entry;

        if (!hash_find_entry(table, phone, &entry))
        {
            return false; // table full mid-reconstruction
        }

        entry->state = ENTRY_OCCUPIED;
        entry->id = hash;
        entry->sector = slot_index;
        table->num_elems++;
        // Need to also increment free list used count (NOTE: should add
        // func to insert contact ptr to entry directly)
        table->free_stack->used_count++;

        used_bit_vec &= used_bit_vec - 1; // clear lowest set bit
    }


  return true;
}

/**
  * @brief  Reconstruct the in-RAM HashTable from persistent contact data
  *         after a restart, using the usage bitmap so only used sectors
  *         need to be read (rather than scanning the whole SD card).
  *
  * For every contact index the usage bitmap marks as used, this reads
  * the owning ContactSector, re-derives the phone hash from the stored
  * phone number (rather than needing a separately persisted ID), and
  * uses hash_find_entry to claim/activate the correct table entry with
  * that contact's sector index. Any index the bitmap marks as free is
  * pushed back onto the free-list allocator so it matches reality.
  *
  * @param table Pointer to a freshly hash_init'd, empty hash table.
  * @retval true Reconstruction completed successfully.
  * @retval false A storage read failed or the hash table filled up mid-reconstruction.
  */
bool hash_reconstruct_contact(HashTable *table)
{
    if (table == NULL || table->storage == NULL || table->htable == NULL)
    {
        return false;
    }

    ContactSectorBuffer cSector;
    uint32_t last_free_index = 0;

    // iterate over every uint32 in the usage bitmap
    for (uint32_t word = 0; word < USAGE_BITMAP_STORAGE_SIZE; word++)
    {
        uint32_t bits = usage_bitmap[word];

        while (bits != 0)
        {
            uint32_t bit = __builtin_ctz(bits);

            // check that the bit that we are on doesn't go past the total number of sectors allocated to contacts
            if ((bit + word * BITS_PER_ELEMENT) >= CONTACT_MEMORY_SECTOR_SIZE )
            {
                break;
            }

            /*
             * The usage bitmap holds one bit per *physical contact sector*
             * (see mem_layout.h: USAGE_BITMAP_SIZE is sized from
             * TOTAL_DATA_SECTOR_SIZE, and write_contact() sets the bit at
             * index / CONTACT_SECTOR_CAPACITY). The free list / entry->sector
             * work in *contact slot* units, CONTACT_SECTOR_CAPACITY of which
             * pack into one physical sector.
             */
            uint16_t phys_sector = (uint16_t)(word * BITS_PER_ELEMENT + bit);
            uint16_t slot_base = (uint16_t)(phys_sector * CONTACT_SECTOR_CAPACITY);

            // Slots before this sector's first slot are unused - hand back
            // to the allocator so it matches reality.
            // FIXME(reconstruct): this frees whole sectors' worth of slots;
            // unused slots *within* a partially-filled sector are not
            // reclaimed here. Fine while starting from a full free list
            // (every free is a no-op), needs revisiting for empty-init.
            free_list_free_range(table->free_stack, (uint16_t)last_free_index, slot_base);

            // read_contact_sector() divides its arg by CONTACT_SECTOR_CAPACITY,
            // so address it with the first slot of this physical sector.
            if (!read_contact_sector(table->storage, slot_base, &cSector))
            {
                return false;
            }

            // Iterate over every used contact in the sector and add it to
            bool contacts_insert_success = insert_contacts_from_sector(table, cSector.sector, slot_base);
            if (!contacts_insert_success)
            {
                return false;
            }

            bits &= bits - 1; // clear the lowest set bit
            last_free_index = slot_base + CONTACT_SECTOR_CAPACITY;
        }
    }

    // Anything after the last used index in the whole bitmap is free (this only frees up to the FLS capacity)
    free_list_free_range(table->free_stack, (uint16_t)last_free_index,
            (uint16_t)(USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT));

    return true;
}



#if defined (HOST_BUILD)

HashEntry* hash_create_software(void)
{
    return (HashEntry*)calloc(HASH_TABLE_SIZE, sizeof(HashEntry));
}

uint8_t* hash_create_sd_mock(void)
{
    return (uint8_t*)calloc(HASH_TABLE_SIZE, sizeof(Contact));
}

void hash_destroy_software(HashTable* table, uint8_t* sd)
{
    free(table->htable);
    free(sd);
}

#endif
