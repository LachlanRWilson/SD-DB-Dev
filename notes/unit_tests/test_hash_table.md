# `tests/test_hash_table.cpp` — Review Notes

Covers the phone-keyed `HashTable` in [`hash_table.c`](../../source/app/src/hash_table.c),
together with the contact store ([`contact.c`](../../source/app/src/contact.c)) and message
chat store ([`message.c`](../../source/app/src/message.c)) it sits on top of. All 36 tests
share one fixture, `HashTableTest`, and currently pass (`ctest -R test_hash_table`).

Several of these tests were written specifically to pin down bugs found while reviewing the
contact/message code (see the "Regression for" notes below) — those are the ones most worth a
careful second read, since they encode the exact broken behaviour that was fixed.

## How to use this file

Tick a box once you've read the test's code in `test_hash_table.cpp` *and* satisfied yourself
it actually checks what its description claims. Leave it unchecked if you're unsure or want to
revisit it.

## Checklist

### `hash_phone()`
- [ ] PhoneHashIgnoresNonDigits

### Contacts: insert / find / remove
- [ ] InsertContact
- [ ] FindContact
- [ ] FindMissingContact
- [ ] RemoveContact
- [ ] RemoveMissingContact
- [ ] RemoveByPhoneRemovesContactAndMessages
- [ ] RemoveByPhoneMissingFails
- [ ] MultipleContacts
- [ ] DuplicateInsertUpdatesInPlace
- [ ] PhoneHashCollisionResolved
- [ ] RemoveFromCollisionChain
- [ ] ReinsertAfterRemove
- [ ] SizeTracksContacts
- [ ] RejectsBadArguments

### `hash_find_entry()`
- [ ] FindEntryMatchesStoredPhone
- [ ] FindEntryDistinguishesCollidingPhones

### `create_message()`
- [ ] CreateMessageRejectsBadArguments
- [ ] CreateMessageStoresContent
- [ ] CreateMessageTruncatesOverlongBody

### Messages: insert / find / remove
- [ ] InsertFirstMessageCreatesContact
- [ ] InsertMessageAttachesToExistingContact
- [ ] FindMessageReturnsLatest
- [ ] FindMessageMissingContactFails
- [ ] FindNMessagesWithinOneSector
- [ ] MessageChatRollsOverToNewSector
- [ ] FindNMessagesAcrossSectorBoundary
- [ ] RemoveMessageChatClearsMessagesOnly
- [ ] RemoveMessageChatAcrossMultipleSectors
- [ ] RemoveMessageChatMissingContactFails
- [ ] RejectsBadMessageArguments

### `hash_reconstruct_contact()`
- [ ] ReconstructFindsAllContacts
- [ ] ReconstructEmptyDatabase
- [ ] ReconstructSkipsRemovedContacts
- [ ] ReconstructPreservesCollisionChain
- [ ] ReconstructedTableAcceptsNewWrites

---

## Fixture: `HashTableTest`

Stands up the full on-disk layout (superheader / usage bitmap / journal / contact region /
message region) over a heap-backed `Storage`, sized from `mem_layout.h`, so the tests exercise
the real read/write/journal code paths rather than mocks.

- `contact_allocator` / `message_allocator` — separate `FreeList`s for contact slots and
  message sectors. Note: the message allocator is sized `TOTAL_MESSAGE_SECTOR_SIZE`
  (`2 * HASH_TABLE_SIZE`) — earlier versions of this fixture never initialised it at all, which
  silently made every message insert fail (`free_list_allocate` on a zeroed `FreeList` always
  returns `UINT16_MAX`).
- `usage_bitmap` is a **global** array touched by `check_usage_bit`/`update_usage_bit`, so
  `SetUp()` memsets it every test — forgetting this would leak state between tests.
- `journal_init()` must run before any contact/message write, since writes journal the sector
  they're about to overwrite for power-cycle rollback.
- Helper methods:
  - `insert(name, phone)` → `hash_insert_contact`, returns `bool`.
  - `sector_for(phone)` → looks up `entry->sector` via `hash_find_entry`. Needed because
    `hash_insert_contact`/`hash_insert_message` return `bool`, not a sector index, so tests
    that need to compare sectors (e.g. "did this update land in the same slot?") can't just use
    the insert's return value.
  - `send(phone, timestamp, direction, text)` → builds a `MessageBuffer` via `create_message`
    and calls `hash_insert_message`.
  - `rebuild()` → simulates a power cycle: builds a second, empty `HashTable` over the *same*
    storage, reloads the usage bitmap from storage, and calls `hash_reconstruct_contact`.

