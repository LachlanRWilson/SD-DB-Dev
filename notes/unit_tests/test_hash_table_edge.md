# `tests/test_hash_table_edge.cpp` — Review Notes

Edge cases for [`hash_table.c`](../../source/app/src/hash_table.c),
[`contact.c`](../../source/app/src/contact.c), and [`message.c`](../../source/app/src/message.c),
complementing the main coverage in [test_hash_table.md](test_hash_table.md). 12 tests, all
passing (`ctest -R "HashTableEdgeTest|HashTableSmallTableTest"`). Uses the shared
[`FailableStorageCtx`](test_support.md) mock for the two storage-failure tests — **read that
file first** if the counter-reset lines look unexplained.

This replaces an earlier `test_hash_table_edge.cpp` written against a completely different,
ID-keyed hash table API (`hash_insert(&table, id)`, a single-`FreeList` `hash_init()`,
`hash_find_message()` returning a raw extent index) that predates the phone-keyed rewrite this
whole test suite targets. Nothing from it carried over.

## Checklist

### contact.c: name/phone length boundaries
- [ ] CreateContactAcceptsNameAndPhoneAtMaxLength
- [ ] CreateContactRejectsOversizedNameOrPhone
- [ ] InsertingContactWithEmptyPhoneDoesNotCrash

### Table exhaustion
- [ ] TableFullRejectsNewInsert

### hash_find_n_message() boundary requests
- [ ] FindNMessagesZeroRequestedReturnsZero
- [ ] FindNMessagesMoreThanAvailableReturnsOnlyWhatExists

### Chat isolation
- [ ] TwoContactsWithMultiSectorChatsDoNotInterfere

### hash_remove_contact() vs. hash_remove() (characterisation)
- [ ] RemoveContactAloneLeaksItsMessageSectors

### hash_cleanup()
- [ ] HashCleanupRebuildsLiveContactsAndDropsTombstones

### Storage-write-failure rollback
- [ ] InsertContactRollsBackHashStateOnWriteFailure
- [ ] InsertMessageFailsWhenMessageAllocatorExhausted
- [ ] InsertMessageLeavesPhantomContactWhenMessageAllocatorExhausted

---

## contact.c: name/phone length boundaries

### CreateContactAcceptsNameAndPhoneAtMaxLength
A name of exactly `MAX_NAME_LEN` and a phone of exactly `MAX_PHONE_LEN` are both accepted (the
check in `create_contact()` is `strlen(x) > MAX_x_LEN`, i.e. equality is fine) — the classic
off-by-one boundary every length check like this deserves a test for.

### CreateContactRejectsOversizedNameOrPhone
One character over either limit and `create_contact()` returns a fully zeroed `ContactBuffer`
(`phone_len`/`name_len` both `0`), not a truncated-but-partially-filled one. Checked
independently for name and phone so a bug in one check being accidentally skipped wouldn't hide
behind the other failing correctly.

### InsertingContactWithEmptyPhoneDoesNotCrash
An empty-string phone number is structurally valid as far as `create_contact()` is concerned
(`phone_len` `0` is `<= MAX_PHONE_LEN`), even though it's a meaningless real-world contact. Since
`hash_phone("")` and the rest of the insert/find path don't special-case an empty string, this
just confirms nothing downstream assumes a non-empty phone (e.g. no code path that would index
`phone[0]` unconditionally, or similar).

---

## Table exhaustion

### TableFullRejectsNewInsert
The only test in this project that actually fills a hash table to true capacity and confirms the
next insert is rejected — the main suite never does this since `HASH_TABLE_SIZE` is 14293 and
nobody wants a test that inserts 14293 contacts to prove a boundary condition. Instead, this
test builds a **separate**, standalone `HashTable` (not the shared fixture) with `table.size`
set to `7` — deliberately a **prime** number. That matters: with a prime capacity, the
double-hash probe sequence `(h1 + i*h2) % capacity` is guaranteed to visit every one of the 7
slots exactly once for any nonzero `h2` (since `gcd(h2, 7) == 1` always holds), so filling all 7
slots with 7 arbitrary distinct phone numbers is *deterministic*, not dependent on how those
particular numbers happen to hash. Confirms: all 7 inserts succeed, an 8th (distinct) phone is
rejected, and `hash_size()` stays at 7 (not incremented by the rejected attempt).

Implementation note worth checking yourself: the backing `HashEntry` array here is still
allocated at the full `HASH_TABLE_SIZE`, even though the table's logical `size` is only 7. That's
not an oversight — `hash_clear()` unconditionally `memset`s `HASH_TABLE_SIZE` entries regardless
of `table->size`, so a smaller backing array would make the `hash_clear()` call at the end of
this test write out of bounds. See the recommended-tests notes for why `hash_clear()`'s use of
the global macro instead of `table->size` is itself worth fixing.

---

## `hash_find_n_message()` boundary requests

### FindNMessagesZeroRequestedReturnsZero
`n = 0` returns `0` messages read, without touching the output buffer or crashing on a
zero-iteration edge in the read loop.

### FindNMessagesMoreThanAvailableReturnsOnlyWhatExists
Requesting 10 messages when only 2 exist returns exactly `2` — the "walk back through linked
sectors until `header.prev == UINT16_MAX`, then stop" path in `read_n_messages()`, confirmed to
actually stop cleanly rather than reading past the start of the chat or returning a wrong count.

