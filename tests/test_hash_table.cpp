#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include "hash_table.h"
#include "contact.h"
#include "message.h"
#include "storage.h"
#include "free_list_stack.h"
#include "heap_storage.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"

// Defined in hash_table.c, not exported through the header.
uint16_t hash_phone(const char *phone);
}

static ContactBuffer make_contact(const std::string& name, const std::string& phone);

/**
 * @brief Fixture for the phone-number-keyed HashTable (hash_table.c),
 *        covering both the contact store and the message chat store.
 *
 * The key is a 16-bit DJB2 hash of the contact's phone number. Two
 * different phone numbers can land on the same hash, so the
 * implementation disambiguates by reading the stored contact back from
 * storage and comparing the phone string.
 *
 * Both the contact and message write/remove paths persist through the
 * storage abstraction and log to the rollback journal, so the fixture
 * stands up the full on-disk layout (superheader / usage bitmap /
 * journal / data), sized from mem_layout.h.
 */
class HashTableTest : public ::testing::Test
{
protected:
    HashTable htable{};
    HashEntry *entries = nullptr;

    Storage *storage = &heap_storage;
    HeapStorageContext storage_ctx{};

    Journal journal{};

    FreeList contact_allocator{};
    FreeList message_allocator{};
    uint16_t *contact_fls_pool = nullptr;
    uint16_t *message_fls_pool = nullptr;

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

        contact_fls_pool = new uint16_t[HASH_TABLE_SIZE];
        ASSERT_NE(contact_fls_pool, nullptr);

        message_fls_pool = new uint16_t[TOTAL_MESSAGE_SECTOR_SIZE];
        ASSERT_NE(message_fls_pool, nullptr);

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

        // The journal must be initialised before any contact/message write/remove.
        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));

        ASSERT_TRUE(free_list_init(&contact_allocator, contact_fls_pool, HASH_TABLE_SIZE));
        ASSERT_TRUE(free_list_init(&message_allocator, message_fls_pool, TOTAL_MESSAGE_SECTOR_SIZE));

        hash_init(&htable, storage, &contact_allocator, &message_allocator, entries, HASH_TABLE_SIZE);
    }

    void TearDown() override
    {
        hash_clear(&htable);

        delete[] entries;
        delete[] contact_fls_pool;
        delete[] message_fls_pool;
        delete[] storage_mem;

        entries = nullptr;
        contact_fls_pool = nullptr;
        message_fls_pool = nullptr;
        storage_mem = nullptr;
    }

    /** @brief Insert a contact keyed by its own phone number. */
    bool insert(const std::string& name, const std::string& phone)
    {
        ContactBuffer c = make_contact(name, phone);
        return hash_insert_contact(&htable, &journal, &c);
    }

    /** @brief Look up the sector a phone number's entry currently points at. */
    uint16_t sector_for(const std::string& phone)
    {
        HashEntry *entry = nullptr;
        if (!hash_find_entry(&htable, phone.c_str(), &entry) || entry->state != ENTRY_OCCUPIED)
        {
            return UINT16_MAX;
        }
        return entry->sector;
    }

    /** @brief Append a message to a phone number's chat (auto-creates the contact). */
    bool send(const std::string& phone, uint16_t timestamp, bool direction, const std::string& text)
    {
        MessageBuffer m = create_message(timestamp, direction, const_cast<char *>(text.c_str()));
        return hash_insert_message(&htable, &journal, phone.c_str(), &m);
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

        hash_init(&rebuilt, storage, &rebuilt_fls, &message_allocator, rebuilt_entries.data(), HASH_TABLE_SIZE);

        // Drop the in-RAM bitmap and reload it from storage, as a real boot would.
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
        if (!read_usage_bitmap(storage))
        {
            return false;
        }

        return hash_reconstruct_contact(&rebuilt);
    }
};

/* ============================================================================
 * hash_phone()
 * ========================================================================== */

