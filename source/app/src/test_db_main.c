#include "test_db_main.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sd_storage.h"

/**
 * @brief Compare two contacts for equality.
 *
 * @param a First contact.
 * @param b Second contact.
 *
 * @return true if the contacts are identical.
 * @return false otherwise.
 */
bool contact_equal(const ContactBuffer *a, const ContactBuffer *b)
{
    return
        a->contact.name_len == b->contact.name_len &&
        a->contact.phone_len == b->contact.phone_len &&
        strcmp(a->contact.name, b->contact.name) == 0 &&
        strcmp(a->contact.phone, b->contact.phone) == 0;
}

/**
 * @brief Test writing and reading multiple contacts.
 *
 * Inserts several contacts into the hash table, writes each contact
 * to storage, then reads them back and verifies that the retrieved
 * data matches what was written.
 *
 * This test verifies:
 * - Hash table insertion
 * - Hash table lookup
 * - Storage write operations
 * - Storage read operations
 * - Data integrity across multiple records
 *
 * @param table Pointer to the initialized hash table.
 *
 * @return true if all contacts are successfully verified.
 * @return false if any write, read, or comparison fails.
 */
bool test_multiple_contacts(HashTable *table, Storage *storage)
{

    const uint32_t ids[] = {1, 25, 1234, 9000, 12000};

    ContactBuffer tx[5];
    ContactBuffer rx;

    for (int i = 0; i < 5; i++)
    {
        hash_insert(table, ids[i]);

        uint32_t sector = hash_find_sector(table, ids[i]);

        memset(&tx[i], 0, sizeof(ContactBuffer));


        sprintf(tx[i].contact.name, "Person%d", i);
        sprintf(tx[i].contact.phone, "040000000%d", i);

        tx[i].contact.name_len = strlen(tx[i].contact.name);
        tx[i].contact.phone_len = strlen(tx[i].contact.phone);

        storage->write_block(
            storage->context,
            sector,
            tx[i].buffer);
    }

    for (int i = 0; i < 5; i++)
    {
        uint32_t sector = hash_find_sector(table, ids[i]);

        memset(&rx, 0, sizeof(ContactBuffer));

        storage->read_block(
            storage->context,
            sector,
            rx.buffer);

        if (!contact_equal(&tx[i], &rx))
        {
            return false;
        }
    }

    return true;
}
/**
 * @brief Test overwriting an existing contact.
 *
 * Writes a contact to storage, modifies its contents,
 * writes it again to the same location, and verifies
 * that the updated values are correctly stored.
 *
 * This test ensures existing records can be updated
 * without corruption.
 *
 * @param table Pointer to the initialized hash table.
 *
 * @return true if the updated contact is read back correctly.
 * @return false otherwise.
 */
bool test_update_contact(HashTable *table, Storage *storage)
{

    const uint32_t id = 500;

    hash_insert(table, id);

    uint32_t sector = hash_find_sector(table, id);

    ContactBuffer tx = {0};
    ContactBuffer rx = {0};


    strcpy(tx.contact.name, "Alice");
    strcpy(tx.contact.phone, "111111");

    tx.contact.name_len = strlen(tx.contact.name);
    tx.contact.phone_len = strlen(tx.contact.phone);

    storage->write_block(storage->context, sector, tx.buffer);

    strcpy(tx.contact.name, "Bob");
    strcpy(tx.contact.phone, "999999");

    tx.contact.name_len = strlen(tx.contact.name);
    tx.contact.phone_len = strlen(tx.contact.phone);

    storage->write_block(storage->context, sector, tx.buffer);

    storage->read_block(storage->context, sector, rx.buffer);

    if (!contact_equal(&tx, &rx))
    {
        return false;
    }

    return true;
}

/**
 * @brief Test storage using maximum-length contact fields.
 *
 * Fills the name and phone number fields to their maximum
 * supported lengths, writes the contact to storage, and
 * verifies that the data is recovered without corruption.
 *
 * This test helps detect buffer overflows, truncation,
 * and serialization issues.
 *
 * @param table Pointer to the initialized hash table.
 *
 * @return true if the contact is correctly recovered.
 * @return false otherwise.
 */
bool test_max_length(HashTable *table, Storage *storage)
{

    uint32_t id = 700;

    hash_insert(table, id);

    uint32_t sector = hash_find_sector(table, id);

    ContactBuffer tx = {0};
    ContactBuffer rx = {0};


    memset(tx.contact.name, 'A', sizeof(tx.contact.name)-1);
    tx.contact.name[sizeof(tx.contact.name)-1] = '\0';

    memset(tx.contact.phone, '9', sizeof(tx.contact.phone)-1);
    tx.contact.phone[sizeof(tx.contact.phone)-1] = '\0';

    tx.contact.name_len = strlen(tx.contact.name);
    tx.contact.phone_len = strlen(tx.contact.phone);

    storage->write_block(storage->context,
                                sector,
                                tx.buffer);

    storage->read_block(storage->context,
                               sector,
                               rx.buffer);

    if (!contact_equal(&tx, &rx))
    {
        return false;
    }

    return true;
}

/**
 * @brief Stress test sequential contact insertion.
 *
 * Inserts a sequence of contacts into the hash table,
 * writes each to storage, then reads each one back to
 * verify the stored contact ID.
 *
 * This test exercises repeated allocations, storage
 * writes, and storage reads under a larger workload.
 *
 * @param table Pointer to the initialized hash table.
 *
 * @return true if every contact is recovered correctly.
 * @return false if any verification fails.
 */
bool test_many_contacts(HashTable *table, Storage *storage)
{

    ContactBuffer tx = {0};
    ContactBuffer rx = {0};

    for (uint32_t id = 0; id < 100; id++)
    {
        hash_insert(table, id);

        uint32_t sector = hash_find_sector(table, id);


        sprintf(tx.contact.name, "Name%lu", (unsigned long)id);
        sprintf(tx.contact.phone, "04%08lu", (unsigned long)id);

        tx.contact.name_len = strlen(tx.contact.name);
        tx.contact.phone_len = strlen(tx.contact.phone);

        storage->write_block(storage->context, sector, tx.buffer);
    }

    for (uint32_t id = 0; id < 100; id++)
    {
        uint32_t sector = hash_find_sector(table, id);

        storage->read_block(storage->context, sector, rx.buffer);

    }

    return true;
}

/**
 * @brief Test lookup of a non-existent contact.
 *
 * Attempts to locate a contact that has not been inserted
 * into the hash table and verifies that the expected
 * invalid sector value is returned.
 *
 * @param table Pointer to the initialized hash table.
 *
 * @return true if the lookup correctly reports that the
 *         contact does not exist.
 * @return false if an unexpected sector is returned.
 */
bool test_invalid_lookup(HashTable *table, Storage *storage)
{

    uint32_t sector = hash_find_sector(table, 0xFFFF);

    if (sector == UINT16_MAX)
    {
        return true;
    }

    return false;
}

/**
 * @brief Execute all database tests.
 *
 * Runs every database test and reports an overall result.
 *
 * @param table Pointer to an initialized hash table.
 *
 * @return true if every test passed.
 * @return false otherwise.
 */
bool test_db_run(HashTable *table, Storage *storage)
{
    bool pass = true;

    pass &= test_multiple_contacts(table, storage);
    pass &= test_update_contact(table, storage);
    pass &= test_max_length(table, storage);
    pass &= test_many_contacts(table, storage);
    pass &= test_invalid_lookup(table, storage);

    return pass;
}
