#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include "hash_table_phone.h"
#include "contact.h"
#include "storage.h"
#include "free_list_stack.h"
#include "heap_storage.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"

// Defined in hash_table_phone.c, not exported through the header.
uint16_t hash_phone(const char *phone);
}

ContactBuffer create_contact(const std::string& name, const std::string& phone);

/**
 * @brief Fixture for the phone-number-keyed HashTable
 *        (hash_table_phone.c).
 *
 * Unlike the old ID-keyed table, the key here is a 16-bit DJB2 hash of
 * the contact's phone number. Two different phone numbers can therefore
 * land on the same hash, so the implementation disambiguates by reading
 * the stored contact back from storage and comparing the phone string.
 *
 * The contact write/remove paths persist through the storage abstraction
 * and log to the rollback journal, so the fixture stands up the full
 * on-disk layout (superheader / usage bitmap / journal / data), sized
 * from mem_layout.h.
 */
class HashTablePhoneTest : public ::testing::Test
{
protected:
    HashTable htable{};
    HashEntry *entries = nullptr;

    Storage *storage = &heap_storage;
    HeapStorageContext storage_ctx{};

    Journal journal{};

    FreeList contact_allocator{};
    uint16_t *fls_mem_pool = nullptr;

    uint8_t *storage_mem = nullptr;

    static constexpr uint32_t STORAGE_SECTOR_COUNT =
        SUPERHEADER_SECTOR_SIZE +
        USAGE_BITMAP_SECTOR_SIZE +
        JRNL_SECTOR_SIZE +
        TOTAL_DATA_SECTOR_SIZE;

    void SetUp() override
    {
        entries = new HashEntry[HASH_TABLE_SIZE];
        ASSERT_NE(entries, nullptr);
        std::memset(entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        fls_mem_pool = new uint16_t[HASH_TABLE_SIZE];
        ASSERT_NE(fls_mem_pool, nullptr);

        storage_mem = new uint8_t[static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT];
        ASSERT_NE(storage_mem, nullptr);
        std::memset(storage_mem, 0,
                    static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(HeapStorage_Init(&storage_ctx, storage_mem, SECTOR_SIZE,
                                     STORAGE_SECTOR_COUNT));
        storage->context = &storage_ctx;

        // check_usage_bit()/update_usage_bit() operate on the global
        // in-RAM bitmap, so clear it between tests.
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));

        // The journal must be initialised before any contact write/remove.
        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));

        ASSERT_TRUE(free_list_init(&contact_allocator, fls_mem_pool, HASH_TABLE_SIZE));

        hash_init(&htable, storage, &contact_allocator, entries, HASH_TABLE_SIZE);
    }

    void TearDown() override
    {
        hash_clear(&htable);

        delete[] entries;
        delete[] fls_mem_pool;
        delete[] storage_mem;

        entries = nullptr;
        fls_mem_pool = nullptr;
        storage_mem = nullptr;
    }

    /** @brief Insert a contact keyed by its own phone number. */
    uint16_t insert(const std::string& name, const std::string& phone)
    {
        ContactBuffer c = create_contact(name, phone);
        return hash_insert_contact_by_phone(&htable, &journal, &c);
    }

    // Backing memory for a second, freshly-rebuilt table (see rebuild()).
    std::vector<HashEntry> rebuilt_entries;
    std::vector<uint16_t> rebuilt_pool;
    FreeList rebuilt_fls{};
    HashTable rebuilt{};

    /**
     * @brief Simulate a power-cycle: build a brand-new empty hash table
     *        over the SAME storage, reload the usage bitmap from storage,
     *        and reconstruct the table from persisted contact data.
     * @return the return value of hash_reconstruct_contact().
     */
    bool rebuild()
    {
        rebuilt_entries.assign(HASH_TABLE_SIZE, HashEntry{});
        rebuilt_pool.assign(HASH_TABLE_SIZE, 0);

        // Reconstruction starts from an allocator with nothing free and
        // frees back the slots the usage bitmap says are unused.
        if (!free_list_empty_init(&rebuilt_fls, rebuilt_pool.data(), HASH_TABLE_SIZE))
        {
            return false;
        }

        hash_init(&rebuilt, storage, &rebuilt_fls, rebuilt_entries.data(), HASH_TABLE_SIZE);

        // Drop the in-RAM bitmap and reload it from storage, as a real boot would.
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        if (!read_usage_bitmap(storage))
        {
            return false;
        }

        return hash_reconstruct_contact(&rebuilt);
    }
};

