# `tests/test_message_history.cpp` — Review Notes

Covers the message history layer in [`message_history.c`](../../source/app/src/message_history.c),
which sits on the generic ring buffer ([test_ring_buffer.md](test_ring_buffer.md)). Each ring
buffer slot is one sector that holds `MESSAGE_HISTORY_SECTOR_CAPACITY` (= 3) whole `Message`s.
`header.head` is the slot of the newest message in that sector. 36 tests in one suite, all
passing (`ctest -R MessageHistoryTest`).

| Group | Tests | What it covers |
|---|---|---|
| add | 9 | `message_history_add()`: filling a sector, moving to the next one, wrapping, error paths |
| get_list | 10 | `message_history_get_range()` from `start = 0` (newest first) |
| get_range | 16 | `message_history_get_range()` with any `start`, i.e. paging |
| persistence | 1 | `init_ring_buffer()` rebuilds the same RAM state from the card |

Things worth knowing before reading the tests:

- **There is no `message_history_get_list()`.** "get_list" in the test names means
  `get_range(0, n)`. The fixture's `get_list()` helper is that call.
- **Only the current sector can be partly full.** Every older sector is `RB_FULL` with
  `head == kCap - 1`. `get_range()` relies on this to work out which sector `start` lands in
  without reading the older sectors. The tests that cover it are `GetRangeStartOnSectorBoundary`
  and `GetRangeStartOnOldestSlotOfFullSector`.
- **Wrapping drops a whole sector.** When the buffer is full, moving to the next slot reuses the
  oldest sector, so all `kCap` of its messages go at once. After a wrap, the number of messages
  stored is `(messages in current sector) + (kSectors - 1) * kCap`. The file-local
  `stored_after()` computes this.
