# Database API benchmark

Timings of the public `database_*` calls (`source/app/include/database.h`), measured through the journal and storage layers. **This file contains measurements only, not a recommendation.** Each benchmark is added after it has run on both the host and the target.

| Benchmark | Platform | Status | File |
|---|---|---|---|
| contacts | Host, `-O3` (non-authoritative) | **done** 2026-10-20 (4 s) | `results/2026-10-20_host_O3_contacts.csv`, `_meta.txt` |
| contacts | STM32H723ZG, `-Os` (project Release) | **done** 2026-10-20 (~9 min) | `results/2026-10-20_target_Os_contacts.csv`, `_meta.txt` |
| contacts, SD wait without `osDelay(1)` | STM32H723ZG, `-Os` | experiment, change reverted (see §1) | `results/2026-10-20_target_Os_contacts_nodelay.csv`, `_meta.txt` |
| messages / chats | Host, `-O3` (non-authoritative) | **done** 2026-10-20 (5 s) | `results/2026-10-20_host_O3_messages.csv`, `_meta.txt` |
| messages / chats | STM32H723ZG, `-Os` | **done** 2026-10-20 (~12 min) | `results/2026-10-20_target_Os_messages.csv`, `_meta.txt` |
| calls | Host, `-O3` (non-authoritative) | **done** 2026-10-20 (1 s) | `results/2026-10-20_host_O3_calls.csv`, `_meta.txt` |
| calls | STM32H723ZG, `-Os` | **done** 2026-10-20 (~9 min; second run, see §3) | `results/2026-10-20_target_Os_calls.csv`, `_meta.txt` |
| init / recovery | Host, `-O3` (non-authoritative) | **done** 2026-10-20 (~1 min) | `results/2026-10-20_host_O3_init.csv`, `_meta.txt` |
| init / recovery | STM32H723ZG, `-Os` | **done** 2026-10-20 (28 min) | `results/2026-10-20_target_Os_init.csv`, `_meta.txt` |

Host times use `heap_storage` (RAM), so they show CPU cost only. **Do not compare them with, or mix them into, the target tables.** The sector counts are identical on both platforms because the code path is the same.

## How to run

All code lives in `source/benchmark/database/`. The firmware hook is `-Wl,--wrap=DB_Init`, applied only when `-DDATABASE_BENCH=ON`, so `main.c` is unchanged and the default build is unaffected.

- Host: `scripts/run_host.sh <bench> [quick]` → `results/<date>_host_O3_<bench>.*`
- Target: `scripts/run_target.sh <bench> [Release|Debug] [quick]` → `results/<date>_target_<Os|O0>_<bench>.*`. Needs the NUCLEO on ST-LINK with an SD card. **The benchmark formats the database on the card.** OpenOCD stays attached for semihosting, and `name_index/scripts/ocd_resume.py` runs alongside as a watchdog.
- Tables: `scripts/summarize.py results/<file>.csv [--stat median|p99|mean|min|max] [--long]`
- Two runs side by side: `scripts/compare.py <base.csv> <new.csv> [--stat ...]`
- `DB_BENCH_TAG=<tag> scripts/run_target.sh ...` appends `_<tag>` to the result names.

CSV columns: `n_contacts, n_chats, msgs_per_chat, history_len` give the database state each row ran against. The contacts CSVs from before the messages benchmark were rewritten to this layout; their two new columns are 0 and no values changed.

## Method (common to all benchmarks)

- **Timed region.** One API call. The clock is the DWT cycle counter → ns at 520 MHz SYSCLK, falling back to the 1 ms HAL tick for regions ≥ 4 s; the host uses `CLOCK_MONOTONIC`. **Interrupts stay enabled**, because the SD driver waits on DMA completion through the scheduler. The benchmark task runs at `osPriorityHigh` and no other DB task is started.
- **Cache.** I-cache on, D-cache off: the firmware default, recorded in `_meta.txt`.
- **I/O counting.** After `database_init()`, `db->storage` is wrapped with counting shims, so every row also reports the average sectors read and written per call. Multi-block calls count each sector.
- **Data.** Contact id *i* has phone `04` + 8 digits (a bijection of *i*) and a "First Last" name from 32 × 40 word lists. Names repeat, as real names do; phones are unique. The xorshift32 RNG is reseeded per (N, operation), so host and target pick the same targets.
- **Repetitions.** Target: 100 timed after 10 warm-up. Host: 1,000 timed after 10 warm-up. Each row gives min, median, p99, max and mean, and `reps` is the actual count.
- **State.** The database is formatted at the start of each benchmark and then filled incrementally through the API (there is no refill between checkpoints).

## 1. Contacts

Checkpoints: N = 100, 500, 1,000, 2,500, 5,000, 7,500, 9,999. The last is one below the 10,000 capacity so `add` still has a free slot. Contact ids 0..N−1 are always stored: each operation that changes the set is undone untimed.

| operation | timed call | undone with (untimed) |
|---|---|---|
| `fill_add` | `contact_add` for each id that takes the DB from the previous checkpoint to N (every add timed; percentiles over ≤ 1,000 evenly strided samples) | – |
| `add` | `contact_add` of a new id | `contact_remove` |
| `get_hit` / `get_miss` | `contact_get` of a stored / never stored phone | – |
| `update_name` | `contact_update` with a new name and the same phone | update back |
| `update_phone` | `contact_update` with the same name and a new phone | update back |
| `remove` | `contact_remove` of a stored id | `contact_add` again |
| `list20` | `contact_list` of 20 from a random rank | – |
| `seek_prefix3` | `contact_seek` with the first 3 letters of a stored name | – |

### Target (STM32H723ZG, `-Os`, SD card)

**median time** (target, -Os)

| operation | N=100 | N=500 | N=1,000 | N=2,500 | N=5,000 | N=7,500 | N=9,999 |
|---|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 23.39 ms | 23.47 ms | 24.20 ms | 24.10 ms | 25.04 ms | 25.72 ms | 26.20 ms |
| add | 22.13 ms | 22.23 ms | 24.78 ms | 22.84 ms | 24.77 ms | 24.24 ms | 28.15 ms |
| get_hit | 899.9 µs | 916.4 µs | 917.4 µs | 939.8 µs | 921.3 µs | 916.8 µs | 934.1 µs |
| get_miss | 780 ns | 780 ns | 780 ns | 784 ns | 784 ns | 923 ns | 1.0 µs |
| update_name | 24.62 ms | 26.43 ms | 26.96 ms | 28.06 ms | 30.05 ms | 31.18 ms | 32.18 ms |
| update_phone | 25.70 ms | 26.67 ms | 26.86 ms | 28.42 ms | 29.39 ms | 31.10 ms | 31.29 ms |
| remove | 18.27 ms | 14.74 ms | 18.75 ms | 21.27 ms | 16.77 ms | 22.89 ms | 18.19 ms |
| list20 | 8.85 ms | 9.08 ms | 9.20 ms | 9.30 ms | 9.14 ms | 8.86 ms | 9.32 ms |
| seek_prefix3 | 1.8 µs | 2.1 µs | 2.4 µs | 2.6 µs | 2.9 µs | 2.9 µs | 2.9 µs |

