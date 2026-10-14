#include "avl_tree.h"

uint8_t avl_max_depth;

static inline int8_t h(const AvlTree *t, uint16_t i)
{
    return (i == TREE_NIL) ? 0 : t->n[i].height;
}

static inline void update_height(AvlTree *t, uint16_t i)
{
    int8_t hl = h(t, t->n[i].left);
    int8_t hr = h(t, t->n[i].right);
    t->n[i].height = (int8_t)(1 + ((hl > hr) ? hl : hr));
}

static inline void note_depth(uint8_t depth)
{
    if (depth > avl_max_depth) { avl_max_depth = depth; }
}

static inline int node_cmp(const AvlTree *t, const uint8_t *key, uint16_t sector, uint16_t i)
{
    return ni_cmp(key, sector, t->n[i].key, t->n[i].contact_index, t->tiebreak);
}

static uint16_t rotate_right(AvlTree *t, uint16_t y)
{
    uint16_t x = t->n[y].left;
    t->n[y].left = t->n[x].right;
    t->n[x].right = y;
    update_height(t, y);
    update_height(t, x);
    return x;
}

static uint16_t rotate_left(AvlTree *t, uint16_t x)
{
    uint16_t y = t->n[x].right;
    t->n[x].right = t->n[y].left;
    t->n[y].left = x;
    update_height(t, x);
    update_height(t, y);
    return y;
}

/**
 * @brief Restore the AVL property at node i (LL, RR, LR and RL cases).
 *
 * @retval The new root of the subtree.
 */
static uint16_t rebalance(AvlTree *t, uint16_t i)
{
    update_height(t, i);
    int bf = h(t, t->n[i].left) - h(t, t->n[i].right);
    if (bf > 1)
    {
        uint16_t l = t->n[i].left;
        if (h(t, t->n[l].left) < h(t, t->n[l].right)) { t->n[i].left = rotate_left(t, l); }
        return rotate_right(t, i);
    }
    if (bf < -1)
    {
        uint16_t r = t->n[i].right;
        if (h(t, t->n[r].right) < h(t, t->n[r].left)) { t->n[i].right = rotate_right(t, r); }
        return rotate_left(t, i);
    }
    return i;
}

static uint16_t node_alloc(AvlTree *t)
{
    uint16_t i = t->free_head;
    if (i != TREE_NIL) { t->free_head = t->n[i].left; }
    return i;
}

static void node_free(AvlTree *t, uint16_t i)
{
    t->n[i].left = t->free_head;
    t->free_head = i;
}

static uint16_t insert_rec(AvlTree *t, uint16_t i, uint16_t fresh, bool *ok, uint8_t depth)
{
    if (i == TREE_NIL)
    {
        note_depth(depth);
        return fresh;
    }
    int c = node_cmp(t, t->n[fresh].key, t->n[fresh].contact_index, i);
    if (c == 0)
    {
        note_depth(depth);
        *ok = false;
        return i;
    }
    if (c < 0) { t->n[i].left = insert_rec(t, t->n[i].left, fresh, ok, (uint8_t)(depth + 1)); }
    else { t->n[i].right = insert_rec(t, t->n[i].right, fresh, ok, (uint8_t)(depth + 1)); }
    return rebalance(t, i);
}

/**
 * @brief Unlink the smallest node of subtree i, rebalancing on the way up.
 *
 * @param min set to the unlinked node
 * @retval The new root of the subtree.
 */
static uint16_t remove_min_rec(AvlTree *t, uint16_t i, uint16_t *min, uint8_t depth)
{
    if (t->n[i].left == TREE_NIL)
    {
        note_depth(depth);
        *min = i;
        return t->n[i].right;
    }
    t->n[i].left = remove_min_rec(t, t->n[i].left, min, (uint8_t)(depth + 1));
    return rebalance(t, i);
}

static uint16_t remove_rec(AvlTree *t, uint16_t i, const uint8_t *key, uint16_t sector, bool *ok, uint8_t depth)
{
    if (i == TREE_NIL)
    {
        note_depth(depth);
        *ok = false;
        return TREE_NIL;
    }
    int c = node_cmp(t, key, sector, i);
    if (c < 0) { t->n[i].left = remove_rec(t, t->n[i].left, key, sector, ok, (uint8_t)(depth + 1)); }
    else if (c > 0) { t->n[i].right = remove_rec(t, t->n[i].right, key, sector, ok, (uint8_t)(depth + 1)); }
    else
    {
        uint16_t l = t->n[i].left;
        uint16_t r = t->n[i].right;
        node_free(t, i);
        if (l == TREE_NIL) { note_depth(depth); return r; }
        if (r == TREE_NIL) { note_depth(depth); return l; }

        uint16_t succ;
        r = remove_min_rec(t, r, &succ, (uint8_t)(depth + 1));
        t->n[succ].left = l;
        t->n[succ].right = r;
        return rebalance(t, succ);
    }
    return rebalance(t, i);
}

/**
 * @brief Exact (key, sector) lookup, iterative.
 *
 * @retval The node index or TREE_NIL.
 */
