#include "database.h"
#include "hash_table.h"
#include "message_extent.h"
#include "free_list_stack.h"
#include "storage.h"


// Contact Sizes
#define CONTACT_TABLE_SIZE 14293
#define CONTACT_ALLOCATOR_SIZE CONTACT_TABLE_SIZE

// Message Sizes
// Arbitrary Size allocation ( double contact size atm)
#define MESSAGE_ALLOCATOR_SIZE CONTACT_TABLE_SIZE * 2

/**
 * @brief Initialise the database.
 *
 * Initialises the contact hash table, message extent manager,
 * storage backend and allocators.
 *
 * @param[out] db Database instance.
 * @param[in] storage Storage backend.
 * @param[in] contact_entries Hash table backing array.
 * @param[in] contact_stack Memory for the contact free-list.
 * @param[in] contact_capacity Number of contacts.
 * @param[in] extent_stack Memory for the message extent free-list.
 * @param[in] extent_capacity Number of message extents.
 *
 * @retval true Initialisation successful.
 * @retval false Initialisation failed.
 */
bool database_init(Database *db, Storage storage, HashEntry *entries, uint16_t *contact_fls_mem,
        uint16_t *message_fls_mem)
{
    // Init FLSs
    free_list_init(&(db->contact_allocator), contact_fls_mem, CONTACT_ALLOCATOR_SIZE);
    free_list_init(&(db->message_allocator), message_fls_mem, MESSAGE_ALLOCATOR_SIZE);
    
    // Init Managing Data Structures
    hash_init(&(db->contacts), &(db->contact_allocator), entries, CONTACT_ALLOCATOR_SIZE);
    message_extent_init( &(db->messages), &storage, &(db->message_allocator),
            MESSAGE_ALLOCATOR_SIZE);

    db->contact_storage = storage;


}

/**
 * @brief Create a new contact.
 *
 * @param db Database.
 * @param contact Contact information.
 *
 * @return Assigned true is contact was allocated correctly, else false
 */
bool database_contact_create(Database *db, ContactBuffer *contact) 
{
   // Insert the contact into the hash table
   uint16_t sector_ind = hash_insert(&(db->contacts), contact->contact.offset_id);

   // Check sector allocation
   if (sector_ind == UINT16_MAX) {
       return false;
   }

   // write inserted contact from hash table to sd card
    return db->contact_storage.write_block(db->contact_storage.context, sector_ind, contact->buffer);
}

/**
 * @brief Find a contact.
 */
bool database_contact_get(Database *db, uint16_t id, ContactBuffer *out) 
{
    // Get the sector index from the in RAM hash table
    uint16_t sector_ind = hash_find_sector(&(db->contacts), id);
    
    // read contact block from given sector id
    return db->contact_storage.read_block(db->contact_storage.context, sector_ind, out->buffer);
}

/**
 * @brief Update an existing contact.
 */
bool database_contact_update(Database *db, uint16_t id, ContactBuffer *contact)
{
    // Get the sector index from the in RAM hash table
    uint16_t sector_ind = hash_find_sector(&(db->contacts), id);
    
    // write contact block from given sector id
    return db->contact_storage.write_block(db->contact_storage.context, sector_ind, contact->buffer);

}