- **The order is always newest first.** Index 0 of the output is the newest message (the current
  sector's `head`), and each later index is one message older.

## Checklist

### add
- [ ] FirstAddClaimsSectorZero
- [ ] AddsWithinSectorAdvanceHead
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
- [ ] GetRangeReturnsFirstTenMessages
- [ ] GetRangeReturnsSecondTenMessages
- [ ] GetRangeOnEmptyBufferReturnsZero
- [ ] GetRangeWithZeroRequestedReturnsZero
- [ ] GetRangeRejectsNullArguments
- [ ] GetRangeStartInsidePartialCurrentSector
- [ ] GetRangeStartOnSectorBoundary
- [ ] GetRangeStartOnOldestSlotOfFullSector
- [ ] GetRangeWithFullCurrentSector
- [ ] GetRangePastOldestIsTruncatedNoDuplicates
- [ ] GetRangeStartOnOldestMessageReturnsOne
- [ ] GetRangeStartPastStoredFails
- [ ] GetRangeOnWrappedBufferCrossesWrap
- [ ] GetRangeEveryStartMatchesExpectedSlice
- [ ] GetRangeDoesNotModifyStorage
- [ ] GetRangePropagatesReadFailure

### persistence
- [ ] ReconstructAfterAddsMatchesRamState

---

## Fixture (`MessageHistoryTest`)

Uses [`FailableStorageCtx`](test_support.md) over a small simulated card:

- **Region:** `kSectors = 8` sectors starting at raw sector
  `kStart = DATA_REGION_START_SECTOR + MESSAGE_HISTORY_DATA_START_SECTOR`, the same address the
  firmware uses. That gives room for 24 messages, enough for the 20-message paging tests while
  still small enough to wrap many times.
- **Format:** `SetUp()` writes every region sector as `RB_EMPTY` with a valid CRC, then calls
  `init_ring_buffer()`.
- **Guard sector:** the sector just after the region (`kGuard`) is written as `RB_OCCUPIED`,
  `head = kCap - 1`, seq `0xDEADBEEF`. A read past the end of the region sees what looks like real
  data, and a write past the end overwrites the guard's seq, which `ManyWrapsStayInsideRegion`
  checks for.
- **Journal:** a zeroed `Journal` pointing at the same storage. The add path journals each sector
  before writing it.
- **Messages:** `make_message(id)` sets `timestamp = id`, alternates `direction`, and sets
  `str = "message <id>"`. `added` counts messages added so far, so the newest message is always
  `make_message(added - 1)`.
- **Helpers:**
  - `add(n)` adds the next `n` messages and asserts that each add returns `STRG_OK`.
  - `stored(i)` reads region sector `i` back from the card.
  - `get_list(n, …)` and `get_range(start, n, …)` size the output to `n + 1` and put a sentinel
    `timestamp = 0xBEEF` in the extra slot. Every call then checks the sentinel is still there, so
    any write past `n` fails the test. `count` starts at `0xFFFF` so that a function which never
    sets it is caught.
  - `expect_newest_first(out, count)` and `expect_range(out, start, count)` compare each output
    slot against the message expected at that position.

Tests that loop over several fill levels (`GetRangeStartPastStoredFails`,
`GetRangeEveryStartMatchesExpectedSlice`, `ReconstructAfterAddsMatchesRamState`) call `SetUp()`
again and reset `added = 0` before each one, so each fill level starts from a freshly formatted
card.

---

## add

### FirstAddClaimsSectorZero
The first add on an empty buffer claims sector 0: `current_index 0`, `occupancy 1`, `seq 0`. The
sector on the card is `RB_OCCUPIED`, `head 0`, `seq 0`, and slot 0 holds message 0.

### AddsWithinSectorAdvanceHead
`kCap` adds fill sector 0 without moving on. `head` ends at `kCap - 1` and slots `0..kCap-1` hold
messages `0..kCap-1` in order.

### AddPastFullSectorMovesToNextSector
Add number `kCap + 1` finds sector 0 full. Sector 0 is marked `RB_FULL` (head stays at
`kCap - 1`), the ring buffer moves to sector 1 (`current_index 1`, `occupancy 2`, `seq 1`), and
the new message goes into slot 0 of sector 1 with `seq 1`.

### SectorsFillSequentiallyWithIncreasingSeq
Fills all 8 sectors exactly. Sector `i` has seq `i`, its first slot holds message `i * kCap`, and
every sector is `RB_FULL` except the last, which is still `RB_OCCUPIED`. Reconstruction depends on
seq going up by one per sector.

### WrapOverwritesOldestSector
One add past full wraps around to sector 0: `current_index 0`, occupancy stays at 8, `seq 8`.
Sector 0 is reset to `RB_OCCUPIED`, `head 0`, `seq 8`, holding the new message.

### AddsAfterWrapKeepFillingReusedSector
Three adds after the wrap. The reused sector must append (`head 2`, slots 0..2 hold the three new
messages). Without this test, a reused sector could reset `head` to 0 on every add and only ever
keep the newest message.

### ManyWrapsStayInsideRegion
Three and a bit full laps. `current_index` stays below `kSectors`, occupancy stays at `kSectors`,
and the guard sector's seq is still `0xDEADBEEF`, so nothing was written past the region.

### AddRejectsCorruptSectorState
The current sector's state is set to `0x7F`, which is not a valid state (it is written with a
valid CRC, so the read succeeds). The add must fail and leave the sector unchanged instead of
treating it as empty.

### AddPropagatesReadFailure
The first read fails, so the add returns `STRG_FAIL`.

---

## get_list

### GetListOnEmptyBufferReturnsZero
A formatted, empty buffer returns `STRG_OK` with `count 0`.

### GetListWithZeroRequestedReturnsZero
`n = 0` on a non-empty buffer returns `count 0`, and the sentinel check confirms nothing was
written.

### GetListWithinOneSectorIsNewestFirst
Three messages, all in sector 0, come back as 2, 1, 0.

### GetListFewerThanStoredReturnsNewest
`kCap + 2` messages are stored and 4 are requested. The result is the 4 newest, which spans the
partly full current sector and the newest slots of the full sector before it.

### GetListSpansSectors
Requests all `2 * kCap + 1` messages across three sectors and gets every one, newest first.

### GetListMoreThanStoredReturnsStoredCountNoDuplicates
Requests 50 when only `kCap + 1` are stored. `count` must equal the stored number. This catches
the iterator wrapping back to the current sector and returning messages a second time.

### GetListOnWrappedBufferReturnsNewestAndStops
After one wrap plus 2 messages, requests 1000. The result is the 2 messages in sector 0, then
sectors 7, 6, … 1, and then it stops. The messages that were overwritten must not appear.

### GetListOnFullUnwrappedBufferReturnsAll
All 8 sectors are full with no wrap yet. Requesting 1000 returns exactly `kSectors * kCap`.

