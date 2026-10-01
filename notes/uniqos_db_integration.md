# UniQOS ↔ DBMS integration: hardcoded UI data and DB access patterns

## Context
The UniQOS UI shows contacts and SMS from hardcoded arrays and string literals. The SD-card DBMS in `thesis/dev` (hash table keyed by phone number) needs to run behind a FreeRTOS task (`kernel/tasks/db_task.c`, which is currently a stub). The UI should get its data through **get / set / list** requests for contacts and messages, keyed by phone number.

## 1. Where the UI uses hardcoded data (UniQOS)

| Location | What is hardcoded | Replace with |
|---|---|---|
| `ui/pages/contacts/contacts.c:17-53` | `names[]` / `phones[]` arrays (16 entries) | **LIST contacts** (paged, `CONTACTS_VISIBLE_COUNT` = 9 at a time) |
| `contacts.c:96, 103` (`contacts_draw_tile`) | `sizeof(names)` bound + `names[item_index]` | contacts held in the page's state from the last LIST response, plus a `total`/`has_more` flag |
| `contacts.c:185` | cursor `max_y = 15` (array length − 1) | set from the count in the LIST response |
| `contacts.c:143-155` (SELECT) | builds a `ContactRecord` from `names[]` and `phones[]` (and `phone_len` is wrong: `SMS_MAX_PHONE_LENGTH - 2`) | use the cached contact, or **GET contact by phone** → push `contact_details_page_create` in `data_response` |
| `contacts.c:174-179` | `contacts_data_request` / `contacts_get_page` stubs that are never called | delete them; use `screen_request` directly |
| `ui/pages/contacts/contact_details.c:115-120` | "Edit Contact" and "Delete Contact" do nothing | **SET contact** (and a delete if you add one) |
| `ui/pages/sms/sms.c:131-137` | "All Messages" opens one hardcoded `.sender="1234567890"` / long `.message` | **LIST messages** (conversations, or the latest N for a phone) |
| `sms.c:139` | "Favourites" opens a fake incoming-text overlay for `"1234567890"` | remove it, or turn it into a favourites LIST |
| `ui/pages/sms/messages.c` (`MessagePageState`) | shows one message passed in by value | page takes a phone number and sends **LIST messages(phone, n)** on create |
| `ui/pages/phone/phone.c:142` | test incoming call from `"1234567890"` | optional: **GET contact** to show the caller's name |
| `kernel/tasks/mm_task.c:58-83` (`incoming_text_callback`) and `handle_show_sms` | received SMS is only shown, never stored | **SET message** (append, direction = received) before showing |
| `ui/pages/sms/new_sms.c:46-51` → `mm_task.c:299-311` | sent SMS goes to the cellular task only | also **SET message** (append, direction = sent) |
| `ui/pages/phone/call.c:44` | "Add to Contacts" option has no handler | **SET contact** |

`menu.c:17` includes `contacts_bptree.h` but doesn't use it. Once the UI moves to the new DB, `contacts_bptree.{c,h}` and `ContactRecord` can be retired.

## 2. Existing plumbing to reuse (no new mechanism needed)
- **Page → task:** `screen_request(type, req)` (`ui/screen.c:117`). It is a single slot where the latest request wins. `mm_task.c:278` polls it each loop and forwards the request with `*_PostCommand`.
- **Task → page:** the task posts `MiddleManagerTask_PostCommand(mm, MANAGER_X, data)`. The handler in `mm_task.c` calls `screen_handle_response(PAGE_RESPONSE_X, data)`, which calls `current_page->data_response`.
- **Reference round trip already working:** battery. `power_page.c:45` `screen_request(PAGE_REQUEST_BATTERY_HC)` → `mm_task.c:313` → `PowerTask_PostCommand` → `MANAGER_SET_BATTERY_PAGE` → `handle_set_battery_page` → `screen_handle_response`. `call.c:345` (`call_data_response`) shows a page consuming a response. The DB requests copy this pattern exactly.

## 3. Changes