**sectors per call** (read / written)

| operation | N=100 | N=500 | N=1,000 | N=2,500 | N=5,000 | N=7,500 | N=9,999 |
|---|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 0.96 / 7.00 | 1.55 / 7.00 | 2.22 / 7.00 | 2.96 / 7.00 | 3.89 / 7.00 | 4.68 / 7.00 | 5.26 / 7.00 |
| add | 0.42 / 7.00 | 2.06 / 7.00 | 1.61 / 7.00 | 2.68 / 7.00 | 4.68 / 7.00 | 5.18 / 7.00 | 5.71 / 7.00 |
| get_hit | 2.00 / 0.00 | 2.00 / 0.00 | 2.01 / 0.00 | 2.01 / 0.00 | 2.03 / 0.00 | 2.04 / 0.00 | 2.11 / 0.00 |
| get_miss | 0.00 / 0.00 | 0.01 / 0.00 | 0.00 / 0.00 | 0.05 / 0.00 | 0.10 / 0.00 | 0.14 / 0.00 | 0.15 / 0.00 |
| update_name | 6.84 / 7.00 | 8.47 / 7.00 | 9.18 / 7.00 | 11.07 / 7.00 | 13.05 / 7.00 | 14.03 / 7.00 | 15.00 / 7.00 |
| update_phone | 6.24 / 7.00 | 7.31 / 7.00 | 8.95 / 7.00 | 10.87 / 7.00 | 12.83 / 7.00 | 14.37 / 7.00 | 14.83 / 7.00 |
| remove | 5.27 / 5.00 | 5.46 / 5.00 | 6.44 / 5.00 | 6.99 / 5.00 | 7.85 / 5.00 | 8.63 / 5.00 | 8.65 / 5.00 |
| list20 | 20.00 / 0.00 | 20.00 / 0.00 | 20.00 / 0.00 | 20.00 / 0.00 | 20.00 / 0.00 | 20.00 / 0.00 | 20.00 / 0.00 |
| seek_prefix3 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 |

**p99 time** (target, -Os)

| operation | N=100 | N=500 | N=1,000 | N=2,500 | N=5,000 | N=7,500 | N=9,999 |
|---|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 29.90 ms | 31.25 ms | 32.11 ms | 34.09 ms | 34.38 ms | 34.44 ms | 35.07 ms |
| add | 27.97 ms | 27.51 ms | 29.19 ms | 27.75 ms | 30.10 ms | 33.72 ms | 32.45 ms |
| get_hit | 905.6 µs | 948.9 µs | 957.5 µs | 967.1 µs | 1.38 ms | 1.35 ms | 1.45 ms |
| get_miss | 784 ns | 926 ns | 926 ns | 480.4 µs | 475.7 µs | 584.7 µs | 484.8 µs |
| update_name | 34.61 ms | 35.98 ms | 36.36 ms | 39.23 ms | 40.28 ms | 38.89 ms | 39.54 ms |
| update_phone | 33.79 ms | 34.33 ms | 35.50 ms | 39.63 ms | 38.29 ms | 44.28 ms | 40.81 ms |
| remove | 19.71 ms | 23.12 ms | 21.81 ms | 24.88 ms | 24.95 ms | 29.74 ms | 24.84 ms |
| list20 | 9.05 ms | 9.24 ms | 9.33 ms | 9.41 ms | 9.23 ms | 8.93 ms | 9.39 ms |
| seek_prefix3 | 2.0 µs | 2.3 µs | 2.5 µs | 2.9 µs | 3.0 µs | 3.1 µs | 3.2 µs |

### Host (`-O3`, heap_storage, non-authoritative)

**median time** (host, -O3)

| operation | N=100 | N=500 | N=1,000 | N=2,500 | N=5,000 | N=7,500 | N=9,999 |
|---|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 23.8 µs | 23.8 µs | 26.3 µs | 29.4 µs | 31.6 µs | 33.2 µs | 39.4 µs |
| add | 21.5 µs | 24.2 µs | 25.0 µs | 29.1 µs | 37.5 µs | 35.1 µs | 40.3 µs |
| get_hit | 5.7 µs | 5.8 µs | 5.7 µs | 6.1 µs | 6.3 µs | 5.8 µs | 6.4 µs |
| get_miss | 36 ns | 41 ns | 53 ns | 46 ns | 55 ns | 56 ns | 75 ns |
| update_name | 39.7 µs | 43.1 µs | 48.9 µs | 56.5 µs | 60.1 µs | 60.9 µs | 74.6 µs |
| update_phone | 39.2 µs | 40.2 µs | 48.3 µs | 55.4 µs | 62.4 µs | 58.4 µs | 64.2 µs |
| remove | 29.5 µs | 31.1 µs | 35.0 µs | 37.6 µs | 37.3 µs | 40.5 µs | 38.9 µs |
| list20 | 56.2 µs | 58.4 µs | 60.5 µs | 60.7 µs | 59.7 µs | 60.6 µs | 60.7 µs |
| seek_prefix3 | 62 ns | 78 ns | 84 ns | 101 ns | 88 ns | 93 ns | 132 ns |

### Observations (measurements, not conclusions)