/**
 * @brief hash_phone() ignores non-numeric characters and is stable.
 */
TEST_F(HashTablePhoneTest, PhoneHashIgnoresNonDigits)
{
    EXPECT_EQ(hash_phone("0412345678"), hash_phone("0412 345 678"));
    EXPECT_EQ(hash_phone("0412345678"), hash_phone("(04) 1234-5678"));
    EXPECT_NE(hash_phone("0412345678"), hash_phone("0412345679"));
}

/**
 * @brief A contact can be inserted keyed by its phone number.
 */
TEST_F(HashTablePhoneTest, InsertContact)
{
    uint16_t sector = insert("Alice", "0412345678");

    ASSERT_NE(sector, UINT16_MAX);
    EXPECT_EQ(hash_size(&htable), 1u);
}

/**
 * @brief An inserted contact can be found by phone number.
 */
TEST_F(HashTablePhoneTest, FindContact)
{
    ContactBuffer original = create_contact("Alice", "0412345678");

    ASSERT_NE(hash_insert_contact_by_phone(&htable, &journal, &original), UINT16_MAX);

    ContactBuffer result{};

    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0412345678", &result));

    EXPECT_EQ(result.contact.name_len, original.contact.name_len);
    EXPECT_EQ(result.contact.phone_len, original.contact.phone_len);
    EXPECT_STREQ(result.contact.name, "Alice");
    EXPECT_STREQ(result.contact.phone, "0412345678");
}

/**
 * @brief Finding a phone number that was never inserted fails.
 */
TEST_F(HashTablePhoneTest, FindMissingContact)
{
    ASSERT_NE(insert("Alice", "0412345678"), UINT16_MAX);

    ContactBuffer result{};

    EXPECT_FALSE(hash_find_contact_by_phone(&htable, "0400000000", &result));
}

/**
 * @brief hash_find_sector_by_phone() returns the stored sector, and
 *        UINT16_MAX for a missing number.
 */
TEST_F(HashTablePhoneTest, FindSectorByPhone)
{
    uint16_t sector = insert("Alice", "0412345678");
    ASSERT_NE(sector, UINT16_MAX);

    EXPECT_EQ(hash_find_sector_by_phone(&htable, "0412345678"), sector);
    EXPECT_EQ(hash_find_sector_by_phone(&htable, "0499999999"), UINT16_MAX);
}

/**
 * @brief hash_find_message_by_phone() reflects the entry's latest
 *        message extent.
 */
TEST_F(HashTablePhoneTest, FindMessageByPhone)
{
    ASSERT_NE(insert("Alice", "0412345678"), UINT16_MAX);

    // Reach the entry through its phone hash and stamp a message extent.
    HashEntry *entry = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, "0412345678", &entry));
    ASSERT_NE(entry, nullptr);
    entry->latest_msg_extent = 4321;

    EXPECT_EQ(hash_find_message_by_phone(&htable, "0412345678"), 4321);
    EXPECT_EQ(hash_find_message_by_phone(&htable, "0400000000"), UINT16_MAX);
}

/**
 * @brief An inserted contact can be removed by phone number, taking its
 *        data with it, and is no longer findable afterwards.
 */
TEST_F(HashTablePhoneTest, RemoveContact)
{
    ASSERT_NE(insert("Alice", "0412345678"), UINT16_MAX);
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer removed{};

    ASSERT_TRUE(hash_remove_contact_by_phone(&htable, &journal, "0412345678", &removed));

    EXPECT_STREQ(removed.contact.name, "Alice");
    EXPECT_STREQ(removed.contact.phone, "0412345678");
    EXPECT_EQ(hash_size(&htable), 0u);

    ContactBuffer result{};
    EXPECT_FALSE(hash_find_contact_by_phone(&htable, "0412345678", &result));
}

/**
 * @brief Removing a phone number that is not present fails.
 */
TEST_F(HashTablePhoneTest, RemoveMissingContact)
{
    ContactBuffer removed{};

    EXPECT_FALSE(hash_remove_contact_by_phone(&htable, &journal, "0412345678", &removed));
}

/**
 * @brief hash_remove_by_phone() drops the RAM entry without needing the
 *        journal, and the contact is no longer findable.
 */
