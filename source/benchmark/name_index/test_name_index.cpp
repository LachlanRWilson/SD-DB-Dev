#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

extern "C"
{
#include "sorted_array.h"
#include "avl_tree.h"
}

static std::vector<std::string> g_names(NI_MAX_CONTACTS);
static std::string g_probe;
static uint32_t g_tiebreak_calls;

/** @brief Case-folded comparison, then raw bytes, of two full names. */
static int full_name_cmp(const std::string &a, const std::string &b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++)
    {
        int ca = std::tolower(static_cast<unsigned char>(a[i]));
        int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) { return ca - cb; }
    }
    if (a.size() != b.size()) { return (a.size() < b.size()) ? -1 : 1; }
    return a.compare(b);
}

/** @brief Tie-break callback over the in-RAM test name table. */
static int test_tiebreak(uint16_t a, uint16_t b)
{
    g_tiebreak_calls++;
    const std::string &na = (a == NI_PROBE_SECTOR) ? g_probe : g_names[a];
    const std::string &nb = (b == NI_PROBE_SECTOR) ? g_probe : g_names[b];
    int c = full_name_cmp(na, nb);
    if (c != 0) { return c; }
    if (a == NI_PROBE_SECTOR || b == NI_PROBE_SECTOR) { return 0; }
    return (a > b) - (a < b);
}

/** @brief Reference ordering: key, then full name, then sector. */
static bool ref_less(uint16_t a, uint16_t b)
{
    uint8_t ka[NI_KEY_LEN], kb[NI_KEY_LEN];
    ni_make_key(g_names[a].c_str(), ka);
    ni_make_key(g_names[b].c_str(), kb);
    int c = memcmp(ka, kb, NI_KEY_LEN);
    if (c != 0) { return c < 0; }
    c = full_name_cmp(g_names[a], g_names[b]);
    if (c != 0) { return c < 0; }
    return a < b;
}

static NameArray g_array;
static AvlTree g_tree;

struct ArrayImpl
{
    static void init() { na_init(&g_array, test_tiebreak); }
    static bool insert(uint16_t s) { return na_insert(&g_array, g_names[s].c_str(), s); }
    static bool remove(const std::string &name, uint16_t s) { return na_remove(&g_array, name.c_str(), s); }
    static uint16_t find(const std::string &name) { g_probe = name; return na_find(&g_array, name.c_str()); }
    static bool set_message(uint16_t s, uint16_t m) { return na_set_message(&g_array, g_names[s].c_str(), s, m); }
    static uint16_t count() { return na_count(&g_array); }
    static int height() { return -1; }
    static bool valid() { return true; }
    static bool rebuild(const char *const *names, const uint16_t *sectors, uint16_t n) { return na_rebuild(&g_array, names, sectors, n); }

    static std::vector<SortedEntry> all()
    {
        std::vector<SortedEntry> out;
        NaIter it;
        SortedEntry e;
        na_begin(&it, &g_array);
        while (na_next(&it, &e)) { out.push_back(e); }
        return out;
    }

    static std::vector<uint16_t> page_at_name(const std::string &name, size_t n)
    {
        std::vector<uint16_t> out;
        NaIter it;
        SortedEntry e;
        g_probe = name;
        na_seek(&it, &g_array, name.c_str());
        while (out.size() < n && na_next(&it, &e)) { out.push_back(e.contact_index); }
        return out;
    }

    static std::vector<uint16_t> page_at_rank(uint16_t rank, size_t n)
    {
        std::vector<uint16_t> out;
        NaIter it;
        SortedEntry e;
        na_seek_rank(&it, &g_array, rank);
        while (out.size() < n && na_next(&it, &e)) { out.push_back(e.contact_index); }
        return out;
    }
};

struct AvlImpl
{
    static void init() { avl_init(&g_tree, test_tiebreak); }
    static bool insert(uint16_t s) { return avl_insert(&g_tree, g_names[s].c_str(), s); }
    static bool remove(const std::string &name, uint16_t s) { return avl_remove(&g_tree, name.c_str(), s); }
    static uint16_t find(const std::string &name) { g_probe = name; return avl_find(&g_tree, name.c_str()); }
    static bool set_message(uint16_t s, uint16_t m) { return avl_set_message(&g_tree, g_names[s].c_str(), s, m); }
    static uint16_t count() { return avl_count(&g_tree); }
    static int height() { return avl_height(&g_tree); }
    static bool valid() { return avl_valid(&g_tree); }
    static bool rebuild(const char *const *names, const uint16_t *sectors, uint16_t n) { return avl_rebuild(&g_tree, names, sectors, n); }

    static std::vector<SortedEntry> all()
    {
        std::vector<SortedEntry> out;
        AvlIter it;
        SortedEntry e;
        avl_begin(&it, &g_tree);
        while (avl_next(&it, &e)) { out.push_back(e); }
        return out;
    }

    static std::vector<uint16_t> page_at_name(const std::string &name, size_t n)
    {
        std::vector<uint16_t> out;
        AvlIter it;
        SortedEntry e;
        g_probe = name;
        avl_seek(&it, &g_tree, name.c_str());
        while (out.size() < n && avl_next(&it, &e)) { out.push_back(e.contact_index); }
        return out;
    }

    static std::vector<uint16_t> page_at_rank(uint16_t rank, size_t n)
    {
        std::vector<uint16_t> out;
        AvlIter it;
        SortedEntry e;
        avl_seek_rank(&it, &g_tree, rank);
        while (out.size() < n && avl_next(&it, &e)) { out.push_back(e.contact_index); }
        return out;
    }
};

static const char *const k_words[] = {
    "Amy", "Ben", "Chloe", "Dan", "Emma", "Finn", "Grace", "Harry", "Isla", "Jack", "Kate", "Liam", "Mia", "Noah",
    "Olivia", "Peter", "Quinn", "Ruby", "Sam", "Tom", "Uma", "Victor", "Will", "Xavier", "Yara", "Zoe",
    "Christopher", "Alexander", "Elizabeth", "Jonathan", "Smith", "Jones", "Brown", "Wilson", "Taylor", "Nguyen",
};

template <typename Impl>
class NameIndexTest : public ::testing::Test
{
protected:
    std::mt19937 rng{20261014u};

    void SetUp() override
    {
        std::fill(g_names.begin(), g_names.end(), std::string());
        g_probe.clear();
        g_tiebreak_calls = 0;
        avl_reset_max_depth();
        Impl::init();
    }

    std::string random_name()
    {
        std::uniform_int_distribution<size_t> word(0, std::size(k_words) - 1);
        std::uniform_int_distribution<int> letter('a', 'z');
        std::string s = std::string(k_words[word(rng)]) + " " + k_words[word(rng)];
        s += static_cast<char>(letter(rng));
        s += static_cast<char>(letter(rng));
        return s;
    }

    void name_sectors(uint16_t n)
    {
        for (uint16_t s = 0; s < n; s++) { g_names[s] = random_name(); }
    }

    std::vector<uint16_t> contact_order()
    {
        std::vector<uint16_t> out;
        for (const SortedEntry &e : Impl::all()) { out.push_back(e.contact_index); }
        return out;
    }

    static std::vector<uint16_t> reference(std::vector<uint16_t> sectors)
    {
        std::sort(sectors.begin(), sectors.end(), ref_less);
        return sectors;
    }

    static std::vector<uint16_t> iota(uint16_t n)
    {
        std::vector<uint16_t> v(n);
        for (uint16_t i = 0; i < n; i++) { v[i] = i; }
        return v;
    }
};

using Implementations = ::testing::Types<ArrayImpl, AvlImpl>;

class ImplNames
{
public:
    template <typename T>
    static std::string GetName(int)
    {
        return std::is_same<T, ArrayImpl>::value ? "SortedArray" : "AvlTree";
    }
};

