# Unit Test Review Notes

One file per active GTest suite in [`tests/`](../../tests), each explaining what every test
checks and why, with a per-test checklist for tracking your own read-through. All suites below
are currently wired into [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) and pass via
`ctest` (141 tests total, run from `build-host/`).

| Suite | File | Tests | Covers |
|---|---|---|---|
| `HashTableTest` | [test_hash_table.md](test_hash_table.md) | 36 | [`hash_table.c`](../../source/app/src/hash_table.c), [`contact.c`](../../source/app/src/contact.c), [`message.c`](../../source/app/src/message.c) |
| `HashTableEdgeTest` / `HashTableSmallTableTest` | [test_hash_table_edge.md](test_hash_table_edge.md) | 12 | same, boundary/failure cases |
| `FreeListTest` | [test_free_list_stack.md](test_free_list_stack.md) | 6 | [`free_list_stack.c`](../../source/app/src/free_list_stack.c) |
| `FreeListEdgeTest` | [test_free_list_stack_edge.md](test_free_list_stack_edge.md) | 20 | same, boundary cases |
| `UsageBitmapTest` | [test_usage_bitmap.md](test_usage_bitmap.md) | 13 | [`usage_bitmap.c`](../../source/app/src/usage_bitmap.c) |
| `UsageBitmapEdgeTest` | [test_usage_bitmap_edge.md](test_usage_bitmap_edge.md) | 11 | same, failure/boundary cases |
| `JournalTest` | [test_journal.md](test_journal.md) | 24 | [`journal.c`](../../source/app/src/journal.c) |
| `JournalEdgeTest` | [test_journal_edge.md](test_journal_edge.md) | 19 | same, failure/boundary cases |

Plus [test_support.md](test_support.md) — not a test suite, but the shared `FailableStorageCtx`
mock the four `*Edge` suites above use to inject storage read/write failures. Worth reading
before any of the edge-suite files, since it explains a gotcha (fixture `SetUp()` consuming a
read/write before the test body runs) that shows up repeatedly in those files' test bodies.

## Not covered here

A few `.cpp` files in `tests/` exist but aren't referenced by `tests/CMakeLists.txt` (commented
out or never added), so they don't compile against the current headers and aren't part of the
`ctest` run: `test_hash_table_messages.cpp`, `test_message_extent.cpp`, `test_my_logic.cpp`,
`test_database.cpp` (empty). No notes were written for these since there's nothing currently
passing to verify — worth deciding whether to revive, rewrite, or delete them before they're
worth documenting. (`test_hash_table_edge.cpp` and `test_journal_edge.cpp` *used* to be in this
category — both have since been rewritten from scratch against the current API and are now live,
documented suites above.)

## Suggested reading order

1. [test_support.md](test_support.md) — the shared failure-injection mock, and its one
   recurring gotcha.
2. `test_free_list_stack.md` + `test_free_list_stack_edge.md`, then `test_usage_bitmap.md` +
   `test_usage_bitmap_edge.md` — smallest, most self-contained, no dependencies on the others.
3. `test_journal.md` + `test_journal_edge.md` — depends on the usage bitmap (it backs up bitmap
   slices) but not on the hash table. `test_journal_edge.md`'s last entry
   (`AddPreservesLargeInRangeSectorValue`) documents a confirmed segfault-on-`UINT16_MAX` bug in
   `journal_add()` — worth reading even if you skip everything else in that file.
4. `test_hash_table.md` + `test_hash_table_edge.md` — the largest and most consequential, since
   it drives contact/message storage through the journal and usage bitmap underneath. Several
   tests across both files were written to pin down bugs found during review (marked
   "**Regression for:**") or to document real-but-non-crashing design gaps (marked
   "**Characterisation**") — read those first if you're pressed for time.

## Recommended additional tests

Not yet written — things worth adding next, roughly in priority order. Several of these came up
directly while writing the edge suites above (a test almost went there, but crossed into needing
either a source-code decision or infrastructure this pass didn't set up).

1. **A death test for `journal_add()`/`journal_rollback()` with an out-of-bitmap-range sector
   index.** `test_journal_edge.md`'s `AddPreservesLargeInRangeSectorValue` confirms (by hand, not
   in the checked-in suite) that `journal_add(&journal, JRNL_CONTACT, UINT16_MAX, content)`
   segfaults — no bounds check on `index` before indexing into the global `usage_bitmap` array.
   A `EXPECT_DEATH(journal_add(...), "")`-based test would pin this down as an executable fact
   without crashing the whole binary the way a normal `EXPECT_FALSE` attempt would. Same
   underlying issue likely affects `check_usage_bit()`/`update_usage_bit()` directly for an index
   `>= USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT` — worth a matching death test there too. No
   suite in this project uses `EXPECT_DEATH`/`ASSERT_DEATH` yet, so this would be establishing
   the pattern, not just adding one more test.

