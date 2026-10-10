# `tests/test_journal.cpp` — Review Notes

Covers the power-cycle-safe rollback journal in
[`journal.c`](../../source/app/src/journal.c) — the single-slot "write the old content here before
you overwrite the real sector" mechanism that `write_contact()`, `write_message()` and the message
history all use, so a crash mid-write can be undone on the next boot. 28 tests, all passing
(`ctest -R JournalTest`).

This file was **rewritten** alongside the per-sector CRC work (commit `a63e0f9`). The old suite
(24 tests, including the stale `InitValidJournal_BUG_MissingReturnForValidState`) is gone. What
changed in `journal.c`, and therefore in what the tests check:

- Every journal sector (header, content copy, bitmap copy) now carries the standard CRC-32 trailer
  instead of the journal computing its own CRCs. "Corrupted" means the trailer doesn't match.
- The header has a **magic** number and an explicit state: `JRNL_EMPTY` (fresh), `JRNL_ACTIVE`
  (a transaction is in flight), `JRNL_COMMITTED` (last transaction finished).
- **Write order is bitmap copy → content copy → header.** The header is the commit point for
  *starting* a transaction: if power drops before it lands, the old committed header is still
  there and nothing gets rolled back.
- The journalled `sector` is a **data-region index**; rollback writes to
  `DATA_SECTOR_TO_RAW(sector)`. Previously it restored to the wrong place (see
  `RollbackTargetsDataRegionSector` and [test_db_recovery.md](test_db_recovery.md)).
- A new entry type, `JRNL_MSG_HIST`, for the ring-buffer message history. It has no usage bits, so
  no bitmap copy is restored on rollback.
- `journal_header_read()`, `journal_content_read()` and `journal_usage_read()` are no longer
  tested directly; they're covered through `get_journal_status()` and `journal_rollback()`.

## Checklist

### `journal_header_init()`
- [ ] HeaderInitWritesEmptyHeaderWithValidTrailer

### `get_journal_status()`
- [ ] StatusBlankZeroHeaderIsUninitialized
- [ ] StatusBlankErasedHeaderIsUninitialized
- [ ] StatusValidCrcWrongMagicIsUninitialized
- [ ] StatusEmptyIsValid
- [ ] StatusCommittedIsValid
- [ ] StatusActiveIsRollbackAndLoadsHeader
- [ ] StatusCorruptHeaderDataIsCorrupted
- [ ] StatusCorruptMagicIsCorrupted
- [ ] StatusCorruptPaddingIsCorrupted

### `journal_data_init()`
- [ ] DataInitFillsActiveHeader

### `journal_write()`
- [ ] WriteStoresAllSectorsWithValidTrailers
- [ ] WriteHeaderIsWrittenLast

### `journal_add()`
- [ ] AddWritesActiveEntry
- [ ] AddSelectsCorrectBitmapSector
- [ ] AddStampsBlankContent

### `journal_free()`
- [ ] FreeCommitsActiveEntry

### `journal_rollback()`
- [ ] RollbackRestoresSectorAndBitmap
- [ ] RollbackTargetsDataRegionSector
- [ ] RollbackCorruptContentCopyFails
- [ ] RollbackCorruptBitmapCopyFails
- [ ] RollbackHistoryEntrySkipsBitmapAndCommits

### `journal_init()`
- [ ] InitBlankCardInitialisesHeader
- [ ] InitValidJournalWritesNothing
- [ ] InitCorruptedHeaderFailsWithoutWriting
- [ ] InitRollsBackInterruptedTransaction
- [ ] InitDoesNotRollBackWhenHeaderWasNeverWritten
- [ ] InitRepeatsInterruptedRollback

---

## Fixture: `JournalTest`

Full-layout storage through [`FailableStorageCtx`](test_support.md), which behaves as plain heap
storage until a failure is armed — so the happy-path tests here can also simulate a power cut with
`fail_write_in(n)`. Zeroes the card and the global `usage_bitmap` before each test.