### GetListDoesNotModifyStorage
Snapshots the whole simulated card, runs a read, and compares. Reads must not write anything,
including the journal.

### GetListPropagatesReadFailure
The first read fails, so `STRG_FAIL` is returned.

---

## get_range

### GetRangeReturnsFirstTenMessages
`get_range(0, 10)` on 10 messages returns all of them, newest first.

### GetRangeReturnsSecondTenMessages
With 20 messages, `(0, 10)` and `(10, 10)` are the two pages. Each page is checked against its
expected messages, and the two pages joined together must be exactly messages 19..0. This catches
an off-by-one at the page boundary, which would show up as a duplicate or a gap.

### GetRangeOnEmptyBufferReturnsZero / GetRangeWithZeroRequestedReturnsZero
Same as the get_list versions, but with a non-zero `start`.

### GetRangeRejectsNullArguments
A null `rb`, `storage`, `out_count` or `out_list` each return `STRG_FAIL`.

### GetRangeStartInsidePartialCurrentSector
The current sector holds 2 messages. `start = 1` is the older of the two, so the page starts
partway through the current sector and continues into the previous one.

### GetRangeStartOnSectorBoundary
`start = 2` equals the number of messages in the current sector, so the page starts at the newest
slot (`kCap - 1`) of the previous sector. This is the `start >= cur_cnt` branch with `rem = 0`.

### GetRangeStartOnOldestSlotOfFullSector
`start = kCap` with 1 message in the current sector lands on slot 0 of the previous sector (the
`rem % kCap == kCap - 1` case), so the page continues from the sector before that.

### GetRangeWithFullCurrentSector
With 3 full sectors, `get_range(kCap, kCap)` returns exactly the middle sector. With the current
sector full, `cur_cnt == kCap` and the boundary sits on a whole sector.

### GetRangePastOldestIsTruncatedNoDuplicates
`(8, 10)` with only 10 stored returns 2, not 10, and does not wrap round to newer messages.

### GetRangeStartOnOldestMessageReturnsOne
`(9, 5)` with 10 stored returns only the oldest message.

### GetRangeStartPastStoredFails
`start == stored` and `start == stored + 50` both return `STRG_FAIL`. This is checked with a
partly full current sector (10 messages) and a full one (9 messages). This is how a pager can
tell it has reached the end.

### GetRangeOnWrappedBufferCrossesWrap
After one wrap plus 2 messages, `current_index` is 0 and the older sectors are 7..1. Three cases:
a page from `start = 1` that crosses from sector 0 into sector 7; a page that starts behind the
wrap (`2 + kCap + 1`), which exercises the negative start index that gets wrapped round to
`occupancy + index`; and a page that reads to the oldest message and stops at `stored - 10`.

### GetRangeEveryStartMatchesExpectedSlice
The broad test. Fill levels are 1, `kCap`, `kCap + 1`, 20, full, full + 1, full + `kCap`, and two
laps + 2. For each fill level, every valid `start` is read with a page size of 4, and each result
is checked against `min(4, stored - start)` messages at the expected positions. It covers every
combination of partly full or full current sector, wrapped or not, and page inside one sector or
crossing sectors.

### GetRangeDoesNotModifyStorage
Same check as the get_list version, but with a non-zero `start`.

### GetRangePropagatesReadFailure
Checks two failure points: the read of the current sector (read 1), and a read of a later sector
partway through the walk (read 3).

---

## persistence

### ReconstructAfterAddsMatchesRamState
For fill levels 1, `kCap`, `kCap + 1`, full, full + 1 and two laps + 3: a new `RingBuffer` is
built with `init_ring_buffer()` from the card, and its `current_index`, `occupancy` and `seq` must
match the in-RAM buffer that did the adds. Then `kCap + 1` more messages are added to the rebuilt
buffer (so it crosses at least one sector boundary), and `get_list(5)` must still return the
newest 5. This is the on-host version of the reboot check the `MH_DEMO` read half does on
hardware.

Not covered here: a power cut partway through an add followed by journal rollback. That rollback
path (`JRNL_MSG_HIST`) is tested at the journal level in `JournalTest.RollbackHistoryEntrySkipsBitmapAndCommits`, not through
`message_history_add()`.
