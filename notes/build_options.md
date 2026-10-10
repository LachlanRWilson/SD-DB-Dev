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
`test_db_recovery`.

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

Persistence workflow: flash with `DB_TEST_WRITE=ON`, then rebuild with `OFF`, flash, and
power-cycle the board (unplug USB). The LCD should show `1/1 PASSED`.

```sh
cmake -S . -B build -DDB_TEST_WRITE=OFF && cmake --build build --target flash
cmake -S . -B build -DDB_TEST_WRITE=ON          # switch back afterwards
```

### Targets

| Target | What it does |
|---|---|
| `cmake --build build --target flash` | Build, then flash over SWD with `STM32_Programmer_CLI` (verify + reset). Uses `sudo` |
| `cmake --build build --target server` | Start an OpenOCD (ST-Link) server in a new gnome-terminal |
| `cmake --build build --target debug` | Start the server, then attach `gdb-multiarch` to `localhost:3333` |

Flashing without sudo:
`STM32_Programmer_CLI -c port=SWD -w build/firmware/MyProject.elf -v -rst`

### Reading results

- **LCD:** live test list and pass/fail summary (`htest_ui.c`).
- **SWD, without halting:** `g_db_report` (or `g_sd_bus_report` for the bus test). Get the
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
