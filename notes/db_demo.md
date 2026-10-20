# Hardware Database Demo (`db_demo.c`)

[`source/app/demos/src/db_demo.c`](../source/app/demos/src/db_demo.c) walks through the whole database ([`database.c`](../source/app/src/database.c)) on the NUCLEO-H723ZG, using the real SD card and the LCD. It shows one section per screen, and the blue **USER** button steps to the next. It's written for showing an examiner every part working. It isn't a test suite (that's [`database_main.c`](../source/app/hw_tests/src/database_main.c), `-DDATABASE_TEST=ON`), but every step still prints a green **OK** or red **FAIL**, and the last screen totals them.

For a PC version with no board, see the CLI in [db_cli.md](db_cli.md).

Once the card is mounted, the demo starts the DB task ([`db_task.c`](../source/app/src/db_task.c)) on its database and makes every database call as a request, waiting for the response on its own reply queue. This is the same request/response path as the CLI's phone UI ([`phone_ui.c`](../source/demo/db_cli/phone_ui.c)). Only the mount (`boot()`), the card reset and HISTORY's sender lookup touch the card directly, and they run while the DB task is waiting for its next request.

Last result (2026-10-20, 520 MHz, SD at 6 MHz): **64/64 checks**, all 14 sections, 793 requests to the DB task. The DB task's 4 KB stack had 976 B free at its lowest point.

## Build and flash

```sh
cmake -S . -B build -DDB_DEMO=ON -DDATABASE_TEST=OFF
cmake --build build --target flash        # or: STM32_Programmer_CLI -c port=SWD -w build/firmware/MyProject.elf -v -rst
cmake -S . -B build -DDB_DEMO=OFF         # back to the db_main.c tests afterwards
```

`main.c` only starts the demo if no earlier option in its list is set, so turn the other test/demo options off (see [build_options.md](build_options.md)).

## Controls

| Action | Effect |
|---|---|
| Reset (black button) | Start screen. The card is reset to an empty database (about 1 s) |
| **Hold blue while pressing reset** | Full `database_format()` instead (about 35 s, clears every history ring sector) |
| Blue button | Next section. The footer shows `BLUE: next n/14` while waiting and `running...` while a section runs |
| Reset after the summary | Run again |

The button is PC13 (B1, pressed = high). The demo sets it up itself (input, pull-down), so `firmware.ioc` is unchanged. A press must read high on 5 samples in a row, 10 ms apart (`button_held()`). PC13 sits next to the SD clock pin (PC12), and with a looser debounce, sections once advanced on their own.

## Screen

```
+------------------------------+
| 5/14 MESSAGES                |  section n/14 and title
|------------------------------|
| alice's chat (3 per sector): |  dim: what's happening
|  -> Lunch tomorrow?          |  white: data read back from the card
| ...                          |
| OK   alice: 4 msgs, 2 sectors|  green OK / red FAIL: a check
|------------------------------|
| BLUE: next 6/14              |  footer
+------------------------------+
```

The log scrolls once a section has more than 20 rows. `+ name OK 7w` means the add returned `STRG_OK` and cost 7 sector writes (journal, contact sector, usage bitmap, journal free).

## Sections

The data set is 8 contacts added in scrambled order (`g_people`):
- names sharing the 8 character key: Alexander Jones / Alexandra Lee
- a lower case name: alice Brown
- the same name twice: Bob Smith ×2, ordered by phone

| # | Section | What it shows | Checks |
|---|---|---|---|
| 0 | START | clock, SD settings, card reset or format, mount time | empty database ready |
| 1 | CONTACTS | 8 adds, each with its write count (7 per add) | 8 adds OK, 8 stored |
| 2 | LIST | the list from the name index (sorted array) | alphabetical order (case-insensitive, same name by phone); index matches hash table |
| 3 | SEARCH | `"al"` → 0, `"B"` → 3, `"da"` → 6, `"zz"` → 8 (past every name), with the rows found | 4 ranks |
| 4 | RULES | duplicate phone, empty phone, 65 character name, unknown number | `EXISTS`, `FAIL`, `FAIL`, `EMPTY`; nothing changed. The request's name field holds 64 characters, so the demo's request wrapper rejects the long name rather than truncating it |
| 5 | MESSAGES | alice's 4 messages over 2 message sectors (3 per sector), bob's separate chat, an SMS from an unknown number | 4 msgs; bob separate; unnamed contact created and listed first |
| 6 | HISTORY | 6 newest messages across all chats, with the sender's name from the chat header | 6 newest; newest is the parcel SMS |
| 7 | CHATS | the chat list (chat order): newest chat first. Bob replies, then Dana and Charlie start chats | ordered by latest message; bob moved to the top; new chats go on top |
| 8 | CALLS | Zoe, an unknown number, Charlie | 3 newest first; known caller named; unknown caller has no contact index and isn't added |
| 9 | UPDATE | Zoe Park → Aaron Park (moves near the top); alice → new number | renamed; old number gone; chat followed (4 msgs); alice's chat kept its place in the chat order |
| 10 | DELETE | remove the first Bob Smith; remove only the parcel chat; final list and chats | contact and chat gone; other Bob kept; chat removed, contact kept; list matches `g_final`; both chats left the chat order |
| 11 | REBOOT | RAM wiped, `database_init()` mounts from the card | mount OK (time shown); same list; chats intact; deleted chat still gone; chat order rebuilt (heapsort by latest message); contact free list exact (used + free = 10,000); call history intact |
| 12 | POWER CUT | a normal add's write count, then power cut after 2 writes, then after 6 | half add rolled back with no trace; list consistent; the after-6 add is either fully there or fully gone |
| 13 | SCALE | 200 more contacts ("Contact 000"…, all on the tiebreak path), progress every 50, then a reboot | 200 added (ms per add shown); mount with every contact; search `"contact 1"`; consistent |
| 14 | SUMMARY | feature list, contacts on the card, total sector reads and writes, DB task requests and stack free, demo time | final `64/64 OK` |

The power cut is simulated by a storage wrapper that makes every write after the Nth fail, followed by a real remount. It isn't an actual loss of power, but the card ends up in the same state.

### Measured per section (2026-10-17)

| Section | Sector reads | Sector writes |
|---|---|---|
| CONTACTS | 7 | 56 |
| LIST | 40 | 0 |
| MESSAGES | 41 | 80 |
| CALLS | 9 | 12 |
| UPDATE | 18 | 14 |
| DELETE | 30 | 15 |
| POWER CUT | 62 | 15 |
| SCALE | 1,775 | 1,400 |
| **Total** | **2,019** | **1,592** |

SCALE takes about 13 s (about 64 ms per add with the worst-case tiebreak). Every other section finishes in a second or two.

## How it starts (card reset)

A full format clears about 10k ring buffer sectors (about 35 s), which is too slow to sit through. Without the button held, `quick_reset()`:
1. writes an all-unused usage bitmap
2. writes a fresh journal header (`journal_header_init`)
3. clears the first 64 sectors of each history ring (`clear_ring_buffer`)

Then it mounts with `database_init()`. Old contact and message sectors stay on the card, but nothing reads a sector the bitmap marks unused. If the mount still fails (a blank card or an older layout), it falls back to `database_format()`.

This wipes whatever another test or demo left on the card. The card holds the demo's data set afterwards, so a reset brings it back to empty.

## Implementation notes

- Task: `DB_Demo_Init()` → `demoTask` (8 KB static stack). It uses the RAM_D1 pools in `database.c` (about 290 KB, 90% of RAM_D1).
- DB task: started by `start_task()` with `db_init(&g_db)` after the first mount, and kept for the rest of the run (a later `boot()` remounts the same `g_db` under it). Requests go through `db_call()`, copied from `phone_ui.c`: it tags each request and waits on a one-slot reply queue. `g_requests` counts the replies, `g_stack_free` is the DB task's lowest free stack (set on the summary screen).
- A storage wrapper is installed over `g_db.storage` after every mount. It counts sector reads and writes, and implements the power cut through `g_writes_left`.
- Message timestamps are a counter (`g_ts`), since there's no RTC.
- Counters for checking without the LCD: `g_ok`, `g_bad`, `g_reads`, `g_writes`, `g_requests`, `g_stack_free`. Get the addresses with `arm-none-eabi-nm build/firmware/MyProject.elf | grep -E ' g_(ok|bad|reads|writes|requests|stack_free)$'`, then read with `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <addr> 4`.
- **Stepping without pressing the button** (as used for the 64/64 run): over SWD, set PC13's pull to up and then back to down:
  ```sh
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -w32 0x5802080C 0x05010000   # press  (GPIOC PUPDR, PC13 pull-up)
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -w32 0x5802080C 0x09010000   # release (PC13 pull-down)
  ```
  Read GPIOC_PUPDR first (`-r32 0x5802080C 4`) and only change bits 27:26. The values above are for the current pin setup.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `FAIL SD card reset`, footer `SD FAIL` | Card missing or not responding. Check the card and wiring; see [build_options.md](build_options.md) `SD_BUS_TEST` |
| Long `formatting...` at start | The blue button was held at reset, or the card was blank or in an older layout. This is expected |
| Sections advance by themselves | Button noise. Check that the debounce (`BTN_SAMPLES`) wasn't reduced |
| A red FAIL | The line names the check. The same behaviour is covered by the `DATABASE_TEST` suite, so run that to narrow it down |
