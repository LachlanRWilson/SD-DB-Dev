# `tests/test_call_history.cpp` — Review Notes

Covers the call history layer in [`call_history.c`](../../source/app/src/call_history.c). Call
history is a copy of message history with a different entry type, so this suite is a one-to-one
port of [test_message_history.md](test_message_history.md). Read that file for what each shared
test checks and why. This file only lists the differences and the tests that exist only here.
40 tests in one suite, all passing (`ctest -R CallHistoryTest`).

| Group | Tests | What it covers |
|---|---|---|
| init **(call history only)** | 2 | `call_history_init()` |
| add | 9 + 2 call history only | `call_history_add()` |
| get_list | 10 | `call_history_get_range()` from `start = 0` |
| get_range | 16 | `call_history_get_range()` with any `start` |
| persistence | 1 | `init_ring_buffer()` rebuild from the card |

## Differences from the message history suite

- **Entry type.** Each slot holds a `CallHistoryEntry` (84 B): a `uint16_t contact_index` plus a
  full `Contact` (name and phone). `make_entry(id)` sets `contact_index = id`,
  `name = "contact <id>"`, `phone = "04<id, 8 digits>"`, and the matching `name_len` and
  `phone_len`. `contact_index` takes the place of `timestamp` as the ID that the checks and the
  `0xBEEF` overrun sentinel use. `expect_newest_first()` and `expect_range()` also compare the
  phone bytes.
- **Capacity.** `CALL_HISTORY_SECTOR_CAPACITY` is 5 (500 B payload / 84 B), compared with 3 for
  messages, so the 8-sector region holds 40 entries. Every test uses `kCap`, so the same
  scenarios carry over. The one test that hard-coded message capacity is
  `GetRangeStartPastStoredFails`, which used 10 and 9 entries (partly full and full current
  sector when `kCap = 3`). It now uses `2 * kCap + 1` and `2 * kCap`.
- **Region.** `kStart = DATA_REGION_START_SECTOR + CALL_HISTORY_DATA_START_SECTOR`.
- **Names.** `…Messages` / `…Message` in test names became `…Entries` / `…Entry`
  (`GetRangeReturnsFirstTenEntries`, `GetRangeReturnsSecondTenEntries`,
  `GetRangeStartOnOldestEntryReturnsOne`).

## Build changes this suite needed

- `call_history.c` was not in the `app_logic` sources, so it had never been compiled for host. It
  is now listed in [`source/CMakeLists.txt`](../../source/CMakeLists.txt).
- `set_sector_full_move_next()` and `read_first_sector()` are now `static` in `call_history.c`.
  They have the same names as non-static functions in `message_history.c`, so linking both files
  failed with duplicate symbols.

## Checklist

### init
- [ ] InitRejectsNull
- [ ] InitResetsRingBufferState

### add
- [ ] FirstAddClaimsSectorZero
- [ ] AddsWithinSectorAdvanceHead
- [ ] AddStoresWholeEntry
- [ ] AddJournalsCallHistorySector
- [ ] AddPastFullSectorMovesToNextSector
- [ ] SectorsFillSequentiallyWithIncreasingSeq
- [ ] WrapOverwritesOldestSector
- [ ] AddsAfterWrapKeepFillingReusedSector
- [ ] ManyWrapsStayInsideRegion
- [ ] AddRejectsCorruptSectorState
- [ ] AddPropagatesReadFailure

### get_list
- [ ] GetListOnEmptyBufferReturnsZero
- [ ] GetListWithZeroRequestedReturnsZero
- [ ] GetListWithinOneSectorIsNewestFirst
- [ ] GetListFewerThanStoredReturnsNewest
- [ ] GetListSpansSectors
- [ ] GetListMoreThanStoredReturnsStoredCountNoDuplicates
- [ ] GetListOnWrappedBufferReturnsNewestAndStops
- [ ] GetListOnFullUnwrappedBufferReturnsAll
- [ ] GetListDoesNotModifyStorage
- [ ] GetListPropagatesReadFailure

### get_range
- [ ] GetRangeReturnsFirstTenEntries
- [ ] GetRangeReturnsSecondTenEntries
- [ ] GetRangeOnEmptyBufferReturnsZero
- [ ] GetRangeWithZeroRequestedReturnsZero
- [ ] GetRangeRejectsNullArguments
- [ ] GetRangeStartInsidePartialCurrentSector
- [ ] GetRangeStartOnSectorBoundary
- [ ] GetRangeStartOnOldestSlotOfFullSector
- [ ] GetRangeWithFullCurrentSector
- [ ] GetRangePastOldestIsTruncatedNoDuplicates
- [ ] GetRangeStartOnOldestEntryReturnsOne
- [ ] GetRangeStartPastStoredFails
- [ ] GetRangeOnWrappedBufferCrossesWrap
- [ ] GetRangeEveryStartMatchesExpectedSlice
- [ ] GetRangeDoesNotModifyStorage
- [ ] GetRangePropagatesReadFailure

### persistence
- [ ] ReconstructAfterAddsMatchesRamState

---

## Tests only in this suite

### InitRejectsNull
`call_history_init(nullptr)` returns false.

### InitResetsRingBufferState
`current_index` and `occupancy` are set to non-zero values first. After `call_history_init()`
both are 0. (The fixture itself sets up its buffer with `init_ring_buffer()`, not
`call_history_init()`, so this is the only test that calls it.)

### AddStoresWholeEntry
One add, then slot 0 on the card is compared byte for byte with the entry that was passed in
(`memcmp` over the whole 84 B). The shared tests only compare `contact_index`, name and phone, so
this one also catches a truncated copy or a length field that wasn't saved.

### AddJournalsCallHistorySector
Checks the journal header that `journal_add()` left in RAM (`journal.header`, which keeps its
type and sector after `journal_free()` marks it committed). The type must be `JRNL_CALL_HIST`, and
the sector must be `CALL_HISTORY_DATA_START_SECTOR + rb.current_index`. This is checked after the
first add (sector 0) and again after crossing into sector 1.

**Regression for:** `call_history_add()` passed `JRNL_MSG_HIST` and
`MESSAGE_HIST_DATA_SECTOR(rb->current_index)` to `journal_add()`, left over from the message
history code. On a power cut partway through an add, `journal_rollback()` would have written the
saved call history sector over message history sector `current_index`. The fix is
`JRNL_CALL_HIST` and the new `CALL_HIST_DATA_SECTOR()` macro in `mem_layout.h`.

---

## Notes

- `call_history_get_list()` at the bottom of `call_history.c` is an empty stub with no return
  statement, and it isn't declared in the header. Nothing calls it, so it is not tested. As with
  message history, get_list is `get_range(0, n)`.
- `call_history_init()` sets `rb->size = HASH_TABLE_ENTRIES`, the same as `message_history_init()`.
  `InitResetsRingBufferState` deliberately does not check `size`. Neither init function is the
  one the firmware relies on for size, because `init_ring_buffer()` sets it from the region
  length.
- As with message history, a power cut partway through an add followed by rollback is not tested
  through `call_history_add()`. `JournalTest.RollbackHistoryEntrySkipsBitmapAndCommits` covers
  the shared history rollback path: `journal_rollback()` skips the usage bitmap for any type other
  than `JRNL_CONTACT` / `JRNL_MESSAGE`, so that test covers `JRNL_CALL_HIST` too.
