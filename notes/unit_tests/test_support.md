# `tests/test_support/failable_storage.h` — Shared Test Helper

Not a test file itself — a small shared header used by
[test_usage_bitmap_edge.md](test_usage_bitmap_edge.md), [test_journal_edge.md](test_journal_edge.md),
and [test_hash_table_edge.md](test_hash_table_edge.md) to inject storage read/write failures that
real `HeapStorage` can't produce (it only ever fails on out-of-range access — never a "the SD
card returned an error" scenario).

## What it does

Wraps a real `HeapStorageContext` and delegates every call straight through to the real
`HeapStorage_*` functions — so by default it behaves *exactly* like plain heap storage. On top of
that, it counts read and write calls separately and lets a test say "make the Nth read/write
fail" via two counters on `FailableStorageCtx`:

```cpp
FailableStorageCtx ctx{};
ASSERT_TRUE(FailableStorage_Init(&ctx, memory, SECTOR_SIZE, sector_count));
Storage storage = FailableStorage_Make(&ctx);

ctx.fail_after_write = 2; // the 2nd write_block/write_multiblock call fails; all others succeed
ctx.fail_after_read = -1; // -1 (the default) means "never fail"
```

The counters (`read_calls`, `write_calls`) are 1-based and shared across `read_block`/
`read_multiblock` (and `write_block`/`write_multiblock` respectively) — i.e. it counts *storage
operations in that direction*, not calls to a specific function. `fail_after_write`/
`fail_after_read` compare against these counters and reset to "always allow" once you set a new
value — there's no auto-reset after triggering once, so if a test needs to fail once and then let
everything else through, it should set the counter back to `-1` afterwards (see
`InsertContactRollsBackHashStateOnWriteFailure` in `test_hash_table_edge.cpp` for an example).

## The gotcha every one of these edge suites hit at least once

**Fixture `SetUp()` often performs its own reads/writes before the test body runs** —
most commonly `journal_init()` on a freshly-zeroed journal sector, which calls
`journal_header_init()` and performs one write. If a test sets `ctx.fail_after_write = 1`
expecting to target *its own* first write, but `SetUp()` already consumed call #1, the test's
write becomes call #2 and the injected failure never fires — the operation under test silently
succeeds and the test fails with "Expected: false, Actual: true" style output that has nothing
to do with the code being tested.

The fix used throughout these three edge suites: reset the relevant counter to `0` immediately
before setting `fail_after_write`/`fail_after_read`, with a comment noting what already consumed
calls before that point:

```cpp
ASSERT_TRUE(journal_add(&journal, JRNL_CONTACT, target_sector, content)); // 3 writes
ASSERT_EQ(get_journal_status(&journal), JRNL_ROLLBACK);                   // 1 read

ctx.write_calls = 0; // start counting fresh for the write under test
ctx.fail_after_write = 1;
```

When reviewing any test that uses `fail_after_read`/`fail_after_write`, it's worth manually
counting the storage calls made between `SetUp()` and the fail-flag assignment to confirm the
targeted call number is actually correct — this class of off-by-one was the single most common
mistake made while writing these suites, and it fails loudly (a real assertion failure) rather
than silently, but it's easy to mis-diagnose as a source bug on first read.

## Why it's not just `heap_storage` with a bool flag

An earlier, since-discarded attempt at this pattern (visible in this project's git history as a
now-replaced `test_journal_edge.cpp`) used simple `fail_read`/`fail_write` booleans that failed
*every* subsequent call once set. That makes it impossible to test "the 2nd write in a
multi-write sequence fails, but the 1st succeeds" — exactly the shape needed to test, say,
`journal_rollback()`'s two sequential restore writes independently, or `journal_write()`'s
header-then-content-then-bitmap sequence. The counter-based approach trades a small amount of
setup-order bookkeeping (the gotcha above) for that precision.
