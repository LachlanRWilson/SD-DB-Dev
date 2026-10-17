#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

extern "C"
{
#include "sorted_array.h"
#include "hash_table.h"
#include "contact.h"
#include "storage.h"
#include "free_list_stack.h"
#include "heap_storage.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "mem_layout.h"
}

static ContactBuffer make_contact(const std::string& name, const std::string& phone);

/**
 * @brief Fixture for the name-ordered SortedArray (sorted_array.c).
 *
 * Entries hold an 8 byte case-folded key and a hash table slot. Ties on the
 * key are broken by reading the full contact from storage through
 * htable[slot].sector, so the fixture stands up a real hash table over heap
 * storage and inserts every contact there first to get its slot.
 */
class SortedArrayTest : public ::testing::Test
{
protected:
    SortedArray sa{};
    SortedArrayEntry *sa_mem = nullptr;

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
        std::memset(entries, 0, sizeof(HashEntry) * HASH_TABLE_SIZE);

        sa_mem = new SortedArrayEntry[SORTED_ARRAY_ENTRIES];
        std::memset(sa_mem, 0, sizeof(SortedArrayEntry) * SORTED_ARRAY_ENTRIES);

        contact_fls_pool = new uint16_t[HASH_TABLE_ENTRIES];
        message_fls_pool = new uint16_t[TOTAL_MESSAGE_SECTOR_SIZE];

        storage_mem = new uint8_t[static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT];
        std::memset(storage_mem, 0,
                    static_cast<size_t>(SECTOR_SIZE) * STORAGE_SECTOR_COUNT);

        ASSERT_TRUE(HeapStorage_Init(&storage_ctx, storage_mem, SECTOR_SIZE,
                                     STORAGE_SECTOR_COUNT));
        storage->context = &storage_ctx;

        ASSERT_TRUE(init_usage_bitmap(storage));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, storage));

        ASSERT_TRUE(free_list_init(&contact_allocator, contact_fls_pool, HASH_TABLE_ENTRIES));
        ASSERT_TRUE(free_list_init(&message_allocator, message_fls_pool, TOTAL_MESSAGE_SECTOR_SIZE));

        hash_init(&htable, storage, &contact_allocator, &message_allocator, entries, HASH_TABLE_SIZE);

        ASSERT_TRUE(sorted_array_init(&sa, entries, storage, sa_mem));
    }

    void TearDown() override
    {
        hash_clear(&htable);

        delete[] entries;
        delete[] sa_mem;
        delete[] contact_fls_pool;
        delete[] message_fls_pool;
        delete[] storage_mem;
    }

    /** @brief Store a contact in the hash table and return its slot (UINT16_MAX on failure). */
    uint16_t store(const std::string& name, const std::string& phone)
    {
        ContactBuffer c = make_contact(name, phone);
        uint16_t slot = UINT16_MAX;
        if (hash_insert_contact(&htable, &journal, &c, &slot) != STRG_OK)
        {
            return UINT16_MAX;
        }
        return slot;
    }

    /** @brief Store a contact in the hash table, then insert it into the sorted array. */
    STRG_RET insert(const std::string& name, const std::string& phone)
    {
        uint16_t slot = store(name, phone);
        if (slot == UINT16_MAX)
        {
            return STRG_FAIL;
        }
        ContactBuffer c = make_contact(name, phone);
        return sorted_array_insert(&sa, &c.contact, slot);
    }

    /** @brief Read the contact a sorted array entry points at. */
    Contact contact_at(size_t rank)
    {
        ContactBuffer c{};
        uint16_t sector = entries[sa.mem[rank].hash_slot].sector;
        EXPECT_EQ(read_contact(storage, sector, &c), STRG_OK);
        return c.contact;
    }

    std::string name_at(size_t rank)
    {
        Contact c = contact_at(rank);
        return std::string(c.name, c.name_len);
    }

    std::string phone_at(size_t rank)
    {
        Contact c = contact_at(rank);
        return std::string(c.phone, c.phone_len);
    }

    /** @brief All names in sorted array order. */
    std::vector<std::string> names()
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < sa.occupancy; i++)
        {
            out.push_back(name_at(i));
        }
        return out;
    }
};

