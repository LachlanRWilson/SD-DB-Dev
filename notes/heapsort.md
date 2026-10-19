# Heapsort: name index and chat order boot rebuild

At boot, both RAM indexes are rebuilt in the same way: append every entry unsorted, then sort once with an in-place heapsort.

| Index | Sort function | Array | Entry | Comparator |
|---|---|---|---|---|
| Name index (sorted array) | `sorted_array_sort` (`source/app/src/sorted_array.c`) | `sa_mem` | `SortedArrayEntry`: `key[8]`, `hash_slot` (10 B) | `entry_cmp`: key `memcmp`, then a storage tiebreak. **Can fail** |
| Chat order | `chat_scratch_sort` (`source/app/src/database.c`) | `sa_mem` (borrowed as scratch) | `ChatOrderScratch`: `timestamp_hi`, `timestamp_lo`, `hash_slot` (6 B) | `chat_scratch_cmp`: timestamp, then hash slot. RAM only, **can't fail** |

Both sorts use the same algorithm with the same loop structure (`sift_down`, then heapify, then extract). The only differences are the entry type and whether a comparison can return an error.

## Why heapsort and not `qsort`

- **Guaranteed O(n log n).** Quicksort can degrade to O(n²). Heapsort's worst case is the same as its average, so worst-case boot time is fixed.
- **No recursion.** Stack use is a few locals, whatever n is, so the DB task's FreeRTOS stack can be sized exactly.
- **O(1) extra memory.** It sorts in place, with no `malloc` or scratch buffer.
- **The comparator can take context and return errors.** The name index needs `Storage *` and `htable_mem` to break ties, and it has to pass back a failed storage read. A `qsort` comparator only gets two pointers and returns an `int`.
- **Costs:** about 2n log n comparisons (quicksort needs about 1.4n log n), and the memory access jumps between parent and child entries rather than scanning sequentially. It also isn't stable, but neither order has equal elements, so that doesn't matter.

---

## 1. The array as a tree

There's no separate tree structure. The array itself is read as a complete binary tree using index arithmetic:

```
parent(i) = (i - 1) / 2
left(i)   = 2i + 1
right(i)  = 2i + 2

 index:   0   1   2   3   4   5   6
        ┌───┬───┬───┬───┬───┬───┬───┐
 array: │ a │ b │ c │ d │ e │ f │ g │
        └───┴───┴───┴───┴───┴───┴───┘

 tree:           a(0)
               /      \
           b(1)        c(2)
          /   \       /   \
       d(3)  e(4)  f(5)  g(6)
```

**Max-heap property:** every parent is ≥ both of its children under the comparator. The root (index 0) is therefore the entry that belongs **last** in the sorted output.

## 2. `sift_down(root, end)`

`sift_down` repairs one node whose subtrees are already heaps. It looks only at indices `[0, end)`.

```
while root has a child (2*root+1 < end):
    child = the larger of left and right
    if root >= child: stop                 // heap property holds
    swap(root, child); root = child        // sink one level
```

```
   before                 after one step          done
     [3]                     [9]                   [9]
    /   \       3 < 9       /   \      3 < 7      /   \
  [9]   [5]    ─────▶     [3]   [5]   ─────▶    [7]   [5]
  / \                     / \                    / \
[7] [1]                 [7] [1]                [3] [1]
```

At most log₂(n) levels, so each call is O(log n) comparisons.

## 3. The two phases

```
 Phase 1: heapify, O(n)
 ──────────────────────
 for i = n/2 - 1 down to 0:  sift_down(i, n)

   Leaves (i >= n/2) are already one-entry heaps, so start at the last parent
   and work back to the root. Afterwards the whole array is a max heap.

 Phase 2: extract, O(n log n)
 ────────────────────────────
 for end = n - 1 down to 1:
     swap(0, end)            // largest remaining entry goes into its final slot
     sift_down(0, end)       // repair the heap, which is now one entry smaller

        heap (unsorted)          sorted tail (final positions)
   ┌─────────────────────────┬────────────────────────────┐
   │ 0 ............... end-1 │ end ................... n-1│
   └─────────────────────────┴────────────────────────────┘
             ◀── shrinks by one each step ──   grows ──▶
```

The loop `for (size_t i = n / 2; i-- > 0;)` counts down safely with an unsigned `size_t`. The `i--` in the condition is evaluated before the body, so the body sees `n/2 - 1` through to `0`.

---

## 4. Name index: `sorted_array_sort`

**Output order:** ascending by name. That is case-insensitive name first, then case-sensitive name, then phone number. "Largest" means alphabetically last, so phase 2 fills the array from Z down to A.

### Comparator: `entry_cmp(sa, a, b, &order)`

```
              entry_cmp(a, b)
                    │
        memcmp(a->key, b->key, 8)     8-byte lower-case key prefix (RAM only)
                    │
         ┌──── != 0 ┴ == 0 ────┐
         ▼                     ▼
   order = memcmp    read_contact(a)              ◀── storage read #1
   return STRG_OK           │
                      fail? ─────▶ return ret     (sort stops, error passes up)
                            │
                    tiebreak_fn(probe a, b)
                            │
                      read_contact(b)             ◀── storage read #2
                            │
                    1. full name, case-insensitive
                    2. full name, case-sensitive
                    3. phone number
                            │
                     order, STRG_OK
```

- Most comparisons end at the `memcmp`. Only names that share the same first 8 characters, ignoring case, such as `Alexander` and `Alexandra` (both keyed `alexande`), need storage reads.
- Each tied comparison costs **2 contact reads**. Heapsort's extra comparisons therefore matter most when many contacts share a prefix.
- Every comparison returns `STRG_RET`. `sift_down` and `sorted_array_sort` check it after each call and return it straight away, so a failed read during boot leaves the sort part-way and `database_init` returns the error. This is the main reason `qsort` can't be used here.

