#include "sorted_array.h"

bool na_wordcopy;

typedef uint32_t __attribute__((may_alias)) na_word;

/**
 * @brief Move n entries (overlapping allowed): newlib memmove, or a
 *        hand-written 32-bit word loop when na_wordcopy is set.
 */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-loop-distribute-patterns")))
#endif
static void move_entries(SortedEntry *dst, const SortedEntry *src, uint16_t n)
{
    if (!na_wordcopy)
    {
        memmove(dst, src, (size_t)n * sizeof(SortedEntry));
        return;
    }
    na_word *d = (na_word *)dst;
    const na_word *s = (const na_word *)src;
    size_t words = (size_t)n * (sizeof(SortedEntry) / sizeof(na_word));
    if (d < s)
    {
        for (size_t i = 0; i < words; i++) { d[i] = s[i]; }
    }
    else
    {
        for (size_t i = words; i-- > 0;) { d[i] = s[i]; }
    }
}

static inline int entry_cmp(const NameArray *a, const SortedEntry *e, const uint8_t *key, uint16_t sector)
{
    return ni_cmp(e->key, e->contact_index, key, sector, a->tiebreak);
}

/**
 * @brief Three-way binary search for an exact (key, sector) pair.
 *
 * @param pos set to the matching index, or to the insertion point if absent
 * @retval True if the pair is present.
 */
static bool search_exact(const NameArray *a, const uint8_t *key, uint16_t sector, uint16_t *pos)
{
    uint16_t lo = 0;
    uint16_t hi = a->count;
    while (lo < hi)
    {
        uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
        int c = entry_cmp(a, &a->e[mid], key, sector);
        if (c == 0) { *pos = mid; return true; }
        if (c < 0) { lo = (uint16_t)(mid + 1); } else { hi = mid; }
    }
    *pos = lo;
    return false;
}

/**
 * @brief First index whose entry is >= (key, sector).
 */
static uint16_t lower_bound(const NameArray *a, const uint8_t *key, uint16_t sector)
{
    uint16_t lo = 0;
    uint16_t hi = a->count;
    while (lo < hi)
    {
        uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
        if (entry_cmp(a, &a->e[mid], key, sector) < 0) { lo = (uint16_t)(mid + 1); } else { hi = mid; }
    }
    return lo;
}

/**
 * @brief Initialise an empty array.
 *
 * @param a array to initialise
 * @param tiebreak full-name tie-break callback
 */
void na_init(NameArray *a, ni_tiebreak_fn tiebreak)
{
    a->count = 0;
    a->tiebreak = tiebreak;
}

/**
 * @brief Insert a pre-built key, keeping the array ordered.
 *
 * @retval False if the array is full or (key, contact_index) is already present.
 */
bool na_insert_key(NameArray *a, const uint8_t key[NI_KEY_LEN], uint16_t contact_index)
{
    uint16_t pos;
    if (a->count >= NI_MAX_CONTACTS) { return false; }
    if (search_exact(a, key, contact_index, &pos)) { return false; }

    move_entries(&a->e[pos + 1], &a->e[pos], (uint16_t)(a->count - pos));
    memcpy(a->e[pos].key, key, NI_KEY_LEN);
    a->e[pos].contact_index = contact_index;
    a->e[pos].message_index = NO_MESSAGE;
    a->count++;
    return true;
}

/**
 * @brief Insert a contact by name, keeping the array ordered.
 */
bool na_insert(NameArray *a, const char *name, uint16_t contact_index)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    return na_insert_key(a, key, contact_index);
}

/**
 * @brief Remove the entry for (name, contact_index).
 *
 * @retval False if it is not present.
 */
bool na_remove(NameArray *a, const char *name, uint16_t contact_index)
{
    uint8_t key[NI_KEY_LEN];
    uint16_t pos;
    ni_make_key(name, key);
    if (!search_exact(a, key, contact_index, &pos)) { return false; }

    a->count--;
    move_entries(&a->e[pos], &a->e[pos + 1], (uint16_t)(a->count - pos));
    return true;
}

/**
 * @brief Find the first contact with this full name.
 *
 * @retval Its contact_index, or NI_NOT_FOUND.
 */
