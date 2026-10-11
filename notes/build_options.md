# Build Options

Two separate build directories: `build-host/` for the GTest suites on the PC, `build/` for the
STM32 firmware. CMake caches every `-D` option, so an option stays set until you pass it again.

## Host (GTest unit tests)

```sh
cmake -S . -B build-host            # configure (BUILD_STM32 defaults to OFF)
cmake --build build-host            # build app_logic + every test executable
```

| Command | What it does |
|---|---|
| `cmake --build build-host --target run_tests` | Run every suite through ctest (output shown on failure) |
| `N=<regex> cmake --build build-host --target run_tests` | Run only tests whose name matches `<regex>` (ctest `-R`), e.g. `N=HashTableTest` |
| `cmake --build build-host --target test_help` | List every test executable and the tests in it |
| `N=<Suite.Name> F=<test_file> cmake --build build-host --target test_debug` | Run one test under gdb. `F` is optional (auto-detected) |
| `ctest --test-dir build-host -C Debug [-V]` | Plain ctest, `-V` for verbose |
| `build-host/tests/test_hash_table --gtest_filter='HashTableTest.*'` | Run one executable directly with a gtest filter |

Test executables (see `tests/CMakeLists.txt`): `test_hash_table`, `test_free_list`,
`test_usage_bitmap`, `test_journal`, `test_ring_buffer`, `test_sector_crc`,
`test_db_recovery`, `test_message_history`.

## STM32 (on-hardware unit tests)

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON
cmake --build build
```

The Ninja generator and the toolchain file are required. Configuring with `BUILD_STM32=ON`
without them stops with a usage error.

### Options

| Option | Default | Effect |
|---|---|---|
| `-DDB_TEST_WRITE=ON` | ON | `db_main.c` runs the full hardware suite (formats the journal + usage bitmap and writes to the card), then leaves a known data set on the card (`PersistSeed`) |
| `-DDB_TEST_WRITE=OFF` | | Read-only persistence pass: `PersistVerify` rebuilds the table from what a previous `ON` run left on the card. No writes except journal recovery |
| `-DSD_BUS_TEST=ON` | OFF | `main.c` starts the SD bus diagnostic (`sd_bus_test.c`) instead of the DB tests. Writes scratch sectors from 1,000,000 up and reads them back under different bus settings |
| `-DPERSIST_DEMO=ON` | OFF | `main.c` starts the persistence demo (`persist_demo.c`) instead of the DB tests. A demonstration, not a test suite |
| `-DPERSIST_DEMO_WRITE=ON` | ON | Demo write half: formats the usage bitmap, journal and a 4-sector message history ring buffer, then writes 5 contacts and 5 messages (to the chats and the message history), showing each record on the LCD as it is written. Footer `WROTE 10/10` |
| `-DPERSIST_DEMO_WRITE=OFF` | | Demo read half: no writes except journal recovery. Rebuilds the hash table, chats and message history ring buffer from the card and shows them. Green = matches what the write half wrote, red = doesn't. Footer `READ 15/15 OK` |
| `-DMH_TEST=ON` | OFF | `main.c` starts the message history hardware unit tests (`mh_main.c`, port of `MessageHistoryTest`) instead of the DB tests. Uses an 8-sector ring buffer at the start of the message history region plus a guard sector after it. LCD shows the test list and `36/36 PASSED` |
| `-DMH_DEMO=ON` | OFF | `main.c` starts the message history demo (`mh_demo.c`) instead of the DB tests |
| `-DMH_DEMO_WRITE=ON` | ON | Demo write half: formats a 4-sector (12-message) history ring buffer and adds 16 messages, so it wraps and overwrites the oldest sector. Footer `WROTE 16/16` |
| `-DMH_DEMO_WRITE=OFF` | | Demo read half: no writes except journal recovery. Rebuilds the ring buffer from the card, then pages through it with `message_history_get_range()` 4 at a time, newest first, until it reports the end. Green = right message at that position. Footer `READ 10/10 OK` |

Persistence workflow: flash with `DB_TEST_WRITE=ON`, then rebuild with `OFF`, flash, and
power-cycle the board (unplug USB). The LCD should show `1/1 PASSED`.

```sh
cmake -S . -B build -DDB_TEST_WRITE=OFF && cmake --build build --target flash
cmake -S . -B build -DDB_TEST_WRITE=ON          # switch back afterwards
```

Persistence demo: flash the write half, then the read half, then power-cycle to show the data
coming back from the card alone.

```sh
cmake -S . -B build -DPERSIST_DEMO=ON -DPERSIST_DEMO_WRITE=ON  && cmake --build build --target flash
cmake -S . -B build -DPERSIST_DEMO=ON -DPERSIST_DEMO_WRITE=OFF && cmake --build build --target flash
cmake -S . -B build -DPERSIST_DEMO=OFF          # back to the db_main.c tests afterwards
```

The demo's write half wipes the data set left by `DB_TEST_WRITE=ON` (and vice versa).

Message history tests and demo:

```sh
cmake -S . -B build -DMH_TEST=ON && cmake --build build --target flash
cmake -S . -B build -DMH_TEST=OFF -DMH_DEMO=ON -DMH_DEMO_WRITE=ON  && cmake --build build --target flash
cmake -S . -B build -DMH_DEMO=ON -DMH_DEMO_WRITE=OFF && cmake --build build --target flash
cmake -S . -B build -DMH_DEMO=OFF          # back to the db_main.c tests afterwards
```

`main.c` picks the first option set in this order: `SD_BUS_TEST`, `PERSIST_DEMO`, `MH_TEST`,
`MH_DEMO`, else `db_main.c`. `MH_TEST`, `MH_DEMO` and `PERSIST_DEMO` all use the start of the
message history region, so each one wipes the others' history data.

### Targets

| Target | What it does |
|---|---|
| `cmake --build build --target flash` | Build, then flash over SWD with `STM32_Programmer_CLI` (verify + reset). Uses `sudo` |
| `cmake --build build --target server` | Start an OpenOCD (ST-Link) server in a new gnome-terminal |
| `cmake --build build --target debug` | Start the server, then attach `gdb-multiarch` to `localhost:3333` |

Flashing without sudo:
`STM32_Programmer_CLI -c port=SWD -w build/firmware/MyProject.elf -v -rst`

### Reading results

- **LCD:** live test list and pass/fail summary (`htest_ui.c`). The demos (`persist_demo.c`,
  `mh_demo.c`) draw a scrolling record list (`lcd_log.c`) and report only on the LCD.
- **SWD, without halting:** `g_db_report` (`g_mh_report` for `MH_TEST`, `g_sd_bus_report` for the
  bus test). Get the
  address with `arm-none-eabi-nm build/firmware/MyProject.elf | grep g_db_report`, then
  `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <addr> 0x100`. The layout is
  `DbTestReport` in `db_main.c`.
- **LED (PB0):** fast blink = all passed, N pulses = test N failed first. `StartDefaultTask`
  also toggles PB0, so this isn't reliable at the moment.

## Both builds

| Target | What it does |
|---|---|
| `--target misra` | cppcheck + MISRA C:2012 addon over `source/` (`-DMISRA_ADDON=<path>` to override the addon) |
| `--target tags` | ctags index of the project |
| `--target struct_layout` | Print struct layouts and static-assert results (`tools/struct_layout.py`, always host gcc) |
| `-DCMAKE_BUILD_TYPE=<type>` | Defaults to `Debug` |
