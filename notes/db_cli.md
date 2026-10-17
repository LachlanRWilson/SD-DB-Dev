# Database CLI (`db_cli`)

[`source/demo/db_cli/`](../source/demo/db_cli/) is an interactive command prompt for the database. It needs no hardware: it runs the real [`database.c`](../source/app/src/database.c) in the host build (`HOST_BUILD`, heap storage) and keeps the "SD card" in an image file. Use it to demo or poke at the database on a PC. For the on-board demo stepped with the USER button, see [`db_demo.c`](../source/app/demos/src/db_demo.c) (`-DDB_DEMO=ON`).

Nothing in the demo is part of `app_logic`. It only calls the public [`database.h`](../source/app/include/database.h) API, plus `read_contact`/`read_message_sector` to name the sender in `recent`.

## Build and run

```sh
cmake -S . -B build-host
cmake --build build-host --target db_cli

build-host/source/demo/db_cli/db_cli                     # interactive, uses ./card.img
build-host/source/demo/db_cli/db_cli --image demo.img \
    --script source/demo/db_cli/scripts/demo.txt         # scripted walk-through
```

| Flag | Meaning |
|---|---|
| `--image <file>` | Card image to use (default `card.img` in the current directory). It's created if missing, and a missing or blank image is formatted on first boot. It's always 20.6 MB (40,265 sectors). |
| `--script <file>` | Run the commands in a file, then exit. Lines starting with `#` are printed as notes. |
| `--interactive` | With `--script`, keep the prompt open after the script finishes. |
| `--echo` | Echo each command. Useful when piping input in (`printf '...' \| db_cli --echo`). Echo is on automatically when stdin isn't a terminal. |

## Output

Every command prints the `STRG_RET` it got back, then its storage cost:

```
db> add 0400000001 "Amy Lee"
  STRG_OK
  (0 reads, 7 writes, 0.18 ms)
```

The read and write counts are 512 B sector accesses made through the database's storage interface, so they show the journalling overhead directly. The time is host wall time, not target time.

| `STRG_RET` | Typical meaning in the CLI |
|---|---|
| `STRG_OK` | done |
| `STRG_EMPTY` | not found (contact, chat), or start is past the end of the list |
| `STRG_EXISTS` | phone number already in use |
| `STRG_FULL` | 10,000 contacts, or out of message sectors |
| `STRG_FAIL` | bad argument (empty or too long phone, name over 64 chars, timestamp 0), or a write failed (power cut) |
| `STRG_CORRUPT` | at boot: superheader corrupted or outdated, so run `format` |

## Commands

Names and message text can be quoted (`"Amy Lee"`) or left unquoted (all remaining words are used). Phone numbers are 1 to 14 characters.

### Contacts
| Command | Calls | Notes |
|---|---|---|
| `add <phone> <name...>` | `database_contact_add` | `STRG_EXISTS` if the phone is taken |
| `get <phone>` | `database_contact_get` | prints the contact |
| `update <phone> <new_phone> <name...>` | `database_contact_update` | rename and/or change number. To rename only, repeat the same phone. The chat follows a number change. |
| `rm <phone>` | `database_contact_remove` | also deletes the contact's chat |
| `list [start] [n]` | `database_contact_list` | contacts in name order from rank `start` (default 0, 20 rows). Unnamed contacts sort first. |
| `find <prefix>` | `database_contact_seek` | rank of the first name >= prefix (case insensitive), then the next 10 rows |
| `count` | `database_contact_count` | |

### Messages and calls
| Command | Calls | Notes |
|---|---|---|
| `msg <phone> in\|out <text...>` | `database_message_add` | `out` = sent, `in` = received. The timestamp is the current time (always increasing). An unknown number becomes an unnamed contact. |
| `chat <phone> [start] [n]` | `database_message_get_chat` | oldest at the top, like a chat window. `start` counts back from the newest. |
| `rmchat <phone>` | `database_message_remove_chat` | deletes the chat and keeps the contact |
| `recent [n]` | `database_message_recent` | newest messages across every chat (max 20), with the contact's name |
| `call <phone>` | `database_call_add` | an unknown number is logged by number only and isn't added as a contact |
| `calls [n]` | `database_call_recent` | newest calls first (max 20) |

