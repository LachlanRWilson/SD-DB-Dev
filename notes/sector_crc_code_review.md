# Code Review: Sector CRC Protection + Storage Recovery Fixes

Branch with the committed version: `sector-crc-and-recovery-fixes` (`99c7639`).
The same changes are applied uncommitted on `main`, so `git diff` shows everything.

**Breaking change: the card must be reformatted.** The usage bitmap layout changed, every
sector now has a CRC trailer, and message sectors have moved.

Suggested review order: 1 → 2 → 3 → 4 → 5 → 6. Each section lists what changed, why, and
what to check.

---

## 1. CRC trailer convention

**Rule:** every 512 B data sector is laid out as `[ payload: bytes 0..507 | CRC-32: bytes 508..511 ]`.
The CRC covers the payload only.

| File | Change |
|---|---|
| `source/app/include/mem_layout.h:18` | `SECTOR_CRC_BYTES`, `SECTOR_PAYLOAD_BYTES` |
| `source/app/src/storage.c:11` | `sector_crc_stamp()` calculates the CRC and writes the trailer, using `memcpy` because the trailer may not be aligned |
| `source/app/src/storage.c:25` | `sector_crc_valid()` |
| `source/app/src/storage.c:41` | `read_sector_raw()` reads without checking the CRC |
| `source/app/src/storage.c:60` | `read_sector()` reads, then returns **`STRG_CORRUPT`** if the CRC doesn't match |
| `source/app/src/storage.c:85` | `write_sector()` stamps the CRC, then writes. **It changes the caller's buffer.** |
| `source/app/include/storage.h` | `STRG_CORRUPT` is added at the end of `STRG_RET` |

**Why do it here:** contacts and messages already go through `read_sector`/`write_sector`.
Putting the CRC in those two functions protects every record type, and the record code
doesn't have to know about CRCs.

**Check:**
- [ ] `STRG_CORRUPT` is non-zero, so any `if (!read_xxx(...))` check would treat it as
      success. See section 4 for the call sites I fixed. Grep for any others.
- [ ] I only used `read_sector_raw` in tests. The journal still uses `read_block` directly.

---

## 2. Sector layouts (capacities unchanged)

| Struct | Change | Capacity |
|---|---|---|
| `ContactSector` (`contact.h:58`) | `uint32_t crc` added last. Padding goes from 24 to 20 bytes | 6 → 6 |
| `MessageSector` (`message.h:82`) | `uint32_t crc` added last. Padding goes from 158 to 154 bytes. `MESSAGE_BLOCK_PADDING` now has parentheses | 2 → 2 |
| `MessageHistorySector` | `sector_crc` was already last. The capacity macro now uses `SECTOR_PAYLOAD_BYTES` | 254 → 254 |

Each struct now has `STATIC_ASSERT(offsetof(X, crc) == SECTOR_PAYLOAD_BYTES)` plus a
`STATIC_ASSERT` that pins the capacity. If a later struct change moves the CRC or changes
a capacity, the build fails.

**Check:**
- [ ] Adding a `uint32_t` member makes `ContactSector` 4-byte aligned. Offset 508 is
      divisible by 4, so no hidden padding is added (the asserts confirm this).

---

## 3. Usage bitmap CRC

The last word of each bitmap sector (word 127) is now its CRC, so each sector holds
**127 words = 4064 bits** instead of 4096. There are 30,969 data sectors, which need
968 words; 8 × 127 = 1016, so the bitmap is still **8 sectors**.

`mem_layout.h:144–180`:
- `USAGE_WORDS_PER_SECTOR` and `USAGE_BITS_PER_SECTOR`
- `USAGE_BITMAP_FIND_SECTOR` / `FIND_ELEMENT` now use 4064 bits per sector.
- `USAGE_BITMAP_FIND_INDEX` is now the **index into the flat RAM array**:
  `sector * 128 + element`. It used to be `index / 32`, which didn't match `FIND_SECTOR`.
- New `USAGE_BITMAP_WORD_TO_INDEX(word)`, the reverse of `FIND_INDEX`.
- New `USAGE_BITMAP_IS_CRC_WORD(word)`.
- New `USAGE_BITMAP_TOTAL_BITS`.

`usage_bitmap.c`:
- `init_usage_bitmap` (`:29`) stamps the CRC on every sector before writing.
- `read_usage_bitmap` (`:54`) checks every sector's CRC and returns false if any fails.
- `update_usage_bit` (`:108`) recalculates that sector's CRC in RAM before writing it,
  and again (`:126`) when it rolls the bit back after a failed write.
- `check_usage_bit` now uses `FIND_INDEX`.
- `get_next_bit` / `get_prev_bit` skip CRC words, and convert words back to indices with
  `WORD_TO_INDEX`.