/* ============================================================================
 * sorted_array_init()
 * ========================================================================== */

TEST_F(SortedArrayTest, InitRejectsNullArguments)
{
    SortedArray other{};
    EXPECT_FALSE(sorted_array_init(nullptr, entries, storage, sa_mem));
    EXPECT_FALSE(sorted_array_init(&other, entries, nullptr, sa_mem));
    EXPECT_FALSE(sorted_array_init(&other, entries, storage, nullptr));
}

TEST_F(SortedArrayTest, InitStartsEmpty)
{
    EXPECT_EQ(sa.occupancy, 0);
}

/* ============================================================================
 * sorted_array_insert()
 * ========================================================================== */

TEST_F(SortedArrayTest, InsertKeepsNameOrder)
{
    for (const char *n : {"Mallory", "Alice", "Trent", "Bob", "Eve", "Carol"})
    {
        ASSERT_EQ(insert(n, std::string("04") + n), STRG_OK) << n;
    }

    std::vector<std::string> expected = {"Alice", "Bob", "Carol", "Eve", "Mallory", "Trent"};
    EXPECT_EQ(names(), expected);
}

TEST_F(SortedArrayTest, InsertOrderIsCaseInsensitive)
{
    ASSERT_EQ(insert("carol", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);
    ASSERT_EQ(insert("alice", "0400000003"), STRG_OK);

    std::vector<std::string> expected = {"alice", "Bob", "carol"};
    EXPECT_EQ(names(), expected);
}

/**
 * @brief Names sharing the first 8 characters have equal keys, so the order
 *        comes from the full name read back from storage.
 */
TEST_F(SortedArrayTest, InsertTiebreaksOnFullNamePastKey)
{
    ASSERT_EQ(insert("Alexandra",       "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Alexander Smith", "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Alexander",       "0400000003"), STRG_OK);
    ASSERT_EQ(insert("Alexander Jones", "0400000004"), STRG_OK);

    std::vector<std::string> expected = {"Alexander", "Alexander Jones", "Alexander Smith", "Alexandra"};
    EXPECT_EQ(names(), expected);
}

TEST_F(SortedArrayTest, InsertSameNameDifferentCaseOrdersByExactCase)
{
    ASSERT_EQ(insert("bob", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob", "0400000002"), STRG_OK);

    // 'B' (0x42) sorts before 'b' (0x62)
    std::vector<std::string> expected = {"Bob", "bob"};
    EXPECT_EQ(names(), expected);
}

TEST_F(SortedArrayTest, InsertSameNameOrdersByPhone)
{
    ASSERT_EQ(insert("Sam", "0400000003"), STRG_OK);
    ASSERT_EQ(insert("Sam", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Sam", "0400000002"), STRG_OK);

    ASSERT_EQ(sa.occupancy, 3);
    EXPECT_EQ(phone_at(0), "0400000001");
    EXPECT_EQ(phone_at(1), "0400000002");
    EXPECT_EQ(phone_at(2), "0400000003");
}

TEST_F(SortedArrayTest, InsertDuplicateIsRejected)
{
    uint16_t slot = store("Alice", "0400000001");
    ASSERT_NE(slot, UINT16_MAX);
    ContactBuffer c = make_contact("Alice", "0400000001");

    ASSERT_EQ(sorted_array_insert(&sa, &c.contact, slot), STRG_OK);
    EXPECT_NE(sorted_array_insert(&sa, &c.contact, slot), STRG_OK);
    EXPECT_EQ(sa.occupancy, 1);
}

TEST_F(SortedArrayTest, InsertWhenFullReturnsFull)
{
    uint16_t slot = store("Alice", "0400000001");
    ASSERT_NE(slot, UINT16_MAX);
    ContactBuffer c = make_contact("Alice", "0400000001");

    sa.occupancy = SORTED_ARRAY_ENTRIES;
    EXPECT_EQ(sorted_array_insert(&sa, &c.contact, slot), STRG_FULL);
    EXPECT_EQ(sa.occupancy, SORTED_ARRAY_ENTRIES);
}

TEST_F(SortedArrayTest, InsertStoresKeyAndSlot)
{
    uint16_t slot = store("ALEXANDER", "0400000001");
    ASSERT_NE(slot, UINT16_MAX);
    ContactBuffer c = make_contact("ALEXANDER", "0400000001");
    ASSERT_EQ(sorted_array_insert(&sa, &c.contact, slot), STRG_OK);

    EXPECT_EQ(std::memcmp(sa.mem[0].key, "alexande", KEY_LEN), 0);
    EXPECT_EQ(sa.mem[0].hash_slot, slot);
}

TEST_F(SortedArrayTest, ShortNameKeyIsZeroPadded)
{
    ASSERT_EQ(insert("Al", "0400000001"), STRG_OK);

    const char expected[KEY_LEN] = {'a', 'l', 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(std::memcmp(sa.mem[0].key, expected, KEY_LEN), 0);
}

/* ============================================================================
 * sorted_array_find()
 * ========================================================================== */

TEST_F(SortedArrayTest, FindExactMatch)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    ContactBuffer c = make_contact("Bob", "0400000002");
    size_t rank = SIZE_MAX;
    bool match = false;
    ASSERT_EQ(sorted_array_find(&sa, &c.contact, &rank, &match), STRG_OK);
    EXPECT_TRUE(match);
    EXPECT_EQ(rank, 1u);
}

TEST_F(SortedArrayTest, FindMissingGivesInsertionRank)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    ContactBuffer c = make_contact("Bob", "0400000002");
    size_t rank = SIZE_MAX;
    bool match = true;
    ASSERT_EQ(sorted_array_find(&sa, &c.contact, &rank, &match), STRG_OK);
    EXPECT_FALSE(match);
    EXPECT_EQ(rank, 1u);
}

TEST_F(SortedArrayTest, FindSameNameDifferentPhoneIsNotAMatch)
{
    ASSERT_EQ(insert("Sam", "0400000001"), STRG_OK);

    ContactBuffer c = make_contact("Sam", "0400000009");
    size_t rank;
    bool match = true;
    ASSERT_EQ(sorted_array_find(&sa, &c.contact, &rank, &match), STRG_OK);
    EXPECT_FALSE(match);
    EXPECT_EQ(rank, 1u);
}

TEST_F(SortedArrayTest, FindInEmptyArray)
{
    ContactBuffer c = make_contact("Alice", "0400000001");
    size_t rank = SIZE_MAX;
    bool match = true;
    ASSERT_EQ(sorted_array_find(&sa, &c.contact, &rank, &match), STRG_OK);
    EXPECT_FALSE(match);
    EXPECT_EQ(rank, 0u);
}

/* ============================================================================
 * sorted_array_remove()
 * ========================================================================== */

TEST_F(SortedArrayTest, RemoveMiddleKeepsOrder)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    ContactBuffer c = make_contact("Bob", "0400000002");
    EXPECT_TRUE(sorted_array_remove(&sa, &c.contact));

    std::vector<std::string> expected = {"Alice", "Carol"};
    EXPECT_EQ(names(), expected);
}

TEST_F(SortedArrayTest, RemoveFirstAndLast)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    ContactBuffer first = make_contact("Alice", "0400000001");
    ContactBuffer last = make_contact("Carol", "0400000003");
    EXPECT_TRUE(sorted_array_remove(&sa, &first.contact));
    EXPECT_TRUE(sorted_array_remove(&sa, &last.contact));

    std::vector<std::string> expected = {"Bob"};
    EXPECT_EQ(names(), expected);
}

TEST_F(SortedArrayTest, RemoveOnlyTheMatchingPhone)
{
    ASSERT_EQ(insert("Sam", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Sam", "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Sam", "0400000003"), STRG_OK);

    ContactBuffer c = make_contact("Sam", "0400000002");
    EXPECT_TRUE(sorted_array_remove(&sa, &c.contact));

    ASSERT_EQ(sa.occupancy, 2);
    EXPECT_EQ(phone_at(0), "0400000001");
    EXPECT_EQ(phone_at(1), "0400000003");
}

TEST_F(SortedArrayTest, RemoveMissingLeavesArrayUnchanged)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    ContactBuffer c = make_contact("Bob", "0400000002");
    EXPECT_FALSE(sorted_array_remove(&sa, &c.contact));
    EXPECT_EQ(sa.occupancy, 2);
}

TEST_F(SortedArrayTest, RemoveFromEmptyFails)
{
    ContactBuffer c = make_contact("Alice", "0400000001");
    EXPECT_FALSE(sorted_array_remove(&sa, &c.contact));
    EXPECT_EQ(sa.occupancy, 0);
}

/* ============================================================================
 * sorted_array_seek()
 * ========================================================================== */

class SortedArraySeekTest : public SortedArrayTest
{
protected:
    void SetUp() override
    {
        SortedArrayTest::SetUp();
        // order: Adam, Alexander Jones, Alexander Smith, Alice, Bob, Brian, Carl
        ASSERT_EQ(insert("Bob",             "0400000001"), STRG_OK);
        ASSERT_EQ(insert("Adam",            "0400000002"), STRG_OK);
        ASSERT_EQ(insert("Carl",            "0400000003"), STRG_OK);
        ASSERT_EQ(insert("Alexander Smith", "0400000004"), STRG_OK);
        ASSERT_EQ(insert("Brian",           "0400000005"), STRG_OK);
        ASSERT_EQ(insert("Alice",           "0400000006"), STRG_OK);
        ASSERT_EQ(insert("Alexander Jones", "0400000007"), STRG_OK);
    }

    size_t seek(const char *prefix)
    {
        size_t rank = SIZE_MAX;
        EXPECT_EQ(sorted_array_seek(&sa, prefix, &rank), STRG_OK) << prefix;
        return rank;
    }
};

TEST_F(SortedArraySeekTest, EmptyPrefixSeeksToStart)
{
    EXPECT_EQ(seek(""), 0u);
}

TEST_F(SortedArraySeekTest, PrefixSeeksToFirstMatch)
{
    EXPECT_EQ(seek("A"), 0u);
    EXPECT_EQ(seek("Al"), 1u);
    EXPECT_EQ(seek("Ali"), 3u);
    EXPECT_EQ(seek("B"), 4u);
    EXPECT_EQ(seek("Br"), 5u);
    EXPECT_EQ(seek("C"), 6u);
}

TEST_F(SortedArraySeekTest, PrefixIsCaseInsensitive)
{
    EXPECT_EQ(seek("br"), seek("BR"));
    EXPECT_EQ(seek("br"), 5u);
}

/** @brief Prefixes longer than the key fall through to the full name tiebreak. */
TEST_F(SortedArraySeekTest, PrefixLongerThanKey)
{
    EXPECT_EQ(seek("Alexander"), 1u);
    EXPECT_EQ(seek("Alexander S"), 2u);
    EXPECT_EQ(seek("alexander smith"), 2u);
}

TEST_F(SortedArraySeekTest, PrefixWithNoMatchSeeksToInsertionPoint)
{
    EXPECT_EQ(seek("Bz"), 6u);
    EXPECT_EQ(seek("Z"), 7u);
}

TEST_F(SortedArraySeekTest, NullPrefixFails)
{
    size_t rank;
    EXPECT_NE(sorted_array_seek(&sa, nullptr, &rank), STRG_OK);
}

TEST_F(SortedArraySeekTest, TooLongPrefixFails)
{
    std::string prefix(MAX_NAME_LEN + 1, 'a');
    size_t rank;
    EXPECT_NE(sorted_array_seek(&sa, prefix.c_str(), &rank), STRG_OK);
}

/* ============================================================================
 * sorted_array_get_range()
 * ========================================================================== */

TEST_F(SortedArrayTest, GetRangeCopiesEntries)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);
    ASSERT_EQ(insert("Carol", "0400000003"), STRG_OK);

    SortedArrayEntry out[2];
    size_t count = 0;
    ASSERT_TRUE(sorted_array_get_range(&sa, 1, 2, &count, out));
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(std::memcmp(out, &sa.mem[1], 2 * sizeof(SortedArrayEntry)), 0);
}

TEST_F(SortedArrayTest, GetRangeClampsToOccupancy)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);

    SortedArrayEntry out[10];
    size_t count = 0;
    ASSERT_TRUE(sorted_array_get_range(&sa, 1, 10, &count, out));
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(out[0].hash_slot, sa.mem[1].hash_slot);
}

