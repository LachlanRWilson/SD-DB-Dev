#include <string.h>
#include "sorted_array.h"
#include "contact.h"




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
static STRG_RET tiebreak_fn(Storage *storage, const SortedArrayProbe *probe, SortedArrayEntry *entry, int *order)
{
    STRG_RET ret;
    ContactBuffer cBuffer;
    Contact *contact;
    int cmp;

    if ((storage == NULL) || (probe == NULL) || (entry == NULL) || (order == NULL))
    {
        return STRG_FAIL;
    }

    ret = read_contact(storage, entry->contact_index, &cBuffer);
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
static void make_key(const char *name, size_t name_len, char out[KEY_LEN])
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
    make_key(c->name, c->name_len, p->key);
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
    make_key(s, len, p->key);
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

    // case already ignored by make_key
    cmp_val = memcmp(probe->key, entry->key, KEY_LEN);

    // if the keys are the same do the tiebreak
    if (cmp_val == 0) {

        return sa->tiebreak(sa->storage, probe, entry, order);
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


bool sorted_array_insert(SortedArray *sa, const Contact *contact, uint16_t contact_index, uint16_t message_index)
{
    return true;
}

bool sorted_array_remove(SortedArray *sa, const Contact *contact)
{
    return true;

}

bool sorted_array_get_range(SortedArray *sa, size_t start, size_t n, SortedArrayEntry *out)
{
    return true;

}

bool sorted_array_append(SortedArray *sa, SortedArrayEntry in)
{
    return true;
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
bool sorted_array_init(SortedArray *sa, Storage *storage, SortedArrayEntry* sa_memory)
{
    if ((sa == NULL) || (storage == NULL) || (sa_memory == NULL))
    {
        return false;
    }
    sa->storage = storage;
    sa->mem = sa_memory;
    sa->tiebreak = tiebreak_fn;
    sa->occupancy = 0;

    return true;
}

