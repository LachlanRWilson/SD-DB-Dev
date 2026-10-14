# `tests/test_message_range.cpp` — Review Notes

Covers `read_n_messages()` in [`message.c`](../../source/app/src/message.c): reading `n` messages
of one chat starting at message index `startMsg`, so the UI can page back through a conversation.
`hash_find_n_message(table, phone, start, n, out)` is a thin wrapper over it. 19 tests in one
suite, all passing (`ctest -R MessageRangeTest`).

| Group | Tests | What it covers |
|---|---|---|
| get_range | 14 | paging from any `start`, sector boundaries, truncation at the oldest message, argument checks |
| failure | 2 | storage is read-only, read failures are passed back |
| corruption | 3 | unused sector, bad `msg_count`, looping `prev` chain |

Things worth knowing before reading the tests:

- **A chat is a linked list of message sectors.** `startIndex` is the latest sector and
  `header.prev` walks back to older ones, ending at `UINT16_MAX`. Each sector holds
  `MESSAGE_SECTOR_CAPACITY` (`kCap`, = 2) messages.
- **Only the latest sector can be partly full.** Messages can't be deleted one at a time, so
  every older sector has slots `0 .. msg_count - 1` filled. `read_n_messages()` relies on this
  to work out which slot `startMsg` lands on.
- **`startMsg` is a 0-based index from the newest message.** Index 0 is the newest message
  (slot `msg_count - 1` of the latest sector). The read walks back until the sectors read so far
  cover `startMsg` (`msg_passed > startMsg`), then starts at slot `msg_passed - 1 - startMsg` of
  that sector.
- **The fixture spreads sectors out.** `sector_index(i)` = `(i * 977 + 5) % TOTAL_MESSAGE_SECTOR_SIZE`,
  so a read that assumes the previous sector is `index - 1` fails. `latest == UINT16_MAX` means
  the chat has no sectors.
- **`get_range()` checks for overruns.** It sizes `out` to `n + 1` and puts `0xBEEF` in the
  extra slot. Every test then fails if the read writes past `n`.
- **`expect_range(out, start, count)`** checks `out[i]` is message `added - 1 - start - i`
  (timestamp, direction and text), i.e. newest first starting at `start`.

Return values:

| Case | Return | `out_count` |
|---|---|---|
| `n == 0` | `STRG_OK` | 0 |
| empty chat (`startIndex == UINT16_MAX`) | `STRG_OK` | 0 |
| `start` past the oldest message | `STRG_FAIL` | 0 |
| range runs past the oldest message | `STRG_OK` | messages up to and including the oldest |
| `startIndex` sector not in use | `STRG_EMPTY` | 0 |
| bad `msg_count`, looping chain, NULL argument | `STRG_FAIL` | — |

## Checklist

### get_range
- [ ] GetRangeReturnsFirstTenMessages
- [ ] GetRangeReturnsSecondTenMessages
- [ ] GetRangeOnEmptyChatReturnsZero
- [ ] GetRangeWithZeroRequestedReturnsZero
- [ ] GetRangeRejectsNullArguments
- [ ] GetRangeStartInsideFullLatestSector
- [ ] GetRangeStartOnSectorBoundary
- [ ] GetRangeStartOnOldestSlotOfFullSector
- [ ] GetRangeWithFullLatestSector
- [ ] GetRangePastOldestIsTruncatedNoDuplicates
- [ ] GetRangeStartOnOldestMessageReturnsOne
- [ ] GetRangeStartPastStoredFails
- [ ] GetRangeReadsToOldestAndStops
- [ ] GetRangeEveryStartMatchesExpectedSlice

### failure
- [ ] GetRangeDoesNotModifyStorage
- [ ] GetRangePropagatesReadFailure

### corruption
- [ ] GetRangeOnUnusedSectorReturnsEmpty
- [ ] GetRangeRejectsCorruptMessageCount
- [ ] GetRangeStopsOnLoopingPrevChain

---

## get_range