### Card and testing
| Command | What it does |
|---|---|
| `seed [n]` | Bulk-adds `n` generated contacts (default 100). Phones are `04` + 8 digits, and every 5th contact gets a two-message chat. Prints progress every 1,000. Running it again reports `already existed`. |
| `reboot` | A power cycle: wipes the RAM state (`Database` struct, usage bitmap) and runs `database_init()` against the image. Prints the mount time. |
| `powercut <writes> <command...>` | Lets `<writes>` sector writes land, makes every later write fail, runs the command, then reboots. Use it to show journal rollback. |
| `format` | `database_format()`: wipes the card to an empty database |
| `stats` | contacts, name index size, contact and message allocator use, history ring use, card size |
| `run` | full screen phone UI, see [Phone UI](#phone-ui-run) |
| `help`, `quit` | |

## Example session

```
db> add 0400000001 "Amy Lee"
  STRG_OK
  (0 reads, 7 writes, 0.18 ms)
db> add 0400000001 Someone
  STRG_EXISTS
  (1 reads, 0 writes, 0.02 ms)
db> msg 0400000001 out Hi there
  STRG_OK
  (4 reads, 10 writes, 0.34 ms)
db> msg 0499000000 in Unknown sender
  STRG_OK
  (3 reads, 17 writes, 0.62 ms)
db> list
      0  (unnamed)                        0499000000
      1  Amy Lee                          0400000001
  2 shown of 2 (STRG_OK)
db> powercut 2 add 0400000002 Half Written
  power cut after 2 sector writes, running: add 0400000002 Half Written
  STRG_FAIL
  -- power lost: 2 writes landed, 1 dropped --
  boot: STRG_OK, 2 contacts (8.1 ms)
db> stats
  contacts          2 / 10000
  name index        2 entries
  contact slots     2 used, 9998 free
  message sectors   2 used, 28584 free
  message history   1 / 5000 sectors
  call history      0 / 5000 sectors
  card              40265 sectors (20.6 MB)
```

The half-written contact is gone after the power cut: the journal rolled it back at boot. For contact slots, used + free always equals 10,000 (this is the reconstruction bug fixed on 2026-10-17).

## Phone UI (`run`)

`run` switches to a full screen rendering of the UniQOS phone menus ([`../UniQOS/ui/pages`](../../UniQOS/ui/pages)), backed by the database. The code is in [`phone_ui.c`](../source/demo/db_cli/phone_ui.c).

```
┌──────────────────────────────────┐
│ 20:09                     UniQOS │
├──────────────────────────────────┤
│            Zara Patel            │
├──────────────────────────────────┤
│ Name                             │
│  Zara Patel                      │
│ Phone                            │
│  0400000003                      │
│                                  │
│>Messages                         │
│ Call                             │
│ Send Message                     │
│ Edit Contact                     │
│ Delete Contact                   │
├──────────────────────────────────┤
│ Select                      Back │
└──────────────────────────────────┘
  ↑↓ move  ⏎ select  ⌫ back  q quit
  last key: 2 reads, 0 writes
```

| Screen | Source | Actions |
|---|---|---|
| Menu | | Phone, SMS, Contacts |
| Phone | | Call (prompts for a number), Call History |
| Call History | `database_call_recent`, all of it | open contact details (`unknown` = number not a contact) |
| SMS | | New Message (number, then text), All Messages |
| All Messages | `database_message_recent` → each entry's sector header → contact; latest from `database_message_get_chat` | one row per chat, newest first; open chat |
| Contacts | `database_contact_list` for the 10 visible rows only | `/` prefix search (`database_contact_seek`), open details |
| Contact details | `database_contact_get` | Messages, Call, Send Message, Edit Contact, Delete Contact (Add Contact for an unknown number) |
| Chat | `database_message_get_chat`, all of it | Enter replies (sent), `i` adds a received message, ↑ scrolls back |

Keys: arrows (or `hjkl`/`wasd`) move, `←`/`→` page by 10, Enter selects, Backspace/Esc goes back, `q` returns to `db>`. Text is typed on a normal line under the screen. The line `last key` is the sector reads/writes the last key press cost (including reloading the screen), and the last action's `STRG_RET` is shown below it.

All Messages has no chat list API to call, so it walks the whole message history: a few sector reads per history entry whose chat isn't already listed. Entries left by a deleted chat are skipped because their chat no longer resolves. `run` needs a terminal; with piped input it prints `` `run` needs an interactive terminal ``.

## Scripted demo

[`scripts/demo.txt`](../source/demo/db_cli/scripts/demo.txt) starts with `format`, so its output is the same every run. It has 10 steps:

1. contacts added out of order, listed alphabetically (names sharing the 8 character key)
2. duplicate phone → `STRG_EXISTS`
3. prefix search
4. chats, each contact separate
5. message from an unknown number → unnamed contact, `recent`
6. calls, known and unknown
7. rename + number change, chat follows
8. `reboot`, everything rebuilt
9. `powercut 2 add ...` → rolled back
10. `rm` (chat removed too) and `stats`

## How it works

```
db_cli.c ──► database.c ──► Storage interface ──► card_image.c wrapper ──► heap_mem (RAM)
                                                        │                       │
                                                        └── pwrite(sector) ─────┴──► card.img
```

- **Load:** [`card_image.c`](../source/demo/db_cli/card_image.c) reads the image into `database.c`'s host `heap_mem` (declared `extern` there, the same as `tests/test_database.cpp`). Then `database_init()` mounts it, or formats it if blank.
- **Wrapper:** after every `database_init()`/`database_format()`, the CLI swaps `g_db.storage`'s four block callbacks for wrappers. These count sector reads and writes, can drop writes after N (the power cut), and `pwrite` each written sector to the image at the same offset. The image therefore always matches what an SD card would hold, even if the process is killed.
- **Flush after boot:** `database_init()`/`database_format()` write before the wrapper is reinstalled (format, journal rollback), so the whole image is written out after each boot. The writes a boot makes aren't included in the command's write count.
- **Reboot:** clears the RAM structures and remounts from `heap_mem`, which holds exactly the writes that landed. This is the same thing a power cycle does on the board.

## Limitations

- `run` uses ANSI escapes and box drawing characters, so it needs a UTF-8 terminal at least 36 columns by 22 rows.
- The demo only runs on the host. The firmware equivalent is `db_demo.c`.
- Times are host times. For target timings, see the `DATABASE_TEST` hardware suite ([`database_main.c`](../source/app/hw_tests/src/database_main.c), about 64 ms per contact add at 520 MHz with the worst-case tiebreak).
- `chat`, `recent` and `calls` show at most 20 rows per call. Use `start` to page through more.
- The image size follows `mem_layout.h`. If the layout changes, an old image mounts with the same superheader version but reads the wrong sectors. Run `format` (or delete the image) after a layout change.
