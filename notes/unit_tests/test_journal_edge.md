# `tests/test_journal_edge.cpp` — Review Notes

Failure-path and boundary coverage for [`journal.c`](../../source/app/src/journal.c),
complementing the happy-path/CRC-corruption coverage in [test_journal.md](test_journal.md). 19
tests, all passing (`ctest -R JournalEdgeTest`). Uses the shared
[`FailableStorageCtx`](test_support.md) mock — **read that file first**, since several tests
here reset a call counter mid-test for reasons explained there, and it'll look like unexplained
bookkeeping otherwise.

This file replaces an earlier `test_journal_edge.cpp` that was written against an older version
of the `journal.c` API (`journal_add()` used to take a pre-built `JournalHeaderDataB` plus a
separate usage-bitmap buffer; the current API takes just a type and sector index and derives the
bitmap backup from the global bitmap itself) and — more immediately disqualifying — was saved
wrapped in a stray ` ```cpp ` / ` ``` ` markdown fence, so it was never valid C++ and never
compiled. Nothing from it was reusable directly, though its "mock storage with fail_read/
fail_write flags" idea is exactly what `FailableStorageCtx` generalizes.

## Checklist

- [ ] HeaderInitFailsWhenWriteFails
- [ ] StatusReportsReadErrorWhenReadFails
- [ ] AddFailsWhenHeaderWriteFails
- [ ] AddFailsWhenContentWriteFails
- [ ] AddFailsWhenUsageBitmapWriteFails
- [ ] WriteFailsWhenHeaderWriteFails
- [ ] WriteFailsWhenContentWriteFails
- [ ] HeaderReadFailsWhenStorageReadFails
- [ ] ContentReadFailsWhenStorageReadFails
- [ ] UsageReadFailsWhenStorageReadFails
- [ ] RollbackFailsWhenContentReadFails
- [ ] RollbackFailsAndStaysActiveWhenSectorWriteFails
- [ ] RollbackFailsWhenUsageBitmapWriteFails
- [ ] FreeFailsWhenHeaderWriteFails
- [ ] InitFailsWhenStatusReadFails
- [ ] InitFailsWhenUninitialisedHeaderWriteFails
- [ ] InitPropagatesRollbackFailure
- [ ] AddAcceptsSectorZero
- [ ] AddPreservesLargeInRangeSectorValue

---

## `journal_header_init()`

### HeaderInitFailsWhenWriteFails
A failing `write_block()` makes `journal_header_init()` return `false` rather than reporting a
header was written when it wasn't.

---

## `get_journal_status()`

### StatusReportsReadErrorWhenReadFails
A failing header read is reported as `JRNL_READ_ERROR`, distinct from `JRNL_UNINITIALIZED`
(no magic number) or `JRNL_CORRUPTED` (bad CRC) — the three "something's wrong" states are
distinguishable, not collapsed into one.

---

## `journal_add()`

