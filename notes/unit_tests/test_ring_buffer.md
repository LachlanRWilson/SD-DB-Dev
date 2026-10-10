# `tests/test_ring_buffer.cpp` — Review Notes

Covers the generic SD-backed ring buffer in [`ring_buffer.c`](../../source/app/src/ring_buffer.c)
— the structure that message/call history sits on. Only `size`, `current_index`, `occupancy` and
`seq` live in RAM; everything else is on the card, and on boot the RAM state is rebuilt by
binary-searching the region for the sector with the highest sequence number (the head).
33 tests across three suites, all passing (`ctest -R RingBuffer`).

| Suite | Tests | What it covers |
|---|---|---|
| `RingBufferIteratorTest` | 12 | `ring_buffer_iterator_init()` / `ring_iterator_next/prev/get()` |
| `RingBufferStateTest` | 6 | RAM cursor helpers: `move_next/prev_ring_buffer()`, `add_ring_buffer()`, `inc_seq_ring_buffer()` |
| `RingBufferReconstructTest` | 15 | `reconstruct_ring_buffer()`, `binary_search_head()`, `is_ring_buffer_wrapped()`, `init_ring_buffer()` |

Two things worth knowing before reading the iterator/state tests:

- **The iterator range is `[0, occupancy - 1]`.** With 4 occupied slots the iterator visits
  0..3 and never an unwritten slot. On a *full* buffer the top is `size - 1`, the last sector of
  the region. `NextWrapsAtUpperLimit` and `PrevWrapsAtLowerLimit` pin both ends (they expect `3`
  with `occupancy == 4`).
- **`move_next` and `move_prev` wrap at different points.** `move_next_ring_buffer()` wraps at
  `occupancy - 1`, but `move_prev_ring_buffer()` wraps to `size - 1`. They agree only when the
  buffer is full; on a partially-filled buffer, `prev` from slot 0 lands on an unwritten slot.
  Only the full-buffer case is tested (`MovePrevWrapsToLastSlotWhenFull`).

## Checklist

### `RingBufferIteratorTest`
- [ ] GetReturnsRingBuffersCurrentIndex
- [ ] NextWalksSequentiallyThroughOccupiedRange
- [ ] NextWrapsAtUpperLimit
- [ ] PrevWrapsAtLowerLimit
- [ ] IteratorCursorIsIndependentOfRingBufferCursor
- [ ] RejectsNullArguments
- [ ] PrevWalksSequentiallyThroughOccupiedRange
- [ ] NextFullCycleReturnsToStart
- [ ] NextThenPrevReturnsToSameIndex
- [ ] EmptyBufferStaysAtIndexZero
- [ ] GetFailsWhenStartIsOutsideOccupiedRange
- [ ] MultipleIteratorsAreIndependent

### `RingBufferStateTest`
- [ ] MoveNextWrapsAtSizeWhenFull
- [ ] AddSequenceStaysInRangeAndIsSequential
- [ ] MovePrevWrapsToLastSlotWhenFull
- [ ] AddCapsOccupancyAtSize
- [ ] IncSeqRollsOverAtUint32Max
- [ ] RejectsNull

### `RingBufferReconstructTest`
- [ ] EmptyRegion
- [ ] SingleSector
- [ ] PartialFillNotWrapped
- [ ] ExactlyFullHeadAtLastSector
- [ ] WrappedBuffer
- [ ] WrappedHeadAtSectorZero
- [ ] SeqRollsOverInsideCurrentLap
- [ ] MatchesWriterForEveryWriteCount
- [ ] CorruptSectorFails
- [ ] ReadFailureFails
- [ ] BinarySearchHeadDirect
- [ ] IsWrappedDirect
- [ ] InitOnEmptyRegionResetsAllFields
- [ ] InitReconstructsFromStorage
- [ ] InitRejectsZeroSize

---

## Helpers and fixtures

### `set_ring_buffer()`
A file-local helper that pokes `size`/`current_index` directly and zeroes `occupancy`/`seq`.
It exists because `init_ring_buffer()` now reconstructs from the SD card, so the iterator and
state tests (which never touch storage) can't use it. Always returns `true` so it can sit inside
`ASSERT_TRUE(...)` like the old init call did.