### 3a. Shared request/response types — new `include/kernel/db_types.h`
Keep the UI decoupled from the DBMS internals (`ContactBuffer`, `MessageBuffer`, sector types):
```c
typedef struct { char name[MAX_NAME_LEN]; char phone[SMS_MAX_PHONE_LENGTH+1]; } DbContact;
typedef struct { uint16_t timestamp; bool outgoing; char body[SMS_MAX_MESSAGE_LENGTH+1]; } DbMessage;

typedef struct {               // request (UI -> DB), owned by the caller as static/page state
    char     phone[SMS_MAX_PHONE_LENGTH+1];
    uint16_t start, count;     // for LIST
    union { DbContact contact; DbMessage message; } payload;  // for SET
} DbRequest;

typedef struct {               // response (DB -> UI), static buffer owned by db_task
    int      status;           // 0 ok, <0 error (not found, full, io)
    uint16_t count;            // items filled
    union { DbContact contacts[CONTACTS_VISIBLE_COUNT]; DbMessage messages[DB_MSG_PAGE]; DbContact contact; };
} DbResponse;
```

### 3b. `include/ui/screen.h`
- Add to `PageDataRequest`: `PAGE_REQUEST_DB_CONTACT_GET`, `PAGE_REQUEST_DB_CONTACT_SET`, `PAGE_REQUEST_DB_CONTACT_LIST`, `PAGE_REQUEST_DB_MESSAGE_GET`, `PAGE_REQUEST_DB_MESSAGE_ADD`, `PAGE_REQUEST_DB_MESSAGE_LIST`.
- Add the matching `PAGE_RESPONSE_DB_*` entries to `PageDataResponse`.