- **Writes per mutation are constant in N:** 7 for add and update, 5 for remove. Reads grow with N for add, update and remove, from 0.4–6.8 at N = 100 to 5.7–15 at N = 9,999. This fits name-index tie-breaks, which read the stored contact from SD when 8-byte keys are equal (`sorted_array.c` `tiebreak_fn`). The generator has only 1,280 distinct names, so ties become more common as N grows. Realistic names would tie less often, but not never (see `name_index/RESULTS.md`, where 57% of entries share a key at N = 10,000).
- **Per-sector cost on the target**, from the rows: `list20` makes 20 reads in about 9.1 ms, about 0.45 ms per read; `get_hit` makes 2 reads in about 0.9 ms. `add` (7 writes, a few reads) takes about 22–28 ms, about 3 ms per write. The SD transfer at the 6 MHz SD clock is about 0.17 ms per 512 B in 4-bit mode, so command and busy time dominate.
- **RAM-only paths**, which read no sectors: `get_miss` takes 0.8–1.0 µs median; its p99 reaches about 0.5 ms from N = 2,500, when an occasional probe reads a sector (0.05–0.15 reads on average). `seek_prefix3` takes 1.8–2.9 µs, growing with log N.
- `remove` medians vary from checkpoint to checkpoint (14.7–22.9 ms) with no trend in N.
- **Removing `osDelay(1)` from the SD wait loops** (`sd_storage.c`; experiment, reverted). `fill_add` medians changed by −2.2% to +1.5% at every N, with 100–2,500 adds per value. The 100-rep operations moved ±10–30% in both directions with no consistent sign, and p99 was unchanged or worse. This is consistent with the card usually being back in TRANSFER state when `HAL_SD_WriteBlocks` returns, so the delay would rarely run; that was not measured directly. Result: `results/2026-10-20_target_Os_contacts_nodelay.csv`.

## 2. Messages and chats

One database: 1,000 named contacts, then four phases. Messages are never undone, so the message history only grows. Each row records the state it ran against: contacts, chats, messages in the target chat, and history length (messages added since the format, capped at the 10,000-entry ring).

- **A. Chat count.** Chats are opened with one message each on contacts 0..C−1 for C = 10, 100, 500, 1,000. At each checkpoint the phase times:
  - `get_chat20_newest` on a random open chat;
  - `chat_list20` from rank 0 (`_top`) and from a random rank;
  - `recent20` from 0 (`_top`) and from a random offset;
  - `add_msg` to a random open chat;
  - `add_msg_new_contact`, a message to an unknown phone, which creates an unnamed contact and a new chat.

  `add_msg_new_contact` adds 110 contacts and chats per checkpoint, so the `n_chats` column runs ahead of C (210, 720, 1,330).
- **B. Chat depth.** One named contact's chat is grown to M = 20, 100, 500, 1,000 messages. The phase times `get_chat20` from the newest message, from the middle and from the oldest 20, plus `deep_add_msg`. The 60 `deep_add_msg` calls (50 timed after 10 warm-up) grow the chat between checkpoints.
- **C. `remove_chat`.** Five fresh chats of M = 1, 10, 100 messages are built untimed, each removal is timed, and the owning contact is removed untimed.
- **D. History wrap.** Messages are added round robin over the 1,000 chats until 11,000 have been added since the format, so the 10,000-entry ring has wrapped. The phase then times `recent20`, `chat_list20_top` and `add_msg` again.

Messages are 20–120 characters with a monotonic timestamp. Fill rows (`fill_*`) time every add; their percentiles are taken over at most 1,000 evenly strided samples.

