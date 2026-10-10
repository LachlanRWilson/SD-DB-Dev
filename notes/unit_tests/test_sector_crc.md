# `tests/test_sector_crc.cpp` — Review Notes

Covers the per-sector CRC-32 trailer that every on-disk sector now carries
([`storage.c`](../../source/app/src/storage.c) `sector_crc_stamp()` / `sector_crc_valid()` /
`read_sector()` / `read_sector_raw()`), and checks each record type that sits on top of it
honours it: contacts, messages, the usage bitmap, the superheader, and contact reconstruction.
20 tests in one suite, `SectorCrcTest`, all passing (`ctest -R SectorCrcTest`).

The ground rules the tests pin down:

- `write_sector()` stamps the trailer, and stamps the **caller's buffer** too, so it matches what
  is on the card afterwards.
- `read_sector()` returns `STRG_CORRUPT` on a bad trailer. `read_sector_raw()` skips the check
  (for code that needs to look at a sector it expects might be blank, like the superheader and
  journal "is this card initialised?" checks).
- A never-written all-zero sector is **not** CRC-valid.
- Corruption is reported, never silently "fixed" by re-stamping garbage with a fresh CRC.
- Blank (`0x00` or `0xFF`) or foreign-format sectors are *uninitialised*; a formatted sector
  with a bad CRC is *corrupted*. Mixing those up would make a single bit flip look like a blank
  card, which triggers a format and wipes the database.

## Checklist

### CRC helpers
- [ ] StampDetectsEverySingleByteFlip
- [ ] ReadSectorReportsCorruption
- [ ] BlankSectorIsNotValid

### Contacts
- [ ] ContactSectorCorruptionDetected
- [ ] FirstContactInUnusedSectorSucceeds
- [ ] WriteContactRefusesCorruptSector

### Messages
- [ ] MessageSectorCorruptionDetected

### Usage bitmap
- [ ] BitmapUpdatesKeepTrailerValid
- [ ] BitmapCorruptionDetected
- [ ] BitmapWalkSkipsTrailer

### Superheader
- [ ] SuperheaderRoundTripIsGood
- [ ] SuperheaderCrcCoversAllData
- [ ] SuperheaderPaddingIsCrcProtected
- [ ] SuperheaderCorruptMagicIsNotUninitialised
- [ ] SuperheaderBlankZeroSectorIsUninitialised
- [ ] SuperheaderBlankErasedSectorIsUninitialised
- [ ] SuperheaderValidCrcWrongMagicIsUninitialised
- [ ] SuperheaderOtherVersionIsOutdated
- [ ] SuperheaderReadFailureIsFail

### Reconstruction
- [ ] ReconstructSkipsCorruptContactSector

---

## Fixture: `SectorCrcTest`

Full-layout heap storage (`DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE` sectors), with the
usage bitmap and journal initialised. Two helpers:

- **Write tracking.** `write_block`/`write_multiblock` are swapped for wrappers that record the
  raw index of the last write in `g_last_write_index`. Used by `MessageSectorCorruptionDetected`
  so it can corrupt "whatever sector that message went to" without hardcoding the message-index
  → raw-sector mapping (which was itself buggy until recently — see
  [test_db_recovery.md](test_db_recovery.md)).
- **`corrupt(raw_sector, byte = 100)`** flips one bit in the simulated card.

---

## CRC helpers

### StampDetectsEverySingleByteFlip
Stamps a patterned sector, then flips the top bit of **every** byte (payload and trailer) one at a
time and checks each flip is detected. CRC-32 is guaranteed to catch any single-bit error, so this
is really checking that the CRC covers the whole payload and that the trailer comparison is done
on all four bytes. Note this is still self-consistent (it uses the same `crc32_calculate()` to
stamp and check) — see the README's recommendation about known-vector CRC tests.

### ReadSectorReportsCorruption
Write → `read_sector()` is OK and the caller's buffer is now CRC-valid → corrupt → `read_sector()`
returns `STRG_CORRUPT` while `read_sector_raw()` still returns `STRG_OK`.

