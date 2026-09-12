# `tests/test_journal.cpp` — Review Notes

Covers the power-cycle-safe rollback journal in
[`journal.c`](../../source/app/src/journal.c) — the single-slot "write old content here before
you overwrite the real sector" mechanism that `write_contact()`, `write_message()`, etc. all use
so a crash mid-write can be undone on the next boot. 24 tests, all passing
(`ctest -R JournalTest`).

One of them, `InitValidJournal_BUG_MissingReturnForValidState`, has a name and a comment
claiming it exercises a known, unfixed UB bug in `journal_init()`. **I checked the current
`journal.c` and that's stale** — the bug it describes is already fixed (see that test's writeup
below). Flagging this up front since the name alone would otherwise make you distrust a
perfectly healthy code path.

## Checklist

### `journal_header_init()`
- [ ] HeaderInit

### `get_journal_status()`
- [ ] StatusUninitialized
- [ ] StatusValid
- [ ] StatusCommittedIsValid
- [ ] StatusRollback
- [ ] StatusCorruptedHeader
- [ ] StatusCorruptedHeaderData

### `journal_data_init()`
- [ ] DataInit
- [ ] DataInitContactType

### `journal_write()`
- [ ] Write

### `journal_add()`
- [ ] Add
- [ ] AddSelectsCorrectBitmapSector
- [ ] AddMessageType

### `journal_header_read()`
- [ ] HeaderRead

### `journal_content_read()`
- [ ] ContentRead

### `journal_usage_read()`
- [ ] UsageRead

### `journal_rollback()`
- [ ] Rollback
- [ ] RollbackCorruptedContent
- [ ] RollbackCorruptedUsageBitmap

### `journal_free()`
- [ ] Free

### `journal_init()`
- [ ] InitUninitializedJournal
- [ ] InitValidJournal_BUG_MissingReturnForValidState (name/comment are stale — see notes)
- [ ] InitRollbackJournal

### Full lifecycle
- [ ] CompleteRollbackLifecycle

---

## Fixture: `JournalTest`

Heap-backed `Storage` sized from `mem_layout.h` (superheader / bitmap / journal / data regions),
zeroed each test. Two things worth internalizing before reading the tests:

- **The journal has exactly one slot.** `JRNL_HEADER_SECTOR`, `JRNL_CONTENT_SECTOR`, and
  `JRNL_USAGE_SECTOR` are fixed, single sectors — there's no queue of pending writes, just "the
  one thing currently being protected." Calling `journal_add()` again before `journal_free()`ing
  the previous entry overwrites it (this is relied on elsewhere — see the `write_message()`
  `STRG_FULL` path noted in [`test_hash_table.md`](test_hash_table.md)).
- **The usage-bitmap backup comes from the global `usage_bitmap` array, not a parameter.**
  `journal_add()` reads whichever 512-byte slice of the global in-RAM bitmap corresponds to the
  target sector and backs *that* up alongside the content. The fixture's `fill_global_bitmap_slice()`
  / `bitmap_slice_for_sector()` helpers exist specifically to set up and locate that slice for
  assertions.

Helpers:
- `create_header(state, type, sector, content, usage_bitmap_backup)` — builds a
  `JournalHeaderBuffer` with a correctly-computed header CRC, and (if given non-null buffers)
  correctly-computed content/usage-bitmap CRCs. Passing `content`/`usage_bitmap_backup` as
  `nullptr` (the default) leaves those two CRCs at `0` — used by tests that only care about
  header-level status, not full-record validity.
- `write_header()` / `read_header()` — raw storage access to the header sector, bypassing the
  `Journal` API, so tests can set up or verify state independently of the functions under test.
- `fill_pattern()` — fills a sector-sized buffer with one repeated byte, used to make corruption
  ("stamped vs. actual content differ") trivially checkable via a single byte value.

---

## `journal_header_init()`

### HeaderInit
Calls `journal_header_init()` directly and checks the written header has the right magic,
`JRNL_EMPTY` state, `JRNL_CONTACT` type, sector `0`, a correctly-computed header CRC, and
zeroed content/usage-bitmap CRCs (since nothing has been journalled yet). This is the "factory
default" header shape every other test implicitly assumes when starting from an uninitialized
journal.

---

## `get_journal_status()`

