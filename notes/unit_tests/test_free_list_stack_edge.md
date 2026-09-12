# `tests/test_free_list_stack_edge.cpp` — Review Notes

Edge cases for [`free_list_stack.c`](../../source/app/src/free_list_stack.c), complementing the
happy-path coverage in [test_free_list_stack.md](test_free_list_stack.md). 20 tests, all
passing (`ctest -R FreeListEdgeTest`). No storage/journal dependency — this module is pure
in-memory bookkeeping, so there's no failure-injection needed here (contrast with the other
three edge suites).

## Checklist

- [ ] InitRejectsZeroCapacity
- [ ] EmptyInitRejectsZeroCapacity
- [ ] EmptyInitRejectsNullPool
- [ ] EmptyInitRejectsNullSelf
- [ ] InitSoftwareRejectsZeroCapacity
- [ ] SingleSlotAllocatorAllocatesThenExhausts
- [ ] AllocateOnNullSelfReturnsMax
- [ ] AllocateOnZeroInitialisedAllocatorReturnsMax
- [ ] FreeOnFullyFreeAllocatorDoesNotOvergrowOrUnderflow
- [ ] DoubleFreeDoesNotDuplicateSector
- [ ] FreeRangeEmptyRangeIsNoOp
- [ ] FreeRangeInvertedRangeIsNoOp
- [ ] FreeRangeClampsAtCapacity
- [ ] FreeRangePartialOverlapFreesOnlyInRange
- [ ] FreeSectorRangeTerminatesAndFreesExpectedSlotCount
- [ ] FreeSectorRangeFreesExactSlotWindow
- [ ] FreeSectorRangeEmptyRangeIsNoOp
- [ ] ResetOnNeverInitialisedAllocatorIsSafeNoOp
- [ ] ResetAfterFullExhaustionRestoresEverything
- [ ] InterleavedAllocateFreeStaysLifo

---

## `free_list_init()` / `free_list_empty_init()` argument validation

### InitRejectsZeroCapacity / EmptyInitRejectsZeroCapacity
Both init functions reject `total_sectors == 0` — an allocator that starts with no capacity at
all would otherwise pass `self->capacity = 0` through with `free_stack` left pointing at a
zero-length buffer, and every later `i >= self->capacity` bounds check would immediately trip.

### EmptyInitRejectsNullPool / EmptyInitRejectsNullSelf
`free_list_empty_init()` explicitly checks both `self` and `free_stack` for `NULL` before
touching either. Notably, **`free_list_init()` (the normal, non-empty variant) does not have
this same `self`/`free_stack` NULL check** — only the zero-capacity check. That asymmetry isn't
exercised here (calling `free_list_init(nullptr, ...)` would crash rather than return `false`),
but it's worth knowing the two "same-shaped" init functions don't actually validate arguments
equally. See the recommended-tests notes for how to approach this without crashing the suite.

### InitSoftwareRejectsZeroCapacity
Same zero-capacity rejection for the `malloc`-backed variant used by the plain
`test_free_list_stack.cpp` fixture.

---

## Single-slot allocator

### SingleSlotAllocatorAllocatesThenExhausts
The smallest non-degenerate case: a 1-sector allocator gives out sector `0`, then reports
exhaustion (`UINT16_MAX`) on the second request, and correctly reuses `0` after it's freed.
Worth a dedicated test since off-by-one errors in `stack_top`/`capacity` comparisons often only
show up at extreme small sizes.

---

## `free_list_allocate()` on an unusable allocator

### AllocateOnNullSelfReturnsMax
`free_list_allocate(nullptr)` is explicitly checked (`self == NULL`) and returns `UINT16_MAX`
rather than dereferencing. Confirms the defensive check actually works, since (as noted above)
not every function in this file has the equivalent check.

### AllocateOnZeroInitialisedAllocatorReturnsMax
A `FreeList{}` that was zero-initialised but never passed through any `init` function has
`free_stack == NULL` — `free_list_allocate()` must catch this the same way as the `NULL self`
case, rather than trying to index through a null pointer.

---

## `free_list_free()` defensive behaviour