`hash_table.c`: in all 4 bitmap loops, `FIND_ELEMENT` became `FIND_INDEX`,
`word * 32 + bit` became `WORD_TO_INDEX(word) + bit`, and CRC words are skipped.

**Pre-existing bug fixed as a side effect:** the loops used `FIND_ELEMENT`, the word
*within* a bitmap sector, as an index into the flat array. That wrapped back to word 0
after 4096 bits, so anything past data sector 4095 (most messages) read the wrong word.

**Removed:** `SuperHeader.usage_bitmap_crc`. It was never calculated or checked. Keeping
one CRC for the whole bitmap in the superheader would also need an extra write, which
the journal doesn't cover, on every bitmap update.

**Check:**
- [ ] The CRC stored in RAM always matches the bitmap bits in RAM. The journal copies
      the bitmap sector straight from RAM, so its copy also has a valid CRC.
- [ ] Any new code that walks `usage_bitmap[]` has to skip CRC words. Using
      `get_next_bit()` is the easiest way.

---

## 4. Superheader + STRG_CORRUPT handling

- `superheader.c:53`: the CRC length was `sizeof(SUPR_HEAD_DATA)`. `SUPR_HEAD_DATA` is the
  int literal 12, so that's `sizeof(int)` = 4, and the CRC only covered `magic`. It is now
  `sizeof(SuperHeaderData)`.
- `superheader.c:27`: new `write_superheader()`. It still isn't called from an init or
  format path; that's left for you to wire up.
- `contact.c` and `message.c`: reads compare against `!= STRG_OK` and pass the error up.
- `hash_table.c:131,203`: `read_contact` checks `FAIL || CORRUPT`. I kept the original
  behaviour of treating `STRG_EMPTY` as a non-error.
- `hash_table.c:944`: when rebuilding contacts, a corrupt sector is **skipped**. Its slots
  are not freed, so they won't be reused.

**Check:**
- [ ] Is skipping a corrupt contact sector the policy you want? The alternatives are
      stopping the boot, or zeroing the sector and freeing it.

---

## 5. Addressing bugs (the "bugs 1 and 2")

There are three index spaces. They now have one set of conversion macros
(`mem_layout.h:195–197`):

| Space | Meaning | Used by |
|---|---|---|
| record index | contact slot / message sector index | `HashEntry.sector`, `latest_msg_extent`, allocators |
| data sector | offset from `DATA_REGION_START_SECTOR` | usage bitmap, **journal** |
| raw sector | SD block address | `read_block` / `write_block` |

```c
CONTACT_DATA_SECTOR(slot)   = CONTACT_DATA_START_SECTOR + slot / CONTACT_SECTOR_CAPACITY
MESSAGE_DATA_SECTOR(msgInd) = MESSAGE_DATA_START_SECTOR + msgInd
DATA_SECTOR_TO_RAW(dataSec) = DATA_REGION_START_SECTOR + dataSec
```

**Bug 1: message sectors overlapped contacts.** `message.c:17,30` used
`index + MESSAGE_DATA_START_SECTOR` as the raw address and left out
`DATA_REGION_START_SECTOR` (12). Message sector *i* was written over contact sector
2371 + *i*. Fix: `DATA_SECTOR_TO_RAW(MESSAGE_DATA_SECTOR(index))`. The usage-bit calls
used to mix `+ CONTACT_MEMORY_SECTOR_SIZE` and `+ TOTAL_CONTACT_SECTOR_SIZE`. They had the
same value, but both are now `MESSAGE_DATA_SECTOR()`.

**Bug 2: journal rollback restored the wrong sector.** `journal_rollback` writes to
`DATA_SECTOR_TO_RAW(header.sector)` and restores the bitmap sector at
`FIND_SECTOR(header.sector)`, so it expects a **data sector** index. But:
- `contact.c` passed the contact **slot**. Slot 8 rolled back onto contact sector 8
  instead of sector 1.
- `message.c` passed the message index. A rollback landed in the contact region and
  restored the wrong bitmap sector.

Fix: `journal_add(..., CONTACT_DATA_SECTOR(index), ...)` (`contact.c:109,246`) and
`journal_add(..., MESSAGE_DATA_SECTOR(x), ...)` (5 places in `message.c`).

**Related fixes:**
- `journal.c:166`: `journal_add` now stores its header in `journal->header`. Before,
  `journal_free()` committed whatever was in that field. After a fresh `journal_init` it
  had never been filled in (magic = 0), so every commit wrote a header that looked
  uninitialised.
- `message.c:70`: `write_message` checks whether the sector is full **before** journalling
  and updating the bitmap. Before, a full sector returned `STRG_FULL` and left the journal
  active.

