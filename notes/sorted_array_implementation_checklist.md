# Implementation checklist: sorted-array name index (production)

## Context
The benchmark (`source/benchmark/name_index/`) is finished, and the sorted array is the chosen structure for the RAM-only, alphabetically ordered contact index. At 10,000 contacts it fits in RAM_D1 next to the hash table and both free-list stacks (about 3.5 KB spare). The production code is a stub: `source/app/src/sorted_array.c` has empty bodies, and its header (`source/app/include/sorted_array.h`) has a tie-break signature that can't order entries. This checklist turns the stub into the real index by porting the tested benchmark implementation (`source/benchmark/name_index/sorted_array.c`, `name_index_common.h`) and wiring it into the database.

## 1. Header and API (`source/app/include/sorted_array.h`)
- [ ] Keep `SortedEntry` at 12 B (`key[8]`, `contact_index`, `message_index`), with a `STATIC_ASSERT(sizeof(SortedEntry) == 12)`.
- [ ] Add `#define SORTED_ARRAY_CAPACITY 10000` (the design max, not `HASH_TABLE_SIZE`). Put it in `mem_layout.h` next to `HASH_TABLE_SIZE`.
- [ ] Change the tie-break to a three-way compare: `int (*tiebreak)(Storage *s, uint16_t a, uint16_t b)` returns <0, 0 or >0. The current `bool (*)(Storage*, uint32_t, uint32_t)` can't sort. Use the benchmark's `ni_tiebreak_fn` contract.
- [ ] Add `Storage *storage` to `SortedArray`, so the tie-break can read names. Keep `mem` as an external pointer to the RAM_D1 buffer. Rename `occupancy` to `count` if you like.
- [ ] Final API, mirroring the benchmark's `na_*` functions:
  - `sorted_array_init`
  - `sorted_array_insert(sa, name, contact_index, message_index)`
  - `sorted_array_remove(sa, name, contact_index)`
  - `sorted_array_find(sa, name)` → rank or `UINT16_MAX`
  - `sorted_array_set_message(sa, name, contact_index, msg)`
  - `sorted_array_get_range(sa, start, n, out)` (already declared)
  - `sorted_array_seek(sa, name)` → first rank ≥ name, for "jump to letter"
  - `sorted_array_append` plus `sorted_array_sort`, for the boot rebuild
- [ ] Drop `sorted_array_sort_qsort`: newlib `qsort` recurses and has no context pointer for the tie-break. Use the benchmark's in-place heapsort (`na_rebuild_finish`).
- [ ] Take a name string (`const char *`, length ≤ `MAX_NAME_LEN`), not a `Contact` by value: `Contact` is 81 B and would be copied onto the stack on every call.

## 2. Implementation (`source/app/src/sorted_array.c`)
- [ ] `make_key`: ASCII lower-case the first 8 bytes and zero-pad. Port `ni_make_key` from `name_index_common.h`.
- [ ] `cmp`: `memcmp` the keys. If they're equal, call `tiebreak(storage, a, b)`, then compare `contact_index` so the order is total. Port `ni_cmp`.
- [ ] `lower_bound` binary search. Port from `source/benchmark/name_index/sorted_array.c`.
- [ ] Insert:
  - fail if `count == SORTED_ARRAY_CAPACITY`;
  - open the gap with the hand-written **word-copy loop** (`na_wordcopy` path), because newlib-nano's backward `memmove` is a byte loop (2.4× slower);
  - write the entry.
- [ ] Remove: find the exact entry (key, then tie-break, then `contact_index`) and close the gap with **`memmove`**, which newlib already does with word copies; the word loop is slower here.
- [ ] `get_range`: bounds-check `start + n` against `count` and copy entries out. This is O(1) to position, which is the reason for picking the array.
- [ ] Rebuild: `append` everything without sorting, then one heapsort. No recursion and no scratch memory.
- [ ] No dynamic allocation; nothing on the stack bigger than one name buffer.
- [ ] Keep `mem` 4-byte aligned (`__attribute__((aligned(4)))` on the buffer). The word loop depends on it.