/**
 * @brief hash_phone() ignores non-numeric characters and is stable.
 */
TEST_F(HashTableTest, PhoneHashIgnoresNonDigits)
{
    EXPECT_EQ(hash_phone("0412345678"), hash_phone("0412 345 678"));
    EXPECT_EQ(hash_phone("0412345678"), hash_phone("(04) 1234-5678"));
    EXPECT_NE(hash_phone("0412345678"), hash_phone("0412345679"));
}

/* ============================================================================
 * Contacts: insert / find / remove
 * ========================================================================== */

/**
 * @brief A contact can be inserted keyed by its phone number.
 */
TEST_F(HashTableTest, InsertContact)
{
    EXPECT_TRUE(insert("Alice", "0412345678"));
    EXPECT_EQ(hash_size(&htable), 1u);
}

/**
 * @brief An inserted contact can be found by phone number.
 */
TEST_F(HashTableTest, FindContact)
{
    ContactBuffer original = make_contact("Alice", "0412345678");

    ASSERT_TRUE(hash_insert_contact(&htable, &journal, &original));

    ContactBuffer result{};

    ASSERT_TRUE(hash_find_contact(&htable, "0412345678", &result));

    EXPECT_EQ(result.contact.name_len, original.contact.name_len);
    EXPECT_EQ(result.contact.phone_len, original.contact.phone_len);
    EXPECT_STREQ(result.contact.name, "Alice");
    EXPECT_STREQ(result.contact.phone, "0412345678");
}

/**
 * @brief Finding a phone number that was never inserted fails.
 */
TEST_F(HashTableTest, FindMissingContact)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));

    ContactBuffer result{};

    EXPECT_FALSE(hash_find_contact(&htable, "0400000000", &result));
}

/**
 * @brief An inserted contact can be removed by phone number, taking its
 *        data with it, and is no longer findable afterwards.
 */
TEST_F(HashTableTest, RemoveContact)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer removed{};

    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0412345678", &removed));

    EXPECT_STREQ(removed.contact.name, "Alice");
    EXPECT_STREQ(removed.contact.phone, "0412345678");
    EXPECT_EQ(hash_size(&htable), 0u);

    ContactBuffer result{};
    EXPECT_FALSE(hash_find_contact(&htable, "0412345678", &result));
}

/**
 * @brief Removing a phone number that is not present fails.
 */
TEST_F(HashTableTest, RemoveMissingContact)
{
    ContactBuffer removed{};

    EXPECT_FALSE(hash_remove_contact(&htable, &journal, "0412345678", &removed));
}

/**
 * @brief hash_remove() drops both the contact and message chat in one
 *        call and hands back the removed RAM entry.
 */
TEST_F(HashTableTest, RemoveByPhoneRemovesContactAndMessages)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(send("0412345678", 100, true, "hi"));

    HashEntry *removed = nullptr;
    ASSERT_TRUE(hash_remove(&htable, &journal, "0412345678", &removed));
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->state, ENTRY_DELETED);
    EXPECT_EQ(hash_size(&htable), 0u);

    ContactBuffer cResult{};
    EXPECT_FALSE(hash_find_contact(&htable, "0412345678", &cResult));

    MessageBuffer mResult{};
    EXPECT_FALSE(hash_find_message(&htable, "0412345678", &mResult));
}

/**
 * @brief hash_remove() on a missing phone number fails and leaves the
 *        output entry pointer untouched.
 */
TEST_F(HashTableTest, RemoveByPhoneMissingFails)
{
    HashEntry *removed = nullptr;
    EXPECT_FALSE(hash_remove(&htable, &journal, "0412345678", &removed));
    EXPECT_EQ(removed, nullptr);
}

/**
 * @brief Multiple distinct contacts can be inserted and found
 *        independently.
 */
