#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

extern "C"
{
#include "hash_table.h"
#include "contact.h"
#include "storage.h"
#include "free_list_stack.h"
#include "heap_storage.h"
}

ContactBuffer create_contact(const std::string& name, const std::string& phone);

class HashTableTest : public ::testing::Test
{
protected:
    // Hash Table Struct
    HashTable htable; 

    // Hash entries pointer
    HashEntry *entries = nullptr;

    // Storage Struct (heap storage)
    Storage *storage = &heap_storage;

    HeapStorageContext *storage_ctx = nullptr;

    // Contact allocator
    FreeList *contact_allocator = nullptr;

    // Free List Stack
    uint16_t *fls_mem_pool = nullptr;

    uint8_t *contact_mem_pool = nullptr;


    void SetUp() override
    {
        // allocate heap memory for hash htable
        entries = new HashEntry[HASH_TABLE_SIZE];

        // Check the entries is not still a null pointer
        ASSERT_NE(entries, nullptr);

        // Free List Stack memory pool
        fls_mem_pool = new uint16_t[HASH_TABLE_SIZE];

        // Contact Memory Pool (SD Card Mock) Create in size of contact sectors
        contact_mem_pool = new uint8_t[sizeof(ContactSector) * (HASH_TABLE_SIZE /
            CONTACT_SECTOR_CAPACITY)]; 

        // Initialise heap storage (SD Cark Mock)
        ASSERT_TRUE(HeapStorage_Init( storage_ctx, contact_mem_pool, sizeof(ContactSector),
                    HASH_TABLE_SIZE / CONTACT_SECTOR_CAPACITY));

        // Intialise free_list_init
        ASSERT_TRUE(free_list_init(contact_allocator, fls_mem_pool, HASH_TABLE_SIZE));


        // Initialise Hash Table
        hash_init(&htable, storage, contact_allocator, entries, HASH_TABLE_SIZE);



    }

    void TearDown() override
    {
        // delete hash htable allocated memory
        hash_clear(&htable);

        // Reclaim heap memeory
        delete[] entries;
        delete[] fls_mem_pool;
        delete[] contact_mem_pool;
        entries = nullptr;
        fls_mem_pool = nullptr;
        contact_mem_pool = nullptr;

    }

};

/**
 * @brief Verify a contact can be inserted.
 */
TEST_F(HashTableTest, InsertContact)
{
    const uint16_t id = 100;

    ContactBuffer contact =
        create_contact(
            "Alice",
            "0412345678"
        );

    uint16_t sector = hash_insert_contact( &htable, id, &contact);

    ASSERT_NE(sector, UINT16_MAX);
}


/**
 * @brief Verify an inserted contact can be found.
 */
TEST_F(HashTableTest, FindContact)
{
    // Create contact id
    const uint16_t id = 100;

    // create contact
    ContactBuffer contact = create_contact( "Alice", "0412345678");

    ASSERT_NE( hash_insert_contact( &htable, id, &contact), UINT16_MAX);

    ContactBuffer result{};

    ASSERT_TRUE( hash_find_contact( &htable, id, &result));

    EXPECT_EQ( result.contact.name_len, contact.contact.name_len);

    EXPECT_EQ( result.contact.phone_len, contact.contact.phone_len);

    EXPECT_STREQ( result.contact.name, contact.contact.name);

    EXPECT_STREQ( result.contact.phone, contact.contact.phone);
}


/**
 * @brief Verify finding a contact that does not exist fails.
 */
TEST_F(HashTableTest, FindMissingContact)
{
    ContactBuffer result{};

    EXPECT_FALSE(
        hash_find_contact(
            &htable,
            123,
            &result
        )
    );
}


/**
 * @brief Verify an inserted contact can be removed.
 */
TEST_F(HashTableTest, RemoveContact)
{
    const uint16_t id = 100;

    ContactBuffer contact =
        create_contact(
            "Alice",
            "0412345678"
        );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id,
            &contact
        ),
        UINT16_MAX
    );

    ContactBuffer removed{};

    ASSERT_TRUE(
        hash_remove_contact(
            &htable,
            id,
            &removed
        )
    );

    EXPECT_STREQ(
        removed.contact.name,
        "Alice"
    );

    EXPECT_STREQ(
        removed.contact.phone,
        "0412345678"
    );

    // Contact should no longer be findable
    ContactBuffer result{};

    EXPECT_FALSE(
        hash_find_contact(
            &htable,
            id,
            &result
        )
    );
}


/**
 * @brief Verify removing a contact that does not exist fails.
 */
TEST_F(HashTableTest, RemoveMissingContact)
{
    ContactBuffer removed{};

    EXPECT_FALSE(
        hash_remove_contact(
            &htable,
            123,
            &removed
        )
    );
}


/**
 * @brief Verify multiple contacts can be inserted and found independently.
 */