### Target (STM32H723ZG, `-Os`, SD card)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 1,000 | 0 | – | 0 | 1000 | 23.53 ms | 31.73 ms | 1.83 / 7.00 |
| fill_first_msg | 1,000 | 10 | – | 10 | 10 | 29.28 ms | 54.00 ms | 4.40 / 11.60 |
| get_chat20_newest | 1,000 | 10 | – | 10 | 100 | 937.7 µs | 939.1 µs | 2.00 / 0.00 |
| chat_list20_top | 1,000 | 10 | – | 10 | 100 | 9.24 ms | 9.24 ms | 20.00 / 0.00 |
| chat_list20 | 1,000 | 10 | – | 10 | 100 | 9.24 ms | 9.25 ms | 20.00 / 0.00 |
| recent20_top | 1,000 | 10 | – | 10 | 100 | 2.46 ms | 2.46 ms | 6.00 / 0.00 |
| recent20 | 1,000 | 10 | – | 10 | 100 | 2.46 ms | 2.46 ms | 6.00 / 0.00 |
| add_msg | 1,000 | 10 | – | 10 | 100 | 48.28 ms | 71.03 ms | 5.82 / 13.60 |
| add_msg_new_contact | 1,000 | 10 | – | 120 | 100 | 65.46 ms | 74.76 ms | 6.71 / 19.00 |
| fill_first_msg | 1,110 | 210 | – | 320 | 90 | 26.81 ms | 70.17 ms | 4.50 / 12.00 |
| get_chat20_newest | 1,110 | 210 | – | 320 | 100 | 939.4 µs | 2.44 ms | 2.17 / 0.00 |
| chat_list20_top | 1,110 | 210 | – | 320 | 100 | 18.71 ms | 18.72 ms | 40.00 / 0.00 |
| chat_list20 | 1,110 | 210 | – | 320 | 100 | 18.78 ms | 19.99 ms | 40.00 / 0.00 |
| recent20_top | 1,110 | 210 | – | 320 | 100 | 4.95 ms | 4.95 ms | 11.00 / 0.00 |
| recent20 | 1,110 | 210 | – | 320 | 100 | 5.13 ms | 5.64 ms | 11.43 / 0.00 |
| add_msg | 1,110 | 210 | – | 320 | 100 | 42.51 ms | 64.53 ms | 5.65 / 12.75 |
| add_msg_new_contact | 1,110 | 210 | – | 430 | 100 | 65.75 ms | 75.90 ms | 7.34 / 19.00 |
| fill_first_msg | 1,220 | 720 | – | 940 | 400 | 39.53 ms | 53.12 ms | 4.50 / 12.00 |
| get_chat20_newest | 1,220 | 720 | – | 940 | 100 | 1.13 ms | 2.05 ms | 2.10 / 0.00 |
| chat_list20_top | 1,220 | 720 | – | 940 | 100 | 17.49 ms | 17.50 ms | 40.00 / 0.00 |
| chat_list20 | 1,220 | 720 | – | 940 | 100 | 22.52 ms | 22.84 ms | 40.00 / 0.00 |
| recent20_top | 1,220 | 720 | – | 940 | 100 | 4.59 ms | 4.59 ms | 11.00 / 0.00 |
| recent20 | 1,220 | 720 | – | 940 | 100 | 5.30 ms | 5.50 ms | 11.55 / 0.00 |
| add_msg | 1,220 | 720 | – | 940 | 100 | 44.94 ms | 66.90 ms | 5.54 / 12.20 |
| add_msg_new_contact | 1,220 | 720 | – | 1,050 | 100 | 67.14 ms | 76.22 ms | 8.39 / 19.00 |
| fill_first_msg | 1,330 | 1,330 | – | 1,660 | 500 | 43.33 ms | 56.04 ms | 4.52 / 12.00 |
| get_chat20_newest | 1,330 | 1,330 | – | 1,660 | 100 | 1.14 ms | 1.15 ms | 2.00 / 0.00 |
| chat_list20_top | 1,330 | 1,330 | – | 1,660 | 100 | 18.15 ms | 18.16 ms | 40.00 / 0.00 |
| chat_list20 | 1,330 | 1,330 | – | 1,660 | 100 | 22.66 ms | 22.92 ms | 40.00 / 0.00 |
| recent20_top | 1,330 | 1,330 | – | 1,660 | 100 | 4.58 ms | 4.58 ms | 11.00 / 0.00 |
| recent20 | 1,330 | 1,330 | – | 1,660 | 100 | 5.29 ms | 5.50 ms | 11.54 / 0.00 |
| add_msg | 1,330 | 1,330 | – | 1,660 | 100 | 41.66 ms | 62.40 ms | 5.53 / 12.15 |
| add_msg_new_contact | 1,330 | 1,330 | – | 1,770 | 100 | 66.29 ms | 75.71 ms | 7.43 / 19.00 |
| fill_deep_chat | 1,441 | 1,441 | 20 | 1,900 | 20 | 46.31 ms | 71.09 ms | 5.75 / 13.50 |
| deep_get_chat20_newest | 1,441 | 1,441 | 20 | 1,900 | 100 | 3.44 ms | 3.44 ms | 8.00 / 0.00 |
| deep_get_chat20_middle | 1,441 | 1,441 | 20 | 1,900 | 100 | 3.44 ms | 3.44 ms | 8.00 / 0.00 |
| deep_get_chat20_oldest | 1,441 | 1,441 | 20 | 1,900 | 100 | 3.44 ms | 3.44 ms | 8.00 / 0.00 |
| deep_add_msg | 1,441 | 1,441 | 20 | 1,900 | 50 | 46.00 ms | 71.50 ms | 5.84 / 13.70 |
| fill_deep_chat | 1,441 | 1,441 | 100 | 1,980 | 20 | 48.53 ms | 67.42 ms | 5.85 / 13.75 |
| deep_get_chat20_newest | 1,441 | 1,441 | 100 | 1,980 | 100 | 3.75 ms | 3.76 ms | 9.00 / 0.00 |
| deep_get_chat20_middle | 1,441 | 1,441 | 100 | 1,980 | 100 | 9.82 ms | 9.83 ms | 22.00 / 0.00 |
| deep_get_chat20_oldest | 1,441 | 1,441 | 100 | 1,980 | 100 | 15.71 ms | 15.72 ms | 35.00 / 0.00 |
| deep_add_msg | 1,441 | 1,441 | 100 | 1,980 | 50 | 45.90 ms | 71.55 ms | 5.84 / 13.70 |
| fill_deep_chat | 1,441 | 1,441 | 500 | 2,380 | 340 | 46.22 ms | 71.36 ms | 5.83 / 13.66 |
| deep_get_chat20_newest | 1,441 | 1,441 | 500 | 2,380 | 100 | 3.40 ms | 3.41 ms | 8.00 / 0.00 |
| deep_get_chat20_middle | 1,441 | 1,441 | 500 | 2,380 | 100 | 40.79 ms | 40.79 ms | 88.00 / 0.00 |
| deep_get_chat20_oldest | 1,441 | 1,441 | 500 | 2,380 | 100 | 77.84 ms | 77.86 ms | 168.00 / 0.00 |
| deep_add_msg | 1,441 | 1,441 | 500 | 2,380 | 50 | 45.95 ms | 71.42 ms | 5.84 / 13.70 |
| fill_deep_chat | 1,441 | 1,441 | 1000 | 2,880 | 440 | 46.01 ms | 71.59 ms | 5.83 / 13.67 |
| deep_get_chat20_newest | 1,441 | 1,441 | 1000 | 2,880 | 100 | 3.88 ms | 3.88 ms | 9.00 / 0.00 |
| deep_get_chat20_middle | 1,441 | 1,441 | 1000 | 2,880 | 100 | 79.45 ms | 79.46 ms | 172.00 / 0.00 |
| deep_get_chat20_oldest | 1,441 | 1,441 | 1000 | 2,880 | 100 | 155.30 ms | 155.32 ms | 335.00 / 0.00 |
| deep_add_msg | 1,441 | 1,441 | 1000 | 2,880 | 50 | 46.04 ms | 71.74 ms | 5.84 / 13.70 |
| remove_chat | 1,441 | 1,441 | 1 | 2,940 | 5 | 18.17 ms | 22.68 ms | 3.00 / 5.00 |
| remove_chat | 1,441 | 1,441 | 10 | 2,945 | 5 | 68.22 ms | 69.91 ms | 6.00 / 20.00 |
| remove_chat | 1,441 | 1,441 | 100 | 2,995 | 5 | 571.35 ms | 573.44 ms | 36.00 / 170.00 |
| fill_history | 1,441 | 1,441 | – | 10,000 | 7505 | 39.06 ms | 61.33 ms | 5.80 / 13.45 |
| wrapped_recent20_top | 1,441 | 1,441 | – | 10,000 | 100 | 4.78 ms | 4.78 ms | 11.00 / 0.00 |
| wrapped_recent20 | 1,441 | 1,441 | – | 10,000 | 100 | 5.63 ms | 5.91 ms | 11.57 / 0.00 |
| wrapped_chat_list20_top | 1,441 | 1,441 | – | 10,000 | 100 | 19.12 ms | 19.12 ms | 40.00 / 0.00 |
| wrapped_add_msg | 1,441 | 1,441 | – | 10,000 | 100 | 46.49 ms | 71.22 ms | 5.94 / 14.20 |