TEST_F(HashTableTest, MultipleContacts)
{
    ASSERT_TRUE(insert("Alice", "0411111111"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Charlie", "0433333333"));

    EXPECT_EQ(hash_size(&htable), 3u);

    ContactBuffer result{};

    ASSERT_TRUE(hash_find_contact(&htable, "0411111111", &result));
    EXPECT_STREQ(result.contact.name, "Alice");

    ASSERT_TRUE(hash_find_contact(&htable, "0422222222", &result));
    EXPECT_STREQ(result.contact.name, "Bob");

    ASSERT_TRUE(hash_find_contact(&htable, "0433333333", &result));
    EXPECT_STREQ(result.contact.name, "Charlie");
}

/**
 * @brief Inserting the same phone number again updates the existing
 *        contact rather than creating a second one.
 */
TEST_F(HashTableTest, DuplicateInsertUpdatesInPlace)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    uint16_t first_sector = sector_for("0412345678");

    ASSERT_TRUE(insert("Alice Updated", "0412345678"));
    uint16_t second_sector = sector_for("0412345678");

    // Same phone -> same slot/sector, no extra contact.
    EXPECT_EQ(second_sector, first_sector);
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact(&htable, "0412345678", &result));
    EXPECT_STREQ(result.contact.name, "Alice Updated");
}

/**
 * @brief Two different phone numbers that collide on the 16-bit phone
 *        hash are both inserted and found, disambiguated by the stored
 *        phone string.
 *
 * "0400000601" and "0400002060" both hash to the same value via hash_phone().
 */
TEST_F(HashTableTest, PhoneHashCollisionResolved)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b))
        << "test precondition: the two numbers must collide";

    ASSERT_TRUE(insert("Collide A", phone_a));
    ASSERT_TRUE(insert("Collide B", phone_b));

    EXPECT_NE(sector_for(phone_a), sector_for(phone_b));
    EXPECT_EQ(hash_size(&htable), 2u);

    ContactBuffer result{};

    ASSERT_TRUE(hash_find_contact(&htable, phone_a, &result));
    EXPECT_STREQ(result.contact.name, "Collide A");
    EXPECT_STREQ(result.contact.phone, phone_a);

    ASSERT_TRUE(hash_find_contact(&htable, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");
    EXPECT_STREQ(result.contact.phone, phone_b);
}

/**
 * @brief Removing one contact from a hash-collision chain leaves the
 *        other reachable.
 */
TEST_F(HashTableTest, RemoveFromCollisionChain)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";

    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    ASSERT_TRUE(insert("Collide A", phone_a));
    ASSERT_TRUE(insert("Collide B", phone_b));

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, phone_a, &removed));
    EXPECT_STREQ(removed.contact.name, "Collide A");

    ContactBuffer result{};

    // The other colliding contact is still reachable past the tombstone.
    ASSERT_TRUE(hash_find_contact(&htable, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");

    // The removed one is gone.
    EXPECT_FALSE(hash_find_contact(&htable, phone_a, &result));
}

/**
 * @brief A tombstoned slot is reused by a later insert.
 */
TEST_F(HashTableTest, ReinsertAfterRemove)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0412345678", &removed));
    EXPECT_EQ(hash_size(&htable), 0u);

    ASSERT_TRUE(insert("Alice Again", "0412345678"));
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact(&htable, "0412345678", &result));
    EXPECT_STREQ(result.contact.name, "Alice Again");
}

/**
 * @brief hash_size() tracks inserts and removes.
 */
TEST_F(HashTableTest, SizeTracksContacts)
{
    EXPECT_EQ(hash_size(&htable), 0u);

    ASSERT_TRUE(insert("Alice", "0411111111"));
    EXPECT_EQ(hash_size(&htable), 1u);

    ASSERT_TRUE(insert("Bob", "0422222222"));
    EXPECT_EQ(hash_size(&htable), 2u);

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0411111111", &removed));
    EXPECT_EQ(hash_size(&htable), 1u);
}

/**
 * @brief NULL / bad arguments are rejected.
 */