TEST_F(SortedArrayTest, GetRangeStartPastEndFails)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);

    SortedArrayEntry out[1];
    size_t count = 0;
    EXPECT_FALSE(sorted_array_get_range(&sa, 1, 1, &count, out));
    EXPECT_FALSE(sorted_array_get_range(&sa, 0, 1, &count, nullptr));
}

/* ============================================================================
 * sorted_array_append() / sorted_array_sort()
 * ========================================================================== */

TEST_F(SortedArrayTest, AppendWhenFullFails)
{
    SortedArrayEntry e{};
    sa.occupancy = SORTED_ARRAY_ENTRIES;
    EXPECT_FALSE(sorted_array_append(&sa, &e));
    EXPECT_EQ(sa.occupancy, SORTED_ARRAY_ENTRIES);
}

TEST_F(SortedArrayTest, SortEmptyAndSingle)
{
    EXPECT_EQ(sorted_array_sort(&sa), STRG_OK);

    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    EXPECT_EQ(sorted_array_sort(&sa), STRG_OK);
    EXPECT_EQ(name_at(0), "Alice");
}

TEST_F(SortedArrayTest, SortWithoutHashTableFails)
{
    ASSERT_EQ(insert("Alice", "0400000001"), STRG_OK);
    ASSERT_EQ(insert("Bob",   "0400000002"), STRG_OK);

    sa.htable_mem = nullptr;
    EXPECT_EQ(sorted_array_sort(&sa), STRG_FAIL);
}