TEST_F(HashTableTest, MultipleContacts)
{
    const uint16_t alice_id = 1;
    const uint16_t bob_id = 2;
    const uint16_t charlie_id = 3;

    ContactBuffer alice =
        create_contact(
            "Alice",
            "0411111111"
        );

    ContactBuffer bob =
        create_contact(
            "Bob",
            "0422222222"
        );

    ContactBuffer charlie =
        create_contact(
            "Charlie",
            "0433333333"
        );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            alice_id,
            &alice
        ),
        UINT16_MAX
    );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            bob_id,
            &bob
        ),
        UINT16_MAX
    );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            charlie_id,
            &charlie
        ),
        UINT16_MAX
    );

    ContactBuffer result{};

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            alice_id,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Alice"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0411111111"
    );

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            bob_id,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Bob"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0422222222"
    );

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            charlie_id,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Charlie"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0433333333"
    );
}


/**
 * @brief Verify inserting the same ID twice does not create
 *        a second contact.
 */
TEST_F(HashTableTest, DuplicateInsert)
{
    const uint16_t id = 100;

    ContactBuffer contact =
        create_contact(
            "Alice",
            "0412345678"
        );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id,
            &contact
        ),
        UINT16_MAX
    );

    ContactBuffer duplicate =
        create_contact(
            "Alice Duplicate",
            "0499999999"
        );

    EXPECT_EQ(
        hash_insert_contact(
            &htable,
            id,
            &duplicate
        ),
        UINT16_MAX
    );

    // Original contact should still be present
    ContactBuffer result{};

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            id,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Alice"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0412345678"
    );
}


/**
 * @brief Verify contacts with colliding IDs can both be
 *        inserted and found.
 */
TEST_F(HashTableTest, CollisionHandling)
{
    const uint16_t id1 = 10;

    const uint16_t id2 =
        static_cast<uint16_t>(
            id1 + HASH_TABLE_SIZE
        );

    ContactBuffer contact1 =
        create_contact(
            "Alice",
            "0411111111"
        );

    ContactBuffer contact2 =
        create_contact(
            "Bob",
            "0422222222"
        );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id1,
            &contact1
        ),
        UINT16_MAX
    );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id2,
            &contact2
        ),
        UINT16_MAX
    );

    ContactBuffer result{};

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            id1,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Alice"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0411111111"
    );

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            id2,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Bob"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0422222222"
    );
}


/**
 * @brief Verify removing one contact from a collision chain
 *        does not prevent the other contact from being found.
 */
TEST_F(HashTableTest, RemoveCollisionChain)
{
    const uint16_t id1 = 10;

    const uint16_t id2 =
        static_cast<uint16_t>(
            id1 + HASH_TABLE_SIZE
        );

    ContactBuffer contact1 =
        create_contact(
            "Alice",
            "0411111111"
        );

    ContactBuffer contact2 =
        create_contact(
            "Bob",
            "0422222222"
        );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id1,
            &contact1
        ),
        UINT16_MAX
    );

    ASSERT_NE(
        hash_insert_contact(
            &htable,
            id2,
            &contact2
        ),
        UINT16_MAX
    );

    // Remove first contact in collision chain
    ContactBuffer removed{};

    ASSERT_TRUE(
        hash_remove_contact(
            &htable,
            id1,
            &removed
        )
    );

    EXPECT_STREQ(
        removed.contact.name,
        "Alice"
    );

    EXPECT_STREQ(
        removed.contact.phone,
        "0411111111"
    );

    // Second contact must still be findable
    ContactBuffer result{};

    ASSERT_TRUE(
        hash_find_contact(
            &htable,
            id2,
            &result
        )
    );

    EXPECT_STREQ(
        result.contact.name,
        "Bob"
    );

    EXPECT_STREQ(
        result.contact.phone,
        "0422222222"
    );

    // First contact must no longer be findable
    EXPECT_FALSE(
        hash_find_contact(
            &htable,
            id1,
            &result
        )
    );
}

/**
  * @brief  Create a Contact
  * @param  table: Hash Table struct being initialised
  * @param  storage: 
  * @param  fstacks: pointer to array of FLSs (allowing multiple FLSs) 
  * @param  entries: In RAM storage of hash table entries
  * @param  size: number of elements in hash table
  */
ContactBuffer create_contact(const std::string& name, const std::string& phone)
{
    ContactBuffer contact{};

    // Check phone and name len
    if (name.length() > MAX_NAME_LEN || phone.length() > MAX_PHONE_LEN) {
        return contact;
    }

    contact.contact.name_len = static_cast<uint8_t>(name.length());

    memcpy(contact.contact.name, name.c_str(), contact.contact.name_len);

    contact.contact.phone_len = static_cast<uint8_t>(phone.length());
    memcpy(contact.contact.phone, phone.c_str(), contact.contact.phone_len);

    return contact;
}
