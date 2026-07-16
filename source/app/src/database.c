#include "database.h"

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
bool database_init(Database *db, Storage *storage, HashEntry *contact_entries, uint32_t
        *contact_stack, uint32_t contact_capacity, uint32_t *extent_stack, uint32_t
        extent_capacity)
{

}
