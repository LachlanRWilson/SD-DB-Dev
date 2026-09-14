# `tests/test_usage_bitmap.cpp` — Review Notes

Covers the global in-RAM/on-storage usage bitmap in
[`usage_bitmap.c`](../../source/app/src/usage_bitmap.c) — one bit per data sector, tracking
whether it's currently allocated. This is the structure `hash_reconstruct_contact()` and
`hash_reconstruct_message()` scan after a power cycle to know which sectors to re-read (see
[`test_hash_table.md`](test_hash_table.md) for how a bug in that scan's bounds went unnoticed).
13 tests, all passing (`ctest -R UsageBitmapTest`).

## Checklist

- [X] ReadUsageBitmap
- [X] CheckUnusedBit
- [X] SetUsageBit
- [X] ClearUsageBit
- [X] AllBitsInWord
- [X] BitmapSectorBoundary
- [X] EveryBitmapSectorBoundary
- [X] DoesNotModifyNeighbouringBits
- [X] MultipleBitmapSectors
- [X] LastDataSector
- [X] RepeatedSetClear
- [X] WordBoundary
- [X] EveryDataSector

---

## Fixture: `UsageBitmapTest`

Sizes a heap-backed `Storage` from `mem_layout.h` (superheader + bitmap sectors + data sectors),
zeroes both the simulated SD card and the **global** `usage_bitmap` RAM array before each test.
That global-state reset matters: `check_usage_bit()`/`update_usage_bit()` operate on a
module-level array, not something passed in per-call, so any test that forgets to reset it would
silently see bits left set by a previous test.

Key size relationship exercised throughout: one 512-byte bitmap sector holds `512 * 8 = 4096`
bits, i.e. covers 4096 data-sector indices. Most of these tests are really different ways of
probing that boundary.

---

## Tests

### ReadUsageBitmap
Writes a known `0xAA` byte pattern directly to every bitmap sector on the simulated SD card
(bypassing `update_usage_bit()` entirely), calls `read_usage_bitmap()`, and checks every
`uint32_t` in the global RAM array now reads `0xAAAAAAAA`. This is the one test that validates
the bulk read path in isolation — everything else below only ever reads back through
`check_usage_bit()`.

### CheckUnusedBit
A handful of arbitrary untouched indices (`0`, `1`, `32`, `4095`, `4096` — note `4095`/`4096`
straddle the first sector boundary) all read as unset on a freshly-zeroed bitmap.

### SetUsageBit
Sets bit `0`, checks it reads back true via `check_usage_bit()` (RAM), and separately reads the
raw bytes back off simulated storage to confirm the on-disk word is exactly `0x00000001` — i.e.
checks the RAM and on-storage views agree, not just one or the other.

### ClearUsageBit
Same as above but sets then clears the bit, checking both RAM and storage return to `0`
afterwards. Together with `SetUsageBit`, this is the core set/clear round-trip.

### AllBitsInWord
Sets all 32 bits of the first `uint32_t` one at a time (indices `0..31`), checks each reads back
true individually, then confirms the underlying word is exactly `0xFFFFFFFF` — i.e. setting bit
31 doesn't clobber bit 0, etc., across a full word.

### BitmapSectorBoundary
The most targeted boundary test: sets index `4095` (last bit representable by bitmap sector 0)
and index `4096` (first bit of bitmap sector 1), and checks:
- both read back true and don't interfere with each other,
- bitmap sector 0's on-disk word 127 (`4095`'s word within that sector) is exactly `0x80000000`
  (bit 31 — the *last* bit of that word, confirming `4095` lands where expected),
- bitmap sector 1's on-disk word 0 is exactly `0x00000001` (bit 0 — the *first* bit of the next
  sector).

This is exactly the boundary a bug in `USAGE_BITMAP_FIND_SECTOR`/`_FIND_ELEMENT`/`_FIND_BIT`
would corrupt, so it's worth re-deriving the `127`/`31`/`0`/`0` numbers by hand against those
macros in `mem_layout.h` rather than just trusting the comment above the test.

### EveryBitmapSectorBoundary
Generalizes `BitmapSectorBoundary` into a loop over *every* bitmap sector the current
`mem_layout.h` layout has (`USAGE_BITMAP_SECTOR_SIZE` of them): for each one, sets its first and
last representable index, checks both round-trip, and checks the corresponding on-disk sector
has bit 0 of word 0 set and — only for a sector that's fully covered by data (i.e. not the last,
possibly-partial one) — bit 31 of word 127 set. Subsumes `BitmapSectorBoundary` for every
sector rather than just the 0/1 boundary, so if this passes, that one is somewhat redundant
(though cheap to keep as a documented, easy-to-read special case).

### DoesNotModifyNeighbouringBits
Sets index `100` and checks its immediate neighbours (`99`, `101`) and the first/last bit of its
containing word (`96`, `127`) all remain clear — a "no bleed" check within a single word, using
an index that isn't itself a word boundary (unlike `WordBoundary` below, which specifically
targets the boundaries).

### MultipleBitmapSectors
Sets one bit each in three different bitmap sectors (indices `0`, `4096`, `8192` — each exactly
on a sector boundary), checks all three read back true, and reads each of the three underlying
on-disk sectors to confirm each holds exactly `0x00000001` — i.e. writes to different bitmap
sectors don't cross-contaminate each other's storage.

### LastDataSector
Sets the very last valid index (`TOTAL_DATA_SECTOR_SIZE - 1`) and confirms it round-trips and
lands in a valid bitmap sector (`< USAGE_BITMAP_SECTOR_SIZE`) — an off-by-one guard at the top
end of the whole addressable range, which is exactly where an off-by-one in bitmap sizing would
either overflow the array or silently alias back to a lower sector.

### RepeatedSetClear
Toggles a single index (`12345`) true/false 100 times in a row, checking `check_usage_bit()`
after every toggle — catches state that doesn't fully reset between updates (e.g. a
read-modify-write bug that ORs instead of properly clearing, which might survive a single
set/clear pair but drift after repetition).

### WordBoundary
Sets 8 indices straddling four word boundaries (`31/32`, `63/64`, `95/96`, `127/128` — i.e. the
last bit of one `uint32_t` and the first bit of the next, four times over) and checks every one
of the 8 remains independently set at the end — a `DoesNotModifyNeighbouringBits`-style check
but specifically aimed at the boundary case that test deliberately avoids.

### EveryDataSector
The heaviest test here: sets *every* valid index from `0` to `TOTAL_DATA_SECTOR_SIZE - 1`,
verifies they're all set, clears every one, verifies they're all clear — an exhaustive
sweep rather than sampled boundary checks. Good confidence that there's no isolated index
anywhere in the whole range that's mishandled, at the cost of being the slowest test in this
file (still comfortably fast in practice, given the CI timing shown in `ctest` output).