### GetRangeReturnsFirstTenMessages
10 messages, `get_range(0, 10)` returns all 10 newest first.

### GetRangeReturnsSecondTenMessages
20 messages, read as two pages: `(0, 10)` and `(10, 10)`. Each page is the right slice, and
together the two pages are every message exactly once (timestamps 19 down to 0). This is the
paging the UI will do.

### GetRangeOnEmptyChatReturnsZero
A chat with no sectors (`startIndex == UINT16_MAX`) returns `STRG_OK` with a count of 0.
`read_n_messages()` checks for this before walking the chain. `hash_find_n_message()` never gets
here, since it returns -1 when the contact's `latest_msg_extent` is `UINT16_MAX`.

### GetRangeWithZeroRequestedReturnsZero
`n = 0` returns `STRG_OK` with a count of 0, without reading anything.

### GetRangeRejectsNullArguments
A NULL `storage`, `out_count` or `out` returns `STRG_FAIL`.

### GetRangeStartInsideFullLatestSector
4 messages (two full sectors), `start = 1`: the read starts on the older slot of the latest
sector and carries on into the next sector.

### GetRangeStartOnSectorBoundary
3 messages, so the latest sector holds 1. `start = 1` is the newest message of the sector
behind it. This is the case where `msg_passed == startMsg` after the first sector, so the loop
has to read one more sector. A `<` loop condition stops one sector early and the slot offset
wraps to 255.

### GetRangeStartOnOldestSlotOfFullSector
5 messages, `start = kCap` (2): the read starts on slot 0 of a full sector, then moves to the next
sector for the second message.

### GetRangeWithFullLatestSector
6 messages (three full sectors), `start = kCap`, `n = kCap`: returns exactly the middle sector.

### GetRangePastOldestIsTruncatedNoDuplicates
10 messages, `get_range(8, 10)` returns only the 2 that exist. It stops at the end of the chain
without repeating messages or reading past it.

### GetRangeStartOnOldestMessageReturnsOne
10 messages, `start = 9` (the oldest): returns 1 message.

### GetRangeStartPastStoredFails
With 10 messages (full latest sector) and with 9 (partly full), `start = n` and `start = n + 50`
both return `STRG_FAIL`. A start past the oldest message is an error, unlike a range that runs
past it (previous two tests).

### GetRangeReadsToOldestAndStops
25 messages, `get_range(10, 1000)` returns the 15 from index 10 to the oldest.

### GetRangeEveryStartMatchesExpectedSlice
For chats of 1, `kCap`, `kCap + 1`, `2 * kCap + 1`, 20 and 31 messages, reads a page of 4 from
every valid `start` and checks the slice. This covers every slot position and sector boundary for
both full and partly full latest sectors.

---

## failure

### GetRangeDoesNotModifyStorage
A read leaves the storage bytes unchanged.

### GetRangePropagatesReadFailure
Uses `FailableStorageCtx` ([test_support.md](test_support.md)) to fail the 1st, 2nd and 3rd sector
read: the latest sector, a sector while walking back to `start`, and a sector after the read has
started. Each returns `STRG_FAIL`.

---

## corruption

### GetRangeOnUnusedSectorReturnsEmpty
`startIndex` points at a sector whose usage bit is clear. Returns `STRG_EMPTY`, the same as
`read_message()` and `remove_message_sector()` for an unused sector.

### GetRangeRejectsCorruptMessageCount
The sector behind the latest one says it holds `kCap + 5` messages (`corrupt_header()` re-stamps
the CRC so the read itself succeeds). Starting before it and starting inside it both return
`STRG_FAIL` rather than reading past the end of `messages[]`.

### GetRangeStopsOnLoopingPrevChain
The oldest sector's `prev` points back at the latest one, so the chain never reaches
`UINT16_MAX`. With a `start` or `n` larger than every sector could hold, the read stops after
`TOTAL_MESSAGE_SECTOR_SIZE` sectors and returns `STRG_FAIL` instead of looping forever or
returning repeated messages.