TEST_F(HashTableTest, RejectsBadArguments)
{
    ContactBuffer c = make_contact("Alice", "0412345678");
    ContactBuffer out{};

    EXPECT_FALSE(hash_insert_contact(nullptr, &journal, &c));
    EXPECT_FALSE(hash_insert_contact(&htable, &journal, nullptr));

    EXPECT_FALSE(hash_find_contact(nullptr, "0412345678", &out));
    EXPECT_FALSE(hash_find_contact(&htable, nullptr, &out));

    EXPECT_FALSE(hash_remove_contact(&htable, &journal, nullptr, &out));
}

/* ============================================================================
 * hash_find_entry()
 * ========================================================================== */

/**
 * @brief hash_find_entry() returns the OCCUPIED slot for a stored phone
 *        and a non-occupied (free) slot for an unknown phone.
 */
TEST_F(HashTableTest, FindEntryMatchesStoredPhone)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    uint16_t sector = sector_for("0412345678");
    ASSERT_NE(sector, UINT16_MAX);

    HashEntry *hit = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, "0412345678", &hit));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->state, ENTRY_OCCUPIED);
    EXPECT_EQ(hit->sector, sector);
    EXPECT_EQ(hit->id, hash_phone("0412345678"));

    HashEntry *miss = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, "0400000000", &miss));
    ASSERT_NE(miss, nullptr);
    EXPECT_NE(miss->state, ENTRY_OCCUPIED); // an insertion point, not a match
}

/**
 * @brief hash_find_entry() disambiguates two phone numbers that share the
 *        same 16-bit hash, returning a distinct entry for each.
 */
TEST_F(HashTableTest, FindEntryDistinguishesCollidingPhones)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    ASSERT_TRUE(insert("Collide A", phone_a));
    ASSERT_TRUE(insert("Collide B", phone_b));

    HashEntry *ea = nullptr;
    HashEntry *eb = nullptr;
    ASSERT_TRUE(hash_find_entry(&htable, phone_a, &ea));
    ASSERT_TRUE(hash_find_entry(&htable, phone_b, &eb));

    EXPECT_EQ(ea->state, ENTRY_OCCUPIED);
    EXPECT_EQ(eb->state, ENTRY_OCCUPIED);
    EXPECT_NE(ea, eb);
}

/* ============================================================================
 * create_message()
 * ========================================================================== */

/**
 * @brief create_message() rejects a zero timestamp or a NULL body,
 *        returning an all-zero (empty) MessageBuffer.
 */
TEST_F(HashTableTest, CreateMessageRejectsBadArguments)
{
    MessageBuffer zero_ts = create_message(0, true, const_cast<char *>("hello"));
    MessageBuffer empty{};
    EXPECT_EQ(std::memcmp(zero_ts.buffer, empty.buffer, sizeof(MessageBuffer)), 0);

    MessageBuffer null_str = create_message(100, true, nullptr);
    EXPECT_EQ(std::memcmp(null_str.buffer, empty.buffer, sizeof(MessageBuffer)), 0);
}

/**
 * @brief create_message() accepts a valid, nonzero timestamp and copies
 *        the message body.
 */
TEST_F(HashTableTest, CreateMessageStoresContent)
{
    MessageBuffer msg = create_message(1234, true, const_cast<char *>("hello"));

    EXPECT_EQ(msg.msg.timestamp, 1234);
    EXPECT_TRUE(msg.msg.direction);
    EXPECT_STREQ(msg.msg.str, "hello");
}

/**
 * @brief create_message() truncates a body longer than the message
 *        capacity rather than overflowing/over-reading.
 */
TEST_F(HashTableTest, CreateMessageTruncatesOverlongBody)
{
    std::string long_body(SMS_MAX_MESSAGE_LENGTH + 50, 'x');

    MessageBuffer msg = create_message(1, false, const_cast<char *>(long_body.c_str()));

    EXPECT_EQ(std::strlen(msg.msg.str), static_cast<size_t>(SMS_MAX_MESSAGE_LENGTH - 1));
}

