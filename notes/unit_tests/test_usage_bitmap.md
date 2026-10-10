# `tests/test_usage_bitmap.cpp` — Review Notes

Covers the global in-RAM/on-storage usage bitmap in
[`usage_bitmap.c`](../../source/app/src/usage_bitmap.c) — one bit per data sector, tracking
whether it's currently allocated. This is the structure `hash_reconstruct_contact()` and
`hash_reconstruct_message()` scan after a power cycle to know which sectors to re-read (see
[`test_hash_table.md`](test_hash_table.md) for how a bug in that scan's bounds went unnoticed).
27 tests, all passing (`ctest -R UsageBitmapTest`): the original 13 bit-level tests plus 14
iterator tests added with `usage_bitmap_iterator_init()`.

**Changed since first review:** bitmap sectors now carry the standard CRC-32 trailer in their
last word (see [test_sector_crc.md](test_sector_crc.md)), so a bitmap sector holds
`USAGE_BITS_PER_SECTOR = 127 * 32 = 4064` usable bits, not `512 * 8 = 4096`. The original tests
were updated to use `USAGE_BITS_PER_SECTOR`/`USAGE_WORDS_PER_SECTOR` instead of hardcoded
`4095`/`4096`/`127`; the descriptions below reflect that. The boxes for those tests are left
ticked, but `ReadUsageBitmap`, `BitmapSectorBoundary`, `EveryBitmapSectorBoundary` and
`MultipleBitmapSectors` are worth a second look.

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

### Iterator
- [ ] IteratorGetFailsBeforeFirstAdvance
- [ ] IteratorNextVisitsSetBitsInAscendingOrder
- [ ] IteratorPrevVisitsSetBitsInDescendingOrder
- [ ] IteratorNextDoesNotRepeatCurrentBit
- [ ] IteratorPrevMovesTowardsStart
- [ ] IteratorOverEmptyBitmapFindsNothing
- [ ] IteratorRespectsTotalBitsLimit
- [ ] IteratorRejectsNullArguments
- [ ] IteratorWithZeroTotalBitsFindsNothing
- [ ] IteratorIncludesLastBitBeforeLimit
- [ ] IteratorPrevRespectsTotalBitsLimitMidWord
- [ ] IteratorNextThenPrevReturnsToPreviousBit
- [ ] IteratorPrevFromBitZeroIsExhausted
- [ ] IteratorVisitsEverySetBitOverLargeRange

---

## Fixture: `UsageBitmapTest`

Sizes a heap-backed `Storage` from `mem_layout.h` (superheader + bitmap sectors + data sectors),
zeroes both the simulated SD card and the **global** `usage_bitmap` RAM array before each test.
That global-state reset matters: `check_usage_bit()`/`update_usage_bit()` operate on a
module-level array, not something passed in per-call, so any test that forgets to reset it would
silently see bits left set by a previous test.

Key size relationship exercised throughout: one 512-byte bitmap sector holds 127 usage words plus
a 4-byte CRC trailer, i.e. `USAGE_BITS_PER_SECTOR = 4064` bits covering 4064 data-sector indices.
Most of these tests are really different ways of probing that boundary.

---

## Tests

### ReadUsageBitmap
Writes a known `0xAA` byte pattern (CRC-stamped, since `read_usage_bitmap()` now rejects sectors
with a bad trailer) directly to every bitmap sector on the simulated SD card (bypassing
`update_usage_bit()` entirely), calls `read_usage_bitmap()`, and checks every usage word in the
global RAM array now reads `0xAAAAAAAA`, skipping the trailer words (`USAGE_BITMAP_IS_CRC_WORD`). This is the one test that validates
the bulk read path in isolation — everything else below only ever reads back through
`check_usage_bit()`.

### CheckUnusedBit
A handful of arbitrary untouched indices (`0`, `1`, `32`, `4095`, `4096`) all read as unset on a
freshly-zeroed bitmap. (`4095`/`4096` used to straddle the first sector boundary; since the CRC
change the boundary is `4063`/`4064`, so these are now just two indices in sector 1.)

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
The most targeted boundary test: sets index `USAGE_BITS_PER_SECTOR - 1` (`4063`, the last bit
representable by bitmap sector 0) and `USAGE_BITS_PER_SECTOR` (`4064`, the first bit of bitmap
sector 1), and checks:
- both read back true and don't interfere with each other,
- bitmap sector 0's on-disk word `USAGE_WORDS_PER_SECTOR - 1` (word 126 — word 127 is now the CRC
  trailer) is exactly `0x80000000` (bit 31, the *last* usage bit of the sector),
- bitmap sector 1's on-disk word 0 is exactly `0x00000001` (bit 0 — the *first* bit of the next
  sector).

This is exactly the boundary a bug in `USAGE_BITMAP_FIND_SECTOR`/`_FIND_ELEMENT`/`_FIND_BIT`
would corrupt, so it's worth re-deriving the `126`/`31`/`0`/`0` numbers by hand against those
macros in `mem_layout.h` rather than just trusting the comment above the test.