static uint16_t find_exact(const AvlTree *t, const uint8_t *key, uint16_t sector)
{
    uint16_t i = t->root;
    while (i != TREE_NIL)
    {
        int c = node_cmp(t, key, sector, i);
        if (c == 0) { return i; }
        i = (c < 0) ? t->n[i].left : t->n[i].right;
    }
    return TREE_NIL;
}

void avl_reset_max_depth(void)
{
    avl_max_depth = 0;
}

/**
 * @brief Initialise an empty tree and chain every node onto the free list.
 */
void avl_init(AvlTree *t, ni_tiebreak_fn tiebreak)
{
    for (uint16_t i = 0; i < NI_MAX_CONTACTS; i++)
    {
        t->n[i].left = (uint16_t)(i + 1);
    }
    t->n[NI_MAX_CONTACTS - 1].left = TREE_NIL;
    t->free_head = 0;
    t->root = TREE_NIL;
    t->count = 0;
    t->tiebreak = tiebreak;
}

/**
 * @brief Insert a pre-built key.
 *
 * @retval False if the pool is full or (key, contact_index) is already present.
 */
bool avl_insert_key(AvlTree *t, const uint8_t key[NI_KEY_LEN], uint16_t contact_index)
{
    uint16_t fresh = node_alloc(t);
    if (fresh == TREE_NIL) { return false; }

    AvlNode *nd = &t->n[fresh];
    memcpy(nd->key, key, NI_KEY_LEN);
    nd->contact_index = contact_index;
    nd->message_index = NO_MESSAGE;
    nd->left = TREE_NIL;
    nd->right = TREE_NIL;
    nd->height = 1;

    bool ok = true;
    t->root = insert_rec(t, t->root, fresh, &ok, 1);
    if (!ok)
    {
        node_free(t, fresh);
        return false;
    }
    t->count++;
    return true;
}

/**
 * @brief Insert a contact by name.
 */
bool avl_insert(AvlTree *t, const char *name, uint16_t contact_index)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    return avl_insert_key(t, key, contact_index);
}

/**
 * @brief Remove the entry for (name, contact_index).
 *
 * @retval False if it is not present.
 */
bool avl_remove(AvlTree *t, const char *name, uint16_t contact_index)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    bool ok = true;
    t->root = remove_rec(t, t->root, key, contact_index, &ok, 1);
    if (ok) { t->count--; }
    return ok;
}

/**
 * @brief Find the first contact (in index order) with this full name.
 *
 * @retval Its contact_index, or NI_NOT_FOUND.
 */
uint16_t avl_find(const AvlTree *t, const char *name)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    uint16_t i = t->root;
    uint16_t match = TREE_NIL;
    while (i != TREE_NIL)
    {
        int c = ni_cmp(t->n[i].key, t->n[i].contact_index, key, NI_PROBE_SECTOR, t->tiebreak);
        if (c >= 0)
        {
            if (c == 0) { match = i; }
            i = t->n[i].left;
        }
        else
        {
            i = t->n[i].right;
        }
    }
    return (match == TREE_NIL) ? NI_NOT_FOUND : t->n[match].contact_index;
}

/**
 * @brief Update message_index of an existing entry in place (no reordering).
 */
bool avl_set_message(AvlTree *t, const char *name, uint16_t contact_index, uint16_t message_index)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    uint16_t i = find_exact(t, key, contact_index);
    if (i == TREE_NIL) { return false; }
    t->n[i].message_index = message_index;
    return true;
}

uint16_t avl_count(const AvlTree *t)
{
    return t->count;
}

int avl_height(const AvlTree *t)
{
    return h(t, t->root);
}

static int valid_rec(const AvlTree *t, uint16_t i, uint16_t *seen)
{
    if (i == TREE_NIL) { return 0; }
    if (i >= NI_MAX_CONTACTS || ++(*seen) > t->count) { return -1; }
    int hl = valid_rec(t, t->n[i].left, seen);
    int hr = valid_rec(t, t->n[i].right, seen);
    if (hl < 0 || hr < 0 || hl - hr > 1 || hr - hl > 1) { return -1; }
    int hi = 1 + ((hl > hr) ? hl : hr);
    return (hi == t->n[i].height) ? hi : -1;
}

/**
 * @brief Check heights, balance factors and node count (test helper).
 */
bool avl_valid(const AvlTree *t)
{
    uint16_t seen = 0;
    return valid_rec(t, t->root, &seen) >= 0 && seen == t->count;
}

static bool push_left(AvlIter *it, uint16_t i)
{
    while (i != TREE_NIL)
    {
        if (it->top >= AVL_ITER_DEPTH) { return false; }
        it->stack[it->top++] = i;
        i = it->t->n[i].left;
    }
    return true;
}

/**
 * @brief Position an iterator on the smallest entry.
 *
 * @retval False if the tree is deeper than AVL_ITER_DEPTH.
 */
bool avl_begin(AvlIter *it, const AvlTree *t)
{
    it->t = t;
    it->top = 0;
    return push_left(it, t->root);
}

/**
 * @brief Copy the current entry to out and advance.
 *
 * @retval False at the end.
 */