/* ============================================================================
 * Messages: insert / find / remove
 * ========================================================================== */

/**
 * @brief Sending the first message to a brand-new phone number
 *        auto-creates an (empty-named) contact and is retrievable.
 */
TEST_F(HashTableTest, InsertFirstMessageCreatesContact)
{
    ASSERT_TRUE(send("0412345678", 100, true, "hello"));
    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer contact{};
    ASSERT_TRUE(hash_find_contact(&htable, "0412345678", &contact));
    EXPECT_STREQ(contact.contact.phone, "0412345678");

    MessageBuffer result{};
    ASSERT_TRUE(hash_find_message(&htable, "0412345678", &result));
    EXPECT_EQ(result.msg.timestamp, 100);
    EXPECT_STREQ(result.msg.str, "hello");
}

/**
 * @brief Sending a message to an already-inserted contact attaches the
 *        message to it rather than creating a duplicate.
 */
TEST_F(HashTableTest, InsertMessageAttachesToExistingContact)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(send("0412345678", 100, true, "hello"));

    EXPECT_EQ(hash_size(&htable), 1u);

    ContactBuffer contact{};
    ASSERT_TRUE(hash_find_contact(&htable, "0412345678", &contact));
    EXPECT_STREQ(contact.contact.name, "Alice");
}

/**
 * @brief hash_find_message() always returns the most recently sent
 *        message for a contact.
 */
TEST_F(HashTableTest, FindMessageReturnsLatest)
{
    ASSERT_TRUE(send("0412345678", 100, true, "first"));
    ASSERT_TRUE(send("0412345678", 200, false, "second"));

    MessageBuffer result{};
    ASSERT_TRUE(hash_find_message(&htable, "0412345678", &result));
    EXPECT_EQ(result.msg.timestamp, 200);
    EXPECT_STREQ(result.msg.str, "second");
    EXPECT_FALSE(result.msg.direction);
}

/**
 * @brief Finding a message for a phone number with no chat fails.
 */
TEST_F(HashTableTest, FindMessageMissingContactFails)
{
    MessageBuffer result{};
    EXPECT_FALSE(hash_find_message(&htable, "0400000000", &result));
}

/**
 * @brief hash_find_n_message() returns messages newest-first, entirely
 *        within a single (not yet full) sector.
 */
TEST_F(HashTableTest, FindNMessagesWithinOneSector)
{
    ASSERT_TRUE(send("0412345678", 100, true, "first"));
    ASSERT_TRUE(send("0412345678", 200, false, "second"));

    MessageBuffer results[2]{};
    int n = hash_find_n_message(&htable, "0412345678", 2, results);

    ASSERT_EQ(n, 2);
    EXPECT_EQ(results[0].msg.timestamp, 200);
    EXPECT_STREQ(results[0].msg.str, "second");
    EXPECT_EQ(results[1].msg.timestamp, 100);
    EXPECT_STREQ(results[1].msg.str, "first");
}

/**
 * @brief Sending more messages than fit in one sector rolls the chat
 *        over onto a new, linked sector, and the latest message is
 *        still the one just sent.
 *
 * MESSAGE_BLOCK_CAPACITY is small (computed from a 512B sector), so
 * three messages is enough to force a rollover.
 */
TEST_F(HashTableTest, MessageChatRollsOverToNewSector)
{
    for (int i = 0; i < MESSAGE_BLOCK_CAPACITY + 1; i++)
    {
        ASSERT_TRUE(send("0412345678", static_cast<uint16_t>(100 + i), true,
                          "msg " + std::to_string(i)))
            << "failed sending message " << i;
    }

    MessageBuffer latest{};
    ASSERT_TRUE(hash_find_message(&htable, "0412345678", &latest));
    EXPECT_EQ(latest.msg.timestamp, 100 + MESSAGE_BLOCK_CAPACITY);
}