`MESSAGE_BLOCK_CAPACITY` (how many messages fit in one 512B message sector) currently evaluates
to **2**, given `Message` is 164B and the sector header/type overhead is 26B. Several message
tests deliberately send `MESSAGE_BLOCK_CAPACITY + 1` (i.e. 3) messages to force a sector
rollover — worth confirming that arithmetic still holds if `Message` or `MessageSectorHeader`
ever change size.

---

## `hash_phone()`

### PhoneHashIgnoresNonDigits
Confirms `hash_phone()` strips non-digit characters before hashing (so `"0412345678"`,
`"0412 345 678"`, and `"(04) 1234-5678"` all hash identically), and that changing an actual
digit changes the hash. This underpins every phone-collision test below — they rely on being
able to predict which numbers collide.

---

## Contacts: insert / find / remove

### InsertContact
Baseline: insert one contact, check `hash_insert_contact` returns `true` and `hash_size()`
becomes 1.

### FindContact
Insert then find by phone; checks the round-tripped `ContactBuffer` (name, phone, lengths)
matches what was written.

### FindMissingContact
A phone number that was never inserted returns `false` from `hash_find_contact`, not a stale or
garbage result.

### RemoveContact
Insert, remove, check the returned (removed) contact data, and confirm both `hash_size()` drops
to 0 and the contact is no longer findable afterwards.
**Regression for:** `hash_remove_contact()` used to reset `entry->sector = UINT16_MAX` but never
set `entry->state = ENTRY_DELETED` or decremented `table->num_elems` — so `hash_size()` kept
counting a removed contact, and a later `hash_find_entry()` could still match the tombstone-less
slot and try to read contact data from sector `UINT16_MAX`.

### RemoveMissingContact
Removing a phone number that was never inserted returns `false` (not a crash, not a false
success).