### Boot flow: `name_index_build`

```
 hash table (slot order)                    sa_mem
 ┌──────┬──────┬──────┬──────┐   append   ┌──────────┬──────────┬──────────┐
 │ Zed  │ ---- │alice │ Bob  │ ─────────▶ │ "zed\0.."│ "alice\0"│ "bob\0.."│  unsorted
 └──────┴──────┴──────┴──────┘            └──────────┴──────────┴──────────┘
   read_contact() each slot,                              │
   make_key(name)                                         │ sorted_array_sort
                                                          ▼
                                          ┌──────────┬──────────┬──────────┐
                                          │ "alice\0"│ "bob\0.."│ "zed\0.."│  rank 0..n-1
                                          └──────────┴──────────┴──────────┘
```

---

## 5. Chat order: `chat_scratch_sort`

**Output order:** newest latest message first, with ties broken by ascending hash slot so the order is the same on every boot.

### Comparator: `chat_scratch_cmp(a, b)`

```
 ta = (timestamp_hi << 16) | timestamp_lo        // rebuild the 32-bit timestamp
 tb = ...

 ta != tb ?  ── yes ──▶  newer first: (ta > tb) ? -1 : +1
     │
     no
     ▼
 a->hash_slot - b->hash_slot                     // deterministic tiebreak
```

"Less than" here means *comes first*, which is the **newer** chat. The max heap's root is therefore the **oldest** chat, and phase 2 fills the array from the tail (oldest) back to the head (newest).

The timestamp is stored as two `uint16_t` halves so the struct is 6 B with 2-byte alignment. As one `uint32_t` it would pad to 8 B, and the `STATIC_ASSERT` that the scratch fits in `sa_mem` would have less margin.

### Memory: `sa_mem` borrowed as scratch

```
 RAM_D1
 ┌───────────────────────────────────────────────────────┐
 │ sa_mem   SORTED_ARRAY_ENTRIES × 10 B                  │
 │ ┌───────────────────────────────────┐                 │
 │ │ ChatOrderScratch[CHAT_ORDER_ENTRIES] × 6 B           │  step 1: chat order scratch
 │ └───────────────────────────────────┘                 │
 │                     ... overwritten by ...            │
 │ ┌───────────────────────────────────────────────────┐ │
 │ │ SortedArrayEntry[...]                             │ │  step 2: name index
 │ └───────────────────────────────────────────────────┘ │
 ├───────────────────────────────────────────────────────┤
 │ co_mem   CHAT_ORDER_ENTRIES × 2 B  (uint16_t slots)   │  final chat order
 └───────────────────────────────────────────────────────┘
```

Because of this ordering, `database_init` must call `chat_order_build` **before** `name_index_build`.

### Boot flow: `chat_order_build`

```
 for each occupied slot with latest_msg_extent != UINT16_MAX:
     read_n_messages(latest_msg_extent, 0, 1)      ◀── all storage reads happen here
     scratch[chats++] = { ts_hi, ts_lo, slot }

 chat_scratch_sort(scratch, chats)                  ◀── RAM only, can't fail

 for i in 0..chats-1:
     chat_order_append(slot)                        ──▶ co_mem, rank 0 = newest
```

All storage I/O happens before the sort starts. This is why the chat order comparator has no error path and `chat_scratch_sort` returns `void`.

### Worked example (n = 5)

The entries are `(timestamp, slot)`. The target output is `E B D A C`.

```
 initial (append order)
 idx:   0        1        2        3        4
      A(5,s3)  B(9,s1)  C(2,s7)  D(7,s4)  E(9,s0)

 ── Phase 1: heapify (i = 1, then 0) ────────────────────────────────

 i=1: B vs children D(7), E(9,s0). The larger (older) child is D. B is newer, so swap.
      A(5)  D(7)  C(2)  B(9,s1)  E(9,s0)

 i=0: A vs children D(7), C(2). The larger (older) child is C. A is newer, so swap.
      C(2)  D(7)  A(5)  B(9,s1)  E(9,s0)

               C(2)              max heap: root = oldest chat
              /    \
          D(7)      A(5)
          /  \
     B(9,s1) E(9,s0)

 ── Phase 2: extract ──────────────────────────────────────────────

 end=4  swap 0↔4, sift:  A(5)    D(7)    E(9,s0) B(9,s1) │ C(2)
 end=3  swap 0↔3, sift:  D(7)    B(9,s1) E(9,s0)         │ A(5) C(2)
 end=2  swap 0↔2, sift:  B(9,s1) E(9,s0)                 │ D(7) A(5) C(2)
        (B vs E: same timestamp, s1 > s0, so B is "larger" and goes after E)
 end=1  swap 0↔1:        E(9,s0)                         │ B(9,s1) D(7) A(5) C(2)

 result → co_mem:  E  B  D  A  C      rank 0 = newest
```

---

## 6. Side by side

|  | Name index | Chat order |
|---|---|---|
| Entries sorted | every occupied contact | contacts with ≥ 1 message |
| Entry size | 10 B | 6 B |
| Comparator cost | `memcmp` of 8 B; ties add 2 SD reads | two shifts and a compare |
| Storage I/O | during the sort (ties only) | before the sort (one read per chat) |
| Error handling | `STRG_RET` checked after every compare | none needed (`void`) |
| Root of max heap | alphabetically last | oldest chat |
| Result stored in | `sa_mem` (in place) | copied from `sa_mem` scratch to `co_mem` |
| Time | O(n log n) worst case | O(n log n) worst case |
| Stack / extra memory | O(1) / none | O(1) / none |