/**
 * @brief hash_find_n_message() walks backwards across the sector
 *        boundary and returns every message in newest-first order.
 */
TEST_F(HashTableTest, FindNMessagesAcrossSectorBoundary)
{
    const int total = MESSAGE_BLOCK_CAPACITY + 1;

    for (int i = 0; i < total; i++)
    {
        ASSERT_TRUE(send("0412345678", static_cast<uint16_t>(100 + i), true,
                          "msg " + std::to_string(i)));
    }

    std::vector<MessageBuffer> results(total);
    int n = hash_find_n_message(&htable, "0412345678", total, results.data());

    ASSERT_EQ(n, total);
    for (int i = 0; i < total; i++)
    {
        // Newest first: message (total - 1 - i) was the i-th most recent.
        uint16_t expected_ts = static_cast<uint16_t>(100 + (total - 1 - i));
        EXPECT_EQ(results[i].msg.timestamp, expected_ts) << "at position " << i;
    }
}

/**
 * @brief Removing a contact's message chat clears it (and only it) -
 *        the contact itself is untouched.
 */
TEST_F(HashTableTest, RemoveMessageChatClearsMessagesOnly)
{
    ASSERT_TRUE(insert("Alice", "0412345678"));
    ASSERT_TRUE(send("0412345678", 100, true, "hello"));

    MessageBuffer out{};
    ASSERT_TRUE(hash_remove_message(&htable, &journal, "0412345678", &out));

    MessageBuffer result{};
    EXPECT_FALSE(hash_find_message(&htable, "0412345678", &result));

    // Contact itself must still be present.
    ContactBuffer contact{};
    EXPECT_TRUE(hash_find_contact(&htable, "0412345678", &contact));
    EXPECT_STREQ(contact.contact.name, "Alice");
}

/**
 * @brief Removing a message chat that spans multiple linked sectors
 *        walks the whole chain and clears all of it.
 */
TEST_F(HashTableTest, RemoveMessageChatAcrossMultipleSectors)
{
    const int total = MESSAGE_BLOCK_CAPACITY + 1;

    for (int i = 0; i < total; i++)
    {
        ASSERT_TRUE(send("0412345678", static_cast<uint16_t>(100 + i), true,
                          "msg " + std::to_string(i)));
    }

    MessageBuffer out{};
    ASSERT_TRUE(hash_remove_message(&htable, &journal, "0412345678", &out));

    MessageBuffer result{};
    EXPECT_FALSE(hash_find_message(&htable, "0412345678", &result));
}

/**
 * @brief Removing a message chat for a phone number with no chat fails.
 */
TEST_F(HashTableTest, RemoveMessageChatMissingContactFails)
{
    MessageBuffer out{};
    EXPECT_FALSE(hash_remove_message(&htable, &journal, "0400000000", &out));
}

/**
 * @brief NULL / bad arguments are rejected by the message insert path.
 */
TEST_F(HashTableTest, RejectsBadMessageArguments)
{
    MessageBuffer m = create_message(1, true, const_cast<char *>("hi"));

    EXPECT_FALSE(hash_insert_message(nullptr, &journal, "0412345678", &m));
    EXPECT_FALSE(hash_insert_message(&htable, &journal, nullptr, &m));
    EXPECT_FALSE(hash_insert_message(&htable, &journal, "0412345678", nullptr));
}

/* ============================================================================
 * hash_reconstruct_contact()
 * ========================================================================== */

/**
 * @brief After a simulated restart, every persisted contact is rebuilt
 *        into the RAM table and is findable by phone number.
 */