TYPED_TEST_SUITE(NameIndexTest, Implementations, ImplNames);

/**
 * @brief Inserting N random names and iterating gives the reference order.
 */
TYPED_TEST(NameIndexTest, RandomInsertMatchesReferenceOrder)
{
    const uint16_t n = 3000;
    this->name_sectors(n);
    std::vector<uint16_t> order = this->iota(n);
    std::shuffle(order.begin(), order.end(), this->rng);
    for (uint16_t s : order) { ASSERT_TRUE(TypeParam::insert(s)); }

    EXPECT_EQ(TypeParam::count(), n);
    EXPECT_EQ(this->contact_order(), this->reference(this->iota(n)));
    EXPECT_TRUE(TypeParam::valid());
}

/**
 * @brief Names sharing an 8-character prefix are ordered by the tie-break
 *        callback, which is actually called.
 */
TYPED_TEST(NameIndexTest, SharedPrefixUsesTiebreak)
{
    const char *names[] = {"Christopher Zed", "Christopher Abe", "christop", "Christopher", "CHRISTOPHER bob", "Christina"};
    const uint16_t n = static_cast<uint16_t>(std::size(names));
    for (uint16_t s = 0; s < n; s++) { g_names[s] = names[s]; }
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }

    EXPECT_GT(g_tiebreak_calls, 0u);
    EXPECT_EQ(this->contact_order(), this->reference(this->iota(n)));
    EXPECT_EQ(TypeParam::find("Christopher Abe"), 1);
    EXPECT_EQ(TypeParam::find("christopher abe"), NI_NOT_FOUND);
}

/**
 * @brief Duplicate full names with different sectors are ordered by sector,
 *        and find() deterministically returns the lowest sector.
 */
TYPED_TEST(NameIndexTest, DuplicateFullNamesOrderedBySector)
{
    const uint16_t n = 50;
    for (uint16_t s = 0; s < n; s++) { g_names[s] = "Sam Smith"; }
    std::vector<uint16_t> order = this->iota(n);
    std::shuffle(order.begin(), order.end(), this->rng);
    for (uint16_t s : order) { ASSERT_TRUE(TypeParam::insert(s)); }

    EXPECT_EQ(this->contact_order(), this->iota(n));
    EXPECT_EQ(TypeParam::find("Sam Smith"), 0);
    EXPECT_FALSE(TypeParam::insert(7));
    EXPECT_EQ(TypeParam::count(), n);
}

/**
 * @brief Removing random, first and last entries keeps the reference order;
 *        removing a non-existent entry returns false.
 */
TYPED_TEST(NameIndexTest, RemoveRandomFirstLastAndMissing)
{
    const uint16_t n = 1000;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }

    std::vector<uint16_t> expected = this->reference(this->iota(n));
    uint16_t first = expected.front();
    uint16_t last = expected.back();
    ASSERT_TRUE(TypeParam::remove(g_names[first], first));
    ASSERT_TRUE(TypeParam::remove(g_names[last], last));
    expected.erase(expected.begin());
    expected.pop_back();

    std::vector<uint16_t> victims = expected;
    std::shuffle(victims.begin(), victims.end(), this->rng);
    victims.resize(300);
    for (uint16_t s : victims)
    {
        ASSERT_TRUE(TypeParam::remove(g_names[s], s));
        expected.erase(std::find(expected.begin(), expected.end(), s));
    }

    EXPECT_FALSE(TypeParam::remove(g_names[first], first));
    EXPECT_FALSE(TypeParam::remove("Nobody Here", 5));
    EXPECT_FALSE(TypeParam::remove(g_names[expected[0]], static_cast<uint16_t>(expected[0] + 5000)));
    EXPECT_EQ(TypeParam::count(), expected.size());
    EXPECT_EQ(this->contact_order(), expected);
    EXPECT_TRUE(TypeParam::valid());
}

