# `db_main.c` — Hardware Unit Test Briefing

For debugging a fresh Claude session with no prior context on this. If you're pasting this in
cold: this project is an STM32 phone-keyed contact+message database (SD-card backed, journalled
for power-cycle safety). [`source/app/src/db_main.c`](../source/app/src/db_main.c) is a FreeRTOS
task that runs the same scenarios as the host gtest suite
([`tests/test_hash_table.cpp`](../tests/test_hash_table.cpp)) but against the real SD card via
[`sd_storage.c`](../source/app/src/sd_storage.c), reporting pass/fail on an LED since there's no
console attached.

**Status: this file was rewritten to use the current `hash_table.c` (contacts + messages) API,
compiles clean for the real `arm-none-eabi-gcc` target (both `DB_TEST_WRITE=1` and `=0`), but has
not yet been run on actual hardware.** If you're debugging a specific failure, you're likely the
first real hardware run of this version — treat every assumption below as unverified until the
LED (or a debugger) says otherwise.

## How it runs

- Entry point: `DB_Init()` spawns `dbTask()` (8192B stack, `.ram_d1`-placed backing arrays —
  `g_entries`, `g_contact_fls_mem`, `g_message_fls_mem` total ~204KB, see below).
- `DB_TEST_WRITE` (CMake option, default `ON`) selects which of two functions `dbTask()` calls:
  - `DB_TEST_WRITE=1` → `db_run_write()`: wipes the journal + usage bitmap, inserts the fixed
    contact/message set through the real journalled write path, exercises every removal variant,
    reconstructs the table in place, verifies, then does a scratch write+remove to prove the
    rebuilt table is still writable.
  - `DB_TEST_WRITE=0` → `db_run_persist()`: touches nothing except what journal recovery might
    roll back. Reloads the usage bitmap and reconstructs *purely* from what a prior
    `DB_TEST_WRITE=1` run persisted, then verifies.
  - Intended workflow: flash with `1` once, then power-cycle or reflash with `0`. A pass on the
    second run proves data survived with no writer running.
- Result: `blink_forever(code)` on PB0. `T_OK` (0) = fast continuous blink = everything passed.
  Any other code = blink that many times, pause, repeat.

## Failure codes (LED blink count)

| # | Name | Meaning |
|---|------|---------|
| 1 | `T_STORAGE_INIT` | `SDStorage_Init()` failed |
| 2 | `T_BITMAP_INIT` | `init_usage_bitmap()` failed |
| 3 | `T_JOURNAL_INIT` | `journal_init()` failed |
| 4 | `T_INSERT` | `hash_insert_contact()` failed |
| 5 | `T_VERIFY_INSERT` | an inserted contact wasn't found / had the wrong name right after insert |
| 6 | `T_MESSAGE_SEND` | `hash_insert_message()` failed |
| 7 | `T_MESSAGE_FIND` | a contact's latest/only message missing or wrong timestamp |
| 8 | `T_MESSAGE_ROLLOVER` | Charlie's multi-sector chat wrong (count or ordering) |
| 9 | `T_MESSAGE_REMOVE` | `hash_remove_message()` failed, or the chat/contact state after it is wrong |
| 10 | `T_REMOVE` | `hash_remove_contact()` failed, or the contact is still findable after |
| 11 | `T_REMOVE_ALL` | `hash_remove()` (combined contact+chat) failed, or either survived |
| 12 | `T_COLLISION` | the Collide A / Collide B phone-hash-collision pair not correctly resolved |
| 13 | `T_BITMAP_RELOAD` | `read_usage_bitmap()` failed during reconstruction |
| 14 | `T_RECON_CONTACT_RUN` | `hash_reconstruct_contact()` failed |
| 15 | `T_RECON_MESSAGE_RUN` | `hash_reconstruct_message()` failed |
| 16 | `T_RECON_SIZE` | rebuilt table has the wrong contact count |
| 17 | `T_RECON_FIND` | a survivor missing/wrong name after rebuild |
| 18 | `T_RECON_REMOVED` | a contact that should be gone reappeared after rebuild |
| 19 | `T_RECON_WRITABLE` | the rebuilt table rejected a new scratch contact/message write |