TEST_F(HashTableTest, ReconstructFindsAllContacts)
{
    struct Person { const char *name; const char *phone; };

    Person people[] = {
        {"Alice",   "0411111111"},
        {"Bob",     "0422222222"},
        {"Charlie", "0433333333"},
        {"Dana",    "0444444444"},
        {"Erin",    "0455555555"},
    };

    for (auto& p : people)
    {
        ASSERT_TRUE(insert(p.name, p.phone));
    }

    ASSERT_TRUE(rebuild());

    EXPECT_EQ(hash_size(&rebuilt), sizeof(people) / sizeof(people[0]));

    for (auto& p : people)
    {
        ContactBuffer result{};
        ASSERT_TRUE(hash_find_contact(&rebuilt, p.phone, &result)) << p.name;
        EXPECT_STREQ(result.contact.name, p.name);
        EXPECT_STREQ(result.contact.phone, p.phone);
    }
}

/**
 * @brief Reconstruction of an empty database yields an empty table.
 */
TEST_F(HashTableTest, ReconstructEmptyDatabase)
{
    ASSERT_TRUE(rebuild());
    EXPECT_EQ(hash_size(&rebuilt), 0u);

    ContactBuffer result{};
    EXPECT_FALSE(hash_find_contact(&rebuilt, "0412345678", &result));
}

/**
 * @brief A contact removed before the restart does not reappear after
 *        reconstruction; the survivors still do.
 */
TEST_F(HashTableTest, ReconstructSkipsRemovedContacts)
{
    ASSERT_TRUE(insert("Alice", "0411111111"));
    ASSERT_TRUE(insert("Bob", "0422222222"));
    ASSERT_TRUE(insert("Charlie", "0433333333"));

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&htable, &journal, "0422222222", &removed));

    ASSERT_TRUE(rebuild());

    EXPECT_EQ(hash_size(&rebuilt), 2u);

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact(&rebuilt, "0411111111", &result));
    EXPECT_TRUE(hash_find_contact(&rebuilt, "0433333333", &result));
    EXPECT_FALSE(hash_find_contact(&rebuilt, "0422222222", &result));
}

/**
 * @brief Colliding phone numbers survive a reconstruction and remain
 *        independently findable.
 */
TEST_F(HashTableTest, ReconstructPreservesCollisionChain)
{
    const char *phone_a = "0400000601";
    const char *phone_b = "0400002060";
    ASSERT_EQ(hash_phone(phone_a), hash_phone(phone_b));

    ASSERT_TRUE(insert("Collide A", phone_a));
    ASSERT_TRUE(insert("Collide B", phone_b));

    ASSERT_TRUE(rebuild());

    EXPECT_EQ(hash_size(&rebuilt), 2u);

    ContactBuffer result{};
    ASSERT_TRUE(hash_find_contact(&rebuilt, phone_a, &result));
    EXPECT_STREQ(result.contact.name, "Collide A");
    ASSERT_TRUE(hash_find_contact(&rebuilt, phone_b, &result));
    EXPECT_STREQ(result.contact.name, "Collide B");
}

/**
 * @brief The rebuilt table is fully functional: new contacts can be
 *        inserted and removed after reconstruction.
 */
TEST_F(HashTableTest, ReconstructedTableAcceptsNewWrites)
{
    ASSERT_TRUE(insert("Alice", "0411111111"));

    ASSERT_TRUE(rebuild());

    ASSERT_EQ(hash_size(&rebuilt), 1u);

    ContactBuffer bob = make_contact("Bob", "0422222222");
    ASSERT_TRUE(hash_insert_contact(&rebuilt, &journal, &bob));
    EXPECT_EQ(hash_size(&rebuilt), 2u);

    ContactBuffer result{};
    EXPECT_TRUE(hash_find_contact(&rebuilt, "0422222222", &result));

    ContactBuffer removed{};
    ASSERT_TRUE(hash_remove_contact(&rebuilt, &journal, "0411111111", &removed));
    EXPECT_STREQ(removed.contact.name, "Alice");
    EXPECT_EQ(hash_size(&rebuilt), 1u);
}

/**
 * @brief Create a ContactBuffer from a name and phone number.
 */
static ContactBuffer make_contact(const std::string& name, const std::string& phone)
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