### EveryBitmapSectorBoundary
Generalizes `BitmapSectorBoundary` into a loop over *every* bitmap sector the current
`mem_layout.h` layout has (`USAGE_BITMAP_SECTOR_SIZE` of them): for each one, sets its first and
last representable index, checks both round-trip, and checks the corresponding on-disk sector
has bit 0 of word 0 set and — only for a sector that's fully covered by data (i.e. not the last,
possibly-partial one) — bit 31 of the last usage word (`USAGE_WORDS_PER_SECTOR - 1`) set. Subsumes `BitmapSectorBoundary` for every
sector rather than just the 0/1 boundary, so if this passes, that one is somewhat redundant
(though cheap to keep as a documented, easy-to-read special case).

### DoesNotModifyNeighbouringBits
Sets index `100` and checks its immediate neighbours (`99`, `101`) and the first/last bit of its
containing word (`96`, `127`) all remain clear — a "no bleed" check within a single word, using
an index that isn't itself a word boundary (unlike `WordBoundary` below, which specifically
targets the boundaries).

### MultipleBitmapSectors
Sets one bit each in three different bitmap sectors (indices `0`, `USAGE_BITS_PER_SECTOR`,
`2 * USAGE_BITS_PER_SECTOR` — each exactly on a sector boundary), checks all three read back true, and reads each of the three underlying
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

---

## Iterator tests

Cover `usage_bitmap_iterator_init(ctx, total_bits, total_words)` and the `next`/`prev`/`get`
functions behind the generic [`Iterator`](../../source/app/include/iterator.h) interface. The
iterator walks set bits in `[0, total_bits)` using `get_next_bit()`/`get_prev_bit()` underneath
(which `hash_reconstruct_message()` also calls directly). Same semantics as the hash table
iterator ([test_hash_table_iterator.md](test_hash_table_iterator.md)): no position until the
first `next()`/`prev()`, `get()` returns the bit index, and exhaustion makes `next`/`prev` return
false and `get()` fail.

Most tests use a 64-bit, 2-word range, so word boundary 31/32 is always in play.

### IteratorGetFailsBeforeFirstAdvance
A fresh iterator: `get()` returns false.

### IteratorNextVisitsSetBitsInAscendingOrder
Bits `{0, 5, 31, 32, 40, 63}` are visited by `next()` in exactly that order (crossing the word
boundary and hitting both ends of the range), then `get()` fails.

### IteratorPrevVisitsSetBitsInDescendingOrder
Same bits, walked with `prev()` from an unpositioned iterator: exact reverse order.

### IteratorNextDoesNotRepeatCurrentBit
**Regression for:** `get_next_bit()`'s range includes the start position, so `next()` must search
from `current + 1` or it returns the same bit forever. Bits 3 and 4 → `next` gives 3 then 4.

### IteratorPrevMovesTowardsStart
**Regression for:** `prev()` searching towards the end of the range instead of index 0. From bit
60, `prev()` must land on 3, then be exhausted.

### IteratorOverEmptyBitmapFindsNothing
No bits set → `next`, `prev` and `get` all fail.

### IteratorRespectsTotalBitsLimit
Bits 10 and 40 set, iterator limited to 32 bits: only 10 is visited.

### IteratorRejectsNullArguments
NULL iterator to `next`/`prev`/`get`, and NULL output to `get`, all return false.

### IteratorWithZeroTotalBitsFindsNothing
`total_bits == 0` visits nothing even though bit 0 is set (no `0 - 1` underflow into a huge range).

### IteratorIncludesLastBitBeforeLimit
Bits 39 and 40 set, limit 40: both `next` and `prev` visit 39 and nothing else. Pins the range as
half-open `[0, total_bits)`.

### IteratorPrevRespectsTotalBitsLimitMidWord
Bits 33 and 45 set, limit 40 (inside word 1): an unpositioned `prev()` starts at bit 39 and finds
33, skipping 45 even though it's in the same `uint32_t` word.

### IteratorNextThenPrevReturnsToPreviousBit
Bits `{2, 31, 32, 50}`: advance to 32, `prev` → 31 (back across the word boundary), `next` → 32,
`next` → 50. Mixing directions neither skips nor repeats.

### IteratorPrevFromBitZeroIsExhausted
Positioned on bit 0, `prev()` returns false and `get()` then fails — no underflow wrap to 65535.

### IteratorVisitsEverySetBitOverLargeRange
Every 7th bit in `[0, 1024)` (32 words): `next()` visits exactly those bits in order.

Note these iterator tests all stay inside the first bitmap sector. The CRC-trailer skip when
crossing *sector* boundaries is covered by `BitmapWalkSkipsTrailer` in
[test_sector_crc.md](test_sector_crc.md).