### StatusUninitialized
A completely zeroed header sector (no magic number) is classified `JRNL_UNINITIALIZED`.

### StatusValid
A header with `state = JRNL_EMPTY` and a correct magic/CRC is classified `JRNL_VALID` (nothing
to roll back), and reading it into `journal.header` preserves the magic/state fields.

### StatusCommittedIsValid
A header with `state = JRNL_COMMITTED` (a completed, already-applied journal entry) is *also*
`JRNL_VALID` — i.e. both "nothing was ever written" and "something was written and finished" are
equally safe states that need no rollback. Only `JRNL_ACTIVE` (mid-write) is not.

### StatusRollback
A header with `state = JRNL_ACTIVE` is classified `JRNL_ROLLBACK` — the one state that means a
previous write was interrupted and the target sector needs restoring.

### StatusCorruptedHeader
Writes a valid header, then flips every bit of the stored `header_crc` (XOR `0xFFFFFFFF`) and
rewrites it — status must come back `JRNL_CORRUPTED`. Checks the CRC is actually load-bearing,
not just computed and ignored.

### StatusCorruptedHeaderData
Same idea but corrupts the *data* (`state` field) without recomputing the CRC to match — so the
stored CRC no longer matches the stored data. Also must be `JRNL_CORRUPTED`. Together with
`StatusCorruptedHeader`, this checks corruption detection works whether the CRC field or the
data field is the one that got torn.

---

## `journal_data_init()`

### DataInit
`journal_data_init()` on a fresh `JournalHeaderDataB` sets magic, `state = JRNL_ACTIVE` (always
active — this is preparing to journal something about to be written), and the given
type/sector (`JRNL_MESSAGE`, `42`).

### DataInitContactType
Same, with `JRNL_CONTACT` / sector `0` — checks the type isn't hardcoded/ignored.

---

## `journal_write()`

### Write
The lowest-level write primitive: given a pre-built header plus separate content and
usage-bitmap-backup buffers, `journal_write()` writes all three to their respective fixed
sectors. Checked by reading each of the three sectors straight back off storage and comparing
against what was passed in. Every higher-level journal test (`journal_add`, `journal_rollback`,
etc.) depends on this primitive being correct.

---

## `journal_add()`

### Add
The real entry point used by `write_contact()`/`write_message()` before they modify a sector:
given just a type, target sector, and content, `journal_add()` must build a header with all
three CRCs computed correctly (header CRC over the data fields, content CRC over the passed
content, usage-bitmap CRC over the *correct slice of the global bitmap* for that sector — not a
hardcoded or zeroed value), and persist content + that bitmap slice alongside it. This is the
test that would catch the CRC/slice-selection logic silently drifting from what
`journal_rollback()` later expects to verify against.

### AddSelectsCorrectBitmapSector
A sharper version of the "correct slice" claim above: fills bitmap sector 0's backing memory
with one pattern (`0x11`) and bitmap sector 1's with another (`0x22`), then calls `journal_add()`
targeting a data sector that maps to bitmap sector 1 (`4096`, the first index bitmap sector 1
covers). The journal's on-disk usage-bitmap backup must contain sector 1's pattern (`0x22`), not
sector 0's — i.e. `journal_add()` doesn't just always back up bitmap sector 0 regardless of the
target.

### AddMessageType
`journal_add()` with `JRNL_MESSAGE` instead of `JRNL_CONTACT` persists that type correctly — a
one-line check that the type parameter isn't silently normalized to `JRNL_CONTACT` somewhere.

---

## `journal_header_read()`

### HeaderRead
Builds a fully-populated header (content + bitmap CRCs both non-zero, via `create_header()` with
real buffers) directly on storage, then checks `journal_header_read()` reads back a byte-for-byte
identical structure. A read-path counterpart to the write-path tests above.

---

## `journal_content_read()`

### ContentRead
Writes a known pattern directly to `JRNL_CONTENT_SECTOR`, calls `journal_content_read()`, and
checks it landed in `journal->content` (the in-RAM scratch buffer used during rollback).

---

## `journal_usage_read()`

### UsageRead
Same idea for `JRNL_USAGE_SECTOR` → `journal->usage_bitmap_sector`.

---

## `journal_rollback()`

