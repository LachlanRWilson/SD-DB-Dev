# `tests/test_free_list_stack.cpp` — Review Notes

Covers the `FreeList` sector allocator in
[`free_list_stack.c`](../../source/app/src/free_list_stack.c) — a simple LIFO (stack-based)
allocator used to hand out contact/message sector indices. 6 tests, all passing
(`ctest -R FreeListTest`).

## Checklist

- [X] AllocateSequentially
- [X] AllocationIsLIFO
- [X] FreeReusesSector
- [X] FullCycleAllocateFreeAllocate
- [X] CountersTrackCorrectly
- [X] ResetRestoresAllSectors

---

## Fixture: `FreeListTest`

A single 10-sector allocator (`free_list_init_software(allocator, 10)`), which `malloc`s its own
backing `free_stack` array internally (unlike `free_list_init()`, which takes a caller-supplied
pool — see the hash table tests). `TearDown()` only calls `free_list_reset()`, not a
`free()`/`delete`, so each test leaks its 20-byte backing array; harmless for a short-lived test
binary, but worth knowing if you ever run this under a leak-checker like ASan/valgrind and see
noise from here.

`free_list_init_software()` seeds the stack as `free_stack[i] = i` for `i` in `[0, 10)`, with
`stack_top` starting at `10`. Since `free_list_allocate()` pops from `--stack_top`, the *first*
allocation returns `9`, not `0` — i.e. allocation order is the reverse of the seeded array, which
is what makes the "descending order" and "LIFO" tests below meaningful rather than arbitrary.

---

## Tests

### AllocateSequentially
Allocates all 10 sectors in a loop and checks they come out in strictly descending order
(`9, 8, 7, ..., 0`), matching the seeding described above. Then confirms the 11th allocation
(now that the stack is empty) returns `UINT16_MAX` rather than an invalid/wrapped index.

### AllocationIsLIFO
A narrower version of the above: just the first two allocations, explicitly checked to be `9`
then `8`. This is really testing the same thing as `AllocateSequentially` but reads more like a
one-line spec of "it's LIFO" — worth checking whether you consider it redundant with the test
above or intentionally kept as a minimal smoke test.

### FreeReusesSector
Allocates two sectors (`a`, `b`), frees `a`, then allocates again and checks the new allocation
equals `a` — i.e. `free_list_free()` pushes back onto the *top* of the stack so it's the very
next thing handed out (LIFO reuse, not FIFO).

### FullCycleAllocateFreeAllocate
Exhausts the allocator (all 10), confirms the next allocation fails (`UINT16_MAX`), frees one
specific previously-allocated sector (`sectors[5]`, not necessarily numerically `5` — it's
whatever value that slot's allocation actually returned), and confirms the very next allocation
reuses exactly that value. A slightly more realistic "fill then reuse" scenario than
`FreeReusesSector`.

### CountersTrackCorrectly
Checks `free_list_used()` and `free_list_available()` both start correct (`0` used / `10`
available) and update correctly after two allocations (`2` used / `8` available). Pure
bookkeeping check — doesn't touch `free_list_free()` at all, so it won't catch a counter bug
that only shows up on the free path (see `ResetRestoresAllSectors` below for a test that does
exercise counters after a mix of allocation).

### ResetRestoresAllSectors
Allocates 5, then calls `free_list_reset()`, and checks both counters snap back to the initial
state (`0` used / `10` available) *and* that all 10 sectors can genuinely be allocated again
afterwards (not just that the counters look right — this actually drains the stack a second
time to prove it's really reset, not just cosmetically zeroed).