/**
 * @brief Start-up path: entries appended in arbitrary order then sorted must
 *        end up identical to building the array with insert. The name pool
 *        is small and shares key prefixes so the sort exercises every
 *        tiebreak level (full name, exact case, phone).
 */
TEST_F(SortedArrayTest, AppendThenSortMatchesInsertOrder)
{
    const char *pool[] = {
        "Alexander", "alexander", "Alexander Jones", "Alexandra", "Bob", "bob",
        "Carol", "Christopher", "Christophe", "Dave", "Eve", "Zed",
    };
    const size_t n_pool = sizeof(pool) / sizeof(pool[0]);

    for (int i = 0; i < 500; i++)
    {
        char phone[16];
        std::snprintf(phone, sizeof(phone), "04%08d", i);
        ASSERT_EQ(insert(pool[i % n_pool], phone), STRG_OK) << i;
    }

    std::vector<SortedArrayEntry> expected(sa.mem, sa.mem + sa.occupancy);
    std::vector<SortedArrayEntry> shuffled = expected;
    std::mt19937 rng(1234);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    sa.occupancy = 0;
    for (SortedArrayEntry &e : shuffled)
    {
        ASSERT_TRUE(sorted_array_append(&sa, &e));
    }
    ASSERT_EQ(sorted_array_sort(&sa), STRG_OK);

    ASSERT_EQ(sa.occupancy, expected.size());
    for (size_t i = 0; i < expected.size(); i++)
    {
        EXPECT_EQ(sa.mem[i].hash_slot, expected[i].hash_slot) << "rank " << i;
        EXPECT_EQ(std::memcmp(sa.mem[i].key, expected[i].key, KEY_LEN), 0) << "rank " << i;
    }
}

TEST_F(SortedArrayTest, SortReversedInput)
{
    for (const char *n : {"Alice", "Bob", "Carol", "Dave", "Eve"})
    {
        ASSERT_EQ(insert(n, std::string("04") + n), STRG_OK);
    }
    std::reverse(sa.mem, sa.mem + sa.occupancy);

    ASSERT_EQ(sorted_array_sort(&sa), STRG_OK);

    std::vector<std::string> expected = {"Alice", "Bob", "Carol", "Dave", "Eve"};
    EXPECT_EQ(names(), expected);
}

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