Three tests, one per write `journal_write()` performs internally (header, then content, then the
usage-bitmap backup — confirmed by reading `journal_write()`'s source, not assumed):

### AddFailsWhenHeaderWriteFails
Failing the 1st write makes `journal_add()` return `false`.

### AddFailsWhenContentWriteFails
Failing the 2nd write also fails the call — but note what's checked afterwards: **the header
write from step 1 already landed on storage** (still `JRNL_MAGIC`/`JRNL_ACTIVE`), since
`journal_write()` doesn't undo earlier writes when a later one fails. `journal_add()` itself has
no rollback-on-partial-failure logic; if storage really did fail partway through in the field,
you'd be left with a valid-looking active journal header pointing at content that was never
actually updated. Whether that's acceptable depends on what the *caller* does with a `false`
return here — worth checking that every `journal_add()` call site actually stops and doesn't
proceed to modify the real target sector after getting `false` back.

### AddFailsWhenUsageBitmapWriteFails
Failing the 3rd write also fails the call (same "no rollback of steps 1-2" characteristic as
above, not re-asserted a second time).

---

## `journal_write()`

The lower-level primitive `journal_add()` sits on top of. Two tests here (header write failing,
content write failing) are enough to confirm the primitive itself propagates failure correctly —
`journal_add()`'s three tests above already establish it's really `journal_write()` doing the
work.

### WriteFailsWhenHeaderWriteFails / WriteFailsWhenContentWriteFails
Both return `STRG_FAIL` (not a `bool`, note — `journal_write()`'s return type is `STRG_RET`,
where `STRG_FAIL == 0`, so `EXPECT_EQ(..., STRG_FAIL)` is doing real enum comparison here, not
just truthiness).

---

## `journal_header_read()` / `journal_content_read()` / `journal_usage_read()`

### HeaderReadFailsWhenStorageReadFails / ContentReadFailsWhenStorageReadFails / UsageReadFailsWhenStorageReadFails
Each of the three read primitives propagates a storage read failure as `false`. Straightforward,
but worth having explicitly since `journal_rollback()` depends on both `journal_content_read()`
and `journal_usage_read()` succeeding before it even looks at CRCs — see
`RollbackFailsWhenContentReadFails` below.

---

## `journal_rollback()` failure paths

`test_journal.cpp` already covers CRC-mismatch rejection (rollback successfully *reads* bad data
and correctly refuses to apply it). These three cover the read/write failures underneath that —
scenarios where the read or write itself doesn't succeed at all.

### RollbackFailsWhenContentReadFails
If `journal_content_read()`'s underlying read fails, rollback must fail before it ever gets to
CRC checking or writing anything back.

### RollbackFailsAndStaysActiveWhenSectorWriteFails
Journals a real sector via `journal_add()`, confirms the status is `JRNL_ROLLBACK`, then fails
the *first* write rollback performs (restoring the target data sector) and checks:
- `journal_rollback()` returns `false`,
- the in-RAM `journal.header` state is still `JRNL_ACTIVE` — i.e. `journal_free()` (which would
  mark it `JRNL_COMMITTED`) is never reached, so a subsequent boot will correctly attempt the
  rollback again rather than treating this failed attempt as done.

### RollbackFailsWhenUsageBitmapWriteFails
Same shape, but lets the first write (target sector restore) succeed and fails the *second*
(usage-bitmap sector restore) instead — checks the second write's failure is caught
independently of the first one's success, and that the state still ends up `JRNL_ACTIVE` rather
than committed on a half-completed rollback.

---

## `journal_free()`

### FreeFailsWhenHeaderWriteFails
**Characterisation test.** `journal_free()` sets `journal->header.var.data.var.state =
JRNL_COMMITTED` (and recomputes the header CRC) in RAM *before* attempting the write. If that
write fails, the in-RAM struct says `COMMITTED` while storage still says `ACTIVE` — checked
explicitly here by reading both. This mirrors the same RAM-before-storage ordering documented for
`update_usage_bit()` in [test_usage_bitmap_edge.md](test_usage_bitmap_edge.md); it's a pattern
that shows up more than once in this codebase and is worth being aware of as a pattern, not just
a one-off.

---

## `journal_init()` failure paths not covered by `test_journal.cpp`

### InitFailsWhenStatusReadFails
If `get_journal_status()`'s own header read fails, `journal_init()` propagates that failure
rather than treating a read error the same as "uninitialized."

### InitFailsWhenUninitialisedHeaderWriteFails
Starting from a genuinely uninitialized (zeroed) journal, `journal_init()` calls
`journal_header_init()` — if *that* write fails, `journal_init()` must fail too rather than
reporting success with no header actually written.

### InitPropagatesRollbackFailure
Journals a sector, then simulates a fresh boot (`journal_init()` on a brand-new `Journal`
struct) with the rollback's sector-restore write set to fail. `journal_init()` must propagate
that failure rather than reporting a successful init when the required rollback didn't actually
complete.

---

## Boundary sector values

### AddAcceptsSectorZero
Sector `0` round-trips through the header correctly — not a special-cased or accidentally-treated-as-"unset" value.

### AddPreservesLargeInRangeSectorValue
**Read this one even if you skip everything else in this file.** It's *not* testing
`UINT16_MAX` — an earlier version of this test did, and it segfaulted the entire test binary.
`journal_add()` computes `USAGE_BITMAP_FIND_SECTOR(index) * ELEMENTS_PER_SECTOR` to locate which
512-byte slice of the **global** `usage_bitmap` array to back up, with no check that this lands
inside the array's real size (`USAGE_BITMAP_STORAGE_SIZE`, currently 1024 `uint32_t` elements).
Any `index` at or beyond `USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT` (32768 for the current
`mem_layout.h` — only slightly above `TOTAL_DATA_SECTOR_SIZE`, ~30969) makes `journal_add()`
compute a CRC over memory outside that array. `UINT16_MAX` (65535) is well past that boundary.
This test instead uses the *largest legitimate* sector index (`TOTAL_DATA_SECTOR_SIZE - 1`) to
confirm the header round-trip works right up to the real boundary, without going over it. The
`UINT16_MAX` case is a genuine, confirmed (crashed on my machine) latent bug in `journal_add()`/
`journal_rollback()` — not a test gap — and is called out again in the recommended-tests notes
with a suggested (safer) way to pin it down.