### Rollback
The core recovery test: journals a sector via `journal_add()` (so the journal now holds the
sector's *old* content plus the corresponding bitmap slice), confirms `get_journal_status()`
correctly reports `JRNL_ROLLBACK`, then calls `journal_rollback()` and checks:
- the target data sector on storage is restored to the journalled (old) content,
- the real usage-bitmap sector on storage is restored to the journalled bitmap slice,
- the journal header's state ends up `JRNL_COMMITTED` (rollback completes and closes out the
  journal entry, rather than leaving it `JRNL_ACTIVE` for a second rollback attempt).

Note the comment in the test about `journal.header` needing to be freshly read — `journal_add()`
only touches storage, so anything asserting on `journal.header` in RAM after it needs to know
that field isn't automatically kept in sync by `journal_add()` itself.

### RollbackCorruptedContent
Stamps a header whose CRC was computed for one content buffer (`stamped_content`), but writes a
*different* buffer (`actual_content`) to the actual content sector — simulating a torn/partial
write of the content sector itself. `journal_rollback()` must detect the CRC mismatch, return
`false`, and — critically — leave the target data sector untouched (still zero from `SetUp`),
since the recovered content can't be trusted enough to apply. This is really testing "fail safe,
don't apply corrupted data" rather than just "detect corruption."

### RollbackCorruptedUsageBitmap
Same idea, but the mismatch is in the usage-bitmap backup rather than the content — a stamped
CRC that doesn't match what's actually stored in `JRNL_USAGE_SECTOR`. Also must fail.

---

## `journal_free()`

### Free
After journalling something (`state = JRNL_ACTIVE`), `journal_free()` marks it `JRNL_COMMITTED`
both in the in-RAM `journal.header` and on storage — this is what every successful write path
(`write_contact`, `write_message`, etc.) calls once it's done modifying the real sector, closing
out the journal entry without a rollback.

---

## `journal_init()`

### InitUninitializedJournal
On a completely fresh/zeroed journal sector, `journal_init()` must detect
`JRNL_UNINITIALIZED` and call through to `journal_header_init()`, leaving a valid empty header
behind (checked directly on storage).

### InitValidJournal_BUG_MissingReturnForValidState
The name and the comment directly above this test in `test_journal.cpp` describe a bug that
**no longer exists**: they claim `journal_init()`'s `switch` on `get_journal_status()` has no
`case JRNL_VALID` and no `default`, so control falls off the end of the function (UB) when the
journal is already valid. I checked the current `journal_init()` in `journal.c` and it already
has both:

```c
case JRNL_VALID:
    return true;
default:
    // unkown return from get_journal_status
    return false;
```

So this test — a healthy `JRNL_EMPTY` header already on storage, expecting `journal_init()` to
return `true` and leave `state == JRNL_EMPTY` — is in fact exercising perfectly well-defined,
correct behaviour today. The test itself is fine and worth reviewing normally; it's the
**name and comment that are stale** and should probably be updated (or the test renamed to
something like `InitValidJournal`) so a future reader doesn't waste time re-litigating an
already-fixed bug, or worse, assume the switch is still broken without checking. I haven't
renamed it myself since that's a one-line judgment call for you to make, not something worth a
drive-by edit.

### InitRollbackJournal
Journals a sector (leaving the on-disk journal `JRNL_ACTIVE`), then builds a **second, fresh**
`Journal` struct — simulating a real power-cycle, where nothing in RAM survives — and calls
`journal_init()` on it. It must discover the active journal from storage alone, perform the
rollback, restore the target sector, and leave the header `JRNL_COMMITTED`. This is the
realistic "boot after crash" path, as opposed to `Rollback` above which calls
`journal_rollback()` directly.

---

## Full lifecycle

### CompleteRollbackLifecycle
End-to-end: starts from `JRNL_UNINITIALIZED`, calls `journal_init()` (→ empty valid journal),
journals a sector via `journal_add()` (→ `JRNL_ACTIVE`), then — as in `InitRollbackJournal` —
builds a brand-new `Journal` struct to simulate a power-loss restart and calls `journal_init()`
on it, checking it rolls back correctly and both the data sector and the real usage-bitmap
sector end up restored, with the header finally `JRNL_COMMITTED`. This is the single test that
exercises the whole init → write → crash → recover cycle in one place; if you only have time to
re-verify one journal test by hand, this is the one that ties the others together.