## 3. Tie-break callback (database side)
- [ ] Implement `contact_name_cmp(Storage*, uint16_t a, uint16_t b)`: read both names with `read_contact()` (`source/app/src/contact.c:152`), case-fold-compare them, then compare raw bytes.
- [ ] Support the probe convention for `find`/`seek`: the reserved index `0xFFFE` (`NI_PROBE_SECTOR`) means "the name being searched". Store the probe name in the `SortedArray`, or pass it in a context struct.
- [ ] Cost note: each tie-break is up to 2 SD reads (about 0.4 ms each). Realistic names share their 8-byte key often (57% at N = 10,000), and the benchmark measured about 2–4 tie-break calls per insert or find at N = 10,000. Consider caching the probe's own name, so only the other side needs a read.

## 4. Memory placement
- [ ] Add `__attribute__((section(".ram_d1"))) static SortedEntry g_sorted_mem[SORTED_ARRAY_CAPACITY];` next to `g_entries` and the free lists (`source/app/hw_tests/src/db_main.c:127-134` pattern; the real home is wherever `database_init` callers allocate).
- [ ] Check `--print-memory-usage`: RAM_D1 should be about 324,200 / 327,680 B.
- [ ] Optional headroom: shrink the contact free list to 10,000 (saves 8.6 KB), or move `usage_bitmap` (4 KB) to RAM_D2.

## 5. Database integration (`source/app/src/database.c`, `include/database.h`)
- [ ] Add `SortedArray names` to `Database`, and pass `SortedEntry *sorted_mem` to `database_init()`.
- [ ] `database_contact_create`: after a successful `hash_insert` and SD write, call `sorted_array_insert`. If the insert fails (full), roll back the hash entry.
- [ ] Contact delete path (`remove_contact`, `hash_remove`): call `sorted_array_remove` with the old name **before** the name is overwritten on SD.
- [ ] `database_contact_update`: if the name changed, remove the old entry and insert the new one; otherwise leave the index alone.
- [ ] When a contact's latest message extent changes, call `sorted_array_set_message`, or drop `message_index` from the entry if the hash table's `latest_msg_extent` already covers it.
- [ ] `database_clear`: reset `count` to 0.
- [ ] Boot or recovery (`test_db_recovery` path): rebuild the index from the hash table by iterating occupied entries (`get_nth_contact_index` or the hash iterator), `append`ing each one and then sorting once. This was measured at about 0.31 s for 10,000 entries on target at `-Os`, excluding the SD reads for the tie-break names.
- [ ] Journal or crash consistency: the index is RAM-only and is always rebuilt at boot, so it needs no journaling. Document that.

## 6. Tests (`tests/`, gtest, add to `tests/CMakeLists.txt` like `test_hash_table`)
- [ ] `test_sorted_array.cpp`: port the benchmark's typed tests (`source/benchmark/name_index/test_name_index.cpp`), checked against a `std::multimap` reference. Fixtures reset state in `SetUp()`, with a docstring comment above each test. Cover:
  - empty index, single entry, capacity full;
  - insert front, end and middle; remove first, last and missing;
  - duplicate keys resolved by tie-break; exact duplicate names resolved by `contact_index`;
  - `get_range` at the edges; `seek`; rebuild order equal to incremental-insert order.
- [ ] `test_database.cpp`: create, rename and delete a contact, and check the index order and count each time; recovery rebuild gives the same order as before.
- [ ] Use a heap-storage (`heap_storage.c`) backed tie-break in host tests.

## 7. Target verification
- [ ] Build firmware Release and Debug; check RAM_D1 usage.
- [ ] HW test (extend `db_main.c` or add a `NAME_INDEX_TEST` guard like `MH_TEST`):
  - insert 10,000 contacts;
  - check the order by walking `get_range` in pages of 20;
  - time one insert and one `get_range` with DWT and compare with the benchmark (insert_random about 52 k cycles with the word loop; `page_rank20` about 0.8 k).

## Verification
- `cmake --build build-host && ctest --test-dir build-host`: all existing tests plus the new `SortedArray*` tests pass.
- Target `-Os` and `-O0` builds link, with RAM_D1 ≤ 327,680 B.
- The HW test reports order OK, and timings within about 25% of the benchmark (the layout spread).
