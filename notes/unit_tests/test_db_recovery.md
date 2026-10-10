# `tests/test_db_recovery.cpp` — Review Notes

Cross-module tests for on-disk addressing, journal recovery after a power cut, and message
reconstruction across the whole database layout. Each group is a **regression** for a specific
bug fixed alongside the per-sector CRC work (commit `a63e0f9`), labelled "Bug 1/2/3" in the
source. 7 tests in one suite, `DbRecoveryTest`, all passing (`ctest -R DbRecoveryTest`).

## Checklist

### Bug 1: message sectors must live in the message region
- [ ] MessageSectorDoesNotOverlapContactRegion
- [ ] LastMessageSectorIsInsideDataRegion

### Bug 2: journal rollback must restore the journalled sector
- [ ] RollbackRestoresJournalledContactSector
- [ ] RollbackRestoresJournalledMessageSector
- [ ] FullMessageSectorLeavesJournalCommitted

### Bug 3: message reconstruction
- [ ] ReconstructMessageRestoresLatestSectors
- [ ] ReconstructMessageEmptyDatabase

---

## Fixture: `DbRecoveryTest`

Full-layout heap storage, bitmap and journal initialised. Two additions:

- **Power-cut simulation.** `write_block` is wrapped by `PowerCut_WriteBlock`: set
  `g_writes_left = n` and the first `n` writes succeed, then every write after that fails (unlike
  [`FailableStorageCtx`](test_support.md), which fails exactly one call). That models power
  dropping: nothing written after the cut reaches the card. Reset to `-1` (never fail) in
  `SetUp()`, `TearDown()` and `reboot()`.
- **`reboot()`** wipes the RAM bitmap and journal struct, re-reads the bitmap from the card and
  runs `journal_init()`, which performs any pending rollback — the same sequence as boot.
- `sector_is_blank(raw)` checks a raw sector is still all zeroes.

The power-cut tests count writes: a contact/message write is *journal (3 writes: bitmap copy,
content copy, header) + bitmap update (1) + data sector (1) + journal commit (1)*. Setting
`g_writes_left = 5` therefore lets everything through except the commit, leaving an ACTIVE
journal on the card.

---

## Bug 1: message sectors must live in the message region

### MessageSectorDoesNotOverlapContactRegion
**Regression for:** message sector 0 used to map to raw sector `MESSAGE_DATA_START_SECTOR`
without adding the data region offset, which put it inside the contact region. The test writes a
contact into exactly the contact sector that used to get clobbered, writes message sector 0, and
checks the contact is unchanged and that `DATA_SECTOR_TO_RAW(MESSAGE_DATA_START_SECTOR)` is no
longer blank.

### LastMessageSectorIsInsideDataRegion
Writes and reads back message sector `TOTAL_MESSAGE_SECTOR_SIZE - 1`. Heap storage fails
out-of-range accesses, so if the mapping pushed the last sector past the end of the data region
this would fail.

---

## Bug 2: journal rollback must restore the journalled sector

### RollbackRestoresJournalledContactSector
**Regression for:** rollback used to restore to a sector numbered by the contact *slot*, not the
contact *sector* that was journalled. Slots 7 and 8 share contact sector 1. Write Ada to slot 7,
then write Bob to slot 8 with the power cut before the journal commit. After `reboot()`:

- slot 7 still reads Ada,
- slot 8 reads `STRG_EMPTY` (the bitmap was rolled back too),
- the old wrong target (`CONTACT_DATA_START_SECTOR + 8`) is still blank.

### RollbackRestoresJournalledMessageSector
Same scenario for messages: message sector 4 holds one message, a second `write_message()` is cut
before commit. After reboot the sector holds exactly the first message (`msg_count == 1`) and its
usage bit is still set.

### FullMessageSectorLeavesJournalCommitted
Fill a message sector to `MESSAGE_BLOCK_CAPACITY`, then one more `write_message()` returns
`STRG_FULL` and the journal is still `JRNL_VALID`. **Regression for:** the full check used to
happen after `journal_add()`, leaving an ACTIVE journal behind for a write that never happened —
the next boot would then roll back a perfectly good sector.

---

## Bug 3: message reconstruction

### ReconstructMessageRestoresLatestSectors
The first real test of `hash_reconstruct_message()` (previously listed as untested in the
README). Three contacts: one with `MESSAGE_BLOCK_CAPACITY + 1` messages (two message sectors), one
with one message, one with none — 3 message sectors used. It records each contact's
`latest_msg_extent`, then rebuilds a fresh table from an *empty* allocator after `reboot()` and
checks:

- every contact is occupied with the same `latest_msg_extent`,
- the rebuilt message allocator has exactly 3 used and the rest available,
- a fresh allocation never returns a sector whose usage bit is set,
- a new message for the one-message contact lands in its existing sector (`msg_count` becomes 2).

### ReconstructMessageEmptyDatabase
Empty card → contact and message reconstruction both succeed and the message allocator is fully
free.