Helpers:

- `raw(sector)` — direct pointer into the simulated card.
- `stored_header()` — the header currently on the card (not the RAM copy in `journal.header`).
- `corrupt(sector, byte)` — flip one bit.
- `fail_write_in(n)` — the n-th write from now fails (1 = the very next one).
- `stamped_sector(value)` — a sector filled with `value` with a valid trailer.
- `bitmap_sector_for(data_sector)` / `fill_bitmap_sector(data_sector, value)` — locate / fill the
  slice of the **global** RAM bitmap that `journal_add()` backs up for a given data sector. The
  bitmap backup comes from that global, not from a parameter.

The journal has exactly **one slot**. A second `journal_add()` before `journal_free()` overwrites
the first entry (`AddSelectsCorrectBitmapSector` relies on this).

---

## `journal_header_init()`

### HeaderInitWritesEmptyHeaderWithValidTrailer
The initial header on the card has the magic, state `JRNL_EMPTY`, type 0, sector 0, and a valid
trailer.

---

## `get_journal_status()`

The status function has to separate four cases: blank card (initialise it), healthy (do nothing),
active (roll back), and damaged (stop). The tests below check each boundary between them.

### StatusBlankZeroHeaderIsUninitialized
All-zero header → `JRNL_UNINITIALIZED`, not `JRNL_CORRUPTED` (an all-zero sector also fails the
CRC, so this checks the blank test happens before the CRC test).

### StatusBlankErasedHeaderIsUninitialized
All-`0xFF` (erased) header → `JRNL_UNINITIALIZED`.

### StatusValidCrcWrongMagicIsUninitialized
A CRC-valid sector without the journal magic → `JRNL_UNINITIALIZED`.

### StatusEmptyIsValid
Freshly initialised (`JRNL_EMPTY`) → `JRNL_VALID`.

### StatusCommittedIsValid
`journal_add()` then `journal_free()` → `JRNL_VALID`.

### StatusActiveIsRollbackAndLoadsHeader
`journal_add()` without `journal_free()` → `JRNL_ROLLBACK`. The test zeroes the RAM header first
and then checks `get_journal_status()` loaded state/type/sector back into `journal.header` — that
side effect is what `journal_rollback()` relies on.

### StatusCorruptHeaderDataIsCorrupted
Flip a bit in the `state` field → `JRNL_CORRUPTED`, and the RAM header is **not** loaded (magic
still 0). A damaged header must not be trusted for a rollback target.

### StatusCorruptMagicIsCorrupted
Flip a bit in the magic of an active journal → `JRNL_CORRUPTED`, not `JRNL_UNINITIALIZED`. If it
were treated as uninitialised, `journal_init()` would write a fresh header and the pending rollback
would be silently discarded. Same idea as `SuperheaderCorruptMagicIsNotUninitialised` in
[test_sector_crc.md](test_sector_crc.md).

### StatusCorruptPaddingIsCorrupted
Flip a byte in the header's padding → `JRNL_CORRUPTED`. The trailer covers the whole payload.

---

## `journal_data_init()`

### DataInitFillsActiveHeader
Fills magic, `JRNL_ACTIVE`, the type and the sector.

---

## `journal_write()`

### WriteStoresAllSectorsWithValidTrailers
After a write, header, content and bitmap sectors all pass `read_sector()`, and content/bitmap
payloads match what was passed in.

### WriteHeaderIsWrittenLast
Starts from a valid `EMPTY` journal and fails the 1st, 2nd and 3rd write in turn:

| Failing write | Card afterwards | Status |
|---|---|---|
| 1 (bitmap) | nothing changed | `JRNL_VALID` |
| 2 (content) | bitmap copy written, content not | `JRNL_VALID` |
| 3 (header) | both copies written, header not | `JRNL_VALID` |

In every case the status stays valid, i.e. an interrupted `journal_write()` never produces a
rollback of a half-written journal. This is the core power-safety property of the new write order.