/**
 * @brief Removing everything leaves an empty index that can be refilled.
 */
TYPED_TEST(NameIndexTest, RemoveAllThenReinsert)
{
    const uint16_t n = 500;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }
    std::vector<uint16_t> order = this->iota(n);
    std::shuffle(order.begin(), order.end(), this->rng);
    for (uint16_t s : order) { ASSERT_TRUE(TypeParam::remove(g_names[s], s)); }

    EXPECT_EQ(TypeParam::count(), 0);
    EXPECT_TRUE(TypeParam::all().empty());
    EXPECT_EQ(TypeParam::find(g_names[0]), NI_NOT_FOUND);

    for (uint16_t s : order) { ASSERT_TRUE(TypeParam::insert(s)); }
    EXPECT_EQ(this->contact_order(), this->reference(this->iota(n)));
    EXPECT_TRUE(TypeParam::valid());
}

/**
 * @brief Filling to NI_MAX_CONTACTS succeeds and one more insert fails
 *        without changing the contents.
 */
TYPED_TEST(NameIndexTest, FillToCapacityThenInsertFails)
{
    const uint16_t n = NI_MAX_CONTACTS;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }

    std::vector<uint16_t> before = this->contact_order();
    ASSERT_TRUE(TypeParam::remove(g_names[123], 123));
    ASSERT_TRUE(TypeParam::insert(123));
    EXPECT_FALSE(TypeParam::insert(123));
    EXPECT_FALSE(TypeParam::insert(0));
    EXPECT_EQ(TypeParam::count(), n);
    EXPECT_EQ(this->contact_order(), before);
    EXPECT_TRUE(TypeParam::valid());
}

/**
 * @brief Ascending and descending insertion give the reference order, and the
 *        AVL height stays within 1.44 log2(n) + 2.
 */
TYPED_TEST(NameIndexTest, SortedInsertionKeepsHeightBounded)
{
    const uint16_t n = 4096;
    this->name_sectors(n);
    std::vector<uint16_t> asc = this->reference(this->iota(n));

    for (int pass = 0; pass < 2; pass++)
    {
        TypeParam::init();
        std::vector<uint16_t> order = asc;
        if (pass == 1) { std::reverse(order.begin(), order.end()); }
        for (uint16_t s : order) { ASSERT_TRUE(TypeParam::insert(s)); }

        EXPECT_EQ(this->contact_order(), asc);
        EXPECT_TRUE(TypeParam::valid());
        if (TypeParam::height() >= 0)
        {
            EXPECT_LE(TypeParam::height(), 1.44 * std::log2(static_cast<double>(n)) + 2.0);
            EXPECT_LE(avl_max_depth, AVL_ITER_DEPTH);
        }
    }
}

/**
 * @brief Bulk rebuild from an unsorted list gives the same contents and order
 *        as inserting one by one, and the rebuilt index stays updatable.
 */
TYPED_TEST(NameIndexTest, RebuildMatchesSequentialInserts)
{
    const uint16_t n = 2500;
    this->name_sectors(n);
    for (uint16_t s = 0; s < 20; s++) { g_names[s + 100] = g_names[s]; }
    std::vector<uint16_t> order = this->iota(n);
    std::shuffle(order.begin(), order.end(), this->rng);

    for (uint16_t s : order) { ASSERT_TRUE(TypeParam::insert(s)); }
    std::vector<uint16_t> sequential = this->contact_order();

    std::vector<const char *> names;
    for (uint16_t s : order) { names.push_back(g_names[s].c_str()); }
    TypeParam::init();
    ASSERT_TRUE(TypeParam::rebuild(names.data(), order.data(), n));

    EXPECT_EQ(TypeParam::count(), n);
    EXPECT_EQ(this->contact_order(), sequential);
    EXPECT_TRUE(TypeParam::valid());

    ASSERT_TRUE(TypeParam::remove(g_names[order[0]], order[0]));
    ASSERT_TRUE(TypeParam::insert(order[0]));
    EXPECT_EQ(this->contact_order(), sequential);
    EXPECT_TRUE(TypeParam::valid());
}

