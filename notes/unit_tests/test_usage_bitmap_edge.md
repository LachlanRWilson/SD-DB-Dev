# `tests/test_usage_bitmap_edge.cpp` — Review Notes

Edge cases for [`usage_bitmap.c`](../../source/app/src/usage_bitmap.c), complementing the
boundary-crossing coverage in [test_usage_bitmap.md](test_usage_bitmap.md). 11 tests, all
passing (`ctest -R UsageBitmapEdgeTest`). Uses the shared
[`FailableStorageCtx`](test_support.md) mock to inject read/write failures that real
`HeapStorage` can't produce on its own.

## Checklist

- [ ] InitClearsPreviouslySetBitsInRamAndStorage
- [ ] InitPropagatesStorageWriteFailure
- [ ] ReadPropagatesStorageReadFailure
- [ ] FailedReadLeavesExistingRamBitsIntact
- [ ] UpdatePropagatesStorageWriteFailure
- [ ] FailedWriteLeavesRamAheadOfStorage
- [ ] SettingAlreadySetBitIsIdempotent
- [ ] ClearingAlreadyClearBitIsNoOp
- [ ] FirstPaddingBitAfterLastDataSectorIsUsable
- [ ] LastAddressableArrayBitIsUsable
- [ ] UpdateFailsWhenTargetSectorExceedsStorageCapacity

---

## `init_usage_bitmap()`

### InitClearsPreviouslySetBitsInRamAndStorage
Sets a couple of bits, then calls `init_usage_bitmap()` and checks both are cleared — in RAM via
`check_usage_bit()`, and on storage by reading bitmap sector 0 directly and comparing against an
all-zero buffer. `test_usage_bitmap.cpp` never actually calls `init_usage_bitmap()` at all (it
resets the RAM bitmap via a raw `memset` in `SetUp()` instead), so this is the only place that
function is exercised.

### InitPropagatesStorageWriteFailure
If the underlying `write_multiblock()` call fails, `init_usage_bitmap()` must return `false`
rather than reporting success with a RAM bitmap that storage doesn't actually match (the RAM
side is unconditionally zeroed *before* the write is attempted, so a failure here always leaves
RAM ahead of storage — the same characteristic documented more directly in
`FailedWriteLeavesRamAheadOfStorage` below for `update_usage_bit()`).

---

## `read_usage_bitmap()`

### ReadPropagatesStorageReadFailure
A failing `read_multiblock()` call makes `read_usage_bitmap()` return `false`.

### FailedReadLeavesExistingRamBitsIntact
Sets a bit, then triggers a failed read. Since `HeapStorage_ReadMultiBlock()` either fills the
whole output buffer or doesn't touch it at all (checked before the `memcpy`, not partway
through), a failed read must leave whatever was already in RAM untouched — checked by confirming
the previously-set bit survives the failed read.

---

## `update_usage_bit()`

### UpdatePropagatesStorageWriteFailure
A failing `write_block()` call makes `update_usage_bit()` return `STRG_FAIL` rather than
`STRG_OK`.

### FailedWriteLeavesRamAheadOfStorage
**Characterisation test, not a "this is correct" assertion.** `update_usage_bit()` mutates the
global RAM bitmap word *before* attempting the storage write:
```c
if (used_state) { write_sector[element] |= (1U << bit); } else { write_sector[element] &= ~(1U << bit); }
return storage->write_block(...);
```
So when the write fails, RAM already reflects the *new* state (`check_usage_bit()` returns
`true`) while storage still holds the *old* state (confirmed here by reading the raw sector back
and comparing against zero). This test exists purely to pin that ordering down as an explicit,
regression-tested fact — if a future change reorders this (write-then-mutate-RAM, or a
rollback-on-failure), this test's expectations would need to flip, and that should be a
deliberate decision made with this test in front of you, not something noticed by accident three
files away. Worth thinking about whether callers (`write_contact()`, `write_message()`, etc.)
actually rely on RAM/storage staying in sync here, given the journal is supposed to be the
mechanism that recovers from exactly this kind of interrupted write.

### SettingAlreadySetBitIsIdempotent / ClearingAlreadyClearBitIsNoOp
Repeating a set or clear on a bit already in that state doesn't corrupt neighbouring bits or
otherwise misbehave — narrow idempotency checks that complement `RepeatedSetClear` in the main
suite (which toggles back and forth 100 times; these two check each direction settles cleanly on
its own).

---

## Addressable-range boundaries

`USAGE_BITMAP_STORAGE_SIZE` is rounded up to a whole number of 512B sectors, so the global
`usage_bitmap` array is sized somewhat larger than `TOTAL_DATA_SECTOR_SIZE` bits actually need —
there's a padding region above the last real data-sector index that's still safely inside the
array. These two tests characterise exactly where that safe region ends.

### FirstPaddingBitAfterLastDataSectorIsUsable
The first index past `TOTAL_DATA_SECTOR_SIZE - 1` — not a real sector, but still mechanically
usable since it's inside the padded array.

### LastAddressableArrayBitIsUsable
The very last bit the array can address (`USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT - 1`)
also round-trips correctly.

**Deliberately not tested here:** anything at or beyond that last index. `check_usage_bit()` and
`update_usage_bit()` do zero bounds checking on their `index` parameter — one past the last
addressable bit reads/writes past the end of the global array, which is undefined behaviour, not
a clean `false`/`STRG_FAIL`. See the recommended-tests notes for why this is a real latent bug
(not hypothetical — `journal_add()` hits the equivalent problem and was confirmed to segfault
during this review) and how you might pin it down with a death test instead of risking the whole
suite.

---

## Degenerate / undersized storage

### UpdateFailsWhenTargetSectorExceedsStorageCapacity
`update_usage_bit()` doesn't itself check that the target bitmap sector actually exists on the
given storage backend — it relies entirely on the underlying `write_block()`'s own
`index >= capacity_blocks` bounds check. This test confirms that protection is actually present:
a storage with only 1 total sector (nowhere near enough to reach `USAGE_BITMAP_START_SECTOR`)
correctly fails the write rather than writing out of range of *that* storage's backing memory.
Note this is a different (and much safer) kind of "out of range" than the RAM-array boundary
above — this one is caught by `HeapStorage`'s own bounds check, not left as UB.