### BlankSectorIsNotValid
An all-zero sector fails the check. Important because the CRC of an all-zero payload is not zero, so a
blank card never accidentally looks like valid data.

---

## Contacts

### ContactSectorCorruptionDetected
Write and read back a contact, corrupt its sector, and `read_contact()` returns `STRG_CORRUPT`.

### FirstContactInUnusedSectorSucceeds
Writing into slot `3 * CONTACT_SECTOR_CAPACITY + 2` (an unused sector, confirmed via the usage
bit) succeeds. `write_contact()` is a read-modify-write, so if it read the blank sector through
the CRC check it would fail every first write — this pins that it uses the usage bit to skip the
read instead.

### WriteContactRefusesCorruptSector
Corrupt the sector holding contact 0, then try to write contact 1 into the same sector. The write
must fail, and the sector must still have a bad CRC afterwards — i.e. the code didn't read the
damaged sector, splice in contact 1 and re-stamp the whole thing as valid.

---

## Messages

### MessageSectorCorruptionDetected
Same as the contact version for `write_message_sector()` / `read_message_sector()`, using the
tracked last-write index to find the raw sector.

---

## Usage bitmap

### BitmapUpdatesKeepTrailerValid
Sets and clears bits in two different bitmap sectors, wipes the RAM bitmap, then
`read_usage_bitmap()` (which checks every sector's CRC) succeeds and the right bits come back.
Since the bitmap is updated a word at a time, this checks the trailer is re-stamped on every
update, not just on init.

### BitmapCorruptionDetected
Corrupt byte 0 of bitmap sector 1 → `read_usage_bitmap()` fails.

### BitmapWalkSkipsTrailer
Each bitmap sector now has fewer usable bits (`USAGE_BITS_PER_SECTOR`) because the last word is
the CRC. Sets the last usable bit of sector 0 and the first of sector 1, confirms the trailer word
in RAM between them is non-zero (it holds a CRC), and checks `get_next_bit()`/`get_prev_bit()`
step straight over it. Without this, the usage-bitmap iterator would report the CRC's set bits as
used data sectors.

---

## Superheader

`make_superheader()` builds one matching the compiled layout (magic, current version, data region
start/end).

### SuperheaderRoundTripIsGood
Write, check the caller's buffer was stamped, check the raw sector passes `read_sector()` (so the
superheader uses the standard trailer, not its own CRC scheme), then `superheader_check()` returns
`SUPR_GOOD` with byte-identical contents.

### SuperheaderCrcCoversAllData
Corrupting `db_end` (the last field of `SuperHeaderData`) is detected. Guards against a CRC length
that stops short of the last field.

### SuperheaderPaddingIsCrcProtected
Corrupting a byte inside the padding is detected too — the trailer covers the whole payload, not
just the struct fields.

### SuperheaderCorruptMagicIsNotUninitialised
A bit flip in the magic of a formatted card is `SUPR_CORRUPTED`, **not** `SUPR_UNINITIALISED`.
This is the most important test in the superheader group: getting it wrong means a single bit
error makes boot think the card is blank and format it.

### SuperheaderBlankZeroSectorIsUninitialised
All-zero sector → `SUPR_UNINITIALISED`.

### SuperheaderBlankErasedSectorIsUninitialised
All-`0xFF` (erased flash) sector → `SUPR_UNINITIALISED`.

### SuperheaderValidCrcWrongMagicIsUninitialised
A CRC-valid sector with someone else's magic → `SUPR_UNINITIALISED` (it's a different format, not
a damaged one of ours).

### SuperheaderOtherVersionIsOutdated
Valid sector, our magic, `version + 1` → `SUPR_OUTDATED`.

### SuperheaderReadFailureIsFail
`read_block` swapped for a function that always fails → `SUPR_FAIL`.

---

## Reconstruction

### ReconstructSkipsCorruptContactSector
Contact "Bad" in sector 0 (corrupted), contact "Good" in sector 1 (healthy). After
`hash_reconstruct_contact()` into a fresh table: the call still succeeds, "Good" is found at the
right slot, and "Bad" isn't occupied. Pins the design choice that one bad sector costs you the
contacts in it, not the whole database.