---

## `journal_add()`

### AddWritesActiveEntry
The stored header is CRC-valid with magic, `ACTIVE`, the type and the sector; the RAM header
matches it (so `journal_free()` commits *this* entry); the content copy and the bitmap copy are
byte-identical to the input and to the right slice of the global bitmap.

### AddSelectsCorrectBitmapSector
Data sectors `USAGE_BITS_PER_SECTOR - 1` and `USAGE_BITS_PER_SECTOR` belong to bitmap sectors 0
and 1. Filling those with `0x11` and `0x22` and journalling each shows the right slice is backed
up on each side of the boundary. Note the boundary is `USAGE_BITS_PER_SECTOR` (which excludes the
CRC trailer word), not `512 * 8`.

### AddStampsBlankContent
Journalling a blank (all-zero, CRC-invalid) sector works, and afterwards the **caller's** buffer is
CRC-valid. This is how a first write to a never-used sector gets journalled: `write_sector()`
stamps it, so the stored copy is valid and can be restored.

---

## `journal_free()`

### FreeCommitsActiveEntry
After `journal_free()` the stored header is valid, `JRNL_COMMITTED`, and keeps the type and sector
of the entry it committed (useful when debugging which write last happened).

---

## `journal_rollback()`

### RollbackRestoresSectorAndBitmap
Journal data sector 2, overwrite it on the card, then roll back: the data sector and its bitmap
sector are byte-identical to the originals, and the header is `JRNL_COMMITTED`.

### RollbackTargetsDataRegionSector
**Regression for:** rollback writing to raw sector `sector` instead of
`DATA_SECTOR_TO_RAW(sector)`. Journals `MESSAGE_DATA_SECTOR(4)`, rolls back, and checks the data
region copy is restored while raw sector `sector` (the old wrong target) is untouched.

### RollbackCorruptContentCopyFails
Corrupt the content copy → rollback fails **before** touching the data sector (still holds the
modified data) and the journal stays `ACTIVE`, so the next boot tries again.

### RollbackCorruptBitmapCopyFails
Corrupt the bitmap copy → rollback fails, the bitmap sector on the card isn't overwritten, journal
stays `ACTIVE`.

### RollbackHistoryEntrySkipsBitmapAndCommits
A `JRNL_MSG_HIST` entry rolls back the data sector, leaves the bitmap sector on the card alone (it
was never written during the add — `journal_add()` passes no bitmap for history), and commits.

---

## `journal_init()`

### InitBlankCardInitialisesHeader
Blank card → `journal_init()` writes an `EMPTY` header and the status becomes `JRNL_VALID`.

### InitValidJournalWritesNothing
Valid journal → `journal_init()` succeeds with **zero** writes. Matters on an SD card (no
needless wear on boot) and confirms the valid path returns early.

### InitCorruptedHeaderFailsWithoutWriting
Active journal with a corrupted `sector` field → `journal_init()` fails with zero writes. It must
not "repair" the header (losing the rollback) or roll back to a sector index it can't trust.

### InitRollsBackInterruptedTransaction
Power cut after the data sector was modified but before commit. On reboot, `journal_init()`
restores the original and the journal is valid again.

### InitDoesNotRollBackWhenHeaderWasNeverWritten
A committed earlier transaction on sector 7, then a new `journal_add()` for sector 2 loses power on
its 3rd write (the header). On reboot: zero writes, sector 2 keeps its current data, and sector 7
is **not** rolled back to the earlier content. Without the header-last order, the old committed
header plus the new content copy could produce a rollback of the wrong sector.

### InitRepeatsInterruptedRollback
First boot: rollback writes the data sector and bitmap, then loses power on the commit (3rd
write) → `journal_init()` fails and the header is still `ACTIVE`. Second boot: the rollback runs
again and succeeds. Checks rollback is idempotent — repeating it from the same copies is safe.