### Host (`-O3`, heap_storage, non-authoritative)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 1,000 | 0 | – | 0 | 1000 | 24.1 µs | 48.7 µs | 1.83 / 7.00 |
| fill_first_msg | 1,000 | 10 | – | 10 | 10 | 41.1 µs | 58.0 µs | 4.40 / 11.60 |
| get_chat20_newest | 1,000 | 10 | – | 10 | 1000 | 5.3 µs | 6.7 µs | 2.00 / 0.00 |
| chat_list20_top | 1,000 | 10 | – | 10 | 1000 | 53.3 µs | 56.2 µs | 20.00 / 0.00 |
| chat_list20 | 1,000 | 10 | – | 10 | 1000 | 53.3 µs | 64.6 µs | 20.00 / 0.00 |
| recent20_top | 1,000 | 10 | – | 10 | 1000 | 16.9 µs | 79.6 µs | 6.00 / 0.00 |
| recent20 | 1,000 | 10 | – | 10 | 1000 | 16.7 µs | 18.9 µs | 6.00 / 0.00 |
| add_msg | 1,000 | 10 | – | 10 | 1000 | 53.2 µs | 74.8 µs | 5.83 / 13.67 |
| add_msg_new_contact | 1,000 | 10 | – | 1,020 | 1000 | 74.2 µs | 90.8 µs | 8.38 / 19.00 |
| fill_first_msg | 2,010 | 1,110 | – | 2,120 | 90 | 42.2 µs | 73.5 µs | 4.50 / 12.00 |
| get_chat20_newest | 2,010 | 1,110 | – | 2,120 | 1000 | 5.3 µs | 24.0 µs | 2.62 / 0.00 |
| chat_list20_top | 2,010 | 1,110 | – | 2,120 | 1000 | 106.5 µs | 201.9 µs | 40.00 / 0.00 |
| chat_list20 | 2,010 | 1,110 | – | 2,120 | 1000 | 106.9 µs | 130.8 µs | 40.00 / 0.00 |
| recent20_top | 2,010 | 1,110 | – | 2,120 | 1000 | 29.3 µs | 37.3 µs | 11.00 / 0.00 |
| recent20 | 2,010 | 1,110 | – | 2,120 | 1000 | 30.5 µs | 33.9 µs | 11.48 / 0.00 |
| add_msg | 2,010 | 1,110 | – | 2,120 | 1000 | 53.2 µs | 70.5 µs | 5.81 / 13.53 |
| add_msg_new_contact | 2,010 | 1,110 | – | 3,130 | 1000 | 87.7 µs | 216.0 µs | 13.42 / 19.00 |
| fill_first_msg | 3,020 | 2,520 | – | 4,540 | 400 | 43.7 µs | 53.2 µs | 4.50 / 12.00 |
| get_chat20_newest | 3,020 | 2,520 | – | 4,540 | 1000 | 5.4 µs | 21.3 µs | 2.68 / 0.00 |
| chat_list20_top | 3,020 | 2,520 | – | 4,540 | 1000 | 106.6 µs | 122.8 µs | 40.00 / 0.00 |
| chat_list20 | 3,020 | 2,520 | – | 4,540 | 1000 | 107.6 µs | 122.4 µs | 40.00 / 0.00 |
| recent20_top | 3,020 | 2,520 | – | 4,540 | 1000 | 29.4 µs | 44.4 µs | 11.00 / 0.00 |
| recent20 | 3,020 | 2,520 | – | 4,540 | 1000 | 32.9 µs | 36.3 µs | 11.50 / 0.00 |
| add_msg | 3,020 | 2,520 | – | 4,540 | 1000 | 53.7 µs | 75.6 µs | 5.71 / 13.07 |
| add_msg_new_contact | 3,020 | 2,520 | – | 5,550 | 1000 | 94.5 µs | 111.4 µs | 14.85 / 19.00 |
| fill_first_msg | 4,030 | 4,030 | – | 7,060 | 500 | 51.4 µs | 60.0 µs | 4.52 / 12.00 |
| get_chat20_newest | 4,030 | 4,030 | – | 7,060 | 1000 | 5.7 µs | 24.8 µs | 2.52 / 0.00 |
| chat_list20_top | 4,030 | 4,030 | – | 7,060 | 1000 | 107.6 µs | 126.2 µs | 40.00 / 0.00 |
| chat_list20 | 4,030 | 4,030 | – | 7,060 | 1000 | 107.4 µs | 132.1 µs | 40.00 / 0.00 |
| recent20_top | 4,030 | 4,030 | – | 7,060 | 1000 | 30.5 µs | 133.1 µs | 11.00 / 0.00 |
| recent20 | 4,030 | 4,030 | – | 7,060 | 1000 | 31.5 µs | 34.9 µs | 11.49 / 0.00 |
| add_msg | 4,030 | 4,030 | – | 7,060 | 1000 | 53.5 µs | 76.5 µs | 5.71 / 13.01 |
| add_msg_new_contact | 4,030 | 4,030 | – | 8,070 | 1000 | 97.4 µs | 118.4 µs | 15.54 / 19.00 |
| fill_deep_chat | 5,041 | 5,041 | 20 | 9,100 | 20 | 73.8 µs | 284.3 µs | 8.75 / 13.50 |
| deep_get_chat20_newest | 5,041 | 5,041 | 20 | 9,100 | 1000 | 25.1 µs | 61.0 µs | 9.00 / 0.00 |
| deep_get_chat20_middle | 5,041 | 5,041 | 20 | 9,100 | 1000 | 24.3 µs | 26.3 µs | 9.00 / 0.00 |
| deep_get_chat20_oldest | 5,041 | 5,041 | 20 | 9,100 | 1000 | 23.9 µs | 25.5 µs | 9.00 / 0.00 |
| deep_add_msg | 5,041 | 5,041 | 20 | 9,100 | 50 | 61.2 µs | 78.2 µs | 8.84 / 13.70 |
| fill_deep_chat | 5,041 | 5,041 | 100 | 9,180 | 20 | 61.2 µs | 77.3 µs | 8.85 / 13.75 |
| deep_get_chat20_newest | 5,041 | 5,041 | 100 | 9,180 | 1000 | 26.6 µs | 29.4 µs | 10.00 / 0.00 |
| deep_get_chat20_middle | 5,041 | 5,041 | 100 | 9,180 | 1000 | 63.8 µs | 138.9 µs | 23.00 / 0.00 |
| deep_get_chat20_oldest | 5,041 | 5,041 | 100 | 9,180 | 1000 | 95.6 µs | 116.6 µs | 36.00 / 0.00 |
| deep_add_msg | 5,041 | 5,041 | 100 | 9,180 | 50 | 61.2 µs | 77.1 µs | 8.84 / 13.70 |
| fill_deep_chat | 5,041 | 5,041 | 500 | 9,580 | 340 | 61.2 µs | 78.3 µs | 8.83 / 13.66 |
| deep_get_chat20_newest | 5,041 | 5,041 | 500 | 9,580 | 1000 | 23.9 µs | 27.7 µs | 9.00 / 0.00 |
| deep_get_chat20_middle | 5,041 | 5,041 | 500 | 9,580 | 1000 | 240.4 µs | 335.1 µs | 89.00 / 0.00 |
| deep_get_chat20_oldest | 5,041 | 5,041 | 500 | 9,580 | 1000 | 456.8 µs | 552.9 µs | 169.00 / 0.00 |
| deep_add_msg | 5,041 | 5,041 | 500 | 9,580 | 50 | 61.2 µs | 78.6 µs | 8.84 / 13.70 |
| fill_deep_chat | 5,041 | 5,041 | 1000 | 10,000 | 440 | 64.0 µs | 102.7 µs | 8.83 / 13.67 |
| deep_get_chat20_newest | 5,041 | 5,041 | 1000 | 10,000 | 1000 | 26.6 µs | 27.8 µs | 10.00 / 0.00 |
| deep_get_chat20_middle | 5,041 | 5,041 | 1000 | 10,000 | 1000 | 465.1 µs | 578.0 µs | 173.00 / 0.00 |
| deep_get_chat20_oldest | 5,041 | 5,041 | 1000 | 10,000 | 1000 | 901.8 µs | 1.80 ms | 336.00 / 0.00 |
| deep_add_msg | 5,041 | 5,041 | 1000 | 10,000 | 50 | 61.2 µs | 81.8 µs | 8.84 / 13.70 |
| remove_chat | 5,041 | 5,041 | 1 | 10,000 | 50 | 21.4 µs | 32.0 µs | 3.24 / 5.00 |
| remove_chat | 5,041 | 5,041 | 10 | 10,000 | 50 | 69.1 µs | 146.5 µs | 6.12 / 20.00 |
| remove_chat | 5,041 | 5,041 | 100 | 10,000 | 50 | 547.6 µs | 591.4 µs | 36.12 / 170.00 |
| wrapped_recent20_top | 5,041 | 5,041 | – | 10,000 | 1000 | 29.2 µs | 30.5 µs | 11.00 / 0.00 |
| wrapped_recent20 | 5,041 | 5,041 | – | 10,000 | 1000 | 31.6 µs | 33.9 µs | 11.51 / 0.00 |
| wrapped_chat_list20_top | 5,041 | 5,041 | – | 10,000 | 1000 | 106.5 µs | 182.0 µs | 40.00 / 0.00 |
| wrapped_add_msg | 5,041 | 5,041 | – | 10,000 | 1000 | 53.6 µs | 73.2 µs | 5.81 / 13.51 |