Note: `T_MESSAGE_FIND`/`T_MESSAGE_ROLLOVER` (7/8) are reused by the shared `verify_messages()`
check both *before* reconstruction (right after the write pass's removals) and *after* it — the
blink count alone won't tell you which phase failed. Attach a debugger and check where `rc` was
returned from if you need that distinction, or bisect by temporarily short-circuiting
`db_run_write()` to return right after the pre-reconstruction `verify_messages()` call.

## Test data (`g_contacts[]`)

Seven fixed contacts, each exercising a different combination of message/removal behaviour:

| Name | Removal | Messages |
|---|---|---|
| Alice | survives | 1 message |
| Bob | `hash_remove()` — contact + chat together | 1 message before removal |
| Charlie | survives | `MESSAGE_BLOCK_CAPACITY + 1` messages — forces a 2nd linked sector |
| Dana | survives | 1 message, then chat-only removed via `hash_remove_message()` |
| Collide A | survives | 1 message |
| Collide B | survives | 1 message — same `hash_phone()` value as Collide A, tests message isolation between colliding entries |
| Eve | `hash_remove_contact()` — contact-only, never had a chat | none |

`MESSAGE_BLOCK_CAPACITY` is computed from `SECTOR_SIZE`/`Message`/`MessageSectorHeader` sizes in
[`message.h`](../source/app/include/message.h) — currently `2` on both host and target (same
struct sizes), so Charlie gets 3 messages and should land on 2 linked sectors.

## Key functions to know

- `storage_bringup()` — `SDStorage_Init()` + wires `sd_storage.context`.
- `reconstruct_table()` — fresh empty-init'd allocators, `hash_init()`, reload usage bitmap,
  `hash_reconstruct_contact()` then `hash_reconstruct_message()` (order matters — messages are
  matched to already-reconstructed contact entries).
- `verify_reconstructed()` — contact count + presence/absence/name check against `g_contacts`.
- `verify_messages()` — per-contact message presence/absence + latest-timestamp check, plus full
  ordered chat check for Charlie via `hash_find_n_message()`. Shared between pre- and
  post-reconstruction checks (see note above).
- `make_contact()` / `send_message()` — only compiled under `#if DB_TEST_WRITE` (unused, and
  would be dead code, in the persistence-only build).

## Where this plugs in

- API: [`hash_table.h`](../source/app/include/hash_table.h),
  [`message.h`](../source/app/include/message.h),
  [`contact.h`](../source/app/include/contact.h).
- Storage/journalling underneath: [`journal.c`](../source/app/src/journal.c),
  [`usage_bitmap.c`](../source/app/src/usage_bitmap.c),
  [`sd_storage.c`](../source/app/src/sd_storage.c) (real SD-card `Storage` backend, vs.
  `heap_storage.c` used by the host tests).
- Layout constants: [`mem_layout.h`](../source/app/include/mem_layout.h) — `HASH_TABLE_SIZE`,
  `TOTAL_MESSAGE_SECTOR_SIZE`, sector offsets for the superheader/bitmap/journal/data regions.
- Build wiring: [`source/CMakeLists.txt`](../source/CMakeLists.txt)'s `BUILD_STM32` branch — this
  is what was just changed to link `hash_table.c` + `message.c` instead of the now-legacy,
  no-longer-compiling `hash_table_phone.c`.

## Where to look first if something's failing

1. **RAM budget.** `.ram_d1` usage is ~204KB of a 320KB region (`g_entries` ~112KB,
   `g_contact_fls_mem` ~28KB, `g_message_fls_mem` ~56KB). Confirmed by the linker, but the
   8192B FreeRTOS task stack is a judgment call, not verified on real hardware — `verify_messages()`
   stack-allocates a `MessageBuffer all[MESSAGE_BLOCK_CAPACITY + 1]` (a few hundred bytes) per
   call, which is new versus the previous contact-only version of this file. If it hangs or hard
   faults rather than blinking a code, stack overflow is a reasonable first suspect.
2. **`sd_storage.c`'s real read/write callbacks** — the host tests never exercise real SD/SDMMC
   hardware timing, DMA, or error paths at all (they use `heap_storage.c`, a `memcpy` mock). If
   the code compiles and the logic passed on host but hardware blinks something storage-related
   early (codes 1-3, 13), suspect the SD card driver / `sdmmc.h` glue before suspecting the
   database logic itself.
3. **`journal_init()`'s rollback path (code 3)** on a *second* boot specifically — this is where
   a real interrupted write would surface, and it's the one path that behaves differently
   depending on what state a *previous* run left the card in. If you're bisecting between the
   `DB_TEST_WRITE=1` and `=0` runs, this is the seam between them.
4. **This is the first hardware run of the message-related code paths** (multi-sector rollover,
   collision-pair message isolation, combined contact+message removal). All of it passed on the
   host gtest suite (`tests/test_hash_table.cpp`, see
   [`notes/unit_tests/test_hash_table.md`](unit_tests/test_hash_table.md)) against a heap-backed
   mock — that rules out logic bugs in `hash_table.c`/`message.c` themselves being *new*, but
   does not rule out something timing- or hardware-specific in how `sd_storage.c` interacts with
   them.
