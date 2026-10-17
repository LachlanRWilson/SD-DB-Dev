# db_cli: interactive database demo (no hardware)

`db_cli` runs the real `database.c` (host build, heap storage) behind a small command prompt. The "SD card" is an image file (`card.img` by default, 20.6 MB), so data survives `quit`, `reboot` and simulated power cuts. Every command prints the `STRG_RET` it got back and its cost in sector reads and writes, which makes the journalling overhead visible.

## Build and run
```sh
cmake -S . -B build-host
cmake --build build-host --target db_cli

build-host/source/demo/db_cli/db_cli                         # interactive, card.img in the current directory
build-host/source/demo/db_cli/db_cli --image demo.img \
    --script source/demo/db_cli/scripts/demo.txt             # scripted walk-through (starts with `format`)
```
Flags:
- `--image <file>`: card image to use.
- `--script <file>`: run commands from a file. Lines starting with `#` are printed as notes.
- `--interactive`: keep the prompt open after the script.
- `--echo`: echo typed commands (useful when piping input).

## Commands
| Command | What it does |
|---|---|
| `add <phone> <name...>` | add a contact |
| `get <phone>` | look up a contact |
| `update <phone> <new_phone> <name...>` | rename and/or change number (the chat follows) |
| `rm <phone>` | remove a contact and its chat |
| `list [start] [n]` | contacts in name order |
| `find <prefix>` | jump to the first name >= prefix (case insensitive) |
| `count` | number of contacts |
| `msg <phone> in\|out <text...>` | add a message (an unknown number becomes an unnamed contact) |
| `chat <phone> [start] [n]` | show a chat, oldest at the top |
| `rmchat <phone>` | delete a chat, keep the contact |
| `recent [n]` | latest messages across all chats |
| `call <phone>` / `calls [n]` | log a call / latest calls |
| `seed [n]` | bulk add n generated contacts (every 5th gets a short chat) |
| `reboot` | wipe RAM and mount from the card, like a power cycle |
| `powercut <writes> <command...>` | let `<writes>` sector writes land, drop the rest, then reboot |
| `format` | wipe the card to an empty database |
| `stats` | allocator and ring buffer usage |
| `help` / `quit` | |

Names and text can be quoted (`add 0400000001 "Amy Lee"`) or left unquoted (all remaining words are used).

## How it works
- On start, the image is loaded into `database.c`'s host `heap_mem`, then `database_init()` runs. A blank image formats itself.
- `card_image.c` then replaces the database's storage callbacks with wrappers. These count every access, copy each written sector to the image file (`pwrite`), and can drop writes after N to simulate a power cut.
- `database_init()` and `database_format()` write before the wrapper is installed, so the whole image is flushed after each one.

Nothing here is part of `app_logic`. The demo only calls the public `database.h` API, plus `read_contact`/`read_message_sector` to show who a recent message belongs to.
