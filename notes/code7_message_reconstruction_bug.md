# Code 7 (`T_MESSAGE_FIND`) — Debugging Summary

Findings from a GDB session against real hardware (OpenOCD + ST-Link), investigating
the persistent code-7 failure in `db_run_write()`. Kept here so debugging can
continue without re-deriving all of this from scratch. `hash_table.c` changes made
during this session were reverted by request — nothing below is currently applied
except the `message.c` offset fix (see §1).

## Summary of where things stand

- **§1 (message.c bitmap offset) — fixed, confirmed working.**
- **§2 (hash_reconstruct_message scan bounds) — real bug, partial fix attempted and
  reverted. Even with that fix applied, code 7 persisted and `insert_message_from_sector`
  was never called — so there is at least one more bug in this path not yet found.**

## 1. Contact/message usage-bitmap collision in `message.c` (fixed)

The usage bitmap (`usage_bitmap.c`) is sized to hold one bit per physical sector
across the **combined** data region — contacts first, then messages (see
`mem_layout.h`: `USAGE_BITMAP_SIZE` is derived from `TOTAL_DATA_SECTOR_SIZE`, and
`MESSAGE_DATA_START_SECTOR = CONTACT_DATA_START_SECTOR + TOTAL_CONTACT_SECTOR_SIZE`).

`contact.c` correctly computes its bitmap bit as `index / CONTACT_SECTOR_CAPACITY`,
landing in `[0, TOTAL_CONTACT_SECTOR_SIZE)`. But `message.c`'s `check_usage_bit()` /
`update_usage_bit()` calls used the raw message-allocator-local index directly, with
no offset — so message sector 0 shared a bit with contact physical sector 0, and a
contact insert/remove could silently flip a bit a message chat's "is this sector
in use" check depended on, or vice versa.

**Fix applied**: every `check_usage_bit`/`update_usage_bit` call in `message.c`
(`write_message`, `write_new_message_sector`, `write_next_message_sector`,
`read_message`, `read_n_messages`, `remove_message_sector`) now adds
`CONTACT_MEMORY_SECTOR_SIZE` (aliased to `TOTAL_CONTACT_SECTOR_SIZE`) to the index,
placing message bits in their own reserved slice of the bitmap.

Confirmed via GDB: after this fix, the **pre-reconstruction** `verify_messages()`
call (`db_main.c:491`, checking state right after the live write pass's removals)
returns `0` (T_OK) — consistently, across repeated flashes/resets. So the live
write path (insert, send, remove, collision handling) is correct.

## 2. `hash_reconstruct_message()` scan bounds are wrong (`hash_table.c`, NOT currently applied)

With §1's fix in place, code 7 still occurs — but now confirmed (via breaking on
`verify_messages` and using `finish` to read its return value) to come specifically
from the **second**, post-reconstruction call at `db_main.c:496`. The bug is
therefore in reconstruction, not the live write path.

`hash_reconstruct_message()` computes its scan range like this:

```c
int first_message_usage_elem = USAGE_BITMAP_FIND_ELEMENT(DATA_REGION_START_SECTOR - MESSAGE_DATA_START_SECTOR);
int first_message_usage_bit  = USAGE_BITMAP_FIND_BIT(DATA_REGION_START_SECTOR - MESSAGE_DATA_START_SECTOR);
int last_message_usage_elem  = USAGE_BITMAP_FIND_ELEMENT(MESSAGE_SECTOR_SIZE);
```

Problems, confirmed live via GDB on real hardware (`first_message_usage_elem=53`,
`first_message_usage_bit=17`, `last_message_usage_elem=125`):

- **Reversed subtraction.** `DATA_REGION_START_SECTOR - MESSAGE_DATA_START_SECTOR`
  is negative (message region starts *after* the contact region), and wraps around
  as unsigned inside the `USAGE_BITMAP_FIND_ELEMENT`/`_BIT` macros, giving a
  meaningless starting word/bit (53/17) instead of the correct offset. The correct
  value is `MESSAGE_DATA_START_SECTOR - DATA_REGION_START_SECTOR`, which equals
  `TOTAL_CONTACT_SECTOR_SIZE` (confirmed = 2383 on this build).