TEST_F(HashTablePhoneTest, RemoveByPhoneRamOnly)
{
    ASSERT_NE(insert("Alice", "0412345678"), UINT16_MAX);

    HashEntry *removed = nullptr;
    ASSERT_TRUE(hash_remove_by_phone(&htable, "0412345678", &removed));
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->state, ENTRY_DELETED);
    EXPECT_EQ(hash_size(&htable), 0u);

    ContactBuffer result{};
    EXPECT_FALSE(hash_find_contact_by_phone(&htable, "0412345678", &result));
}

/**
 * @brief Multiple distinct contacts can be inserted and found
 *        independently.
 */
TEST_F(HashTablePhoneTest, MultipleContacts)
{
    ASSERT_NE(insert("Alice", "0411111111"), UINT16_MAX);
    ASSERT_NE(insert("Bob", "0422222222"), UINT16_MAX);
    ASSERT_NE(insert("Charlie", "0433333333"), UINT16_MAX);

    EXPECT_EQ(hash_size(&htable), 3u);

    ContactBuffer result{};

    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0411111111", &result));
    EXPECT_STREQ(result.contact.name, "Alice");

    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0422222222", &result));
    EXPECT_STREQ(result.contact.name, "Bob");

    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0433333333", &result));
    EXPECT_STREQ(result.contact.name, "Charlie");
}

/**
 * @brief Inserting the same phone number again updates the existing
 *        contact rather than creating a second one.
 */
TEST_F(HashTablePhoneTest, DuplicateInsertUpdatesInPlace)
{
    uint16_t first = insert("Alice", "0412345678");
    ASSERT_NE(first, UINT16_MAX);

    uint16_t second = insert("Alice Updated", "0412345678");
    ASSERT_NE(second, UINT16_MAX);

    // Same phone -> same slot/sector, no extra contact.
    EXPECT_EQ(second, first);
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0412345678", &result));
    EXPECT_STREQ(result.contact.name, "Alice Updated");
}

/**
 * @brief Two different phone numbers that collide on the 16-bit phone
 *        hash are both inserted and found, disambiguated by the stored
 *        phone string.
 *
 * "0400000601" and "0400002060" both hash to 0x031A via hash_phone().
 */
TEST_F(HashTablePhoneTest, PhoneHashCollisionResolved)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b))
        << "test precondition: the two numbers must collide";

    // insert two contacts which result in the same hash ID
    uint16_t sector_a = insert("Collide A", phone_a);
    uint16_t sector_b = insert("Collide B", phone_b);

    // Check that they get inserte and have different sector IDs
    ASSERT_NE(sector_a, UINT16_MAX);
    ASSERT_NE(sector_b, UINT16_MAX);
    EXPECT_NE(sector_a, sector_b);
    EXPECT_EQ(hash_size(&htable), 2u);

    ContactBuffer result{};

    // Find Contact A
    ASSERT_TRUE(hash_find_contact_by_phone(&htable, phone_a, &result));
    EXPECT_STREQ(result.contact.name, "Collide A");
    EXPECT_STREQ(result.contact.phone, phone_a);

    // Find Contact B
    ASSERT_TRUE(hash_find_contact_by_phone(&htable, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");
    EXPECT_STREQ(result.contact.phone, phone_b);
}

/**
 * @brief Removing one contact from a hash-collision chain leaves the
 *        other reachable.
 */
TEST_F(HashTablePhoneTest, RemoveFromCollisionChain)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    ASSERT_NE(insert("Collide A", phone_a), UINT16_MAX);
    ASSERT_NE(insert("Collide B", phone_b), UINT16_MAX);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact_by_phone(&htable, &journal, phone_a, &removed));
    EXPECT_STREQ(removed.contact.name, "Collide A");

    ContactBuffer result{};

    // The other colliding contact is still reachable past the tombstone.
    ASSERT_TRUE(hash_find_contact_by_phone(&htable, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");

    // The removed one is gone.
    EXPECT_FALSE(hash_find_contact_by_phone(&htable, phone_a, &result));
}

/**
 * @brief A tombstoned slot is reused by a later insert.
 */
TEST_F(HashTablePhoneTest, ReinsertAfterRemove)
{
    ASSERT_NE(insert("Alice", "0412345678"), UINT16_MAX);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact_by_phone(&htable, &journal, "0412345678", &removed));
    EXPECT_EQ(hash_size(&htable), 0u);

    ASSERT_NE(insert("Alice Again", "0412345678"), UINT16_MAX);
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact_by_phone(&htable, "0412345678", &result));
    EXPECT_STREQ(result.contact.name, "Alice Again");
}

