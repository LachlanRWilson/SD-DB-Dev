# Hardware Unit Tests (`db_main.c`)

[`source/app/src/db_main.c`](../source/app/src/db_main.c) runs the host `HashTableTest` suite
([`tests/test_hash_table.cpp`](../tests/test_hash_table.cpp)) on the NUCLEO-H723ZG against the
real SD card ([`sd_storage.c`](../source/app/src/sd_storage.c)) instead of the heap-backed mock.
Each `TEST_F` has a `test_<Name>()` function with the same checks. Build options are in
[build_options.md](build_options.md).

Last result (2026-10-10): **41/41 pass** with `DB_TEST_WRITE=ON` and **1/1** with `OFF`.

## How it runs

- `main.c` calls `DB_Init()`, which starts `dbTask` (16KB static stack).
- Before every test, `fixture_setup()` formats the usage bitmap and journal on the card, resets
  both free lists and calls `hash_init()` (the same as the gtest `SetUp()`). After every test,
  `fixture_teardown()` calls `hash_clear()`, which only touches RAM.
- `rebuild()` simulates a reboot: it empties the RAM table and contact allocator, reloads the
  usage bitmap from the card and calls `hash_reconstruct_contact()`.
- A failed `CHECK()` records the line and ends that test. The suite then moves on to the next
  test.
- Results go to the LCD ([`htest_ui.c`](../source/app/src/htest_ui.c)), to `g_db_report`
  (readable over SWD) and to the PB0 LED.

## Differences from the host suite

| Host | Hardware | Why |
|---|---|---|
| Fresh zeroed heap "card" per test | Usage bitmap + journal re-formatted on the card, old data sectors left behind | Nothing reads a sector the bitmap marks unused |
| `rebuild()` builds a second table | `rebuild()` reuses the same table | Two 14293-entry tables don't fit in RAM_D1 |
| `EXPECT_*` continues after a failure | First failed check ends the test | Simpler. The line number is reported |
| 4 scale tests (5000/10000 contacts, 5000 messages) | Not run | Host-only proof of concept; ~30 min over SD |
| No persistence test | `PersistSeed` / `PersistVerify` | Checks data survives a real power cycle |

## Tests

### hash_phone()
| # | Test | Checks |
|---|---|---|
| 1 | PhoneHashIgnoresNonDigits | Spaces, brackets and dashes don't change the hash; a different digit does |

### Contacts: insert / find / remove
| # | Test | Checks |
|---|---|---|
| 2 | InsertContact | Insert succeeds and `hash_size()` is 1 |
| 3 | FindContact | Found contact has the same name, phone and lengths as the one inserted |
| 4 | FindMissingContact | Looking up a phone that was never inserted fails |
| 5 | RemoveContact | Remove returns the contact's data, size drops to 0 and it's no longer findable |
| 6 | RemoveMissingContact | Removing an unknown phone fails |
| 7 | RemoveByPhoneRemovesContactAndMessages | `hash_remove()` deletes the contact and its chat, and returns the entry marked `ENTRY_DELETED` |
| 8 | RemoveByPhoneMissingFails | `hash_remove()` on an unknown phone fails and leaves the output pointer NULL |
| 9 | MultipleContacts | Three contacts are each found with the right name |
| 10 | DuplicateInsertUpdatesInPlace | Re-inserting a phone keeps the same sector and size and updates the name |
| 11 | PhoneHashCollisionResolved | `0400000601` / `0400002060` share a hash but get different sectors and are both found |
| 12 | RemoveFromCollisionChain | Removing one of a colliding pair leaves the other reachable past the tombstone |
| 13 | ReinsertAfterRemove | A phone can be inserted again after removal and is found with the new name |
| 14 | SizeTracksContacts | `hash_size()` follows inserts and removes |
| 15 | RejectsBadArguments | NULL table, contact or phone is rejected by insert, find and remove |

### hash_find_entry()
| # | Test | Checks |
|---|---|---|
| 16 | FindEntryMatchesStoredPhone | Hit gives the occupied entry with the right sector and id. A miss gives a free insertion slot |
| 17 | FindEntryDistinguishesCollidingPhones | Colliding phones resolve to two different occupied entries |

### create_message()
| # | Test | Checks |
|---|---|---|
| 18 | CreateMessageRejectsBadArguments | Timestamp 0 or NULL body gives an all-zero message |
| 19 | CreateMessageStoresContent | Timestamp, direction and body are copied |
| 20 | CreateMessageTruncatesOverlongBody | A 210-char body is cut to `SMS_MAX_MESSAGE_LENGTH - 1` |