**Check:**
- [ ] `test_journal.cpp`: the `Rollback` tests read back raw sector `target_sector` (= 2).
      Raw sector 2 is `JRNL_CONTENT_SECTOR`, so the test was comparing the journal's copy
      against itself and always passed. They now read `DATA_SECTOR_TO_RAW(target_sector)`.

---

## 6. Message reconstruction (the "bug 3")

`hash_table.c:1038`, `hash_reconstruct_message()`, rewritten. Problems with the old version:
- The loop never cleared the bit it had just processed (`bits &= bits - 1`), so it looped
  forever on the first used message sector. That would hang `db_main.c` on hardware.
- It passed a bitmap/data-sector index to `read_message_sector()`, which expects a
  message index.
- It never gave free message sectors back to the allocator. `last_free_sector` was
  never updated.
- `insert_message_from_sector` did `used_count++`, which counted sectors twice when the
  allocator was set up with `free_list_empty_init`.

The new version walks the message range with `get_next_bit()`. For each used sector it:
1. frees the gap since the previous used sector,
2. reads the sector, skipping it if corrupt,
3. sets the matching hash entry to this sector if the sector has no `next` (it's the
   newest in its chat).

At the end it frees everything after the last used sector.

`insert_message_from_sector` (`:972`) now copies the phone into a NUL-terminated buffer,
because the header field isn't terminated when it's 15 characters long.

**Root bug found while testing:** `create_new_message_sector` (`message.c:530`) **never
stored the phone number** in the sector header, and left the rest of the sector as
uninitialised stack memory. With no phone, reconstruction can't work out which contact a
chat belongs to. It now zero-fills the sector, sets `type`, `state`, `phone` and
`phone_len`.

**Why the existing tests didn't catch any of this:** no test called
`hash_reconstruct_message()`. `hash_cleanup()` has it commented out, so `db_main.c` on the
board was the only caller. The contact reconstruction tests only used the first few
contact sectors, so they never reached the overlap or rollback bugs.

**Check:**
- [ ] `hash_reconstruct_message` must run after `hash_reconstruct_contact`, with the
      message allocator set up by `free_list_empty_init` (as `db_main.c:reconstruct_table` does).
- [ ] `hash_cleanup()` still doesn't rebuild messages, because it has no journal parameter.
- [ ] A message sector that has no matching contact still creates an empty contact (this
      behaviour is unchanged).

---

## 7. Tests

| File | What it covers |
|---|---|
| `tests/test_sector_crc.cpp` (new, 12 tests) | every single-byte flip is detected; `read_sector` vs `read_sector_raw`; contact and message corruption; writing the first contact into an unused sector; `write_contact` refusing to change a corrupt sector; bitmap CRC on update and read; bitmap walk skipping CRC words; superheader CRC covering `db_end`; rebuild skipping a corrupt sector |
| `tests/test_db_recovery.cpp` (new, 7 tests) | message 0 doesn't overwrite contact sector 2371; last message sector is inside the data region; simulated power cut + reboot rolls back the right contact/message sector; full sector leaves the journal committed; message reconstruction (latest sectors, allocator counts, appending after reboot); empty database |
| `test_usage_bitmap*.cpp` | values that assumed 4096 bits per sector now use `USAGE_BITS_PER_SECTOR` / `USAGE_WORDS_PER_SECTOR`; the raw pattern test stamps a CRC first |
| `test_hash_table.cpp` | setup calls `init_usage_bitmap(storage)` so bitmap sectors on storage have a valid CRC |
| `test_journal.cpp` | rollback tests read the real data sector (see section 5) |

Result: **201/201 host tests pass, and the firmware builds.** Not yet run on hardware.

**Suggested hardware check:** run `db_main.c` with `DB_TEST_WRITE=ON` (format + write),
then `OFF` (reconstruct only, after a power cycle).

---

## 8. Known remaining issues (not changed)

- On firmware, `crc.c` uses the STM32 CRC peripheral, because the `USE_SW_CRC=1` define
  isn't checked anywhere. The comment in `source/CMakeLists.txt` says `hcrc` isn't set
  up, but `main.c:105` does call `MX_CRC_Init()`. Still worth checking on the board that
  the hardware CRC matches the software CRC over a 508-byte buffer. If they differ, a
  card written by one can't be read by the other.
- `write_contact` calls `journal_free()` twice on success, which costs an extra write.
- `free_list_free_range(..., USAGE_BITMAP_STORAGE_SIZE * BITS_PER_ELEMENT)` at the end of
  `hash_reconstruct_contact` frees a slot range using a bitmap-bit count. It's harmless
  because the free list stops at its capacity, but it's mixing units.