### Observations (measurements, not conclusions)

- **Adding a message** to an existing chat takes 42–48 ms median (p99 62–72 ms) with 12–14 sector writes and ~5.7 reads. That is about twice a contact add. It holds steady from 10 to 1,330 chats, in chats of 1 to 1,000 messages, and after the history ring wraps (46.5 ms, 14.2 writes). A message to an unknown phone takes 65–67 ms with 19 writes, because the unnamed contact is created as well.
- **Reading a deep chat is linear in the offset:** `get_chat20` reads about one sector per 3 messages it skips, walking the extent list from the newest message. With M = 1,000, the newest 20 take 3.9 ms (9 reads), the middle 79 ms (172 reads) and the oldest 20 155 ms (335 reads). The newest page stays at 3.4–3.9 ms at any M.
- **`remove_chat` is linear in chat length:** 18 ms for 1 message, 68 ms for 10 and 571 ms for 100 (170 writes, about 5 per freed message sector).
- **`chat_list20` makes 2 reads per chat listed** (contact + latest message), 40 reads for 20 chats: 17.5–19 ms from rank 0. From a random rank it takes 22.5–22.7 ms once there are 720 or more chats, for the same 40 reads. The extra ~4–5 ms is CPU time, consistent with walking the chat order to the start rank.
- **`recent20` reads 11 sectors** (2 history entries per sector) in 4.6–5.6 ms, before and after the ring wraps.
- **The p99 of `get_chat20_newest` reaches 2.0–2.4 ms at 210–720 chats** against a ~1 ms median. The extra read on some calls (2.10–2.17 average) is consistent with hash probing.

## 3. Calls

The database holds 1,000 named contacts. The call history (a 25,000-entry ring: 5,000 sectors × 5 entries) is filled to checkpoints of 100, 1,000, 5,000, 25,000 and 30,000 calls. Callers are 80% random known contacts and 20% new unknown numbers. At each checkpoint the run times:
- `call_recent20` from 0 (`_top`) and from a random offset;
- `call_add_known` and `call_add_unknown`.

The timed adds also add to the history (220 calls per checkpoint on the target, 2,020 on the host). On the host this pushes the count past the 1,000 checkpoint, so its rows are labelled with the actual counts. Rows after the ring has wrapped carry a `wrapped_` prefix. `history_len` is the number of calls added since the format, capped at 25,000.

The target run was repeated once after adding the `wrapped_` labels and moving the last checkpoint from 27,000 to 30,000. Both runs had 0 failures and gave the same numbers within noise, including the slow long fills described below.