### `RingBufferIteratorTest`
Just a `RingBuffer` plus `fill(n)`, which sets `occupancy` without writing anything — the
iterator only ever looks at `occupancy` and `current_index`.

### `RingBufferStateTest`
No fixture — plain `TEST()`s on a stack `RingBuffer`.

### `RingBufferReconstructTest`
The interesting one. Uses [`FailableStorageCtx`](test_support.md) over a small simulated card:

- The ring region starts at raw sector `kStart = 4` and is `kSectors = 15` long. 15 is
  deliberately not a power of two, so the binary search's midpoint arithmetic gets exercised on
  uneven halves.
- `SetUp()` "formats" the region: every sector gets a CRC-valid header with `state = RB_EMPTY`.
- **Guard sector:** the sector straight after the region (`kGuard`) is written as `RB_OCCUPIED`
  with seq `0xDEADBEEF`. Any read past the end of the region (e.g. checking `head + 1` when the
  head is the last sector) sees an occupied sector and reports a wrap that never happened, so an
  off-by-one in the bounds shows up as a wrong `occupancy` instead of silently passing.
- **Read budget:** `reconstruct()` arms `fail_after_read = 32` before every call. A binary search
  over 15 sectors needs roughly `log2(15) + 3` reads; if the search stops converging, the 32nd
  read fails, the search returns an error and the test fails instead of hanging.
  `expect_state()` additionally asserts `read_calls < kReadBudget`.
- `write_seqs({...})` writes `seqs[i]` to sector `i` and marks it occupied; `simulate_writes(n,
  seq0)` writes `n` sequential sectors from `seq0`, wrapping round the region the way the real
  writer does.

Assumed on-disk semantics (documented at the top of the fixture): seq goes up by exactly one per
sector modulo 2^32, every written sector is `RB_OCCUPIED`, and reconstruct reports index and
occupancy in sectors.

---

## `RingBufferIteratorTest`

### GetReturnsRingBuffersCurrentIndex
A new iterator starts on the ring buffer's own `current_index` (here 2), not at 0.

### NextWalksSequentiallyThroughOccupiedRange
From 0 with occupancy 4, three `next()` calls yield 1, 2, 3 — the last occupied slot.

### NextWrapsAtUpperLimit
Walks to 3 (`occupancy - 1`), then one more `next()` wraps to 0. Asserts the iterator never
reaches index `occupancy`, which would be one past the last written slot.

### PrevWrapsAtLowerLimit
`prev()` from 0 wraps to `occupancy - 1` (3), the mirror of the test above.

### IteratorCursorIsIndependentOfRingBufferCursor
Advances the iterator to 2, then calls `move_next_ring_buffer()` three times on the ring buffer
itself; the iterator still reads 2. Confirms the iterator copies `current_index` into its own
context at init rather than holding a pointer to it.

### RejectsNullArguments
`ring_iterator_next/prev/get(nullptr, ...)` return false, and `get()` with a NULL output pointer
returns false — no crashes.

### PrevWalksSequentiallyThroughOccupiedRange
Starting at 3, three `prev()` calls yield 2, 1, 0.

### NextFullCycleReturnsToStart
For every start index in `[0, 4]` with occupancy 5, stepping `next()` `occupancy` times returns
to the start: the cycle length equals the number of occupied slots.

### NextThenPrevReturnsToSameIndex
`next()` then `prev()` is a no-op at six consecutive positions, which includes crossing the wrap
point.

### EmptyBufferStaysAtIndexZero
Occupancy 0 means the range is `[0, 0]`, so both `next()` and `prev()` stay on 0 (and `get()`
succeeds — the "empty" iterator still reports a position).

### GetFailsWhenStartIsOutsideOccupiedRange
If `current_index` (6) is outside `[0, occupancy - 1]` (0..3), `get()` fails. One `next()` brings it back
to 0, since anything not `< upper_lim` wraps to `lower_lim`.

### MultipleIteratorsAreIndependent
Two iterators over the same buffer, each with its own `RBIteratorCtx`, move independently (one
forward twice to 3, the other back once to 0).

---

## `RingBufferStateTest`

### MoveNextWrapsAtSizeWhenFull
On a full buffer (`occupancy == size == 8`), `move_next` from slot 7 goes to 0. On a full buffer
`occupancy - 1 == size - 1`, so this doesn't distinguish the two wrap points.