uint16_t na_find(const NameArray *a, const char *name)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    uint16_t pos = lower_bound(a, key, NI_PROBE_SECTOR);
    if (pos < a->count && entry_cmp(a, &a->e[pos], key, NI_PROBE_SECTOR) == 0)
    {
        return a->e[pos].contact_index;
    }
    return NI_NOT_FOUND;
}

/**
 * @brief Update message_index of an existing entry in place (no reordering).
 */
bool na_set_message(NameArray *a, const char *name, uint16_t contact_index, uint16_t message_index)
{
    uint8_t key[NI_KEY_LEN];
    uint16_t pos;
    ni_make_key(name, key);
    if (!search_exact(a, key, contact_index, &pos)) { return false; }
    a->e[pos].message_index = message_index;
    return true;
}

uint16_t na_count(const NameArray *a)
{
    return a->count;
}

/**
 * @brief Position an iterator on the first (smallest) entry.
 */
void na_begin(NaIter *it, const NameArray *a)
{
    it->a = a;
    it->pos = 0;
}

/**
 * @brief Position an iterator on the entry of rank `rank` (direct index).
 *
 * @retval False if rank is past the end.
 */
bool na_seek_rank(NaIter *it, const NameArray *a, uint16_t rank)
{
    it->a = a;
    it->pos = rank;
    return rank < a->count;
}

/**
 * @brief Position an iterator on the first entry >= name.
 */
void na_seek(NaIter *it, const NameArray *a, const char *name)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    it->a = a;
    it->pos = lower_bound(a, key, NI_PROBE_SECTOR);
}

/**
 * @brief Copy the current entry to out and advance.
 *
 * @retval False at the end.
 */
bool na_next(NaIter *it, SortedEntry *out)
{
    if (it->pos >= it->a->count) { return false; }
    *out = it->a->e[it->pos];
    it->pos++;
    return true;
}

/**
 * @brief Start a bulk rebuild: entries are appended unsorted, then sorted once.
 */
void na_rebuild_begin(NameArray *a)
{
    a->count = 0;
}

/**
 * @brief Append one entry (unsorted) during a bulk rebuild.
 */
bool na_rebuild_add(NameArray *a, const uint8_t key[NI_KEY_LEN], uint16_t contact_index)
{
    if (a->count >= NI_MAX_CONTACTS) { return false; }
    SortedEntry *e = &a->e[a->count++];
    memcpy(e->key, key, NI_KEY_LEN);
    e->contact_index = contact_index;
    e->message_index = NO_MESSAGE;
    return true;
}

static void sift_down(NameArray *a, uint16_t root, uint16_t end)
{
    SortedEntry *e = a->e;
    for (;;)
    {
        uint32_t child = 2u * root + 1u;
        if (child >= end) { return; }
        if (child + 1u < end && entry_cmp(a, &e[child], e[child + 1u].key, e[child + 1u].contact_index) < 0) { child++; }
        if (entry_cmp(a, &e[root], e[child].key, e[child].contact_index) >= 0) { return; }
        SortedEntry tmp = e[root];
        e[root] = e[child];
        e[child] = tmp;
        root = (uint16_t)child;
    }
}

/**
 * @brief Sort the appended entries (in-place heapsort: no recursion, no scratch).
 */
void na_rebuild_finish(NameArray *a)
{
    uint16_t n = a->count;
    if (n < 2) { return; }
    for (uint16_t i = (uint16_t)(n / 2); i-- > 0;)
    {
        sift_down(a, i, n);
    }
    for (uint16_t end = (uint16_t)(n - 1); end > 0; end--)
    {
        SortedEntry tmp = a->e[0];
        a->e[0] = a->e[end];
        a->e[end] = tmp;
        sift_down(a, 0, end);
    }
}

/**
 * @brief Rebuild from an unsorted list of names and their contact sectors.
 *
 * @retval False if n exceeds the capacity (the array is left empty).
 */
bool na_rebuild(NameArray *a, const char *const *names, const uint16_t *sectors, uint16_t n)
{
    na_rebuild_begin(a);
    if (n > NI_MAX_CONTACTS) { return false; }
    for (uint16_t i = 0; i < n; i++)
    {
        uint8_t key[NI_KEY_LEN];
        ni_make_key(names[i], key);
        na_rebuild_add(a, key, sectors[i]);
    }
    na_rebuild_finish(a);
    return true;
}
