# Unit Test Review Notes

One file per active GTest test file in [`tests/`](../../tests), each explaining what every test
checks and why, with a per-test checklist for tracking your own read-through. All suites below
are currently wired into [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) and pass via
`ctest` (232 tests total, run from `build-host/`, last checked 2026-10-10).

| Suite | File | Tests | Covers |
|---|---|---|---|
| `HashTableTest` | [test_hash_table.md](test_hash_table.md) | 44 | [`hash_table.c`](../../source/app/src/hash_table.c), [`contact.c`](../../source/app/src/contact.c), [`message.c`](../../source/app/src/message.c) |
| `HashTableEdgeTest` / `HashTableSmallTableTest` | [test_hash_table_edge.md](test_hash_table_edge.md) | 12 | same, boundary/failure cases |
| `HashTableIteratorTest` **(new)** | [test_hash_table_iterator.md](test_hash_table_iterator.md) | 7 | hash table iterator |
| `FreeListTest` | [test_free_list_stack.md](test_free_list_stack.md) | 6 | [`free_list_stack.c`](../../source/app/src/free_list_stack.c) |
| `FreeListEdgeTest` | [test_free_list_stack_edge.md](test_free_list_stack_edge.md) | 20 | same, boundary cases |
| `UsageBitmapTest` | [test_usage_bitmap.md](test_usage_bitmap.md) | 27 | [`usage_bitmap.c`](../../source/app/src/usage_bitmap.c), incl. iterator |
| `UsageBitmapEdgeTest` | [test_usage_bitmap_edge.md](test_usage_bitmap_edge.md) | 11 | same, failure/boundary cases |
| `JournalTest` **(rewritten)** | [test_journal.md](test_journal.md) | 28 | [`journal.c`](../../source/app/src/journal.c) |
| `JournalEdgeTest` **(rewritten)** | [test_journal_edge.md](test_journal_edge.md) | 17 | same, failure/boundary cases |
| `RingBufferIteratorTest` / `RingBufferStateTest` / `RingBufferReconstructTest` **(new)** | [test_ring_buffer.md](test_ring_buffer.md) | 33 | [`ring_buffer.c`](../../source/app/src/ring_buffer.c) |
| `SectorCrcTest` **(new)** | [test_sector_crc.md](test_sector_crc.md) | 20 | per-sector CRC trailer in [`storage.c`](../../source/app/src/storage.c), plus contacts, messages, bitmap, [`superheader.c`](../../source/app/src/superheader.c) |
| `DbRecoveryTest` **(new)** | [test_db_recovery.md](test_db_recovery.md) | 7 | cross-module: message addressing, power-cut rollback, `hash_reconstruct_message()` |

Plus [test_support.md](test_support.md) — not a test suite, but the shared `FailableStorageCtx`
mock most of the failure-path suites use to inject storage read/write failures. Worth reading
before the `*Edge` files, since it explains a gotcha (fixture `SetUp()` consuming a read/write
before the test body runs) that shows up in several of them.

## What changed since the first review (2026-09-14)

- **Per-sector CRC (commit `a63e0f9`).** Every on-disk sector now ends in a CRC-32 trailer. This
  rewrote the journal suites entirely, shifted the usage bitmap sector boundary from 4096 to 4064
  bits (the tests were updated, the ticked boxes in `test_usage_bitmap.md` were kept — see the
  note at the top of that file), and added `SectorCrcTest` and `DbRecoveryTest`, which pin down
  three addressing/rollback bugs fixed at the same time.
- **Iterators (`e46d6d6`, `dd07ddf`).** A generic `Iterator` interface with usage bitmap, hash
  table and ring buffer implementations, each with its own tests.
- **Ring buffer (`ef232c2`).** SD-backed ring buffer for message/call history, reconstructed on
  boot by binary search. `test_ring_buffer.md` flags two **characterisation** cases (iterator
  range includes `index == occupancy`; `move_next`/`move_prev` wrap at different points) that
  look like off-by-ones worth a decision.
- **Contact list paging and scale tests (`22cd532`, `b941475`)** in `HashTableTest`.

## Not covered here

A few `.cpp` files in `tests/` exist but aren't referenced by `tests/CMakeLists.txt`, so they
aren't part of the `ctest` run: `test_hash_table_messages.cpp`, `test_message_extent.cpp`,
`test_my_logic.cpp`, `test_database.cpp` (empty). No notes were written for these since there's
nothing currently passing to verify — worth deciding whether to revive, rewrite, or delete them.
(Stale build artefacts for some of them, e.g. `tests/test_message_extent`, are still sitting in
`tests/`.)

The hardware runner (`test_db_main.c`, see [`hw_unit_test.md`](../hw_unit_test.md)) runs the same
logic on the STM32 and isn't documented test-by-test here.

## Suggested reading order

1. [test_support.md](test_support.md) — the shared failure-injection mock, and its one
   recurring gotcha.
2. `test_free_list_stack.md` + `test_free_list_stack_edge.md`, then `test_usage_bitmap.md` +
   `test_usage_bitmap_edge.md` — smallest, most self-contained, no dependencies on the others.
3. `test_sector_crc.md` — the CRC trailer every later file assumes. Then `test_journal.md` +
   `test_journal_edge.md` — depends on the usage bitmap (it backs up bitmap sectors) but not on
   the hash table. `WriteHeaderIsWrittenLast` and the `journal_init()` power-cut tests are the
   core of the power-safety argument.