### FreeOnFullyFreeAllocatorDoesNotOvergrowOrUnderflow
Calling `free_list_free()` on an allocator where `stack_top` is already at `capacity` (nothing
allocated yet) must be a no-op — the `stack_top >= capacity` guard exists specifically to stop
this pushing a value past the end of the backing array. Also checks `used_count` doesn't go
negative (it's `size_t`, so "negative" would actually wrap to a huge number) — the
`if (used_count > 0)` guard in `free_list_free()` is what prevents that.

### DoubleFreeDoesNotDuplicateSector
Allocates one sector, frees it, then frees the *same* sector index again. The second free must
be rejected by the same `stack_top >= capacity` check (since after the first free, the stack is
back at full capacity) rather than pushing a duplicate entry that could later be handed out
twice by `free_list_allocate()`.

---

## `free_list_free_range()`

### FreeRangeEmptyRangeIsNoOp / FreeRangeInvertedRangeIsNoOp
`startIndex == endIndex` and `startIndex > endIndex` both free nothing — the underlying
`for (int i = startIndex; i < endIndex; i++)` loop simply never executes in either case. Worth
confirming by hand that this holds even though `startIndex`/`endIndex` are `uint16_t` and `i` is
`int` (no wraparound surprises at the sizes used here).

### FreeRangeClampsAtCapacity
`endIndex` far beyond `capacity` (100 vs. a 4-slot allocator) frees exactly `[0, capacity)` and
stops — the `if (i >= self->capacity) return;` bounds check inside the loop is what's being
verified here, since without it this would walk off the end of the backing array.

### FreeRangePartialOverlapFreesOnlyInRange
A range strictly inside a larger allocator (`[3, 7)` of a 10-slot allocator) frees exactly those
4 slots, leaving the rest marked used — checked via both `free_list_available()` and
`free_list_used()` to make sure the counters agree with each other.

---

## `free_list_free_sector_range()`

This is the function fixed earlier in this project's history: it used to contain
`for (int i = startSector; startSector < endSector; i++)` — a loop condition that checks
`startSector`, never updated inside the loop, instead of `i` — making it infinite whenever
`startSector < endSector`, combined with a body that ignored `i` entirely and just repeated the
same `free_list_free_range(self, startSector * CONTACT_SECTOR_CAPACITY, endSector *
CONTACT_SECTOR_CAPACITY)` call forever. It's now a single delegated call — no loop at all needed,
since converting a sector range to a slot range is one multiplication.

### FreeSectorRangeTerminatesAndFreesExpectedSlotCount
The most important test in this file, precisely because of the bug above: **if this test hangs
or times out, that is itself the failure signal** for a regression of the infinite loop — not a
wrong assertion value. Beyond that, it checks the freed slot *count* matches
`(endSector - startSector) * CONTACT_SECTOR_CAPACITY`.

### FreeSectorRangeFreesExactSlotWindow
A sharper check than the count alone: drains the allocator via `free_list_allocate()` in a loop
and confirms every single value handed out falls inside `[startSector * CAP, endSector * CAP)` —
i.e. the freed window is exactly right, not just the right size but shifted.

### FreeSectorRangeEmptyRangeIsNoOp
`startSector == endSector` frees nothing, mirroring `FreeRangeEmptyRangeIsNoOp` for the
sector-based wrapper.

---

## `free_list_reset()`

### ResetOnNeverInitialisedAllocatorIsSafeNoOp
`free_list_reset()` on a `FreeList{}` that was never initialised (`free_stack == NULL`) must not
dereference that null pointer — checked by calling it and then confirming
`free_list_allocate()` still correctly reports `UINT16_MAX` afterwards (not, say, a crash or a
bogus non-`UINT16_MAX` value from an untouched-but-garbage struct).

### ResetAfterFullExhaustionRestoresEverything
Exhausts a 5-slot allocator completely, resets it, and checks both counters snap back to the
initial state. (`test_free_list_stack.cpp`'s own `ResetRestoresAllSectors` does something
similar from a partial-allocation state; this one specifically starts from full exhaustion, the
more extreme end of the range.)

---

## LIFO ordering under interleaved use

### InterleavedAllocateFreeStaysLifo
Allocates two sectors, frees the *first* one allocated (not the second), then allocates again
and checks the reused value is the one just freed — not the other allocated-but-not-freed one,
and not a wrong slot from lower in the stack. A slightly more adversarial ordering than the
plain `FreeReusesSector` test in the main suite, which frees whichever it calls `a`.