### 3c. `include/kernel/tasks/db_task.h` / `kernel/tasks/db_task.c`
- Replace `DATABASE_REQUEST` with `DB_CMD_CONTACT_GET/SET/LIST` and `DB_CMD_MESSAGE_GET/ADD/LIST`.
- **Implement** `DatabaseTask_PostCommand`. It is declared but missing, so it would fail to link. Copy `MiddleManagerTask_PostCommand`, `mm_task.c:376`.
- Add `DatabaseTask_SetManagerContext(DBTaskContext*, MMTaskContext*)`.
- Fix the copy-pasted thread name `"Middle Manager Task"` → `"Database Task"`.
- Main loop: after `sdcard_init()`, call the DB init (`database_init` / hash-table reconstruct from `thesis/dev`). Replace `osDelay(3)` with blocking `xQueueReceive(ctx->queue, &msg, portMAX_DELAY)` and a command table like `mm_cmd_table`.
- Each handler copies the request first, because `screen_request`'s data can be overwritten. It then calls the DBMS, fills a `static DbResponse`, and posts `MANAGER_DB_RESPONSE` back to the MM task with `{response_type, &resp}`.
- DBMS calls to wire in (from `thesis/dev/source/app/include/hash_table.h`; it's the phone-keyed API and is newer than `database.h`):
  - contact GET → `hash_find_contact(table, phone, &ContactBuffer)`
  - contact SET → `hash_insert_contact(table, journal, &ContactBuffer)` (`create_contact(name, phone)` in `contact.h:144`)
  - contact LIST → `hash_get_contact_list(table, start, n, ContactBuffer*)`
  - message GET (latest) → `hash_find_message(table, phone, &MessageBuffer)`
  - message ADD → `hash_insert_message(table, journal, phone, &MessageBuffer)` (`create_message(ts, dir, str)` in `message.h:232`)
  - message LIST (per phone) → `hash_find_n_message(table, phone, n, MessageBuffer*)`
  - conversation list (for "All Messages") → `hash_table_iterator_*` over entries that have a message index. This is still WIP in the DBMS (latest commit "iterator … needs to be added to hash table"), so stub it until the iterator lands.
- Convert `ContactBuffer`/`MessageBuffer` ↔ `DbContact`/`DbMessage` here, and only here.
- Stack: `DB_TASK_STACK_SIZE 2048` is tight with 512 B sector buffers, journal and list buffers. Raise it to about 4096, or make the buffers `static`.

### 3d. `kernel/tasks/mm_task.c` / `mm_task.h`
- Add `DBTaskContext *db_ctx` to `MMTaskContext` plus `MiddleManagerTask_SetDatabaseContext`.
- In the `screen_get_pending_request` switch (`mm_task.c:281`), map each `PAGE_REQUEST_DB_*` to `DatabaseTask_PostCommand(ctx->db_ctx, DB_CMD_*, request_data)`.
- Add a `MANAGER_DB_RESPONSE` command whose handler calls `screen_handle_response(resp->type, resp->data)` (same as `handle_set_battery_page`).
- SMS persistence: in `PAGE_REQUEST_SMS_SEND` also post `DB_CMD_MESSAGE_ADD` (outgoing). In `handle_show_sms` post `DB_CMD_MESSAGE_ADD` (incoming).
- Also fix the missing `break` after `PAGE_REQUEST_BATTERY_HC` (`mm_task.c:316`). It falls through to `default`, which is harmless now but will bite once cases are added.

### 3e. `kernel/core/kernel.c`
- `db_ctx = DatabaseTask_Init();`, then `DatabaseTask_SetManagerContext(db_ctx, mm_ctx)` and `MiddleManagerTask_SetDatabaseContext(mm_ctx, db_ctx)`. `db_task.c` is already in `kernel/Makefile`.
- Add the DBMS sources from `thesis/dev/source/app/src` (contact, message, hash_table, journal, free_list_stack, usage_bitmap, storage, sd_storage, superheader, crc, iterator, ring_buffer) to the Makefile. Either vendor them under `UniQOS/kernel/db/` or reference them as a submodule. Make `sd_storage` use UniQOS's `drivers/peripherals/sdcard.c`.
- Watch for clashing macros: `MAX_NAME_LEN` / `MAX_PHONE_LEN` exist in both `contacts_bptree.h` and the DBMS `mem_layout.h`. Drop the `contacts_bptree.h` includes from `contacts.h`, `contact_details.h` and `menu.c`.

### 3f. UI pages
- **contacts.c**
  - State gets `DbContact items[CONTACTS_VISIBLE_COUNT]; uint16_t count; bool loading;`.
  - `contacts_page_create` / `update_page_offset` set a static `DbRequest{start=page_offset, count=9}` and send `screen_request(PAGE_REQUEST_DB_CONTACT_LIST, &req)`.
  - New `contacts_data_response`: copy the contacts into state, set `cursor.max_y`, `mark_all_tiles_dirty()`.
  - `draw_tile` reads `state->items[ty]`.
  - SELECT builds the details page from `items[cursor.y - page_offset]`.
- **contact_details.c**: switch to `DbContact`. "Edit" and "Delete" post `PAGE_REQUEST_DB_CONTACT_SET` (and delete).
- **messages.c**
  - `messages_page_create(const char *phone)` sends `PAGE_REQUEST_DB_MESSAGE_LIST{phone, count=N}`.
  - `data_response` stores the messages and redraws.
  - The "From:" line can do a CONTACT_GET to show the name instead of the number.
- **sms.c**: "All Messages" pushes a conversation-list page (a new page like `contacts.c`, backed by MESSAGE_LIST without a phone). Remove the hardcoded literal.
- **mm_task.c `incoming_text_callback`**: open `messages_page_create(sms->sender)`.

**Important pitfall:** `screen_request` keeps only one slot, and the response always goes to the *current* page. So:
1. Never send two requests in the same tick. Chain them: send the second from the first's `data_response`.
2. Every `data_response` must check `type` before using `resp`, because a page can get a response meant for a page that was popped.

## 4. DBMS access patterns: needed vs currently implemented
Source: `thesis/dev/source/app/src/hash_table.c` (working tree, including uncommitted changes as of 2026-10-01). Everything is keyed by phone number through `hash_find_entry` (DJB2 + double hashing).

| # | Access pattern the UI needs | Used by | DBMS function | Status |
|---|---|---|---|---|
| 1 | **GET contact** by phone | contact details, caller/sender name on incoming call/SMS overlays, "From:" in messages | `hash_find_contact(table, phone, out)` | ✅ Implemented |
| 2 | **SET contact** (add or edit) by phone | Edit Contact, "Add to Contacts" from call page | `hash_insert_contact(table, journal, contact)` | ✅ Implemented. It upserts: if the phone exists, it overwrites in place. |
| 3 | **DELETE contact** by phone | Delete Contact | `hash_remove_contact(table, journal, phone, out)` | ✅ Implemented |
| 4 | **LIST contacts**, paged (`start`, `n` = 9) | contacts page | `hash_get_contact_list(table, start, n, out)` via `get_nth_contact_index` | ⚠️ Implemented, but returns contacts in **SD sector/storage order, not alphabetical**. The old B+ tree gave name order. Each page also rescans the usage bitmap from the start, so cost is O(start). |
| 5 | **LIST contacts sorted by name** / name prefix search | contacts page (expected UX), new-SMS recipient picker | none | ❌ Not implemented. Needs a sorted secondary index (name → phone) or sorting in RAM. |
| 6 | **GET latest message** for a phone | conversation-list preview line | `hash_find_message(table, phone, out)` | ✅ Implemented (reads position 0 of `latest_msg_extent`) |
| 7 | **ADD message** to a phone (sent/received) | `PAGE_REQUEST_SMS_SEND`, incoming SMS in `handle_show_sms` | `hash_insert_message(table, journal, phone, in)` | ✅ Implemented. An unknown number auto-creates a contact with an **empty name**, so the contacts list must show the phone when `name_len == 0`, or filter those out. |
| 8 | **LIST messages for a phone**, latest N | messages (conversation) page | `hash_find_n_message(table, phone, n, out)` → `read_n_messages` | ⚠️ Partial. Only "latest N" works; there is **no start offset**, so the UI can't page back through older history. Needs a `start` parameter or a `ring_buffer`/extent iterator. |
| 9 | **LIST conversations** (phones that have messages, most recent first) | SMS "All Messages" | `hash_table_iterator_*` | ❌ Not usable yet. The iterator walks hash slots in **slot order**. It returns only the slot index (`uint16_t`), not contact or message data. It also includes entries with no messages (`latest_msg_extent == UINT16_MAX`). Needs: filter to entries with messages, return phone and name, and order by recency (e.g. a ring buffer of recent conversation phones, which matches the WIP `ring_buffer.c`). |
| 10 | **DELETE conversation** / single message | future SMS options | `hash_remove_message(table, journal, phone, out)` | ✅ Implemented in `hash_table.c`. `remove_message` / `add_message` in `message.h` are declared but **not implemented**. |
| 11 | **Count** contacts / messages for a phone | scroll bounds (`cursor.max_y`) | `hash_size` (contacts) | ⚠️ Contacts ✅. There's no per-phone message count in `hash_table.c`. `database_message_count` is in `database.h`, which is the older ID-keyed API. |
| 12 | **Boot reconstruct** (rebuild RAM hash table from SD) | `db_task` startup | `hash_reconstruct_contact`, `hash_reconstruct_message` | ⚠️ Both parts are implemented, but the top-level `hash_reconstruct()` declared in `hash_table.h` has **no definition**. Call the two parts directly or implement it. |
| 13 | **Call history** (recent calls) | `ui/pages/phone/call_logs.c` (empty file) | `ring_buffer.c` (WIP) | ❌ Not implemented |

**Other DBMS-side notes that affect the integration**
- `Message.timestamp` is a `uint16_t`. That can't hold an RTC date/time, so widen it or store an epoch offset before the UI shows message times.
- `database.h` (ID-keyed `database_contact_get(db, uint16_t id, …)`) and `hash_table.h` (phone-keyed) overlap. Pick `hash_table.h` as the API for `db_task` and treat `database.h` as legacy, or update it to wrap the phone-keyed calls.
- `hash_table.h` and `hash_table_phone.h` both declare `hash_find_entry`, `hash_init` and others with different signatures. Only include `hash_table.h` in UniQOS.

**Priority for the UI integration:** #4 and #7 unblock the contacts page and SMS persistence now. #8 (offset) and #9 (conversation list) are needed before "All Messages" can stop being hardcoded. #5 (sorted names) is a UX improvement, not a blocker.

## 5. Verification
1. **Host unit tests:** the DBMS already has them in `thesis/dev/tests`. Add a test for the `ContactBuffer ↔ DbContact` conversion helpers.
2. **Build:** `make -C UniQOS/kernel all` builds cleanly. Check that `DatabaseTask_PostCommand` links and there are no macro redefinition warnings.
3. **On target:**
   - Seed the SD card by temporarily posting `DB_CMD_CONTACT_SET` for the old 16 names on boot.
   - Open Contacts. It should show 9 names, and scrolling past 9 should trigger a new LIST and show the next page.
   - Select a contact. The details page should show the right number.
   - Send an SMS from new_sms. Reopen that contact's messages and check the sent message appears.
   - Receive an SMS. Press Open on the overlay and check the conversation shows it.
   - Power-cycle. Contacts and messages should still be there, which proves the hash table reconstructs.
4. **Debugging:** use `make debug` (gdb via openocd) with a breakpoint in the DB handlers to confirm the round trip MM → DB → MM → `data_response`.