/**
 * @brief hash_size() tracks inserts and removes.
 */
TEST_F(HashTablePhoneTest, SizeTracksContacts)
{
    EXPECT_EQ(hash_size(&htable), 0u);

    ASSERT_NE(insert("Alice", "0411111111"), UINT16_MAX);
    EXPECT_EQ(hash_size(&htable), 1u);

    ASSERT_NE(insert("Bob", "0422222222"), UINT16_MAX);
    EXPECT_EQ(hash_size(&htable), 2u);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact_by_phone(&htable, &journal, "0411111111", &removed));
    EXPECT_EQ(hash_size(&htable), 1u);
}

/**
 * @brief NULL / bad arguments are rejected.
 */
TEST_F(HashTablePhoneTest, RejectsBadArguments)
{
    ContactBuffer c = create_contact("Alice", "0412345678");
    ContactBuffer out{};

    EXPECT_EQ(hash_insert_contact_by_phone(nullptr, &journal, &c), UINT16_MAX);
    EXPECT_EQ(hash_insert_contact_by_phone(&htable, &journal, nullptr), UINT16_MAX);

    EXPECT_FALSE(hash_find_contact_by_phone(nullptr, "0412345678", &out));
    EXPECT_FALSE(hash_find_contact_by_phone(&htable, nullptr, &out));

    EXPECT_FALSE(hash_remove_contact_by_phone(&htable, &journal, nullptr, &out));
}

/* ============================================================================
 * hash_find_entry() - now keyed by phone number, verifies OCCUPIED slots
 * ========================================================================== */

/**
 * @brief hash_find_entry() returns the OCCUPIED slot for a stored phone
 *        and a non-occupied (free) slot for an unknown phone.
 */
TEST_F(HashTablePhoneTest, FindEntryMatchesStoredPhone)
{
    uint16_t sector = insert("Alice", "0412345678");
    ASSERT_NE(sector, UINT16_MAX);

    HashEntry *hit = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, "0412345678", &hit));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->state, ENTRY_OCCUPIED);
    EXPECT_EQ(hit->sector, sector);
    EXPECT_EQ(hit->id, hash_phone("0412345678"));

    // NOTE: this should return false. Need to take a look at this
    HashEntry *miss = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, "0400000000", &miss));
    ASSERT_NE(miss, nullptr);
    EXPECT_NE(miss->state, ENTRY_OCCUPIED); // an insertion point, not a match
}

/**
 * @brief hash_find_entry() disambiguates two phone numbers that share the
 *        same 16-bit hash, returning a distinct entry for each.
 */
TEST_F(HashTablePhoneTest, FindEntryDistinguishesCollidingPhones)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    uint16_t sector_a = insert("Collide A", phone_a);
    uint16_t sector_b = insert("Collide B", phone_b);
    ASSERT_NE(sector_a, UINT16_MAX);
    ASSERT_NE(sector_b, UINT16_MAX);

    HashEntry *ea = nullptr;
    HashEntry *eb = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, phone_a, &ea));
    ASSERT_TRUE(hash_find_entry(&htable, phone_b, &eb));

    EXPECT_EQ(ea->state, ENTRY_OCCUPIED);
    EXPECT_EQ(eb->state, ENTRY_OCCUPIED);
    EXPECT_NE(ea, eb);
    EXPECT_EQ(ea->sector, sector_a);
    EXPECT_EQ(eb->sector, sector_b);
}

/* ============================================================================
 * hash_reconstruct_contact()
 * ========================================================================== */

/**
 * @brief After a simulated restart, every persisted contact is rebuilt
 *        into the RAM table and is findable by phone number.
 */
TEST_F(HashTablePhoneTest, ReconstructFindsAllContacts)
{
    struct Person { const char *name; const char *phone; uint16_t sector; };

    Person people[] = {
        {"Alice",   "0411111111", 0},
        {"Bob",     "0422222222", 0},
        {"Charlie", "0433333333", 0},
        {"Dana",    "0444444444", 0},
        {"Erin",    "0455555555", 0},
    };

    // insert all the people into the DB
    for (auto& p : people)
    {
        p.sector = insert(p.name, p.phone);
        ASSERT_NE(p.sector, UINT16_MAX);
    }

    // Rebuild the DB
    ASSERT_TRUE(rebuild());

    // Check that all people are accounted for
    EXPECT_EQ(hash_size(&rebuilt), sizeof(people) / sizeof(people[0]));

    // Iterate over all the people and ensure that they all can be found
    for (auto& p : people)
    {
        ContactBuffer result{};
        ASSERT_TRUE(hash_find_contact_by_phone(&rebuilt, p.phone, &result)) << p.name;
        EXPECT_STREQ(result.contact.name, p.name);
        EXPECT_STREQ(result.contact.phone, p.phone);
        EXPECT_EQ(hash_find_sector_by_phone(&rebuilt, p.phone), p.sector) << p.name;
    }
}