### AddSequenceStaysInRangeAndIsSequential
Replays the `add_ring_buffer()` + `move_next_ring_buffer()` sequence used by
`message_history_add()` for three laps of an 8-slot buffer, checking every step that the index is
`< size`, equals `n % size`, and that occupancy grows to `size` and stays there. This is the test
that actually covers the partially-full `move_next` behaviour (wrapping at `occupancy - 1` while
occupancy is still growing).

### MovePrevWrapsToLastSlotWhenFull
`move_prev` from 0 on a full buffer goes to 7. See the asymmetry note at the top for why only the
full case is safe to assert.

### AddCapsOccupancyAtSize
Ten `add_ring_buffer()` calls on a size-4 buffer leave occupancy at 4.

### IncSeqRollsOverAtUint32Max
`UINT32_MAX - 1 → UINT32_MAX → 0 → 1`. The rollover matters because reconstruction compares
sequence numbers and has to cope with the wrap (see `SeqRollsOverInsideCurrentLap`).

### RejectsNull
Every RAM helper, plus `init_ring_buffer()`, returns false on NULL.

---

## `RingBufferReconstructTest`

### EmptyRegion
A freshly formatted region reconstructs to index 0, occupancy 0.

### SingleSector
`[1]` → head 0, occupancy 1, seq 1.

### PartialFillNotWrapped
`[1..6]` in a 15-sector region → head 5, occupancy 6, seq 6. Exercises the
`is_ring_buffer_wrapped()` "next sector is empty" branch.

### ExactlyFullHeadAtLastSector
`[1..15]` → head 14, occupancy 15. The head is the last sector, so `reconstruct_ring_buffer()`
must take its `head == size - 1` shortcut and not read `head + 1` — which would be the guard
sector, and wouldn't break anything here but would in a real layout where the next region starts.

### WrappedBuffer
The worked example `[15..19, 5..14]` → head 4 (seq 19), full. The canonical "wrapped once" case.

### WrappedHeadAtSectorZero
`[16, 2..15]` → head 0. The binary search's target is at the very left edge.

### SeqRollsOverInsideCurrentLap
Sequence numbers roll over `0xFFFFFFFF → 0` partway through the current lap; head is sector 6
with seq 3. Catches any comparison that treats a seq as "bigger" by plain `>` rather than
relative to the sequence at sector 0.

### MatchesWriterForEveryWriteCount
The exhaustive one. For every write count `n` from 1 to 45 (three laps), and from two starting
seqs (`1` and `0xFFFFFFF0`, so the rollover lands at every position across the laps), it
re-runs `SetUp()`, simulates `n` writes and checks the reconstructed head, occupancy and seq
match what the writer would have. If this passes, the hand-picked cases above are mostly there
as readable worked examples.

### CorruptSectorFails
Flips a payload byte in *every* sector (because the search only probes some of them, and which
ones depends on the layout) and checks reconstruction fails. A bad CRC must be an error, never
treated as "empty" — that would silently truncate the history.

### ReadFailureFails
First read fails → reconstruction fails. Calls `reconstruct_ring_buffer()` directly rather than
the `reconstruct()` helper, because the helper would re-arm the read budget and overwrite the
`fail_after_read = 1` setting.

### BinarySearchHeadDirect
`binary_search_head()` on its own: `STRG_EMPTY` for a formatted region; `STRG_OK`, head 4, seq 19
for the worked example.

### IsWrappedDirect
`is_ring_buffer_wrapped()` on `[1, 2, 3]`: head 2 → next sector empty → `STRG_EMPTY`; head 1 →
next sector occupied → `STRG_OK`; read failure → `STRG_FAIL`.

### InitOnEmptyRegionResetsAllFields
Fills the `RingBuffer` with `0xAB` garbage first, then `init_ring_buffer()` on an empty region
sets size and zeroes index, occupancy and seq — i.e. the empty path writes every field, not just
some.

### InitReconstructsFromStorage
Same garbage-fill, then `init_ring_buffer()` over the worked example sets `size = 15` and
reconstructs head 4, occupancy 15, seq 19.

### InitRejectsZeroSize
`size == 0` returns false (it would otherwise make `size - 1` underflow in the reconstruct
logic).
