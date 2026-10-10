# `tests/test_hash_table_iterator.cpp` — Review Notes

Covers the hash table iterator in [`hash_table.c`](../../source/app/src/hash_table.c)
(`hash_table_iterator_init()` and the `next`/`prev`/`get` functions behind the generic
[`Iterator`](../../source/app/include/iterator.h) interface). It walks the occupied slots of
`table->htable` and is what `hash_get_contact_list()` uses to page through contacts. 7 tests in
one suite, `HashTableIteratorTest`, all passing (`ctest -R HashTableIteratorTest`). Built into the
same `test_hash_table` executable as `test_hash_table.cpp` and `test_hash_table_edge.cpp`.

Semantics the tests pin down:

- A new iterator has **no position**; `get()` fails until `next()` or `prev()` is called.
- `next()` walks occupied slots in ascending slot index; `prev()` in descending.
- `get()` returns the **slot index**, not the contact.
- Tombstoned (`ENTRY_DELETED`) slots are skipped.
- Once exhausted, `next()`/`prev()` return false and `get()` fails.

Same semantics as the usage bitmap iterator (see the iterator section of
[test_usage_bitmap.md](test_usage_bitmap.md)), so the two files read well together.

## Checklist

- [ ] GetFailsBeforeFirstAdvance
- [ ] EmptyTableFindsNothing
- [ ] NextVisitsOccupiedSlotsInAscendingOrder
- [ ] PrevVisitsOccupiedSlotsInDescendingOrder
- [ ] NextSkipsTombstonedSlots
- [ ] NextDoesNotRepeatCurrentSlot
- [ ] NextThenPrevReturnsToStart

---

## Fixture: `HashTableIteratorTest`

Same on-disk layout as `HashTableTest` (full heap storage, journal, contact and message free
lists), with `insert(name, phone)` and `remove(phone)` helpers. No message setup — only the
slot array matters here.

---

## Tests

### GetFailsBeforeFirstAdvance
A fresh iterator over an empty table: `get()` returns false.

### EmptyTableFindsNothing
`next()` and `prev()` both return false immediately on an empty table.

### NextVisitsOccupiedSlotsInAscendingOrder
Three contacts. Walking with `next()` visits exactly three slots, each `< HASH_TABLE_SIZE` and
`ENTRY_OCCUPIED`, in ascending order; `get()` fails after exhaustion. Checks both that the
iterator returns slot indices (the test dereferences `entries[value]`) and that the order is by
slot, not insertion.

### PrevVisitsOccupiedSlotsInDescendingOrder
Same three contacts, walked with `prev()` from an unpositioned iterator — starts at the top of the
table and visits all three in descending order.

### NextSkipsTombstonedSlots
Insert three, remove the middle one, walk: only two slots visited, both occupied. A remove leaves
a tombstone (needed to keep linear-probe chains intact), so an iterator that only checked
`!= ENTRY_EMPTY` would wrongly include it.

### NextDoesNotRepeatCurrentSlot
Two `next()` calls return two different slots. **Regression** for the same class of bug as
`IteratorNextDoesNotRepeatCurrentBit` in the usage bitmap: a search range that includes the
current position returns the same slot forever.

### NextThenPrevReturnsToStart
With two contacts: `next` → first, `next` → second, `prev` → first again, then `prev` is
exhausted. Checks `prev()` searches strictly below the current slot and doesn't restart from the
top.