2. **`free_list_free()` and `free_list_free_range()` with `self == NULL`.** Unlike
   `free_list_allocate()` (which explicitly checks `self == NULL`), `free_list_free()`
   dereferences `self->free_stack` immediately with no null check — calling it with a null
   `self` crashes. Same death-test approach as above would confirm this without taking down the
   suite.

3. **Fix (then test) `hash_clear()`'s use of the global `HASH_TABLE_SIZE` instead of
   `table->size`.** Noted in `test_hash_table_edge.md`'s `TableFullRejectsNewInsert`: any
   `HashTable` initialised with `hash_init(..., size)` where `size < HASH_TABLE_SIZE` and a
   backing `HashEntry` array allocated to match that smaller `size` (rather than over-allocating
   to `HASH_TABLE_SIZE`, as this project's tests currently do to work around it) would have
   `hash_clear()` write past the end of that array. Once fixed to use `table->size`, add a test
   that constructs a small table with a *correctly, minimally sized* backing array and confirms
   `hash_clear()` (and by extension `TearDown()`-style cleanup) doesn't overrun it.

4. **A decision, then a test, for the two characterised-but-unresolved design gaps:**
   - `hash_remove_contact()` leaking a contact's message sectors if it has any
     (`RemoveContactAloneLeaksItsMessageSectors`).
   - `hash_insert_message()` leaving a phantom empty contact behind when the message-sector
     allocation fails right after a brand-new contact was just committed
     (`InsertMessageLeavesPhantomContactWhenMessageAllocatorExhausted`).

   Both are pinned down as *current behaviour* today, not asserted as *correct* behaviour. Once
   you decide how each should actually work (leave as documented traps in the API, or change the
   implementation to close them), the existing characterisation tests should either be updated to
   assert the new, fixed behaviour, or left as a named "this is the old, now-superseded behaviour"
   regression marker — whichever fits how you want to track the change.

5. **`hash_reconstruct_message()`** has no dedicated test anywhere in this project.
   `hash_reconstruct_contact()` gets thorough coverage in `test_hash_table.md`, but the message
   side of database reconstruction after a simulated power cycle is untested. Given
   `hash_reconstruct_message()` is exactly the kind of bitmap-bounds-sensitive code that already
   had a real bug fixed in it this review cycle (freeing into the wrong allocator), and shares the
   same "scan the usage bitmap for used sectors" shape as `hash_reconstruct_contact()` (whose
   *own* bitmap-bounds bug was only caught by writing a reconstruction test that inserted enough
   contacts to reach a later bitmap word), this is a meaningfully higher-risk gap than it might
   look at first glance.

6. **A dedicated `contact.c`/`message.c` edge suite**, if you want lower-level coverage than
   going through the full `HashTable` API: `write_contact()`/`read_contact()`/`remove_contact()`
   directly against a raw `ContactSectorBuffer` (multiple contacts packed into one physical
   sector, the used-bitmap header field, a sector that fills up and the next slot rolling into a
   new physical sector), and the equivalent for `write_message_sector()`/`read_message_sector()`
   in isolation from the hash table layer. Right now every test exercises these exclusively
   through `hash_insert_contact()`/`hash_insert_message()`, so a bug isolated to (say) the
   used-bitmap bit-packing in `write_contact()` would only surface as a confusing hash-table-level
   failure several layers removed from the actual defect.

7. **`storage.c`'s `read_sector()`/`write_sector()`** (the thin layer between `contact.c`/
   `message.c` and the `Storage` function-pointer interface) has no test file of its own — it's
   only ever exercised transitively. Probably low-value to test in isolation given how thin it
   is, but worth a quick read to confirm that's actually true rather than assumed.

8. **`crc.c`** is exercised extensively but only ever *through* `journal.c` (every journal test
   indirectly depends on `crc32_calculate()` being correct). A handful of direct unit tests
   against known CRC-32 test vectors (including the empty-input and all-zero-input cases) would
   decouple "is the CRC implementation correct" from "does the journal correctly react to a CRC
   mismatch" — right now a subtly wrong CRC implementation that's merely *consistent* (same wrong
   answer every time) would pass every journal test, since they all compute expected values by
   calling the same function under test.

9. **`superheader.c`** has no tests at all in this project. Worth at least a quick look at what
   it's responsible for (database format/version stamping, from its name) to judge whether that's
   an oversight or intentionally out of scope for the host test suite.

10. **Concurrent/re-entrant access is entirely untested**, but may not be worth testing at all
    depending on the target's actual threading model: everything here (`usage_bitmap`
    particularly, being a bare global array with no locking) assumes single-threaded, run-to-completion
    access. If the STM32 firmware ever calls into any of this from both an interrupt handler and
    main-line code, that assumption becomes load-bearing in a way none of these tests would catch
    on the host. Flagging this as a question to answer (does that scenario exist?) rather than a
    concrete test to write, since the right test depends entirely on the answer.