### RemoveByPhoneRemovesContactAndMessages
Insert a contact, send it a message, then call the combined `hash_remove()` (not
`hash_remove_contact`). Checks it hands back the removed `HashEntry*` with `state ==
ENTRY_DELETED`, and that *both* the contact and the message chat are gone afterwards.
**Regression for:** `hash_remove()` (and `hash_remove_message()`) used to pass `entry->sector`
(the **contact's** storage sector) to `remove_message_chat()` instead of
`entry->latest_msg_extent` (the actual head of the message chain) — silently deleting the wrong
sector rather than the chat.

### RemoveByPhoneMissingFails
`hash_remove()` on an unknown phone number returns `false` and leaves the caller's output
pointer untouched (still `nullptr`).

### MultipleContacts
Three distinct contacts, inserted and found independently — a basic sanity check that inserts
don't clobber each other.

### DuplicateInsertUpdatesInPlace
Inserting the same phone number twice updates the existing contact (new name) in the *same*
sector, rather than allocating a second one — checked via `sector_for()` before/after, and via
`hash_size()` staying at 1.
Note: this test needed rewriting alongside the `bool`-return-type fix, since it used to compare
the sector index returned directly from `hash_insert_contact` — that's no longer possible once
the function returns `bool`.

### PhoneHashCollisionResolved
`"0400000601"` and `"0400002060"` are precomputed to collide on `hash_phone()`. Confirms both
still get distinct sectors, both are stored (`hash_size() == 2`), and both are found correctly —
i.e. the double-hash probing + phone-string disambiguation in `hash_find_entry()` actually
resolves the collision instead of one contact clobbering the other.

### RemoveFromCollisionChain
Same colliding pair; removes one and checks the other is still reachable "past the tombstone"
(the probe sequence must keep walking through `ENTRY_DELETED` slots rather than stopping early),
and that the removed one is actually gone.

### ReinsertAfterRemove
After removing a contact, inserting the same phone number again reuses the tombstoned slot
(`hash_size()` back to 1, not 2).

### SizeTracksContacts
`hash_size()` increments on insert and decrements on remove across a short sequence — a direct,
narrow check of the counter (this is exactly the counter the `RemoveContact` regression above
was silently breaking).

### RejectsBadArguments
NULL-argument handling for `hash_insert_contact`, `hash_find_contact`, and `hash_remove_contact`
(NULL table, NULL contact, NULL phone) — all must return `false`/fail cleanly, no crash.

---

## `hash_find_entry()`

### FindEntryMatchesStoredPhone
After inserting a contact, `hash_find_entry()` returns a pointer to an `ENTRY_OCCUPIED` slot
whose `sector`/`id` match what was inserted. For an unknown phone number it still returns
`true` (an entry pointer is always handed back — either a match or a free insertion point) but
the returned slot is *not* `ENTRY_OCCUPIED`. Worth double-checking this "always true, check
state separately" contract against how callers actually use it elsewhere in `hash_table.c`.

### FindEntryDistinguishesCollidingPhones
For the same colliding pair used above, checks `hash_find_entry()` returns two *distinct*
`HashEntry*` pointers, both `ENTRY_OCCUPIED` — i.e. it doesn't just return the first match by
hash and ignore the phone-string check.

---

## `create_message()`

### CreateMessageRejectsBadArguments
`create_message(0, ...)` (zero timestamp) and `create_message(ts, ..., nullptr)` (NULL body)
both return an all-zero `MessageBuffer`.
**Regression for:** the guard used to be `if (timestamp > 0 || str == NULL)`, i.e. it rejected
every *valid* (nonzero) timestamp and only "succeeded" for `timestamp == 0` — inverted from what
was clearly intended (and from what this test now checks).

### CreateMessageStoresContent
A valid nonzero timestamp and body are stored correctly (`timestamp`, `direction`, `str` all
round-trip).

### CreateMessageTruncatesOverlongBody
A body longer than `SMS_MAX_MESSAGE_LENGTH` is truncated to fit (and null-terminated) rather
than overflowing the fixed-size `str` buffer.
**Regression for:** `create_message()` used to unconditionally `memcpy` a fixed
`SMS_MAX_MESSAGE_LENGTH` (160) bytes from the caller's string regardless of its real length —
an out-of-bounds read if the caller's buffer was shorter.

---

## Messages: insert / find / remove

### InsertFirstMessageCreatesContact
Sending a message to a phone number with no existing contact auto-creates an (empty-named)
contact as a side effect, and the message is immediately findable via `hash_find_message()`.

### InsertMessageAttachesToExistingContact
Insert a contact first (no message), *then* send it a message — the message must attach to the
existing contact rather than failing or creating a duplicate.
**Regression for:** a contact-only entry never initialised `entry->latest_msg_extent` to a
"no messages yet" sentinel (`UINT16_MAX`); it defaulted to `0`. `hash_insert_message()` then
treated `0` as an existing, appendable message sector and tried to write into it, which failed
because sector `0` was never actually allocated/marked used for that contact. Fixed by
initialising `latest_msg_extent = UINT16_MAX` on contact creation (including during
reconstruction) and checking that sentinel independently of "is this a brand new hash entry".

### FindMessageReturnsLatest
Two messages sent in sequence; `hash_find_message()` must return the second (most recent) one,
not the first.

### FindMessageMissingContactFails
`hash_find_message()` on a phone number with no chat returns `false` — including the case where
the contact doesn't exist at all.

### FindNMessagesWithinOneSector
Two messages (fits in one sector, `MESSAGE_BLOCK_CAPACITY == 2`); `hash_find_n_message(n=2)`
must return them **newest-first**.

### MessageChatRollsOverToNewSector
Sends `MESSAGE_BLOCK_CAPACITY + 1` (3) messages — more than fit in one sector — and checks the
*latest* message is still the last one sent, i.e. the chat correctly rolled over onto a second,
linked sector rather than losing the overflow message or wrapping incorrectly.
**Regression for two separate, compounding bugs:**
1. `write_next_message_sector()` had an inverted check — `if (is_used) return STRG_EMPTY;` on
   the *previous* (currently-full) sector, when the previous sector being in-use is exactly the
   expected/correct case. This bailed out before ever creating the new sector.
2. `point_message_sector_to_next()`, once the new sector was created, wrote the **updated
   previous-sector data back into the new (`next`) sector's slot** instead of back into `prev` —
   overwriting the just-written new message with a stale copy of the old sector.

### FindNMessagesAcrossSectorBoundary
Same 3-message rollover scenario, but requests all 3 via `hash_find_n_message()` and checks the
full newest-first ordering across the sector boundary.
**Regression for:** `read_n_messages()`'s per-message index formula was
`msg_count - (msg_read % MESSAGE_BLOCK_CAPACITY) - 1`, which produces a negative array index
(undefined behaviour) as soon as more than one message is read back — the `msg_read %
CAPACITY` term doesn't correspond to "position within the current sector" once you've crossed a
sector boundary or read more than one message from a partially-full sector. Simplified to
`msg_count - 1` (since `msg_count` is already a per-sector countdown).

### RemoveMessageChatClearsMessagesOnly
Insert a contact, send one message, remove the message chat (`hash_remove_message`) — the
message must be gone (`hash_find_message` → `false`) but the **contact** must still be present
and unchanged. Checks message removal doesn't have side effects on the contact record.

### RemoveMessageChatAcrossMultipleSectors
Same removal, but for a chat that spans `MESSAGE_BLOCK_CAPACITY + 1` (3) messages / 2 linked
sectors — checks the whole chain is walked and cleared, not just the head sector.
**Regression for:** `remove_message_chat()`'s loop condition was `while (curIndex !=
UINT16_MAX)`, correctly walking the `prev` chain — but the body called
`remove_message_sector(..., startIndex, ...)` every iteration instead of `curIndex`. For a
single-sector chat this happened to still "work" (one iteration only); for a multi-sector chat,
the second iteration tried to re-remove the already-removed head sector, hit a cleared usage
bit, and the whole removal failed, leaving the tail sector orphaned.

### RemoveMessageChatMissingContactFails
Removing a message chat for a phone number that was never inserted returns `false`.

### RejectsBadMessageArguments
NULL-argument handling for `hash_insert_message` (NULL table, NULL phone, NULL message buffer).

---

## `hash_reconstruct_contact()`

These simulate a power cycle via `rebuild()`: a second empty `HashTable` is built over the same
backing storage, the usage bitmap is dropped and reloaded from storage, and
`hash_reconstruct_contact()` re-derives the in-RAM table purely from what's on disk.

### ReconstructFindsAllContacts
Inserts 5 contacts, rebuilds, and checks every single one is findable afterwards with the
correct name/phone.
**Regression for:** `hash_reconstruct_contact()` bounded its usage-bitmap scan using
`USAGE_BITMAP_FIND_ELEMENT(HASH_TABLE_SIZE)` — but `HASH_TABLE_SIZE` (14293) is the number of
*contact slots*, not the number of *physical contact sectors* (`CONTACT_MEMORY_SECTOR_SIZE`,
~2383, since several slots pack into one 512B sector). This capped the scan at bitmap word 62
when sectors could live up to word ~74, so any contact whose sector fell past that point was
silently skipped during reconstruction. This is the one most worth re-deriving the arithmetic
for yourself (see `CONTACT_SECTOR_CAPACITY`, `CONTACT_MEMORY_SECTOR_SIZE` in `mem_layout.h`) to
confirm the fix (`USAGE_BITMAP_FIND_ELEMENT(CONTACT_MEMORY_SECTOR_SIZE)`) is actually correct
and not just "big enough to pass this particular test".

### ReconstructEmptyDatabase
Rebuilding with nothing ever inserted yields an empty table (`hash_size() == 0`), not a crash or
phantom entries.

### ReconstructSkipsRemovedContacts
Insert 3, remove 1, rebuild — the 2 survivors are found, the removed one is not.

### ReconstructPreservesCollisionChain
The colliding phone pair survives a reconstruction and both remain independently findable
afterwards (i.e. reconstruction re-derives the same collision-resolved layout, not just "some"
layout).

### ReconstructedTableAcceptsNewWrites
After rebuilding, the new (rebuilt) table is fully live: a new contact can be inserted into it
and an old one removed from it, with `hash_size()` tracking correctly throughout — checks the
rebuilt table isn't read-only or left in some partially-initialised state.