bool avl_next(AvlIter *it, SortedEntry *out)
{
    if (it->top == 0) { return false; }
    uint16_t i = it->stack[--it->top];
    const AvlNode *nd = &it->t->n[i];
    memcpy(out->key, nd->key, NI_KEY_LEN);
    out->contact_index = nd->contact_index;
    out->message_index = nd->message_index;
    push_left(it, nd->right);
    return true;
}

/**
 * @brief Position an iterator on the entry of rank `rank` by walking from the
 *        start (O(rank): nodes do not store subtree sizes).
 */
bool avl_seek_rank(AvlIter *it, const AvlTree *t, uint16_t rank)
{
    if (!avl_begin(it, t)) { return false; }
    for (uint16_t k = 0; k < rank; k++)
    {
        if (it->top == 0) { return false; }
        uint16_t i = it->stack[--it->top];
        push_left(it, t->n[i].right);
    }
    return it->top != 0;
}

/**
 * @brief Position an iterator on the first entry >= name (O(log n)).
 */
bool avl_seek(AvlIter *it, const AvlTree *t, const char *name)
{
    uint8_t key[NI_KEY_LEN];
    ni_make_key(name, key);
    it->t = t;
    it->top = 0;
    uint16_t i = t->root;
    while (i != TREE_NIL)
    {
        if (ni_cmp(t->n[i].key, t->n[i].contact_index, key, NI_PROBE_SECTOR, t->tiebreak) >= 0)
        {
            if (it->top >= AVL_ITER_DEPTH) { return false; }
            it->stack[it->top++] = i;
            i = t->n[i].left;
        }
        else
        {
            i = t->n[i].right;
        }
    }
    return true;
}

/**
 * @brief Start a bulk rebuild: entries are appended to nodes 0..n-1 unsorted,
 *        then sorted in place and linked into a balanced tree.
 */
void avl_rebuild_begin(AvlTree *t)
{
    t->root = TREE_NIL;
    t->count = 0;
}

/**
 * @brief Append one entry (unsorted) during a bulk rebuild.
 */
bool avl_rebuild_add(AvlTree *t, const uint8_t key[NI_KEY_LEN], uint16_t contact_index)
{
    if (t->count >= NI_MAX_CONTACTS) { return false; }
    AvlNode *nd = &t->n[t->count++];
    memcpy(nd->key, key, NI_KEY_LEN);
    nd->contact_index = contact_index;
    nd->message_index = NO_MESSAGE;
    return true;
}

static void sift_down(AvlTree *t, uint16_t root, uint16_t end)
{
    AvlNode *n = t->n;
    for (;;)
    {
        uint32_t child = 2u * root + 1u;
        if (child >= end) { return; }
        if (child + 1u < end && node_cmp(t, n[child].key, n[child].contact_index, (uint16_t)(child + 1u)) < 0) { child++; }
        if (node_cmp(t, n[root].key, n[root].contact_index, (uint16_t)child) >= 0) { return; }
        AvlNode tmp = n[root];
        n[root] = n[child];
        n[child] = tmp;
        root = (uint16_t)child;
    }
}

static uint16_t link_balanced(AvlTree *t, int32_t lo, int32_t hi, uint8_t depth)
{
    if (lo > hi) { return TREE_NIL; }
    note_depth(depth);
    uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
    t->n[mid].left = link_balanced(t, lo, (int32_t)mid - 1, (uint8_t)(depth + 1));
    t->n[mid].right = link_balanced(t, (int32_t)mid + 1, hi, (uint8_t)(depth + 1));
    update_height(t, mid);
    return mid;
}

/**
 * @brief Heapsort the appended nodes in place, link them into a perfectly
 *        balanced tree and put the remaining nodes on the free list.
 */
void avl_rebuild_finish(AvlTree *t)
{
    uint16_t n = t->count;
    if (n >= 2)
    {
        for (uint16_t i = (uint16_t)(n / 2); i-- > 0;)
        {
            sift_down(t, i, n);
        }
        for (uint16_t end = (uint16_t)(n - 1); end > 0; end--)
        {
            AvlNode tmp = t->n[0];
            t->n[0] = t->n[end];
            t->n[end] = tmp;
            sift_down(t, 0, end);
        }
    }
    t->root = link_balanced(t, 0, (int32_t)n - 1, 1);

    t->free_head = TREE_NIL;
    for (uint16_t i = NI_MAX_CONTACTS; i-- > n;)
    {
        node_free(t, i);
    }
}

/**
 * @brief Rebuild from an unsorted list of names and their contact sectors.
 *
 * @retval False if n exceeds the capacity (the tree is left empty).
 */
bool avl_rebuild(AvlTree *t, const char *const *names, const uint16_t *sectors, uint16_t n)
{
    avl_rebuild_begin(t);
    if (n > NI_MAX_CONTACTS)
    {
        avl_init(t, t->tiebreak);
        return false;
    }
    for (uint16_t i = 0; i < n; i++)
    {
        uint8_t key[NI_KEY_LEN];
        ni_make_key(names[i], key);
        avl_rebuild_add(t, key, sectors[i]);
    }
    avl_rebuild_finish(t);
    return true;
}