4. `test_ring_buffer.md` — self-contained apart from storage; read the two characterisation notes
   at the top first.
5. `test_hash_table.md` + `test_hash_table_edge.md` + `test_hash_table_iterator.md` — the largest and most consequential, since
   it drives contact/message storage through the journal and usage bitmap underneath. Several
   tests across both files were written to pin down bugs found during review (marked
   "**Regression for:**") or to document real-but-non-crashing design gaps (marked
   "**Characterisation**") — read those first if you're pressed for time.
6. `test_db_recovery.md` — end-to-end power-cut and reconstruction scenarios across everything
   above; easiest to follow once the journal and hash table are familiar.

## Recommended additional tests

Not yet written — things worth adding next, roughly in priority order. Several of these came up
directly while writing the edge suites above (a test almost went there, but crossed into needing
either a source-code decision or infrastructure this pass didn't set up).

1. **A death test for `journal_add()`/`journal_rollback()` with an out-of-bitmap-range sector
   index.** `journal_add(&journal, JRNL_CONTACT, UINT16_MAX, content)` segfaults — there is
   still no bounds check on `index` before indexing into the global `usage_bitmap` array (the old
   `AddPreservesLargeInRangeSectorValue` test that documented this was dropped in the journal
   rewrite).
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

5. ~~**`hash_reconstruct_message()`** has no dedicated test.~~ **Partly done:**
   `DbRecoveryTest.ReconstructMessageRestoresLatestSectors` / `ReconstructMessageEmptyDatabase`
   now cover it (see [test_db_recovery.md](test_db_recovery.md)). Still missing: a corrupted
   message sector during reconstruction (the contact equivalent is
   `SectorCrcTest.ReconstructSkipsCorruptContactSector`), and enough messages to cross a usage
   bitmap sector boundary.

6. **A dedicated `contact.c`/`message.c` edge suite**, if you want lower-level coverage than
   going through the full `HashTable` API: `write_contact()`/`read_contact()`/`remove_contact()`
   directly against a raw `ContactSectorBuffer` (multiple contacts packed into one physical
   sector, the used-bitmap header field, a sector that fills up and the next slot rolling into a
   new physical sector), and the equivalent for `write_message_sector()`/`read_message_sector()`
   in isolation from the hash table layer. Right now every test exercises these exclusively
   through `hash_insert_contact()`/`hash_insert_message()`, so a bug isolated to (say) the
   used-bitmap bit-packing in `write_contact()` would only surface as a confusing hash-table-level
   failure several layers removed from the actual defect.

7. ~~**`storage.c`'s `read_sector()`/`write_sector()`** has no tests.~~ **Done** by
   `SectorCrcTest` (CRC stamping, `STRG_CORRUPT`, `read_sector_raw()`).

8. **Known-vector tests for `crc.c`.** `SectorCrcTest.StampDetectsEverySingleByteFlip` now tests
   the trailer directly rather than only through the journal, but it still stamps and checks with
   the same `crc32_calculate()`, so a consistently wrong CRC would pass. A few checks against
   published CRC-32 vectors (`crc.c` is standard CRC-32, polynomial `0xEDB88320`, so
   `"123456789"` → `0xCBF43926`) would close that gap.

9. ~~**`superheader.c`** has no tests.~~ **Done** by the nine `Superheader…` tests in
   `SectorCrcTest`. Not covered: the boot path that *acts* on `SUPR_OUTDATED`/`SUPR_CORRUPTED`.

10. **Concurrent/re-entrant access is entirely untested**, but may not be worth testing at all
    depending on the target's actual threading model: everything here (`usage_bitmap`
    particularly, being a bare global array with no locking) assumes single-threaded, run-to-completion
    access. If the STM32 firmware ever calls into any of this from both an interrupt handler and
    main-line code, that assumption becomes load-bearing in a way none of these tests would catch
    on the host. Flagging this as a question to answer (does that scenario exist?) rather than a
    concrete test to write, since the right test depends entirely on the answer.

11. **Ring buffer off-by-one decisions** (see the top of [test_ring_buffer.md](test_ring_buffer.md)).
    Decide whether the iterator's upper limit should be `occupancy - 1`, and whether
    `move_prev_ring_buffer()` should wrap to `occupancy - 1` like `move_next` does, then update
    `NextWrapsAtUpperLimit`/`PrevWrapsAtLowerLimit` and add a partially-full `move_prev` test.

12. **Contact list order.** Both contact list paging test pairs compare results as sorted sets, so
    the order each function returns is untested. If the UI depends on a stable order (storage
    order for `hash_get_contact_list_by_usage()`, slot order for `hash_get_contact_list()`), assert
    it. Also worth tests for a page that runs past the last contact (`start + n > hash_size()`)
    and for `n == 0`.

13. **Message history (`message_history_add()` and friends).** The ring buffer is tested on its
    own, and `RollbackHistoryEntrySkipsBitmapAndCommits` covers a `JRNL_MSG_HIST` rollback, but
    nothing yet drives the history layer end to end (add, power cut, reconstruct). Probably
    waiting on the head-in-sector work noted in commit `ef232c2`.