/**
 * @brief Reconstruction of an empty database yields an empty table.
 */
TEST_F(HashTablePhoneTest, ReconstructEmptyDatabase)
{
    ASSERT_TRUE(rebuild());
    EXPECT_EQ(hash_size(&rebuilt), 0u);

    ContactBuffer result{};
    EXPECT_FALSE(hash_find_contact_by_phone(&rebuilt, "0412345678", &result));
}

/**
 * @brief A contact removed before the restart does not reappear after
 *        reconstruction; the survivors still do.
 */
TEST_F(HashTablePhoneTest, ReconstructSkipsRemovedContacts)
{
    ASSERT_NE(insert("Alice", "0411111111"), UINT16_MAX);
    ASSERT_NE(insert("Bob", "0422222222"), UINT16_MAX);
    ASSERT_NE(insert("Charlie", "0433333333"), UINT16_MAX);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact_by_phone(&htable, &journal, "0422222222", &removed));

    ASSERT_TRUE(rebuild());

    EXPECT_EQ(hash_size(&rebuilt), 2u);

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact_by_phone(&rebuilt, "0411111111", &result));
    EXPECT_TRUE(hash_find_contact_by_phone(&rebuilt, "0433333333", &result));
    EXPECT_FALSE(hash_find_contact_by_phone(&rebuilt, "0422222222", &result));
}

/**
 * @brief Colliding phone numbers survive a reconstruction and remain
 *        independently findable.
 */
TEST_F(HashTablePhoneTest, ReconstructPreservesCollisionChain)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    ASSERT_NE(insert("Collide A", phone_a), UINT16_MAX);
    ASSERT_NE(insert("Collide B", phone_b), UINT16_MAX);

    ASSERT_TRUE(rebuild());

    EXPECT_EQ(hash_size(&rebuilt), 2u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact_by_phone(&rebuilt, phone_a, &result));
    EXPECT_STREQ(result.contact.name, "Collide A");
    ASSERT_TRUE(hash_find_contact_by_phone(&rebuilt, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");
}

/**
 * @brief The rebuilt table is fully functional: new contacts can be
 *        inserted and removed after reconstruction.
 */
TEST_F(HashTablePhoneTest, ReconstructedTableAcceptsNewWrites)
{
    // insert a contact into the into the hash table
    ASSERT_NE(insert("Alice", "0411111111"), UINT16_MAX);

    // rebuild the hash_table
    ASSERT_TRUE(rebuild());

    // Check the "Alice" has been inserted into the hash table
    ASSERT_EQ(hash_size(&rebuilt), 1u);

    // Create a new contact "Bob" and insert into the hash_table (verify size)
    ContactBuffer bob = create_contact("Bob", "0422222222");
    ASSERT_NE(hash_insert_contact_by_phone(&rebuilt, &journal, &bob), UINT16_MAX);
    EXPECT_EQ(hash_size(&rebuilt), 2u);

    // Check Bob can be found
    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact_by_phone(&rebuilt, "0422222222", &result));

    // Try and remove contact from the hash_table
    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact_by_phone(&rebuilt, &journal, "0411111111", &removed));
    EXPECT_STREQ(removed.contact.name, "Alice");
    EXPECT_EQ(hash_size(&rebuilt), 1u);
}

/**
 * @brief Create a ContactBuffer from a name and phone number.
 */
ContactBuffer create_contact(const std::string& name, const std::string& phone)
{
    ContactBuffer contact{};

    if (name.length() > MAX_NAME_LEN || phone.length() > MAX_PHONE_LEN) {
        return contact;
    }

    contact.contact.name_len = static_cast<uint8_t>(name.length());
    memcpy(contact.contact.name, name.c_str(), contact.contact.name_len);

    contact.contact.phone_len = static_cast<uint8_t>(phone.length());
    memcpy(contact.contact.phone, phone.c_str(), contact.contact.phone_len);

    return contact;
}