---

## Chat isolation between distinct contacts

### TwoContactsWithMultiSectorChatsDoNotInterfere
Two different phone numbers, each sent enough messages to roll over onto a second linked message
sector (`MESSAGE_BLOCK_CAPACITY + 1` messages each, interleaved rather than one-then-the-other).
Confirms each contact's `hash_find_message()` returns *its own* latest message, and — the more
interesting part — that removing one contact's entire chat via `hash_remove_message()` leaves
the other contact's multi-sector chat completely intact and still correctly findable afterwards.
This is the test most likely to catch a bug where sector-chain state (e.g. a `prev`/`next`
pointer, or a shared scratch buffer) accidentally leaks between two unrelated chats.

---

## Characterisation: `hash_remove_contact()` vs. `hash_remove()`

### RemoveContactAloneLeaksItsMessageSectors
**This documents a real, confirmed storage leak — not a crash, so it won't show up unless you
know to look for it.** `hash_remove_contact()` only touches the contact record; it has no
awareness of that contact's message chat at all. If a contact that has messages is removed via
`hash_remove_contact()` instead of the combined `hash_remove()`, the message sectors stay marked
used in `message_allocator` forever — the hash entry that pointed at them is gone, so there's no
way back to them through the public API, but nothing ever frees them either. This test inserts a
contact, sends it a message, removes the contact via `hash_remove_contact()`, and confirms
`free_list_used(&message_allocator)` is **unchanged** by the removal (the leak), while
`hash_find_contact()` correctly reports the contact gone. See the recommended-tests notes for how
you might want to close this gap — either `hash_remove_contact()` also frees the chat, or the two
APIs get documentation/naming that makes "contact-only removal, messages are the caller's
problem" an explicit contract rather than an implicit trap.

---

## `hash_cleanup()`

### HashCleanupRebuildsLiveContactsAndDropsTombstones
Not tested anywhere else in this project. Inserts 3 contacts, removes 1 (leaving a tombstone),
calls `hash_cleanup()` (which internally does `hash_clear()` + `hash_reconstruct_contact()` on
the *same* table object, in place — unlike the `rebuild()` helper in `test_hash_table.cpp`,
which builds a second, separate table), and confirms: the 2 survivors are still findable, the
removed one is gone, `hash_size()` reads `2`, and — importantly — **the table still works
afterwards** (a brand-new insert succeeds and is counted). That last check matters because
`hash_cleanup()` tears down and rebuilds the live table you're about to keep using, not a
throwaway copy; if reconstruction left it in a half-initialised state, everything after this
call in a real running system would be affected.

---

## Storage-write-failure rollback

These two (plus the phantom-contact characterisation below) are the only place in this project
where a contact/message write is made to actually fail via `FailableStorageCtx`, rather than
relying on `HeapStorage`'s out-of-range check (which the main suite never triggers, since
everything it does is in-range by construction).

### InsertContactRollsBackHashStateOnWriteFailure
Forces the contact's `write_contact()` call to fail and confirms `hash_insert_contact()`'s
unwind path actually works: `hash_size()` stays at `0`, the contact sector allocated moments
earlier is freed back to `contact_allocator` (checked via `free_list_used()` returning to its
pre-attempt value), and the hash entry's state is not left `ENTRY_OCCUPIED`. Also confirms the
table is still fully usable afterwards — a normal insert of the same phone number immediately
after succeeds. This is a direct regression test for the return-type bug fixed earlier in this
project's history (`hash_insert_contact()` used to be declared `bool` while its body still used
`uint16_t`/`UINT16_MAX` return semantics from an older API) — with that bug present, this test's
`EXPECT_FALSE(hash_insert_contact(...))` would have been comparing against a value that could
spuriously read as `true` on some failure paths.

### InsertMessageFailsWhenMessageAllocatorExhausted
Drains `message_allocator` completely (no sectors left at all), then confirms
`hash_insert_message()` for a brand-new phone number returns `false` rather than crashing or
silently succeeding with a sector index that doesn't actually exist.

### InsertMessageLeavesPhantomContactWhenMessageAllocatorExhausted
**Characterisation test — read this alongside the one above.** For a brand-new phone number,
`hash_insert_message()`'s current implementation creates and commits the **contact** record
*first*, then tries to allocate a message sector. When that message-sector allocation fails (as
in the test above), the overall call still correctly reports `false` — but the contact it just
created is **not** rolled back. It's left behind as a real, findable, empty-named contact, and
`hash_size()` counts it. This test confirms that's exactly what happens: after the failed send,
`hash_size()` is `1` and `hash_find_contact()` finds an empty-named contact for that phone
number. This is a meaningfully different failure mode from `InsertContactRollsBackHashStateOnWriteFailure`
above — that one *does* unwind a brand-new entry when the contact write itself fails, but there's
no equivalent unwind when the contact write succeeds and it's the *subsequent* message-sector
allocation that fails. Whether that asymmetry is acceptable (an empty placeholder contact isn't
corrupt, just surprising) or worth closing (full rollback of the contact too) is a judgment call
flagged in the recommended-tests notes, not something fixed here.
