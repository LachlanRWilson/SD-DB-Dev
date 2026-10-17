#include <string.h>
#include "sorted_array.h"
#include "contact.h"
#include "hash_table.h"




/**
 * @brief Remove the case of a capitalised character
 *
 * @param c input character
 * @retval lower chase character
 */
static int remove_case(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/**
 * @brief compare the bytes of two string either case insensitive or sensitive
 *
 * @param a string a
 * @param la length of string a
 * @param b string b
 * @param lb length of string b
 * @param ignore_case case insensitive flag (true: ignore case)
 */
static int bytes_cmp(const char *a, size_t la, const char *b, size_t lb, bool ignore_case)
{
    // the compare length is the smallest of lengths
    size_t n = la < lb ? la : lb;

    for (size_t i = 0; i < n; i++)
    {
        int ca = ignore_case ? remove_case((uint8_t)a[i]) : (uint8_t)a[i];
        int cb = ignore_case ? remove_case((uint8_t)b[i]) : (uint8_t)b[i];

        // return compare if mismatch character
        if (ca != cb)
        {
            return ca - cb;
        }
    }

    // return compare in length if no character mismatch
    return (int)la - (int)lb;

}

/**
 * @brief Tiebreak function for contact names. Firstly the starting 8 characters
 * of the name is compared in the cmp function. If a tie break is required the
 * full name is retrieved and compared. If both names are the same, phone
 * numbers are compared
 *
 * @param storage storage abstraction pointer to allow accessing contact information
 * @param in input contact to sorted array
 * @param entry sorted array entry input key is currently tied with
 * @param order 0 if same, > 0 if key1 larger, < 0 if key 1 smaller
 * @retval STRG_RET
 */
static STRG_RET tiebreak_fn(Storage *storage, HashEntry *htable_mem, const SortedArrayProbe *probe, SortedArrayEntry *entry, int *order)
{
    STRG_RET ret;
    ContactBuffer cBuffer;
    Contact *contact;
    int cmp;

    if ((storage == NULL) || (probe == NULL) || (entry == NULL) || (order == NULL))
    {
        return STRG_FAIL;
    }

    // check the contact sector index from the hash entry stored in this slot
    uint16_t contact_index = htable_mem[entry->hash_slot].sector;

    ret = read_contact(storage, contact_index, &cBuffer);
    if (ret != STRG_OK)
    {
        return ret;
    }

    contact = &(cBuffer.contact);

    if ((contact->name_len > MAX_NAME_LEN) || (contact->phone_len) > MAX_PHONE_LEN)
    {
        return STRG_CORRUPT;
    }

    *order = bytes_cmp(probe->name, probe->name_len, contact->name, contact->name_len, true);

    if (*order != 0) {
        return STRG_OK;
    }

    // if the probe needs to check phone number
    if (probe->name_only)
    {
        return STRG_OK;
    }

    // if names are the same check for case differences
    *order = bytes_cmp(probe->name, probe->name_len, contact->name, contact->name_len, false);
    if (*order != 0)
    {
        return STRG_OK;
    }


    // if the names are the same check the phone numbers
    *order = bytes_cmp(probe->phone, probe->phone_len, contact->phone, contact->phone_len, false);
    return STRG_OK;
}

/**
 * @brief create a key from contact name, ignore case
 *
 * @param name contact name
 * @param name_len length of contact name
 * @param out pointer to key array
 */
void sorted_array_make_key(const char *name, size_t name_len, char out[KEY_LEN])
{
    size_t n = (name_len < KEY_LEN) ? name_len : KEY_LEN;

    for (size_t i = 0; i < KEY_LEN; i++)
    {
        if (i < n)
        {
            out[i] = remove_case(name[i]);
        } else {
            out[i] = '\0'; // zere pad
        }
    }
}


/**
 * @brief Populate a probe for contact comparison
 *
 * @param p probe pointer
 * @param c input contact
 */
static void probe_from_contact(SortedArrayProbe *p, const Contact *c)
{
    sorted_array_make_key(c->name, c->name_len, p->key);
    p->name = c->name;
    p->name_len = c->name_len;
    p->phone = c->phone;
    p->phone_len = c->phone_len;
    p->name_only = false;
}

/**
 * @brief Populate a probe for string comparison
 *
 * @param p probe pointer
 * @param s input string
 * @param len input string length
 */
static void probe_from_string(SortedArrayProbe *p, const char *s, size_t len)
{
    sorted_array_make_key(s, len, p->key);
    p->name = s;
    p->name_len = len;
    p->phone = NULL;
    p->phone_len = 0;
    p->name_only = true;
}

/**
 * @brief Compare the keys between the input contact and the entry
 *
 * @param sa sorted array struct pointer
 * @param probe probe that is being compared with entry
 * @param entry entry which in being compared with probe
 * @param order 0 if same, > 0 if key1 larger, < 0 if key 1 smaller
 * @retval STRG_RET
 */
static STRG_RET cmp(SortedArray *sa, const SortedArrayProbe *probe, SortedArrayEntry *entry, int *order)
{
    STRG_RET ret;
    int cmp_val;

    // case already ignored by sorted_array_make_key
    cmp_val = memcmp(probe->key, entry->key, KEY_LEN);

    // if the keys are the same do the tiebreak
    if (cmp_val == 0) {

        return sa->tiebreak(sa->storage, sa->htable_mem, probe, entry, order);
    }

    *order = cmp_val;
    return STRG_OK;
}

/**
 * @brief Using binary search, find the next lowest entry compared to the probe.
 * If the lower bound is an exact match set flag to true.
 *
 * @param sa sorted array struct pointer
 * @param probe probe that is being compared with entry
 * @param lower_bound index of the lower entry compared to the probe
 * @param exact_match flag that is true if the lower bound is an exact match to the probe
 * @retval STRG_RET
 */
static STRG_RET binary_search_lower_bound(SortedArray *sa, const SortedArrayProbe *probe, size_t *lower_bound, bool *exact_match)
{
    uint16_t lo = 0;
    uint16_t hi = sa->occupancy;
    bool match = false;

    while (lo < hi)
    {
        uint16_t mid = lo + (hi - lo) / 2;
        int order;
        STRG_RET ret = cmp(sa, probe, &sa->mem[mid], &order);
        if (ret != STRG_OK)
        {
            return ret;
        }

        if (order > 0)
        {
            lo = mid + 1;
        } else {
            hi = mid;
            match = (order == 0);
        }
    }
    *lower_bound = lo;
    *exact_match = match;
    return STRG_OK;
}

/**
 * @brief Using binary search find the first rank <= the prefix
 *
 * @param key key to search
 * @param prefix string that is being searched
 * @param rank pointer to rank which is <= prefix
 * @retval STRG_RET
 */
STRG_RET sorted_array_seek(SortedArray *sa, const char *prefix, size_t *rank)
{
    bool match_found;
    SortedArrayProbe probe;

    if (prefix == NULL)
    {
        return false;
    }

    int prefix_len = strlen(prefix);

    if (prefix_len > MAX_NAME_LEN )
    {
        return false;
    }

    probe_from_string(&probe, prefix, prefix_len);

    return binary_search_lower_bound(sa, &probe, rank, &match_found);
}

/**
 * @brief Using binary search find the contact in the sorted array
 *
 * @param key key to search
 * @param in contact that is being searched for
 * @param rank pointer to rank which is <= prefix
 * @param match_found pointer to bool which stores if the contact is in the sorted array
 * @retval STRG_RET
 */
STRG_RET sorted_array_find(SortedArray *sa, const Contact *in, size_t *rank, bool *match_found)
{
    SortedArrayProbe probe;


    probe_from_contact(&probe, in);

    return binary_search_lower_bound(sa, &probe, rank, match_found);
}


/**
 * @brief Find the lower index of the name of the given contact, then move all the memory greater than index by one
 * and place index into lower index + 1
 *
 * @param sa sorted array struct
 * @param in contact that is being searched for
 * @param contact_index contact sector index
 * @param message_index message sector index
 * @retval STRG_RET
 */
STRG_RET sorted_array_insert(SortedArray *sa, const Contact *contact, uint16_t contact_index)
{
    STRG_RET ret;
    size_t rank;
    bool match_found;

    if (sa->occupancy >= HASH_TABLE_ENTRIES)
    {
        return STRG_FULL;
    }

    ret = sorted_array_find(sa, contact, &rank, &match_found);
    if (ret != STRG_OK)
    {
        return ret;

    }

    if (match_found)
    {
        return false;
    }

    // insert at rank: shift [rank, occupancy) right by one
    memmove(&sa->mem[rank + 1], &sa->mem[rank], (sa->occupancy - rank) * sizeof(SortedArrayEntry));

    // populate memory
    sorted_array_make_key(contact->name, contact->name_len, sa->mem[rank].key);
    sa->mem[rank].hash_slot = contact_index;

    sa->occupancy++;
    return true;
}

/**
 * @brief Find the lower index of the name of the given contact, then move all the memory greater than index back one
 * and place index into lower index
 *
 * @param sa sorted array struct
 * @param contact contact that is being searched for
 * @retval STRG_RET
 */
STRG_RET sorted_array_remove(SortedArray *sa, const Contact *contact)
{
    STRG_RET ret;
    size_t rank;
    bool match_found;

    if (sa->occupancy <= 0)
    {
        return STRG_FULL;
    }

    ret = sorted_array_find(sa, contact, &rank, &match_found);
    if (ret != STRG_OK)
    {
        return ret;
    }

    // if an exact match not found do not delete anything
    if (!match_found)
    {
        return false;
    }

    // remove at rank: shift (rank, occupancy) left by one
    memmove(&sa->mem[rank], &sa->mem[rank + 1], (sa->occupancy - rank - 1) * sizeof(SortedArrayEntry));

    sa->occupancy--;
    return true;

}

bool sorted_array_get_range(SortedArray *sa, size_t start, size_t n, size_t *out_count, SortedArrayEntry *out)
{
    if ((sa == NULL) || (out == NULL) || (start >= sa->occupancy))
    {
        return false;
    }

    if (n > sa->occupancy - start)
    {
        n = sa->occupancy - start;
    }

    memcpy(out, &sa->mem[start], n * sizeof(SortedArrayEntry));
    *out_count = n;
    return true;

}

/**
 * @brief Append a SortedArrayEntry to the end of the sorted array, this is only
 * done on start up for quick population then sorting
 *
 * @param sa sorted array struct
 * @param in sorted array entry being added
 */
bool sorted_array_append(SortedArray *sa, SortedArrayEntry *in)
{
    if (sa->occupancy >= HASH_TABLE_ENTRIES)
    {
        return false;
    }

    sa->mem[sa->occupancy] = *in;
    sa->occupancy++;
    return true;
}

/**
 * @brief Compare two entries already in the sorted array. The keys are compared
 * in RAM first, only on a key tie is entry a read from storage and turned into
 * a probe for the tiebreak against entry b
 *
 * @param sa sorted array struct pointer
 * @param a first entry
 * @param b second entry
 * @param order 0 if same, > 0 if a larger, < 0 if a smaller
 * @retval STRG_RET
 */
static STRG_RET entry_cmp(SortedArray *sa, SortedArrayEntry *a, SortedArrayEntry *b, int *order)
{
    STRG_RET ret;
    ContactBuffer cBuffer;
    SortedArrayProbe probe;
    int cmp_val;

    // case already ignored by sorted_array_make_key
    cmp_val = memcmp(a->key, b->key, KEY_LEN);
    if (cmp_val != 0)
    {
        *order = cmp_val;
        return STRG_OK;
    }

    // keys tied, read entry a's full contact to build a probe
    ret = read_contact(sa->storage, sa->htable_mem[a->hash_slot].sector, &cBuffer);
    if (ret != STRG_OK)
    {
        return ret;
    }

    if ((cBuffer.contact.name_len > MAX_NAME_LEN) || (cBuffer.contact.phone_len > MAX_PHONE_LEN))
    {
        return STRG_CORRUPT;
    }

    // probe points into cBuffer, which stays alive for the tiebreak call
    probe_from_contact(&probe, &cBuffer.contact);

    return sa->tiebreak(sa->storage, sa->htable_mem, &probe, b, order);
}

/**
 * @brief Swap two entries in the sorted array
 *
 * @param a first entry
 * @param b second entry
 */
static void entry_swap(SortedArrayEntry *a, SortedArrayEntry *b)
{
    SortedArrayEntry temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * @brief Move the entry at root down the max heap [0, end) until both of its
 * children are smaller than it
 *
 * @param sa sorted array struct pointer
 * @param root index of the entry being moved down
 * @param end number of entries in the heap
 * @retval STRG_RET
 */
static STRG_RET sift_down(SortedArray *sa, size_t root, size_t end)
{
    STRG_RET ret;
    int order;
    size_t child;

    while ((child = (2 * root) + 1) < end)
    {
        // pick the larger of the two children
        if ((child + 1) < end)
        {
            ret = entry_cmp(sa, &sa->mem[child], &sa->mem[child + 1], &order);
            if (ret != STRG_OK)
            {
                return ret;
            }

            if (order < 0)
            {
                child++;
            }
        }

        // heap property holds once root is not smaller than its larger child
        ret = entry_cmp(sa, &sa->mem[root], &sa->mem[child], &order);
        if (ret != STRG_OK)
        {
            return ret;
        }

        if (order >= 0)
        {
            break;
        }

        entry_swap(&sa->mem[root], &sa->mem[child]);
        root = child;
    }

    return STRG_OK;
}

/**
 * @brief Sort the sorted array in place using heapsort. Used after the array
 * has been populated with sorted_array_append on start up
 *
 * @param sa sorted array struct
 * @retval STRG_RET
 */
STRG_RET sorted_array_sort(SortedArray *sa)
{
    STRG_RET ret;
    size_t n;

    if ((sa == NULL) || (sa->mem == NULL) || (sa->htable_mem == NULL))
    {
        return STRG_FAIL;
    }

    n = sa->occupancy;
    if (n < 2)
    {
        return STRG_OK;
    }

    // build a max heap, starting from the last entry that has a child
    for (size_t i = n / 2; i-- > 0;)
    {
        ret = sift_down(sa, i, n);
        if (ret != STRG_OK)
        {
            return ret;
        }
    }

    // move the largest entry to the end of the array, then repair the heap
    for (size_t end = n - 1; end > 0; end--)
    {
        entry_swap(&sa->mem[0], &sa->mem[end]);

        ret = sift_down(sa, 0, end);
        if (ret != STRG_OK)
        {
            return ret;
        }
    }

    return STRG_OK;
}

/**
 * @brief Initialise a sorted array structure
 *
 * @param sa sorted array struct
 * @param storage storage struct
 * @param sa_memory pre-allocated RAM_D1 memory pointer
 * @param rank pointer to rank which is <= prefix
 * @param match_found pointer to bool which stores if the contact is in the sorted array
 * @retval STRG_RET
 */
bool sorted_array_init(SortedArray *sa, HashEntry *htable_mem, Storage *storage, SortedArrayEntry* sa_memory)
{
    if ((sa == NULL) || (storage == NULL) || (sa_memory == NULL))
    {
        return false;
    }
    sa->storage = storage;
    sa->mem = sa_memory;
    sa->htable_mem = htable_mem;
    sa->tiebreak = tiebreak_fn;
    sa->occupancy = 0;

    return true;
}