### Messages: insert / find / remove
| # | Test | Checks |
|---|---|---|
| 21 | InsertFirstMessageCreatesContact | Messaging a new phone creates an unnamed contact, and the message is readable |
| 22 | InsertMessageAttachesToExistingContact | Messaging an existing contact doesn't create a duplicate or rename it |
| 23 | FindMessageReturnsLatest | `hash_find_message()` returns the newest message (timestamp, body, direction) |
| 24 | FindMessageMissingContactFails | No chat means the find fails |
| 25 | FindNMessagesWithinOneSector | Two messages come back newest-first from one sector |
| 26 | MessageChatRollsOverToNewSector | `MESSAGE_BLOCK_CAPACITY + 1` (3) messages spill onto a second sector, and the latest is still found |
| 27 | FindNMessagesAcrossSectorBoundary | `hash_find_n_message()` walks back across the sector link in newest-first order |
| 28 | RemoveMessageChatClearsMessagesOnly | Removing the chat leaves the contact and its name intact |
| 29 | RemoveMessageChatAcrossMultipleSectors | Removing a 2-sector chat clears all of it |
| 30 | RemoveMessageChatMissingContactFails | Removing a chat that doesn't exist fails |
| 31 | RejectsBadMessageArguments | NULL table, phone or message is rejected |

### hash_reconstruct_contact()
| # | Test | Checks |
|---|---|---|
| 32 | ReconstructFindsAllContacts | 5 contacts survive `rebuild()` with the right names and phones |
| 33 | ReconstructEmptyDatabase | Rebuilding an empty card gives an empty table |
| 34 | ReconstructSkipsRemovedContacts | A contact removed before the rebuild doesn't come back. The other two do |
| 35 | ReconstructPreservesCollisionChain | Both colliding contacts survive the rebuild |
| 36 | ReconstructedTableAcceptsNewWrites | Insert and remove still work on the rebuilt table |

### Contact lists
| # | Test | Checks |
|---|---|---|
| 37 | GetContactListReturnsFirstTenContacts | `hash_get_contact_list_by_usage()` page 0 of 10 returns exactly the 10 inserted phones |
| 38 | GetContactListReturnsSecondTenContacts | With 20 contacts, pages 0 and 10 cover every phone once (no duplicates, none missing) |
| 39 | GetContactListIterReturnsFirstTenContacts | As 37 for `hash_get_contact_list()` (iterator order) |
| 40 | GetContactListIterReturnsSecondTenContacts | As 38 for `hash_get_contact_list()` |

### Persistence (hardware only)
| # | Build | Test | Checks |
|---|---|---|---|
| 41 | `DB_TEST_WRITE=ON` | PersistSeed | Leaves Alice, Bob, Charlie, Dana and Erin on the card, plus a 3-message, 2-sector chat for Charlie (`0433333333`) |
| 1 | `DB_TEST_WRITE=OFF` | PersistVerify | No formatting. Journal recovery only, then rebuild contacts **and** chats from the card. Checks all 5 contacts and names, Charlie's chat in newest-first order with the right bodies, and that Alice has no chat |

`PersistSeed` must have run last for `PersistVerify` to pass. Power-cycle between the two
(unplug USB) to test real persistence. A reflash only resets the MCU.

## Troubleshooting

- **Messages, reconstruction and every test after them fail.** Check the SD clock first. At
  `ClockDiv = 3` (16MHz) on the Nucleo's wiring, reads of data with a `0x0→0xF` nibble come
  back shifted by one nibble (`HAL_SD_ERROR_DATA_CRC_FAIL`). `ClockDiv = 8` (6MHz, set in
  `firmware.ioc` and `sdmmc.c`) fixes it. Build with `-DSD_BUS_TEST=ON` to measure it again.
- **Every test fails with the same line (`run_test`'s setup line).** Fixture setup failed.
  `init_usage_bitmap()` or `journal_init()` couldn't access the card.
- **Test 1 fails before anything runs ("SD card init").** `SDStorage_Init()` failed: the card
  is missing or `MX_SDMMC1_SD_Init()` isn't being called in `main.c`.
- **The screen shows the old scrolling name list.** `fmcTask` is being created again in
  `freertos.c`. Only one task can own the LCD.