- **Wrong upper bound.** `last_message_usage_elem` is derived from `MESSAGE_SECTOR_SIZE`
  alone (the message region's own *size*, ~28586), not its absolute position in the
  bitmap (`TOTAL_CONTACT_SECTOR_SIZE + MESSAGE_SECTOR_SIZE`, ~30969). This caps the
  scan at word 125 when the real message-region bits live much further out.
- **`phys_sector` is an absolute bitmap bit position, used as a message-local index.**
  `phys_sector = word * BITS_PER_ELEMENT + bit` is the *absolute* position across the
  combined bitmap, but it's passed directly to `read_message_sector()`,
  `free_list_free_range(table->message_allocator, ...)`, and
  `insert_message_from_sector(..., phys_sector)` (which sets `entry->latest_msg_extent`)
  — all of which expect the message-*local* index (0-based within the message
  region, no contact-region offset). Needs `local_index = bitmap_bit - TOTAL_CONTACT_SECTOR_SIZE`.
- **Mask applied to every word, not just the first.** `bits &= (~0u) << (first_message_usage_bit - 1)`
  runs unconditionally for every `word` in the loop, not just `word == first_message_usage_elem`,
  incorrectly clearing low bits of every subsequent word too.
- **Missing `bits &= bits - 1;`** to clear the processed bit before the next
  `while (bits != 0)` check (present in the analogous `hash_reconstruct_contact()`
  loop, absent here). If a genuine bit is ever reached with the range/offset bugs
  above fixed, this would spin on it forever. It hasn't bitten yet only because the
  range/offset bugs above mean a real bit is never reached in the first place.

### Why this hasn't been caught by the host gtest suite

`free_list_allocate()` (`free_list_stack.c`) is a **LIFO stack**: `free_list_init()`
pushes indices `0..N-1` in order, and `free_list_allocate()` pops from the top
(`free_stack[--stack_top]`), so the *first* thing allocated gets the *highest*
index, not the lowest. Confirmed live: with 5 surviving message chats in this test
(Alice, Charlie ×2 sectors, Collide A, Collide B — Bob and Dana's chats were
correctly removed), the real message-region bits sit in `usage_bitmap[967] = 0x016c0000`
(bits 18, 19, 21, 22, 24 — i.e. near the *far end* of the message region), while the
buggy scan only ever covers words 53–125. So the loop always finds and reconstructs
**zero** messages, and every surviving chat reports "not found" in the
post-reconstruction `verify_messages()` — exactly matching code 7.

### Fix attempted and reverted

A rewrite correcting all five issues above was applied and rebuilt (host tests:
141/141 pass; STM32 build: clean). On hardware, `first_message_usage_elem`/
`last_message_usage_elem` were not re-verified before the session ended, but code 7
*still* occurred, and a breakpoint on `insert_message_from_sector` was never hit —
meaning even the corrected scan still doesn't reach a real message bit, or something
else prevents the call. **This means there is at least one more bug in this path
beyond the five listed above** — the fix was reverted before that could be isolated.

## Suggested next steps

1. Re-apply a fix along the lines described in §2 (or write a fresh one — the exact
   diff is not preserved, only this description).
2. Break at the top of `hash_reconstruct_message()` (after computing the range
   variables) and print `first_message_usage_elem`, `first_message_usage_bit`,
   `last_message_usage_elem` directly — confirm they now bracket word 967 (or
   wherever the real bits land — check `print/x usage_bitmap[N]` for the word your
   specific test run's surviving messages fall in, since it depends on
   `TOTAL_CONTACT_SECTOR_SIZE`, `MESSAGE_SECTOR_SIZE`, and how many contacts/messages
   were removed before reconstruction).
3. If the range now looks correct but `insert_message_from_sector` still isn't hit,
   step through the inner `while (bits != 0)` loop instruction-by-instruction for the
   word containing real data, and check whether `bitmap_bit >= message_region_end`
   is tripping early, or whether the mask (`bits &= (~0u) << first_message_usage_bit`)
   is being applied to the wrong word and clearing the real bits.
4. Once messages reconstruct correctly, re-run the full `DB_TEST_WRITE=1` write pass,
   then flash `DB_TEST_WRITE=0` to confirm persistence across a real power cycle,
   per the workflow in `notes/db_main_hardware_tests.md`.

## GDB workflow notes (for reference)

- `openocd -f interface/stlink.cfg -f target/stm32h7x.cfg` then
  `gdb-multiarch build/firmware/MyProject.elf`, `target extended-remote :3333`.
- `break verify_messages` + `continue` + `finish` (repeated) is a clean way to read
  each call's return value directly, since `T_MESSAGE_FIND` (7) is reused for both
  the pre- and post-reconstruction checks and the LED blink count alone can't
  distinguish them (see `notes/db_main_hardware_tests.md`).
- Nesting `continue` inside a breakpoint's `commands` block was unreliable in batch
  mode (`gdb-multiarch -batch -x script.gdb`) — it silently stopped resuming after
  the first hit. Plain top-level `continue`/`finish` pairs, repeated explicitly for
  each expected hit, worked reliably instead.
- `detach` (including the implicit one at the end of a `-batch` script) does **not**
  halt the target — it keeps running (or stays halted, if it already was) on the
  MCU. Reconnecting without `monitor reset halt` resumes inspecting *that* state,
  which is useful, but forgetting to reset when you actually wanted a fresh run
  produces confusing "breakpoint never hit" results.