/**
 * @brief Updating message_index in place changes only that entry.
 */
TYPED_TEST(NameIndexTest, SetMessageInPlaceKeepsOrder)
{
    const uint16_t n = 800;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }
    std::vector<uint16_t> before = this->contact_order();

    ASSERT_TRUE(TypeParam::set_message(42, 1234));
    EXPECT_FALSE(TypeParam::set_message(n + 1, 7));

    std::vector<SortedEntry> after = TypeParam::all();
    ASSERT_EQ(after.size(), n);
    for (size_t i = 0; i < after.size(); i++)
    {
        EXPECT_EQ(after[i].contact_index, before[i]);
        EXPECT_EQ(after[i].message_index, (after[i].contact_index == 42) ? 1234 : NO_MESSAGE);
    }
}

/**
 * @brief find() returns existing contacts and NI_NOT_FOUND for absent names.
 */
TYPED_TEST(NameIndexTest, FindHitAndMiss)
{
    const uint16_t n = 1500;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }

    std::vector<uint16_t> ref = this->reference(this->iota(n));
    for (uint16_t s = 0; s < n; s += 7)
    {
        uint16_t got = TypeParam::find(g_names[s]);
        ASSERT_NE(got, NI_NOT_FOUND);
        EXPECT_EQ(g_names[got], g_names[s]);
    }
    EXPECT_EQ(TypeParam::find("Aaaa Missing"), NI_NOT_FOUND);
    EXPECT_EQ(TypeParam::find("Zzzz Missing"), NI_NOT_FOUND);
    EXPECT_EQ(TypeParam::find(""), NI_NOT_FOUND);
}

/**
 * @brief A page of 20 entries from rank k or from a name matches the
 *        reference slice, including a short page at the end.
 */
TYPED_TEST(NameIndexTest, PageFetchByRankAndName)
{
    const uint16_t n = 1000;
    this->name_sectors(n);
    for (uint16_t s = 0; s < n; s++) { ASSERT_TRUE(TypeParam::insert(s)); }
    std::vector<uint16_t> ref = this->reference(this->iota(n));

    for (uint16_t k : {0, 1, 500, 979, 990})
    {
        std::vector<uint16_t> want(ref.begin() + k, ref.begin() + std::min<size_t>(k + 20, n));
        EXPECT_EQ(TypeParam::page_at_rank(k, 20), want);
        EXPECT_EQ(TypeParam::page_at_name(g_names[ref[k]], 20).front(), ref[k]);
    }
    EXPECT_TRUE(TypeParam::page_at_rank(n, 20).empty());
    EXPECT_TRUE(TypeParam::page_at_name("zzzzzzzzzz", 20).empty());
}

/**
 * @brief A long random mix of inserts and removes always matches the reference.
 */
TYPED_TEST(NameIndexTest, RandomOperationSequenceMatchesReference)
{
    const uint16_t universe = 3000;
    this->name_sectors(universe);
    std::vector<bool> present(universe, false);
    std::uniform_int_distribution<int> pick(0, universe - 1);

    for (int op = 0; op < 20000; op++)
    {
        uint16_t s = static_cast<uint16_t>(pick(this->rng));
        if (present[s])
        {
            ASSERT_TRUE(TypeParam::remove(g_names[s], s));
            present[s] = false;
        }
        else
        {
            ASSERT_TRUE(TypeParam::insert(s));
            present[s] = true;
        }
    }

    std::vector<uint16_t> live;
    for (uint16_t s = 0; s < universe; s++) { if (present[s]) { live.push_back(s); } }
    EXPECT_EQ(TypeParam::count(), live.size());
    EXPECT_EQ(this->contact_order(), this->reference(live));
    EXPECT_TRUE(TypeParam::valid());
}
