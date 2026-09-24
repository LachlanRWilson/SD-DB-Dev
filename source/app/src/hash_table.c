#include <stdlib.h>
#include <string.h>

#include "hash_table.h"
#include "message.h"
#include "contact.h"
#include "storage.h"
#include "usage_bitmap.h"
#include "free_list_stack.h"
#include "journal.h"

// Return code from checking if the sector phone number is the same as the search phone number
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
  * @brief  Create a hash table
  * @param  table: Hash Table struct being initialised
  * @param storage: storage struct for Heap or SD Card storage
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs)
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
void hash_init(HashTable* table, Storage* storage, FreeList *contact_fstack, FreeList *message_fstack,
               HashEntry* entries, size_t
        size)
{
    table->htable = entries;
    table->storage = storage;
    table->contact_allocator = contact_fstack;
    table->message_allocator = message_fstack;
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
  * @brief  Check that the phone number stored at the contact index of this entry is the same as the search phone #
  *
  * @param  storage: storage struct to access memory
  * @param phone: phone number to be check
  * @param entry: current entry that is being checked
  *
  * @retval status of phone check
  */
PHONE_CHECK check_contact_phone(Storage *storage, const char* phone, HashEntry *entry)
{
    ContactBuffer contact;

    // read contact where this entry points
    if (!read_contact(storage, entry->sector, &contact))
    {
       return CONTACT_READ_ERROR;
    }

    // read the phone number in the sector to the same length as the search length
    size_t query_len = strlen(phone);

    if (query_len == contact.contact.phone_len &&
        strncmp(phone, contact.contact.phone, contact.contact.phone_len) == 0)
    {
        return SAME_PHONE;
    }
    return DIFF_PHONE;
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
bool hash_find_entry(HashTable *table, const char *phone, HashEntry **entry)
{
    uint16_t target_hash = hash_phone(phone);
    uint16_t h1 = hash_primary(target_hash, table->size);
    uint16_t h2 = hash_secondary(target_hash, table->size);
    ContactBuffer contact;

    // The first free entry. Note that a tombstoned entry is free but there may
    // be an actual entry further down the collision chain
    HashEntry *first_free = NULL;

    // Iterate until I find empty spot for entry
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

        // This could be an earlier entry in the collision chain. Therefore must search to the end of the tombstone
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
bool hash_insert_contact(HashTable *table, Journal *journal, ContactBuffer *contact)
{
    HashEntry *entry;
    uint16_t sector;
    bool is_new_entry;

    if (table == NULL || table->storage == NULL || table->contact_allocator == NULL || contact == NULL)
    {
        return false;
    }

    // create hash number using phone number stored in inserting contact
    const char *phone = contact->contact.phone;
    uint16_t hash = hash_phone(phone);

    // Find the entry associated to the hash number
    if (!hash_find_entry(table, phone, &entry) || entry == NULL)
    {
        return false; // table full
    }

    // If the entry at the hash index (therefore no sector allocated, allocate a new sector)
    is_new_entry = (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED);

    if (is_new_entry)
    {
        sector = free_list_allocate(table->contact_allocator);

        if (sector == UINT16_MAX)
        {
            return false; // allocator exhausted
        }

        entry->sector = sector;
        entry->state = ENTRY_OCCUPIED;
        entry->id = hash; // stored hash used for fast re-probing, not a unique key
        entry->latest_msg_extent = UINT16_MAX; // no message chat yet
        table->num_elems++;
    }
    else
    {
        sector = entry->sector; // find_hash_phone already confirmed same phone number
    }

    if (!write_contact(table->storage, journal, entry->sector, contact))
    {
        // undo entry creation for new entry
        if (is_new_entry)
        {
            entry->state = ENTRY_EMPTY;
            entry->id = 0;
            entry->sector = UINT16_MAX;
            free_list_free(table->contact_allocator, sector);
            table->num_elems--;
        }

        return false;
    }

    return true;
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
bool hash_insert_message(HashTable *table, Journal *journal, const char *phone, MessageBuffer *in)
{
    HashEntry *entry;
    uint16_t sector;
    STRG_RET ret;
    bool is_new_entry;

    if (table == NULL || table->storage == NULL || table->contact_allocator == NULL || in == NULL || phone == NULL)
    {
        return false;
    }

    // create hash number using phone number stored in inserting contact
    uint16_t hash = hash_phone(phone);

    // Find the entry associated to the hash number
    if (!hash_find_entry(table, phone, &entry) || entry == NULL)
    {
        return false; // table full
    }

    // If the entry at the hash index (therefore no sector allocated, allocate a new sector)
    is_new_entry = (entry->state == ENTRY_EMPTY || entry->state == ENTRY_DELETED);

    if (is_new_entry)
    {
        // add new contact
        sector = free_list_allocate(table->contact_allocator);

        if (sector == UINT16_MAX)
        {
            return false; // allocator exhausted
        }

        // create an empty contact with a phone number
        ContactBuffer new_contact = create_contact("", phone);

        // write the new empty contact to the SD card
        ret = write_contact(table->storage, journal, sector, &new_contact);

        if (ret != STRG_OK)
        {
            return false;
        }

        entry->sector = sector;
        entry->state = ENTRY_OCCUPIED;
        entry->id = hash; // stored hash used for fast re-probing, not a unique key
        entry->latest_msg_extent = UINT16_MAX; // no message chat yet
        table->num_elems++;
    }

    // The contact may already exist (e.g. inserted via hash_insert_contact)
    // without ever having received a message, so the chat-empty check must
    // be independent of is_new_entry.
    if (entry->latest_msg_extent == UINT16_MAX)
    {
        // Allocate new sector for message
        sector = free_list_allocate(table->message_allocator);

        if (sector == UINT16_MAX)
        {
            return false; // allocator exhausted
        }

        // write message to the sd card
        ret = write_new_message_sector(table->storage, journal, phone, sector, in);

        if (ret != STRG_OK)
        {
            return false;
        }
        entry->latest_msg_extent = sector;

    } else { // otherwise write message to current message sector

        ret = write_message(table->storage, journal, entry->latest_msg_extent, in);

        // If the currect sector is full of messages create a new one in the linked list
        if (ret == STRG_FULL)
        {
            uint16_t next_sector = free_list_allocate(table->message_allocator);

            // create a next message sector at allocated index and update previous message sector to point to next sector
            ret = write_next_message_sector(table->storage, journal, phone, entry->latest_msg_extent, next_sector, in);

            if (ret != STRG_OK)
            {
                return false;
            }

            // update entry to point at the latest extent
            entry->latest_msg_extent = next_sector;

        } else if (ret != STRG_OK) {
            return false;
        }
    }

    return true;
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
bool hash_find_contact(HashTable *table, const char *phone, ContactBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    if (!hash_find_entry(table, phone, &entry))
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
 * @brief Get the secture index of the nth contact in the hash table
 *
 * @param table Pointer to the hash table.
 * @param n number of contacts to be read
 * @retval contact index number
 * @retval UINT16_MAX if error.
 */
uint16_t get_nth_contact_index(HashTable *table, int n)
{
    STRG_RET ret;
    ContactSectorBuffer cSector;
    ContactSectorHeader cHeader;
    int contact_count = 0;
    int first_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT(CONTACT_DATA_START_SECTOR);
    int first_contact_usage_bit = USAGE_BITMAP_FIND_BIT(CONTACT_DATA_START_SECTOR);
    int last_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT(CONTACT_DATA_START_SECTOR + CONTACT_MEMORY_SECTOR_SIZE);
    uint32_t bits = usage_bitmap[first_contact_usage_elem];

    // if the first bit of the work is not a message, clear bits that are not contacts
    if (first_contact_usage_bit > 0)
    {
            bits &= (~0u) << (first_contact_usage_bit - 1);
    }

    // iterate over every uint32 in the usage bitmap
    for (uint32_t word = first_contact_usage_elem; word <= last_contact_usage_elem ; word++)
    {
        uint32_t bits = usage_bitmap[word];

        while (bits != 0)
        {

            uint32_t bit = __builtin_ctz(bits);

            // if bit is larger than contact sector indexes
            if ((bit + word * BITS_PER_ELEMENT) >= (CONTACT_DATA_START_SECTOR + CONTACT_MEMORY_SECTOR_SIZE))
            {
                break;
            }

            // need to get physical sector and the slot in the sector
            uint16_t phys_sector = (uint16_t)(word * BITS_PER_ELEMENT + bit);
            uint16_t slot_base = (uint16_t)(phys_sector * CONTACT_SECTOR_CAPACITY);

            ret = read_contact_sector(table->storage, phys_sector, &cSector);
            if (ret != STRG_OK)
            {
                return ret;
            }

            cHeader = cSector.sector.header;

            // count the number of set bits in the header
            int sector_contact_count = __builtin_popcount(cHeader.used_bitmap);

            if (sector_contact_count == 0)
            {
                return UINT16_MAX;
            }

            // If the nth contact is within this sector, search the header for its offset.
            // contact_count is the number of contacts in all sectors before this one, so
            // (n - contact_count) is the 0-indexed position of the nth contact within it.
            if (contact_count + sector_contact_count > n)
            {
                uint16_t sec_bit_ind = get_nth_set_bit(cHeader.used_bitmap, n - contact_count);

                if (sec_bit_ind == UINT16_MAX)
                {
                    return UINT16_MAX;
                }

                return sec_bit_ind + slot_base;
            }
            contact_count += sector_contact_count;
            bits &= bits - 1;
        }
    }
    return UINT16_MAX;
}

/**
 * @brief Find and read a contact by phone number.
 *
 * @param table Pointer to the hash table.
 * @param start start contact number
 * @param n number of contacts to be read
 * @param out Pointer to array of contacts
 * @retval true if a matching contact was found and read successfully.
 * @retval false otherwise.
 */
STRG_RET hash_get_contact_list(HashTable *table, int start, int n, ContactBuffer *out)
{

    STRG_RET ret;

    int contact_count = 0;
    ContactSectorBuffer cSector;
    int first_sector = 1;
    // The starting word for contact sector
    uint16_t start_contact_index = get_nth_contact_index(table, start);

    uint8_t start_pos_in_sector = start_contact_index % CONTACT_SECTOR_CAPACITY;

    int start_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT((CONTACT_DATA_START_SECTOR + start_contact_index) / CONTACT_SECTOR_CAPACITY);
    int start_contact_usage_bit = USAGE_BITMAP_FIND_BIT((CONTACT_DATA_START_SECTOR + start_contact_index) / CONTACT_SECTOR_CAPACITY);
    int last_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT(CONTACT_DATA_START_SECTOR + CONTACT_MEMORY_SECTOR_SIZE);


    for (uint32_t word = start_contact_usage_elem; word <= last_contact_usage_elem; word++)
    {
        // copy bits to another variable
        uint32_t bits = usage_bitmap[word];

        // if the start bit is not the first bit, clear all the bits before the start bit
        if ((word == start_contact_usage_elem) && (start_contact_usage_bit > 0))
        {
            clear_bits_to_n_u32(&bits, start_contact_usage_bit);
        }


        while (bits != 0)
        {
            uint32_t bit = __builtin_ctz(bits);

            if ((bit + word * BITS_PER_ELEMENT) >= CONTACT_DATA_START_SECTOR + CONTACT_MEMORY_SECTOR_SIZE)
            {
                break;
            }

            ret = read_contact_sector(table->storage, (bit + word * BITS_PER_ELEMENT), &cSector);
            if (ret != STRG_OK)
            {
                return ret;
            }

            // only apply bit clearing to the first header
            if (first_sector)
            {
                first_sector = 0;
                clear_bits_to_n_u8(&cSector.sector.header.used_bitmap, start_pos_in_sector);
            }
            contact_count += read_n_contacts_in_sector(table->storage, cSector.sector, n - contact_count, &out[contact_count]);

            if (contact_count >= n)
            {
                break;
            }
            bits &= bits - 1;
        }
    }

    return STRG_OK;
}


/**
  * @brief  Find a contacts latest message
  * @param  table: Pointer to the hash table
  * @param phone: phone number the message is associated with
  * @retval Pointer to the matching message extent, or NULL if not found
  */
bool hash_find_message(HashTable *table, const char *phone, MessageBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    if (!hash_find_entry(table, phone, &entry))
    {
        return false;
    }

    if (entry->state != ENTRY_OCCUPIED || entry->latest_msg_extent == UINT16_MAX)
    {
        return false;
    }

    // read the latest message
    return read_message(table->storage, entry->latest_msg_extent, 0, out);
}


/**
  * @brief  Find n number of messages from a contact
  * @param  table: Pointer to the hash table
  * @param phone: phone number the message is associated with
  * @param n: number of messages read from latest backwards
  * @param out: pointer to message buffer array
  * @retval the number of messages read from the sd card, -1 if fault
  */
int hash_find_n_message(HashTable *table, const char *phone, int n, MessageBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    if (!hash_find_entry(table, phone, &entry))
    {
        return false;
    }

    if (entry->state != ENTRY_OCCUPIED || entry->latest_msg_extent == UINT16_MAX)
    {
        return false;
    }

    // read messages from latest backwards
    return read_n_messages(table->storage, entry->latest_msg_extent, n, out);
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
bool hash_remove_contact(HashTable *table, Journal *journal, const char *phone, ContactBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    if (!hash_find_entry(table, phone, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    if (!remove_contact(table->storage, journal, table->contact_allocator, entry->sector, out))
    {
        return false;
    }

    entry->sector = UINT16_MAX;
    entry->state = ENTRY_DELETED;
    table->num_elems--;

    return true;
}

bool hash_remove_message(HashTable *table, Journal *journal, const char *phone, MessageBuffer *out)
{
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
            out == NULL || phone == NULL || table->size == 0)
    {
        return false;
    }

    if (!hash_find_entry(table, phone, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    if (!remove_message_chat(table->storage, journal, table->message_allocator, entry->latest_msg_extent))
    {
        return false;
    }

    entry->latest_msg_extent = UINT16_MAX;

    return true;
}


/**
  * @brief  Remove an entry from the hash table. That include removed the contact and message chat from the SD card
  * @param  table: Pointer to the hash table
  * @param  phone: phone number of entry that is being removed
  * @param removed: output, set to the removed entry (may be NULL if the caller doesn't need it)
  * @retval true if the entry, contact and message chat was removed, false if not found or not removed properly
  */
bool hash_remove(HashTable *table, Journal *journal, const char *phone, HashEntry **removed)
{
    ContactBuffer cOut;
    HashEntry *entry;

    if (table == NULL || table->htable == NULL || table->storage == NULL ||
        phone == NULL || table->size == 0) {
      return false;
    }

    // find hash entry that is being removed
    if (!hash_find_entry(table, phone, &entry) || entry->state != ENTRY_OCCUPIED)
    {
        return false;
    }

    // remove contacts
    bool contact_removed = remove_contact(table->storage, journal, table->contact_allocator, entry->sector, &cOut);
    bool message_chat_removal = remove_message_chat(table->storage, journal, table->message_allocator, entry->latest_msg_extent);

    // check both contact and message change has been removed
    if (!contact_removed || !message_chat_removal) {
        return false;
    }

    // decrease the number of hash entries in the table
    entry->state = ENTRY_DELETED;
    table->num_elems--;

    if (removed != NULL)
    {
        *removed = entry;
    }

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
    // reset allocators
    free_list_reset(table->contact_allocator);
    free_list_reset(table->message_allocator);

    // wipe entire hash table
    memset(table->htable, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

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
    uint32_t last_free_index = 0;
    while (used_bit_vec != 0)
    {
        uint8_t pos_in_sector = __builtin_ctz(used_bit_vec);
        uint16_t slot_index = (uint16_t)(contact_sector_base + pos_in_sector);

        // Free from the last free index
        free_list_free_range(table->contact_allocator, (uint16_t)last_free_index, slot_index);

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
        entry->latest_msg_extent = UINT16_MAX; // no message chat yet; set by hash_reconstruct_message if one exists
        table->num_elems++;
        // Need to also increment free list used count (NOTE: should add
        // func to insert contact ptr to entry directly)
        table->contact_allocator->used_count++;

        used_bit_vec &= used_bit_vec - 1; // clear lowest set bit
    }
        // free from last contact index to end of the sector
        last_free_index = CONTACT_SECTOR_CAPACITY;


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

    // The starting word for contact sector
    int first_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT(CONTACT_DATA_START_SECTOR);
    int first_contact_usage_bit = USAGE_BITMAP_FIND_BIT(CONTACT_DATA_START_SECTOR);

    // Get the last uint32_t in the usage bitmap which stores a contact sector usage bit
    int last_contact_usage_elem = USAGE_BITMAP_FIND_ELEMENT(CONTACT_DATA_START_SECTOR + CONTACT_MEMORY_SECTOR_SIZE);


    // iterate over every uint32 in the usage bitmap
    for (uint32_t word = first_contact_usage_elem; word <= last_contact_usage_elem ; word++)
    {

        uint32_t bits = usage_bitmap[word];

        // clear all non-contact bits that are in this work before contact usage sector bits start
        if ((word == first_contact_usage_elem) && (first_contact_usage_bit > 0))
        {
            clear_bits_to_n_u32(&bits, first_contact_usage_bit);
        }


        while (bits != 0)
        {
            uint32_t bit = __builtin_ctz(bits);

            // check that the bit that we are on doesn't go past the total number of sectors allocated to contacts
            if ((bit + word * BITS_PER_ELEMENT) >= CONTACT_MEMORY_SECTOR_SIZE )
            {
                break;
            }

            // need to get physical sector and the slot in the sector
            uint16_t phys_sector = (uint16_t)(word * BITS_PER_ELEMENT + bit);
            uint16_t slot_base = (uint16_t)(phys_sector * CONTACT_SECTOR_CAPACITY);

            free_list_free_range(table->contact_allocator, (uint16_t)last_free_index, slot_base);

            // read_contact_sector() divides its arg by CONTACT_SECTOR_CAPACITY,
            // so address it with the first slot of this physical sector.
            STRG_RET ret = read_contact_sector(table->storage, phys_sector, &cSector);
            if (ret == STRG_FAIL)
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
    free_list_free_range(table->contact_allocator, (uint16_t)last_free_index,
            (uint16_t)(USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT));

    return true;
}


bool insert_message_from_sector(HashTable *table, Journal *journal, MessageSectorBuffer *mSector, uint16_t index)
{
    const char* phone = mSector->var.header.phone;
    uint16_t hash = hash_phone(phone);
    STRG_RET ret;

    HashEntry *entry;

    if (!hash_find_entry(table, phone, &entry))
    {
        return false; // table full mid-reconstruction
    }

    // If there is a message without a contact (this should never happen as contact are read first)
    if (entry->state != ENTRY_OCCUPIED)
    {
        // create an empty contact with a phone number
        ContactBuffer new_contact = create_contact("", phone);
        uint16_t sector = free_list_allocate(table->contact_allocator);

        // No more contact sector available
        if (sector == UINT16_MAX)
        {
            return false;
        }

        // write the new empty contact to the SD card
        ret = write_contact(table->storage, journal, sector, &new_contact);

        if (ret != STRG_OK)
        {
            return false;
        }
    entry->sector = sector;
    entry->state = ENTRY_OCCUPIED;
    entry->id = hash;
    table->num_elems++;
    }

    // Need to also increment free list used count (NOTE: should add
    // func to insert contact ptr to entry directly)
    entry->latest_msg_extent = index;
    table->message_allocator->used_count++;

    return true;
}

bool hash_reconstruct_message(HashTable *table, Journal *journal)
{
    MessageSectorBuffer mSector;
    uint32_t last_free_sector = 0;

    // The starting word for message sector
    int first_message_usage_elem = USAGE_BITMAP_FIND_ELEMENT(MESSAGE_DATA_START_SECTOR);
    int first_message_usage_bit = USAGE_BITMAP_FIND_BIT(MESSAGE_DATA_START_SECTOR);

    // Get the last uint32_t which stores a message sector usage bit
    int last_message_usage_elem = USAGE_BITMAP_FIND_ELEMENT(MESSAGE_SECTOR_SIZE + MESSAGE_SECTOR_SIZE);



    for (uint32_t word = first_message_usage_elem; word <= last_message_usage_elem; word++)
    {
        uint32_t bits = usage_bitmap[word];

        // if the first bit of the work is not a message, clear bits that are not messages
        if ((word == first_message_usage_elem) && (first_message_usage_bit > 0))
        {
            clear_bits_to_n_u32(&bits, first_message_usage_bit);
        }

        while (bits != 0)
        {
            uint32_t bit = __builtin_ctz(bits);

            // check that the bit that we are on doesn't go past the total number of sectors allocated to contacts
            if ((bit + word * BITS_PER_ELEMENT) >= (MESSAGE_SECTOR_SIZE + MESSAGE_DATA_START_SECTOR))
            {
                break;
            }

            // get the physical sector in the message data memory block
            uint16_t phys_sector = (uint16_t)(word * BITS_PER_ELEMENT + bit);

            free_list_free_range(table->message_allocator, (uint16_t)last_free_sector, phys_sector);

            // read the message sector and determine if it is the latest message
            STRG_RET ret = read_message_sector(table->storage, phys_sector, &mSector);

            if (ret != STRG_OK)
            {
                return false;
            }

            // Only add the latest message in the message linked list
            if (mSector.var.header.next == UINT16_MAX)
            {
                // insert the latest message from message sector into hash entry
               if (!insert_message_from_sector(table, journal, &mSector, phys_sector))
                {
                    return false;
                }
            }
        }
    }

    return true;

}

bool hash_cleanup(HashTable *table) {

    // break the hash table
    hash_clear(table);

    bool reconstruction_success = hash_reconstruct_contact(table);

    if (!reconstruction_success) {
        return false;
    }

    // hash_reconstruct_message(table); <- need to implement
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