### Target (STM32H723ZG, `-Os`, SD card)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 1,000 | 0 | – | 0 | 1000 | 23.75 ms | 32.04 ms | 1.83 / 7.00 |
| fill_calls | 1,000 | 0 | – | 100 | 100 | 14.61 ms | 33.61 ms | 2.81 / 4.76 |
| call_recent20_top | 1,000 | 0 | – | 100 | 100 | 2.05 ms | 2.05 ms | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 100 | 100 | 2.60 ms | 2.64 ms | 5.76 / 0.00 |
| call_add_known | 1,000 | 0 | – | 100 | 100 | 14.66 ms | 33.54 ms | 3.21 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 210 | 100 | 13.77 ms | 32.71 ms | 1.22 / 4.80 |
| fill_calls | 1,000 | 0 | – | 1,000 | 680 | 14.76 ms | 33.66 ms | 2.76 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 1,000 | 100 | 2.05 ms | 2.05 ms | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 1,000 | 100 | 2.66 ms | 2.67 ms | 5.78 / 0.00 |
| call_add_known | 1,000 | 0 | – | 1,000 | 100 | 16.70 ms | 24.16 ms | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 1,110 | 100 | 15.73 ms | 26.58 ms | 1.21 / 4.80 |
| fill_calls | 1,000 | 0 | – | 5,000 | 3780 | 15.01 ms | 33.55 ms | 2.80 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 5,000 | 100 | 2.27 ms | 2.27 ms | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 5,000 | 100 | 2.77 ms | 2.84 ms | 5.84 / 0.00 |
| call_add_known | 1,000 | 0 | – | 5,000 | 100 | 16.69 ms | 33.41 ms | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 5,110 | 100 | 13.74 ms | 33.06 ms | 1.23 / 4.80 |
| fill_calls | 1,000 | 0 | – | 25,000 | 19780 | 27.40 ms | 33.66 ms | 2.80 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 25,000 | 100 | 2.20 ms | 2.21 ms | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 25,000 | 100 | 2.67 ms | 2.86 ms | 5.81 / 0.00 |
| call_add_known | 1,000 | 0 | – | 25,000 | 100 | 16.74 ms | 27.56 ms | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 25,000 | 100 | 15.79 ms | 26.61 ms | 1.21 / 4.80 |
| wrapped_fill_calls | 1,000 | 0 | – | 25,000 | 4780 | 27.45 ms | 33.67 ms | 2.81 / 4.80 |
| wrapped_call_recent20_top | 1,000 | 0 | – | 25,000 | 100 | 2.19 ms | 2.19 ms | 5.00 / 0.00 |
| wrapped_call_recent20 | 1,000 | 0 | – | 25,000 | 100 | 2.71 ms | 2.84 ms | 5.81 / 0.00 |
| wrapped_call_add_known | 1,000 | 0 | – | 25,000 | 100 | 16.77 ms | 27.52 ms | 3.20 / 4.80 |
| wrapped_call_add_unknown | 1,000 | 0 | – | 25,000 | 100 | 15.80 ms | 26.45 ms | 1.23 / 4.80 |

### Host (`-O3`, heap_storage, non-authoritative)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fill_add | 1,000 | 0 | – | 0 | 1000 | 21.5 µs | 37.6 µs | 1.83 / 7.00 |
| fill_calls | 1,000 | 0 | – | 100 | 100 | 18.6 µs | 31.9 µs | 2.81 / 4.76 |
| call_recent20_top | 1,000 | 0 | – | 100 | 1000 | 13.3 µs | 14.3 µs | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 100 | 1000 | 15.9 µs | 19.6 µs | 5.80 / 0.00 |
| call_add_known | 1,000 | 0 | – | 100 | 1000 | 18.6 µs | 34.3 µs | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 1,110 | 1000 | 13.3 µs | 28.0 µs | 1.21 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 2,120 | 1000 | 13.3 µs | 14.3 µs | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 2,120 | 1000 | 16.0 µs | 17.7 µs | 5.79 / 0.00 |
| call_add_known | 1,000 | 0 | – | 2,120 | 1000 | 18.8 µs | 76.8 µs | 3.21 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 3,130 | 1000 | 13.8 µs | 31.4 µs | 1.21 / 4.80 |
| fill_calls | 1,000 | 0 | – | 5,000 | 860 | 18.6 µs | 34.4 µs | 2.88 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 5,000 | 1000 | 13.3 µs | 14.9 µs | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 5,000 | 1000 | 15.9 µs | 17.0 µs | 5.79 / 0.00 |
| call_add_known | 1,000 | 0 | – | 5,000 | 1000 | 18.6 µs | 32.3 µs | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 6,010 | 1000 | 13.3 µs | 27.8 µs | 1.22 / 4.80 |
| fill_calls | 1,000 | 0 | – | 25,000 | 17980 | 18.6 µs | 37.8 µs | 2.81 / 4.80 |
| call_recent20_top | 1,000 | 0 | – | 25,000 | 1000 | 13.3 µs | 17.4 µs | 5.00 / 0.00 |
| call_recent20 | 1,000 | 0 | – | 25,000 | 1000 | 16.1 µs | 19.0 µs | 5.79 / 0.00 |
| call_add_known | 1,000 | 0 | – | 25,000 | 1000 | 18.6 µs | 32.2 µs | 3.20 / 4.80 |
| call_add_unknown | 1,000 | 0 | – | 25,000 | 1000 | 13.3 µs | 27.4 µs | 1.21 / 4.80 |
| wrapped_fill_calls | 1,000 | 0 | – | 25,000 | 2980 | 18.7 µs | 35.8 µs | 2.79 / 4.80 |
| wrapped_call_recent20_top | 1,000 | 0 | – | 25,000 | 1000 | 13.3 µs | 15.8 µs | 5.00 / 0.00 |
| wrapped_call_recent20 | 1,000 | 0 | – | 25,000 | 1000 | 16.1 µs | 18.2 µs | 5.79 / 0.00 |
| wrapped_call_add_known | 1,000 | 0 | – | 25,000 | 1000 | 18.6 µs | 32.2 µs | 3.20 / 4.80 |
| wrapped_call_add_unknown | 1,000 | 0 | – | 25,000 | 1000 | 13.4 µs | 27.1 µs | 1.21 / 4.80 |

### Observations (measurements, not conclusions)

- **`call_add`** takes 13.7–16.8 ms median with 4.8 writes. A known caller makes 3.2 reads, because the contact is copied into the entry; an unknown caller makes 1.2 reads. The median is flat from 100 calls to after the wrap.
- **`call_recent20`** reads 5 sectors from the top (5 entries per sector) in 2.05–2.27 ms. From a random offset it reads 5.8 sectors in 2.6–2.8 ms. Both are flat with history length and after the wrap.
- **The two long fills are slower per call** (the 20,000 adds to reach 25,000, and the 5,000-add wrapped fill): 27.4 ms median, against 14.6–15.0 ms for the fills of up to 3,780 adds and for the 100-call timed bursts. The sector counts are identical (2.8 / 4.8). The p99 is 33.6 ms in every fill row, so per-call times look bimodal (~15 ms and ~27–33 ms), and the slower mode dominates the long fills. This appeared in both runs. In the first run the shorter wrapped fill (1,780 adds) had a 15.8 ms median. The DB code path and I/O count do not change between these rows. A card-side effect under long sustained writing (such as internal cache or garbage collection) would fit, but it was **not verified**.

## 4. Init and recovery

The I/O counting shims wrap the global `sd_storage` / `heap_storage` struct, which `database_init()` copies, so the I/O of every init is counted. The run, in order:

1. `format`: `database_format()` timed 3 times on the target (10 on the host).
2. `mount`: `database_init()` on the formatted card, which journals, rebuilds the hash table, chat order and name index, and finds the ring heads. Timed 5 times per state on the target (50 on the host), each checking that the rebuilt contact and chat counts match.
3. Contacts are filled to 1,000, 2,500, 5,000 and 10,000, with a mount after each.
4. One message is added to each of 1,000 chats, then messages are added round robin up to 5,000; mount after each step.
5. 100 calls are added, then `mount_with_calls`. The call ring head is found by binary search, so its fill level should not matter.
6. `mount_after_power_cut`: every sector write fails after the first 6 writes of a `message_add` (which makes ~12–14 writes). The add returns an error, then `database_init()` is timed. Contact and chat counts must be unchanged, so the interrupted message must not appear.

Times of 4 s or more are measured with the 1 ms HAL tick (see Method).

### Target (STM32H723ZG, `-Os`, SD card)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| format | 0 | 0 | – | 0 | 3 | 30716.00 ms | 30770.00 ms | 2.00 / 10010.00 |
| mount | 0 | 0 | – | 0 | 5 | 5.95 ms | 6.09 ms | 12.00 / 0.00 |
| fill_add | 1,000 | 0 | – | 0 | 1000 | 23.69 ms | 32.22 ms | 1.83 / 7.00 |
| mount | 1,000 | 0 | – | 0 | 5 | 1512.94 ms | 1513.00 ms | 3263.00 / 0.00 |
| fill_add | 2,500 | 0 | – | 0 | 1500 | 24.29 ms | 33.71 ms | 2.96 / 7.00 |
| mount | 2,500 | 0 | – | 0 | 5 | 5307.00 ms | 5308.00 ms | 11570.00 / 0.00 |
| fill_add | 5,000 | 0 | – | 0 | 2500 | 25.11 ms | 33.58 ms | 3.89 / 7.00 |
| mount | 5,000 | 0 | – | 0 | 5 | 13992.00 ms | 13993.00 ms | 29573.00 / 0.00 |
| fill_add | 10,000 | 0 | – | 0 | 5000 | 26.07 ms | 35.24 ms | 4.97 / 7.00 |
| mount | 10,000 | 0 | – | 0 | 5 | 36596.00 ms | 36598.00 ms | 76045.00 / 0.00 |
| mount | 10,000 | 1,000 | – | 1,000 | 5 | 36453.00 ms | 36995.00 ms | 80194.00 / 0.00 |
| mount | 10,000 | 1,000 | – | 5,000 | 5 | 39010.00 ms | 39013.00 ms | 81195.00 / 0.00 |
| mount_with_calls | 10,000 | 1,000 | – | 5,000 | 5 | 36802.00 ms | 36803.00 ms | 81209.00 / 0.00 |
| mount_after_power_cut | 10,000 | 1,000 | – | 5,000 | 5 | 36805.00 ms | 36910.00 ms | 81209.00 / 0.00 |

### Host (`-O3`, heap_storage, non-authoritative)

| operation | contacts | chats | msgs in chat | history | reps | median | p99 | sectors r / w |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| format | 0 | 0 | – | 0 | 10 | 26.99 ms | 29.67 ms | 2.00 / 10010.00 |
| mount | 0 | 0 | – | 0 | 50 | 69.3 µs | 216.0 µs | 12.00 / 0.00 |
| fill_add | 1,000 | 0 | – | 0 | 1000 | 23.0 µs | 42.4 µs | 1.83 / 7.00 |
| mount | 1,000 | 0 | – | 0 | 50 | 8.83 ms | 12.40 ms | 3263.00 / 0.00 |
| fill_add | 2,500 | 0 | – | 0 | 1500 | 24.3 µs | 82.8 µs | 2.96 / 7.00 |
| mount | 2,500 | 0 | – | 0 | 50 | 31.41 ms | 34.67 ms | 11570.00 / 0.00 |
| fill_add | 5,000 | 0 | – | 0 | 2500 | 29.0 µs | 65.4 µs | 3.89 / 7.00 |
| mount | 5,000 | 0 | – | 0 | 50 | 81.70 ms | 86.05 ms | 29573.00 / 0.00 |
| fill_add | 10,000 | 0 | – | 0 | 5000 | 31.2 µs | 50.2 µs | 4.97 / 7.00 |
| mount | 10,000 | 0 | – | 0 | 50 | 209.78 ms | 300.05 ms | 76045.00 / 0.00 |
| mount | 10,000 | 1,000 | – | 1,000 | 50 | 221.26 ms | 242.55 ms | 80194.00 / 0.00 |
| mount | 10,000 | 1,000 | – | 5,000 | 50 | 224.16 ms | 233.82 ms | 81195.00 / 0.00 |
| mount_with_calls | 10,000 | 1,000 | – | 5,000 | 50 | 223.96 ms | 237.77 ms | 81209.00 / 0.00 |
| mount_after_power_cut | 10,000 | 1,000 | – | 5,000 | 20 | 223.13 ms | 237.65 ms | 81209.00 / 0.00 |

### Observations (measurements, not conclusions)

- **Format takes 30.7 s.** It writes 10,010 sectors, almost all to clear the two ring buffers (5,000 message history and 5,000 call history sectors), at about 3.07 ms per write.
- **Mount time is set by sector reads and grows faster than N:** 1.5 s at 1,000 contacts (3,263 reads), 5.3 s at 2,500 (11,570), 14.0 s at 5,000 (29,573) and 36.6 s at 10,000 (76,045). That is about 0.47 ms per read throughout. Chats and messages add little (+5,150 reads for 1,000 chats and 5,000 messages), and calls add 14 reads.
- **Most mount reads come from rebuilding the name index.** `name_index_build` (`database.c`) reads each contact once, about 10,000 reads, and then heapsorts. When two entries have equal 8-byte keys, each comparison reads both contacts from storage (`sorted_array.c` `entry_cmp` → `tiebreak_fn`). At 10,000 contacts that is roughly 64,000 of the 76,045 reads. The 1,280-name generator ties more than real names would, but real names also share keys (57% of entries at N = 10,000 in `name_index/RESULTS.md`).
- **Mount after a power cut** takes the same time and I/O as a normal mount (36.8 s, 81,209 reads, 0 writes). The rebuilt counts were correct in all 5 repetitions. The rollback wrote nothing, so with this cut point (after 6 writes) recovery added no measurable cost.
