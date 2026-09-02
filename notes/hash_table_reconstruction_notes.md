# Checklist: Getting to a Hash Table Reconstruction Unit Test

Ordered by dependency — earlier items block later ones. Goal: a test that creates
contacts, simulates a restart, reconstructs the hash table from the usage bitmap,
and verifies everything comes back correctly. Messages and full journal
crash-recovery are explicitly out of scope for this first pass.

---

## 1. Storage addressing (decide this first — it blocks everything else)

`contact.c` currently computes the physical sector as `index / CONTACT_SECTOR_CAPACITY`,
starting at sector 0, with **no offset**. But sector 0 is the superheader, and sector 1+
is the usage bitmap (`USAGE_BITMAP_START_SECTOR`), per the existing macros in
`usage_bitmap.h` / `journal.h`. If contact data also starts at sector 0, it will
collide with the superheader/bitmap/journal on a shared storage backend.

- **Decide:** one shared `Storage` for everything (superheader, bitmap, journal,
  contact data all at their real relative offsets) — matches production, and is what
  the `JRNL_*_SECTOR` macros already assume — vs. separate per-subsystem storages
  for testing only.
- Recommendation: use the shared-storage approach even for the unit test, since
  that's the only way the test actually exercises the addressing the bitmap/journal
  code assumes. This means adding a `CONTACT_DATA_START_SECTOR` constant (after the
  journal region) and using it in `read_contact_sector` / `write_contact_sector`.

## 2. Usage bitmap init

There's currently no function that zero-initializes the bitmap on first run and
persists it. `read_usage_bitmap()` exists, but nothing writes an initial all-zero
bitmap to storage before that first read. Need a `usage_bitmap_init(Storage*)` (or
fold it into `superheader_init`'s `SUPR_UNINITIALISED` branch per the architecture
overview) that zeroes RAM `usage_bitmap[]` and writes it out.

## 3. `contact.c` fixes

- Fix the inverted `read_contact_sector(...) && journal_add(...)` condition in
  `write_contact` (sequential checks, not combined).
- Wire `update_usage_bit()` calls into `write_contact` (set) and `remove_contact`
  (clear only when the sector's internal `used_bitmap` hits 0).
- Drop the `Contact.id` field if added previously — phone is now the persisted key,
  so it's redundant.

## 4. `free_list_stack.c` / `.h`

- Add `free_list_reserve(FreeList*, uint16_t sector)` — needed so reconstruction can
  mark a specific known-used slot as taken, without going through the normal
  top-of-stack `free_list_allocate`.

## 5. `hash_table.c` — phone-keyed API

- Add `find_hash_phone()` (probe + disambiguate via real `strcmp` on the stored
  phone, since `entry->id` is now just a 16-bit hash and can collide).
- Add `hash_insert_contact_by_phone()`, `hash_find_contact_by_phone()`,
  `hash_remove_contact_by_phone()` (mirrors the numeric versions).
- Retire `hash_insert_phone()` — it's linear-probed (inconsistent with the rest of
  the table) and never actually compares phone numbers on collision, so it can
  silently misidentify contacts.
- Decide whether to keep or delete the numeric-ID versions (`hash_insert_contact`,
  `hash_find_contact`, `hash_remove_contact`) — if phone is the only key going
  forward, keeping both is just dead surface area to keep in sync.

## 6. `hash_table.c` — reconstruction

- Implement `hash_reconstruct()`: for each physical sector index, `check_usage_bit()`;
  if set, `read_contact_sector`, confirm `type == CONTACT_SECTOR`, walk the sector's
  internal `used_bitmap`, and for each live slot recompute `hash_phone()` on the
  stored phone, probe via `find_hash_phone`, and reinsert at the *original* slot
  (not a freshly allocated one) — then `free_list_reserve()` that slot.
- Add its prototype to `hash_table.h` (currently only exists as an empty stub in
  the `.c` file).

## 7. Journal wiring (minimum viable, not full crash recovery yet)

- Something needs to call `journal_init()` before any contact write happens — right
  now nothing does. For a first-pass "happy path" test you don't need to exercise
  `journal_rollback`, just make sure `journal_init` succeeds against a freshly
  zeroed storage region so `write_contact`'s `journal_add` calls don't fail.

## 8. Test harness setup

- One `heap_storage`-backed buffer, sized to cover superheader + bitmap + journal +
  all contact sectors (`SECTOR_SIZE` × total sectors), zero-initialized.
- `HashEntry[]` array and `FreeList` backing array, sized to `HASH_TABLE_SIZE`.
- A helper to "restart": build a **second**, independent `HashTable` + `FreeList` +
  `HashEntry[]` pointing at the *same* backing storage buffer, to simulate power
  loss without actually losing the persisted bytes.

## 9. The actual test sequence

1. Init storage, bitmap, journal, hash table.
2. Insert N contacts via `hash_insert_contact_by_phone`.
3. Sanity-check `hash_find_contact_by_phone` for all N.
4. "Restart": fresh table/free-list/entries over the same storage.
5. `read_usage_bitmap()` then `hash_reconstruct()`.
6. Assert all N contacts are findable again, at the same sectors, with matching data.
7. Optional follow-up case: remove one contact before "restart," reconstruct, confirm
   it's gone and its sector is free again.

---

## Explicitly out of scope for this first test

- Message extents (deferred until contacts + reconstruction are solid).
- Exercising `journal_rollback` itself (i.e. actually simulating a torn write and
  recovering it) — that's a separate, harder test to write once the happy path
  is solid.
