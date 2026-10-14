#ifndef NI_AVL_TREE_H
#define NI_AVL_TREE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "name_index_common.h"

/**
 * Candidate 2: AVL tree in a static node pool addressed by uint16_t indices.
 * No malloc: free nodes are chained through `left`.
 */

#define TREE_NIL 0xFFFF
#define AVL_ITER_DEPTH 24

// 17B of fields, 18B with padding
typedef struct {
    uint8_t key[NI_KEY_LEN];    // case-folded, zero-padded name prefix (8B)
    uint16_t contact_index;     // contact sector (2B)
    uint16_t message_index;     // first message sector, NO_MESSAGE if none (2B)
    uint16_t left;              // left child, or next free node (2B)
    uint16_t right;             // right child (2B)
    int8_t height;              // subtree height, leaf = 1 (1B)
} AvlNode;

typedef struct {
    AvlNode n[NI_MAX_CONTACTS];  // node pool
    uint16_t root;               // root node, TREE_NIL if empty
    uint16_t free_head;          // first free node, TREE_NIL if full
    uint16_t count;              // nodes in use
    ni_tiebreak_fn tiebreak;     // full-name tie-break callback
} AvlTree;

// Stack-based in-order iterator (no recursion)
typedef struct {
    const AvlTree *t;
    uint16_t stack[AVL_ITER_DEPTH];
    uint8_t top;
} AvlIter;

// Deepest insert/delete recursion seen since the last avl_reset_max_depth()
extern uint8_t avl_max_depth;

void avl_reset_max_depth(void);

void avl_init(AvlTree *t, ni_tiebreak_fn tiebreak);

bool avl_insert(AvlTree *t, const char *name, uint16_t contact_index);

bool avl_insert_key(AvlTree *t, const uint8_t key[NI_KEY_LEN], uint16_t contact_index);

bool avl_remove(AvlTree *t, const char *name, uint16_t contact_index);

uint16_t avl_find(const AvlTree *t, const char *name);

bool avl_set_message(AvlTree *t, const char *name, uint16_t contact_index, uint16_t message_index);

uint16_t avl_count(const AvlTree *t);

int avl_height(const AvlTree *t);

bool avl_valid(const AvlTree *t);

bool avl_begin(AvlIter *it, const AvlTree *t);

bool avl_seek_rank(AvlIter *it, const AvlTree *t, uint16_t rank);

bool avl_seek(AvlIter *it, const AvlTree *t, const char *name);

bool avl_next(AvlIter *it, SortedEntry *out);

void avl_rebuild_begin(AvlTree *t);

bool avl_rebuild_add(AvlTree *t, const uint8_t key[NI_KEY_LEN], uint16_t contact_index);

void avl_rebuild_finish(AvlTree *t);

bool avl_rebuild(AvlTree *t, const char *const *names, const uint16_t *sectors, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif
