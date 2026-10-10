# `tests/test_journal_edge.cpp` — Review Notes

Failure-path and boundary coverage for [`journal.c`](../../source/app/src/journal.c),
complementing [test_journal.md](test_journal.md). 17 tests, all passing
(`ctest -R JournalEdgeTest`). Uses the shared [`FailableStorageCtx`](test_support.md) mock to make
a specific read or write fail.

Rewritten together with `test_journal.cpp` for the CRC-trailer journal (commit `a63e0f9`). The old
version's tests for `journal_write()` / `journal_*_read()` in isolation are gone (those paths are
now covered through `journal_add()` and `journal_rollback()`), as are `AddAcceptsSectorZero` and
`AddPreservesLargeInRangeSectorValue` — replaced by the `RollbackFirstDataSector` /
`RollbackLastDataSector` boundary tests. The `UINT16_MAX` out-of-range segfault that
`AddPreservesLargeInRangeSectorValue` documented is **still there**: `journal_add()` has no bounds
check on `index` before indexing the global bitmap (see README recommendation 1).

The call orders every test here relies on (also in comments in the source):

| Function | Reads | Writes |
|---|---|---|
| `journal_add()` | — | bitmap copy (1), content copy (2), header (3) |
| `journal_rollback()` | content copy (1), bitmap copy (2) | data sector (1), bitmap sector (2), commit header (3) |

`fail_read_in(n)` / `fail_write_in(n)` reset the counter and fail the n-th call from now, so the
`SetUp()` counter gotcha described in [test_support.md](test_support.md) doesn't apply here.

## Checklist

### `journal_header_init()`
- [ ] HeaderInitFailsWhenWriteFails

### `get_journal_status()`
- [ ] StatusReportsReadErrorWhenReadFails

### `journal_add()`
- [ ] AddFailsWhenBitmapWriteFails
- [ ] AddFailsWhenContentWriteFails
- [ ] AddFailsWhenHeaderWriteFails
- [ ] FailedAddNeverLeavesActiveHeader

### `journal_free()`
- [ ] FreeFailsWhenHeaderWriteFails

### `journal_rollback()`
- [ ] RollbackFailsWhenContentReadFails
- [ ] RollbackFailsWhenBitmapReadFails
- [ ] RollbackFailsWhenSectorWriteFails
- [ ] RollbackFailsWhenBitmapWriteFails
- [ ] RollbackFailsWhenCommitWriteFails

### `journal_init()`
- [ ] InitFailsWhenStatusReadFails
- [ ] InitFailsWhenUninitialisedHeaderWriteFails
- [ ] InitPropagatesRollbackFailure

### Boundaries
- [ ] RollbackFirstDataSector
- [ ] RollbackLastDataSector

---

## Fixture: `JournalEdgeTest`

Same setup as `JournalTest`. The extra helper is `make_pending_rollback(data_sector)`: journal a
`0xAB` sector, overwrite the data sector on the card with `0x99`, and call
`get_journal_status()` so the header is loaded into RAM — exactly the state `journal_init()` is in
just before it calls `journal_rollback()`. `stored_state()` reads the state field straight off the
card.

---

## `journal_header_init()`

### HeaderInitFailsWhenWriteFails
Failing write → returns false.

---

## `get_journal_status()`

### StatusReportsReadErrorWhenReadFails
Failing header read → `JRNL_READ_ERROR`, distinct from `JRNL_UNINITIALIZED` and
`JRNL_CORRUPTED`. A flaky read must not look like a blank card.

---

## `journal_add()`

### AddFailsWhenBitmapWriteFails / AddFailsWhenContentWriteFails / AddFailsWhenHeaderWriteFails
Failing write 1, 2 or 3 → `journal_add()` returns false. Each is a separate test so a regression
names the exact step.

### FailedAddNeverLeavesActiveHeader
Starting from an `EMPTY` journal, fails each of the three writes in turn and checks the header on
the card is still `EMPTY` after each. Same property as `WriteHeaderIsWrittenLast` in
[test_journal.md](test_journal.md) but through `journal_add()`.

---

## `journal_free()`

### FreeFailsWhenHeaderWriteFails
Commit write fails → `STRG_FAIL`, header still `ACTIVE`. The transaction will be rolled back on the
next boot, which is the safe outcome.

---

## `journal_rollback()`

Every failure must leave the journal `ACTIVE` so the next boot retries.

### RollbackFailsWhenContentReadFails
Content read fails → returns false, data sector still `0x99` (untouched), `ACTIVE`.

### RollbackFailsWhenBitmapReadFails
Bitmap read fails → returns false, `ACTIVE`. Both copies are read before anything is written.

### RollbackFailsWhenSectorWriteFails
Data sector write fails → returns false, data sector still `0x99`, `ACTIVE`.

### RollbackFailsWhenBitmapWriteFails
Bitmap write fails → returns false, `ACTIVE`.

### RollbackFailsWhenCommitWriteFails
Commit write fails → returns false. The data sector **is** restored (`0xAB`) but the journal is
still `ACTIVE`, so the next boot repeats the rollback — which is safe, as shown by
`InitRepeatsInterruptedRollback` in [test_journal.md](test_journal.md).

---

## `journal_init()`

### InitFailsWhenStatusReadFails
Header read fails → `journal_init()` returns false.

### InitFailsWhenUninitialisedHeaderWriteFails
Blank card, header write fails → returns false (doesn't claim the journal is ready).

### InitPropagatesRollbackFailure
Pending rollback, the data sector restore write fails → `journal_init()` returns false and the
journal is still `ACTIVE`.

---

## Boundaries

### RollbackFirstDataSector
Data sector 0 round-trips through add and rollback, and the raw sector just before the data region
(the last bitmap sector) isn't written by the data restore. Catches an off-by-one in
`DATA_SECTOR_TO_RAW`.

### RollbackLastDataSector
Data sector `TOTAL_DATA_SECTOR_SIZE - 1`: the RAM header holds the full index (no truncation),
rollback restores it, and the last bitmap sector on the card is CRC-valid afterwards.
