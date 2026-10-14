# Name index benchmark: sorted array vs AVL tree

Measurements for the ordered, RAM-only contact name index. **This file contains measurements only, not a recommendation.**

| Run | Platform | Status | File |
|---|---|---|---|
| Host, `-O3` | x86-64 Linux (non-authoritative) | **done** | `results/2026-10-14_host_O3.csv`, `_meta.txt` |
| Target, `-Os` (project Release) | STM32H723ZG | **not run** (no board connected in this session); firmware built and ready | `scripts/run_target.sh Release` |
| Target, `-O0` (project Debug, reference) | STM32H723ZG | **not run**; firmware built and ready | `scripts/run_target.sh Debug` |

Host numbers are in **nanoseconds** (the CSV keeps the `cycles_*` column names). They are a sanity check for CI and **must not be compared with, or mixed into, the target tables**. The authoritative numbers are the target runs, which have not been made yet.

## How to run on the target

1. Connect the NUCLEO-H723ZG over ST-LINK with an SD card inserted. The SD baseline **writes to the card's DB region** (the same sectors `db_main.c` wipes).
2. From `source/benchmark/name_index/`, run `scripts/run_target.sh Release` and then `scripts/run_target.sh Debug`. Each script:
   - builds `build-bench-<type>` with `-DNAME_INDEX_BENCH=ON`;
   - flashes and starts the firmware with OpenOCD, with semihosting enabled;
   - waits for `done,1`;
   - moves `target.csv` and `target_meta.txt` into `results/<date>_target_<Os|O0>.*`.

   OpenOCD stays attached for the whole run because the target writes its results over semihosting. Without a debugger attached, the first semihosting call faults.
3. Generate the tables and plots for this file:

   ```
   python3 scripts/summarize.py results/<date>_target_Os.csv --dcache on --mode warm
   python3 scripts/summarize.py results/<date>_target_Os.csv --dcache on --mode cold
   python3 scripts/summarize.py results/<date>_target_Os.csv --dcache off --mode warm
   python3 scripts/plot.py results/<date>_target_Os.csv --dcache on --mode warm
   ```

   Use `--dist ascending|descending|prefix` for the other key distributions.
4. Fill in the target sections below, keeping the host and target tables separate.

## Configuration

### Common to all runs

- **Entry and node size.** `SortedEntry` is 12 B. `AvlNode` is 17 B of fields padded to **18 B**.
- **Capacity.** Both indexes are compiled with `NI_MAX_CONTACTS = 10001` for the benchmark: the 10,000-contact design maximum plus one spare slot, so that "insert into an index already holding 10,000 entries" can be measured. The unit tests use 10,000.
- **Sweep.** N = 100, 500, 1,000, 1,500, 2,000, 2,500, 3,000, 4,000, 5,000, 7,500 and 10,000. The sweep stops at 10,000 (the design maximum) instead of the brief's 50,000. It is dense between 1,500 and 4,000 because that is where the structures cross the size of the 32 KB D-cache.
- **Key and ordering.** The key is the ASCII lower-cased first 8 bytes of the name, zero padded. Entries are ordered by `memcmp` on the key, then by the tie-break callback: case-folded full name, then raw bytes, then sector number.
- **Names.** Each name is "First Last" built from two word lists in `bench_names.c`: 135 first names (8 to 11 letters for 16 of them) and 422 surnames, plus reserved words. The name table holds 4 B per contact (two word indices) plus the precomputed 8 B key, and lives in RAM. The tie-break counts its calls; that `volatile` increment is inside the timed region and costs the same for both indexes.
- **Distributions.** Seed 0x01352896 (xorshift32):
  - `random`: uniform first and last names.
  - `ascending` and `descending`: random names, inserted in index order or reverse order. This is the AVL degenerate case.
  - `prefix`: 30% of entries take one of the 16 long first names, so they share keys; the other 70% use first names of five letters or fewer.

  Each distribution draws its own names. The fraction of entries that share their 8-byte key with another entry was measured:

  | N | random | ascending | descending | prefix |
  |---:|---:|---:|---:|---:|
  | 1,000 | 29.4% | 30.5% | 30.4% | 32.4% |
  | 2,500 | 37.6% | 38.9% | 37.6% | 40.0% |
  | 10,000 | 57.2% | 57.1% | 57.0% | 55.5% |

  With realistic names, the `random` distribution already shares keys about as much as `prefix` does. The heavy-prefix distribution as specified ("about 30% share a prefix") is therefore **not** a harder case than `random` at these N. See the caveats.
- **What each operation times.** The timed region is the public API call only: `insert(name)`, `remove(name, sector)`, `find(name)` and so on. Those calls include building the key from the name string. Composing the string, picking the target, evicting the cache and restoring the structure all happen outside the timed region.
  - Insert repetitions remove the new entry again, untimed.
  - Remove repetitions reinsert the removed entry, untimed.
  - The array returns to exactly the same state. The AVL returns to the same set of entries but **not necessarily the same shape**, because rotations can differ. See the caveats.
- **Warm-up and repetitions.**
  - 50 untimed warm-up repetitions per point (2 for bulk builds).
  - Then 1,000 timed repetitions for single operations; 50 for host cold runs.
  - Traversal: `max(50, 1,000,000 / N)` repetitions, capped at 1,000.
  - Bulk builds: 50 / 10 / 5 repetitions for N ≤ 1,000 / ≤ 5,000 / above.
  - The `reps` column always holds the actual count.
- **Varying the targets.** Each repetition picks a random target (sector, name or rank), so repetitions do not hit the same cache lines. The RNG is reseeded for each (distribution, N, operation), so the array and the AVL get identical target sequences.
- **Results kept live.** Results go to a `volatile` sink, and compiler barriers surround the timer reads.
- **Statistics.** Per point: min, median, p99 and max of per-repetition time, plus the average and maximum number of tie-break calls. `bytes_total` is the header plus N × entry (or node) size.
- **Operations.**
  - `bulk_insert`: n sequential inserts in distribution order.
  - `bulk_rebuild`: append all entries unsorted, then one in-place heapsort; for the AVL, also link a perfectly balanced tree. Neither uses scratch memory or recursion in the sort. The AVL link step recurses about log2(n) deep.
  - `insert_*`, `remove_*`, `find_hit`, `find_miss` and `update_message`: single operations, as in the brief. `find` returns the first entry with that full name.
  - `traverse_all` and `traverse_per_entry`: one full in-order pass. `traverse_per_entry` is the integer quotient by N, so it is coarse on the host.
  - `page_rank20`: jump to a random rank k and read 20 entries. The array indexes directly. **The AVL walks from the start**, because nodes have no subtree sizes, so the cost is O(k).
  - `page_name20`: seek the first entry ≥ a name (O(log n) for both), then read 20.
- **Word-copy variant.** `sorted_array_wordcopy` is the sorted array with a hand-written 32-bit word loop instead of `memmove`. It is timed only for the operations that move entries: `bulk_insert`, `insert_*` and `remove_*`.

### Host run (2026-10-14)

- Intel Core Ultra 5 125H (L2 14 MiB, L3 18 MiB), Linux 7.0.0-38, gcc 13.3.0, CMake Release flags `-O3 -DNDEBUG`.
- Timer: `clock_gettime(CLOCK_MONOTONIC)`, in ns. An empty timed region costs a median of 34 ns (p99 275 ns) warm, and 12 ns (p99 24 ns) cold.
- No interrupt masking and no CPU pinning. Max and p99 include OS preemption and frequency changes.
- "Cold" means writing one byte per 64 B line of a 32 MiB buffer (larger than L3) before each repetition, with 50 repetitions per single operation.
- Run on an otherwise idle machine. A first run that overlapped with firmware builds was discarded and repeated.

### Target runs (prepared, not run)

| Setting | Value |
|---|---|
| MCU | STM32H723ZG (Cortex-M7), NUCLEO-H723ZG |
| Core clock | **64 MHz**: SYSCLK = HSI, PLL configured but not selected, VOS3. `SystemCoreClock` is recorded in `_meta.txt`. Convert with µs = cycles / 64. |
| Toolchain | arm-none-eabi-gcc 13.2.1 (Arm GNU 13.2.rel1), newlib-nano (`--specs=nano.specs`) |
| Flags | `-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -Wall -fdata-sections -ffunction-sections -fstack-usage` plus `-Os -g0` (Release) or `-O0 -g3` (Debug). These are the firmware's own flags, and the indexes are compiled in the same CMake project. The exact string is in `_meta.txt`. |
| Placement | Index arena (union of the two structures, used one at a time) and name table in **RAM_D1 / AXI SRAM** (`.ram_d1`). DTCM is not swept, per the decision to keep DTCM for phone code. |
| Caches | I-cache on throughout. Three passes: D-cache on warm, D-cache on cold (`SCB_CleanInvalidateDCache()` before each repetition), and D-cache off. With the D-cache off, cold and warm are the same, so only `warm` is emitted. The real D-cache geometry (expected 32 KB, 4-way, 32 B lines) is read from CCSIDR into `_meta.txt`. |
| MPU | CubeMX region 0 (4 GB background, no access, subregions 0x87) and region 1 (FMC LCD, 0x60000000). No region covers 0x24000000, so AXI SRAM uses the default map attributes (Normal, write-back, write-allocate). The live MPU registers are dumped into `_meta.txt`. |
| Timer | `DWT->CYCCNT`, with unsigned subtraction for wrap. An empty timed region is measured and reported as `timer,…,empty_timed_region`. |
| Preemption | The benchmark runs as a FreeRTOS task (`osPriorityHigh`), because the SD driver calls `osDelay()`. **PRIMASK is set** (all maskable IRQs off) around every timed index operation. The SD baseline runs with IRQs enabled because the driver needs the tick. |
| SD baseline | Run first, with the D-cache off, using the project's Storage and journal code (`sd_baseline.c`). It times `read_block`, `write_block`, `read_sector` with CRC check, and the journaled `write_contact`. Min, median and max are reported (p99 is also in the CSV). |
| Link | RAM_D1 304,136 / 327,680 B (92.8%), DTCM 59,608 B, FLASH 50,572 B (`-Os`) or 81,172 B (`-O0`) |

## Memory footprint

| N | Sorted array (B) | AVL tree (B) | AVL / array |
|---:|---:|---:|---:|
| 100 | 1,212 | 1,814 | 1.50 |
| 1,000 | 12,012 | 18,014 | 1.50 |
| 2,500 | 30,012 | 45,014 | 1.50 |
| 5,000 | 60,012 | 90,014 | 1.50 |
| 10,000 | 120,012 | 180,014 | 1.50 |

These are the `bytes_total` values: the header plus N entries. A static pool is sized to capacity, so what is actually reserved is `sizeof` at `NI_MAX_CONTACTS`: NameArray 120,024 B and AvlTree 180,032 B at 10,001 entries. At 10,000 entries that is 120,008 B and 180,012 B.

The structures cross the 32 KB D-cache at about 2,730 entries (array) and 1,820 entries (AVL).

**RAM_D1 budget (bytes, observation only).** The current default firmware already uses 204,200 B of RAM_D1. Most of that is the `db_main.c` hash table and its free lists: `HashEntry` 8 B × 14,293 = 114,344 B, plus 28,586 B and 57,172 B of free-list memory. Adding the index at a capacity of 10,000 gives:
- sorted array: 204,200 + 120,008 = 324,208 B, which leaves 3,472 B of the 327,680 B region;
- AVL: 204,200 + 180,012 = 384,212 B, which is 56,532 B over.

The full-name table the real tie-break callback would need is not included. This is the hardware-test layout; the production layout may differ.

**AVL recursion.** The maximum depth observed was:
- 17 levels for `random` and `prefix`;
- 15 levels for `ascending`;
- 16 levels for `descending`.

After insertion-order builds at N = 10,000, the tree height was 14 to 16. The bound is 1.44·log2(10,002) ≈ 19.1, and the iterator's depth limit is 24.

Stack frames from `-fstack-usage` (target):

| Function | `-Os` | `-O0` |
|---|---:|---:|
| `insert_rec` | 40 B | 48 B |
| `remove_rec` | 64 B | 56 B |
| `remove_min_rec` | 24 B | 32 B |
| `rebalance` (leaf call) | 24 B | 32 B |

At the observed depth of 17, the worst case is about 17 × 64 + 24 + 24 ≈ **1.1 KB** at `-Os`. At the theoretical bound of 20 levels, it is about 1.3 KB.

## Target results

**Not measured yet.** Run the target as described above, then add:
- per-operation tables of median and max cycles for the main configuration: D-cache on, AXI SRAM, `-Os`, warm and cold;
- a short section for D-cache off and for `-O0`;
- the SD baseline table: min, median and max cycles and µs for each SD operation.

**What to look for in the target data.** In the toolchain's newlib-nano, `memmove` with dst > src (overlapping) is a **byte loop** (`ldrb`/`strb`); this is what `na_insert` uses to open a gap. Forward moves of more than 15 bytes, with both pointers 4-byte aligned, use word copies; this is what `na_remove` uses to close a gap. This was verified by disassembling `memmove` in the linked ELF, and the array base is 4-byte aligned (checked at runtime, `array_base_align4=1`). So on target:
- array insert costs should be noticeably higher than array remove costs at the same N;
- `sorted_array_wordcopy` exists to measure that difference.

The host's glibc `memmove` is vectorised and cannot show this.

## Host results (non-authoritative, ns)

Main view: `random` distribution. "array" is the sorted array with `memmove`, "array_wc" is the word-copy variant, and "avl" is the AVL tree. Each cell is median / max over the repetitions. The "tb avg" column is the average number of tie-break callback calls per operation.

The crossover line comes from `scripts/summarize.py`. It names the smallest N from which the AVL median stays below the array median for every larger N in the sweep. N values where the two medians are within 10% of each other, or within the empty-timer cost, are flagged as too close to call.

### Warm

#### bulk_insert (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 5350 | 9354 | 3486 | 4668 | 4895 | 6066 | 9.00/8.00 |
| 500 | 36455 | 45129 | 46039 | 58217 | 37694 | 46118 | 178.00/183.00 |
| 1000 | 110871 | 119010 | 132483 | 153257 | 104239 | 125521 | 517.00/517.00 |
| 1500 | 201544 | 210953 | 257848 | 263622 | 201502 | 205246 | 985.00/990.00 |
| 2000 | 305026 | 346116 | 386268 | 421440 | 274490 | 278468 | 1472.00/1477.00 |
| 2500 | 403022 | 429828 | 566179 | 574581 | 378141 | 386922 | 2003.00/2020.00 |
| 3000 | 522203 | 534384 | 728511 | 736277 | 447268 | 533660 | 2626.00/2653.00 |
| 4000 | 805475 | 1009686 | 1181178 | 1191840 | 626973 | 635673 | 3918.00/3957.00 |
| 5000 | 1155089 | 1163296 | 1747292 | 3331890 | 871419 | 887706 | 5510.00/5568.00 |
| 7500 | 2532682 | 2557135 | 3700285 | 3884574 | 1362591 | 1367864 | 10047.00/10139.00 |
| 10000 | 4518645 | 4725114 | 6269116 | 6296826 | 1958696 | 2005488 | 14857.00/14997.00 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500, 2500: too close to call there.

#### bulk_rebuild (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 9991 | 17492 | 11915 | 23870 | 10.00/10.00 |
| 500 | 44899 | 92211 | 48761 | 64798 | 203.00/203.00 |
| 1000 | 103325 | 111499 | 106488 | 118190 | 542.00/542.00 |
| 1500 | 178204 | 193117 | 192217 | 195656 | 1045.00/1045.00 |
| 2000 | 236106 | 240997 | 256913 | 260882 | 1552.00/1552.00 |
| 2500 | 333354 | 343767 | 352238 | 357028 | 2076.00/2076.00 |
| 3000 | 391616 | 413543 | 419982 | 426789 | 2749.00/2749.00 |
| 4000 | 554786 | 575807 | 590433 | 595331 | 4056.00/4056.00 |
| 5000 | 726962 | 730926 | 781948 | 789826 | 5755.00/5755.00 |
| 7500 | 1195010 | 1206634 | 1270035 | 1279542 | 10373.00/10373.00 |
| 10000 | 1726237 | 1780106 | 1879246 | 1885971 | 15352.00/15352.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000: too close to call there.

#### insert_random (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 147 | 384 | 115 | 257 | 166 | 432 | 0.15/0.15 |
| 500 | 129 | 897 | 155 | 543 | 132 | 454 | 0.56/0.53 |
| 1000 | 162 | 420 | 212 | 896 | 148 | 362 | 0.74/0.74 |
| 1500 | 193 | 824 | 272 | 1608 | 162 | 348 | 1.03/1.06 |
| 2000 | 210 | 462 | 305 | 5172 | 164 | 379 | 1.10/1.12 |
| 2500 | 241 | 1147 | 370 | 2077 | 177 | 459 | 1.06/1.07 |
| 3000 | 261 | 579 | 417 | 812 | 179 | 765 | 1.32/1.31 |
| 4000 | 316 | 2614 | 508 | 1015 | 194 | 729 | 1.43/1.44 |
| 5000 | 352 | 787 | 592 | 2803 | 199 | 4968 | 1.71/1.71 |
| 7500 | 487 | 1268 | 834 | 1862 | 213 | 462 | 1.93/1.95 |
| 10000 | 713 | 1860 | 1155 | 2841 | 235 | 541 | 2.22/2.19 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500: too close to call there.

#### insert_front (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 74 | 123 | 82 | 6585 | 97 | 874 | 0.00/0.00 |
| 500 | 84 | 255 | 142 | 2244 | 61 | 200 | 0.00/0.00 |
| 1000 | 125 | 150 | 237 | 3533 | 63 | 226 | 0.00/0.00 |
| 1500 | 167 | 174 | 331 | 358 | 74 | 88 | 0.00/0.00 |
| 2000 | 198 | 1490 | 409 | 1918 | 71 | 240 | 0.00/0.00 |
| 2500 | 249 | 3291 | 528 | 2144 | 74 | 78 | 0.00/0.00 |
| 3000 | 275 | 320 | 595 | 638 | 75 | 93 | 0.00/0.00 |
| 4000 | 362 | 917 | 784 | 852 | 80 | 96 | 0.00/0.00 |
| 5000 | 516 | 1859 | 1006 | 2466 | 78 | 110 | 0.00/0.00 |
| 7500 | 947 | 5577 | 1581 | 2814 | 79 | 94 | 0.00/0.00 |
| 10000 | 1399 | 4643 | 2113 | 3361 | 85 | 104 | 0.00/0.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500: too close to call there.

#### insert_end (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 52 | 56 | 44 | 58 | 61 | 65 | 0.00/0.00 |
| 500 | 37 | 135 | 37 | 52 | 61 | 134 | 0.00/0.00 |
| 1000 | 50 | 70 | 46 | 52 | 79 | 267 | 0.00/0.00 |
| 1500 | 44 | 155 | 43 | 49 | 79 | 96 | 0.00/0.00 |
| 2000 | 43 | 96 | 41 | 45 | 79 | 99 | 0.00/0.00 |
| 2500 | 55 | 183 | 55 | 82 | 80 | 122 | 0.00/0.00 |
| 3000 | 46 | 52 | 44 | 102 | 80 | 1661 | 0.00/0.00 |
| 4000 | 53 | 58 | 51 | 4699 | 79 | 1569 | 0.00/0.00 |
| 5000 | 47 | 57 | 47 | 441 | 79 | 96 | 0.00/0.00 |
| 7500 | 67 | 104 | 66 | 83 | 79 | 4723 | 0.00/0.00 |
| 10000 | 60 | 65 | 58 | 84 | 91 | 292 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 2500, 4000, 5000, 7500, 10000: too close to call there.

#### remove_random (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 144 | 304 | 146 | 273 | 158 | 270 | 1.08/1.08 |
| 500 | 169 | 3753 | 190 | 919 | 183 | 497 | 1.32/1.33 |
| 1000 | 198 | 2089 | 235 | 931 | 196 | 651 | 1.45/1.54 |
| 1500 | 234 | 1938 | 308 | 615 | 207 | 499 | 1.68/1.61 |
| 2000 | 247 | 486 | 337 | 1936 | 202 | 387 | 1.69/1.68 |
| 2500 | 275 | 721 | 400 | 1806 | 218 | 460 | 1.75/1.74 |
| 3000 | 292 | 2140 | 423 | 857 | 212 | 460 | 1.75/1.81 |
| 4000 | 339 | 1815 | 517 | 5077 | 229 | 4979 | 1.95/1.97 |
| 5000 | 408 | 959 | 669 | 18350 | 231 | 476 | 1.94/1.97 |
| 7500 | 606 | 2207 | 875 | 2114 | 236 | 503 | 2.11/2.12 |
| 10000 | 905 | 2304 | 1268 | 7052 | 266 | 588 | 2.44/2.45 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500: too close to call there.

#### remove_first (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 94 | 109 | 115 | 117 | 77 | 2253 | 1.00/1.00 |
| 500 | 140 | 459 | 173 | 189 | 129 | 181 | 3.00/3.00 |
| 1000 | 185 | 195 | 238 | 838 | 150 | 521 | 3.00/4.00 |
| 1500 | 241 | 6055 | 370 | 1698 | 156 | 2030 | 4.00/4.00 |
| 2000 | 269 | 1434 | 382 | 429 | 167 | 1395 | 4.00/5.00 |
| 2500 | 336 | 1687 | 492 | 2508 | 165 | 170 | 5.00/5.00 |
| 3000 | 386 | 1713 | 573 | 607 | 162 | 165 | 5.00/5.00 |
| 4000 | 478 | 1803 | 678 | 758 | 193 | 1697 | 5.00/7.00 |
| 5000 | 627 | 642 | 911 | 4529 | 196 | 1442 | 6.00/7.00 |
| 7500 | 1041 | 1101 | 1299 | 1606 | 215 | 1666 | 7.00/8.00 |
| 10000 | 1486 | 3392 | 1749 | 7401 | 205 | 209 | 8.00/7.00 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500: too close to call there.

#### remove_last (random, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 70 | 86 | 68 | 70 | 76 | 92 | 1.00/1.00 |
| 500 | 75 | 99 | 73 | 81 | 81 | 312 | 1.00/1.00 |
| 1000 | 84 | 87 | 80 | 140 | 92 | 343 | 1.00/1.00 |
| 1500 | 86 | 154 | 86 | 264 | 104 | 363 | 1.00/1.00 |
| 2000 | 82 | 98 | 81 | 85 | 99 | 758 | 1.00/1.00 |
| 2500 | 87 | 2318 | 86 | 1565 | 99 | 102 | 1.00/1.00 |
| 3000 | 85 | 4719 | 84 | 91 | 92 | 2352 | 1.00/1.00 |
| 4000 | 79 | 778 | 80 | 91 | 117 | 120 | 1.00/2.00 |
| 5000 | 111 | 117 | 115 | 120 | 116 | 196 | 2.00/2.00 |
| 7500 | 87 | 92 | 86 | 91 | 117 | 141 | 1.00/2.00 |
| 10000 | 98 | 103 | 96 | 100 | 114 | 127 | 1.00/1.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500, 2000, 2500, 3000, 5000, 7500, 10000: too close to call there.

#### find_hit (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 155 | 3421 | 109 | 246 | 2.15/1.15 |
| 500 | 191 | 2915 | 145 | 2279 | 2.46/1.52 |
| 1000 | 208 | 2035 | 156 | 581 | 2.68/1.70 |
| 1500 | 209 | 683 | 164 | 358 | 2.78/1.76 |
| 2000 | 207 | 450 | 163 | 1228 | 2.89/1.88 |
| 2500 | 229 | 738 | 175 | 400 | 2.99/2.00 |
| 3000 | 219 | 466 | 173 | 386 | 3.09/2.08 |
| 4000 | 224 | 555 | 180 | 414 | 3.16/2.13 |
| 5000 | 228 | 524 | 183 | 456 | 3.33/2.31 |
| 7500 | 239 | 515 | 197 | 479 | 3.52/2.53 |
| 10000 | 247 | 583 | 210 | 1969 | 3.69/2.61 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep.

#### find_miss (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 79 | 182 | 66 | 137 | 0.19/0.15 |
| 500 | 88 | 335 | 84 | 356 | 0.71/0.53 |
| 1000 | 96 | 256 | 90 | 212 | 0.92/0.72 |
| 1500 | 100 | 336 | 90 | 243 | 1.16/0.92 |
| 2000 | 100 | 1521 | 95 | 237 | 1.17/0.94 |
| 2500 | 96 | 314 | 103 | 301 | 1.19/1.07 |
| 3000 | 101 | 1109 | 98 | 284 | 1.27/1.04 |
| 4000 | 108 | 305 | 103 | 320 | 1.34/1.17 |
| 5000 | 98 | 352 | 110 | 1742 | 1.39/1.18 |
| 7500 | 110 | 386 | 115 | 343 | 1.55/1.33 |
| 10000 | 123 | 404 | 127 | 362 | 1.81/1.60 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000: too close to call there.

#### update_message (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 108 | 1568 | 88 | 204 | 1.07/1.07 |
| 500 | 145 | 478 | 130 | 311 | 1.34/1.35 |
| 1000 | 155 | 322 | 148 | 319 | 1.46/1.48 |
| 1500 | 159 | 391 | 156 | 442 | 1.66/1.65 |
| 2000 | 160 | 382 | 160 | 832 | 1.71/1.74 |
| 2500 | 172 | 431 | 168 | 494 | 1.80/1.84 |
| 3000 | 165 | 446 | 159 | 406 | 1.81/1.81 |
| 4000 | 180 | 2472 | 166 | 2815 | 1.92/1.89 |
| 5000 | 184 | 443 | 175 | 415 | 2.17/2.16 |
| 7500 | 191 | 4770 | 186 | 497 | 2.20/2.28 |
| 10000 | 205 | 1614 | 204 | 447 | 2.44/2.51 |

Crossover: from N = 2500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000: too close to call there.

#### traverse_all (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 201 | 208 | 236 | 2340 | 0.00/0.00 |
| 500 | 754 | 3207 | 1268 | 4106 | 0.00/0.00 |
| 1000 | 1501 | 3626 | 2436 | 6362 | 0.00/0.00 |
| 1500 | 2201 | 9657 | 4098 | 5639 | 0.00/0.00 |
| 2000 | 2848 | 4351 | 6396 | 11382 | 0.00/0.00 |
| 2500 | 3551 | 5845 | 8633 | 12337 | 0.00/0.00 |
| 3000 | 4239 | 5612 | 9623 | 17509 | 0.00/0.00 |
| 4000 | 5613 | 7357 | 17239 | 22062 | 0.00/0.00 |
| 5000 | 7050 | 11686 | 20765 | 28137 | 0.00/0.00 |
| 7500 | 10564 | 15017 | 41568 | 46990 | 0.00/0.00 |
| 10000 | 15096 | 17895 | 66935 | 72664 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### traverse_per_entry (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 2 | 2 | 2 | 23 | 0.00/0.00 |
| 500 | 1 | 6 | 2 | 8 | 0.00/0.00 |
| 1000 | 1 | 3 | 2 | 6 | 0.00/0.00 |
| 1500 | 1 | 6 | 2 | 3 | 0.00/0.00 |
| 2000 | 1 | 2 | 3 | 5 | 0.00/0.00 |
| 2500 | 1 | 2 | 3 | 4 | 0.00/0.00 |
| 3000 | 1 | 1 | 3 | 5 | 0.00/0.00 |
| 4000 | 1 | 1 | 4 | 5 | 0.00/0.00 |
| 5000 | 1 | 2 | 4 | 5 | 0.00/0.00 |
| 7500 | 1 | 2 | 5 | 6 | 0.00/0.00 |
| 10000 | 1 | 1 | 6 | 7 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000: too close to call there.

#### page_rank20 (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 50 | 52 | 139 | 694 | 0.00/0.00 |
| 500 | 46 | 53 | 518 | 2641 | 0.00/0.00 |
| 1000 | 46 | 51 | 1017 | 4066 | 0.00/0.00 |
| 1500 | 46 | 49 | 1448 | 4614 | 0.00/0.00 |
| 2000 | 44 | 78 | 1769 | 4132 | 0.00/0.00 |
| 2500 | 44 | 49 | 2144 | 6281 | 0.00/0.00 |
| 3000 | 44 | 53 | 2557 | 6702 | 0.00/0.00 |
| 4000 | 44 | 51 | 3416 | 9580 | 0.00/0.00 |
| 5000 | 44 | 63 | 4380 | 9905 | 0.00/0.00 |
| 7500 | 44 | 50 | 6641 | 21138 | 0.00/0.00 |
| 10000 | 46 | 51 | 8394 | 24774 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### page_name20 (random, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 148 | 7066 | 152 | 292 | 1.15/1.15 |
| 500 | 179 | 311 | 263 | 462 | 1.54/1.56 |
| 1000 | 191 | 2011 | 289 | 2002 | 1.61/1.64 |
| 1500 | 199 | 436 | 312 | 539 | 1.85/1.82 |
| 2000 | 194 | 390 | 300 | 3171 | 1.87/1.89 |
| 2500 | 203 | 430 | 312 | 919 | 1.96/1.98 |
| 3000 | 202 | 425 | 318 | 3950 | 2.09/2.06 |
| 4000 | 212 | 944 | 332 | 599 | 2.10/2.10 |
| 5000 | 220 | 471 | 341 | 2392 | 2.36/2.33 |
| 7500 | 228 | 532 | 356 | 676 | 2.57/2.52 |
| 10000 | 248 | 599 | 364 | 1997 | 2.69/2.68 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 34 timer overhead) at N = 100: too close to call there.

### Cold

#### bulk_insert (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 4770 | 7135 | 4988 | 9859 | 6368 | 10449 | 9.00/8.00 |
| 500 | 40387 | 77414 | 50991 | 100319 | 39046 | 54910 | 178.00/183.00 |
| 1000 | 116361 | 126499 | 142505 | 151403 | 115367 | 196324 | 517.00/517.00 |
| 1500 | 225824 | 319248 | 291957 | 339566 | 203894 | 211740 | 985.00/990.00 |
| 2000 | 298944 | 323403 | 413784 | 425239 | 281026 | 298555 | 1472.00/1477.00 |
| 2500 | 462254 | 679959 | 559845 | 582286 | 379507 | 391735 | 2003.00/2020.00 |
| 3000 | 533629 | 542417 | 780654 | 792828 | 483995 | 531851 | 2626.00/2653.00 |
| 4000 | 853457 | 867119 | 1206920 | 1250579 | 657976 | 697271 | 3918.00/3957.00 |
| 5000 | 1224004 | 1244525 | 1734921 | 1741338 | 843097 | 878590 | 5510.00/5568.00 |
| 7500 | 2538306 | 2575193 | 3855061 | 4146115 | 1383314 | 1408809 | 10047.00/10139.00 |
| 10000 | 4509384 | 4928415 | 6588260 | 6614422 | 2024738 | 2201376 | 14857.00/14997.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 12 timer overhead) at N = 500, 1000, 1500, 2000, 3000: too close to call there.

#### bulk_rebuild (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 8598 | 11417 | 20898 | 29526 | 10.00/10.00 |
| 500 | 46704 | 493789 | 57954 | 65403 | 203.00/203.00 |
| 1000 | 111310 | 150731 | 131431 | 146983 | 542.00/542.00 |
| 1500 | 182213 | 211896 | 212003 | 316050 | 1045.00/1045.00 |
| 2000 | 251580 | 295144 | 270983 | 280240 | 1552.00/1552.00 |
| 2500 | 348319 | 418850 | 371627 | 402122 | 2076.00/2076.00 |
| 3000 | 398501 | 404115 | 463331 | 511319 | 2749.00/2749.00 |
| 4000 | 567864 | 603514 | 638128 | 651845 | 4056.00/4056.00 |
| 5000 | 779682 | 787841 | 840148 | 867641 | 5755.00/5755.00 |
| 7500 | 1270113 | 1304922 | 1320528 | 1339398 | 10373.00/10373.00 |
| 10000 | 1721461 | 1793992 | 1813690 | 1838209 | 15352.00/15352.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 12 timer overhead) at N = 2000, 2500, 5000, 7500, 10000: too close to call there.

#### insert_random (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 685 | 1523 | 578 | 1736 | 939 | 2541 | 0.14/0.14 |
| 500 | 797 | 2176 | 934 | 2891 | 1327 | 3154 | 0.64/0.62 |
| 1000 | 1146 | 3408 | 1465 | 3715 | 1908 | 2972 | 0.66/0.66 |
| 1500 | 1651 | 3189 | 1804 | 5341 | 2243 | 4612 | 0.84/0.88 |
| 2000 | 2117 | 4973 | 2678 | 5177 | 2457 | 18976 | 1.16/1.08 |
| 2500 | 2295 | 13820 | 2187 | 5323 | 2505 | 4855 | 1.02/0.96 |
| 3000 | 2434 | 4665 | 2922 | 6031 | 2595 | 5587 | 1.10/1.04 |
| 4000 | 3294 | 9993 | 2980 | 5649 | 2519 | 5958 | 1.30/1.22 |
| 5000 | 3110 | 8607 | 3050 | 7419 | 2531 | 9380 | 1.74/1.72 |
| 7500 | 3996 | 7949 | 4159 | 10527 | 2420 | 4938 | 1.98/2.06 |
| 10000 | 4410 | 10545 | 5116 | 16738 | 2909 | 9074 | 1.72/1.66 |

Crossover: from N = 4000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 12 timer overhead) at N = 2500, 3000: too close to call there.

#### insert_front (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 667 | 1057 | 539 | 791 | 739 | 1142 | 0.00/0.00 |
| 500 | 780 | 1173 | 870 | 1516 | 761 | 6897 | 0.00/0.00 |
| 1000 | 1231 | 1616 | 1479 | 2333 | 1213 | 1785 | 0.00/0.00 |
| 1500 | 1691 | 3467 | 2003 | 3900 | 1316 | 3660 | 0.00/0.00 |
| 2000 | 1955 | 5438 | 2296 | 4639 | 1352 | 2022 | 0.00/0.00 |
| 2500 | 2392 | 5970 | 2513 | 3654 | 1568 | 2185 | 0.00/0.00 |
| 3000 | 2718 | 6237 | 3393 | 9149 | 1618 | 3708 | 0.00/0.00 |
| 4000 | 3912 | 8468 | 3730 | 5194 | 1588 | 1983 | 0.00/0.00 |
| 5000 | 3566 | 5091 | 4604 | 9154 | 1632 | 2150 | 0.00/0.00 |
| 7500 | 5421 | 10206 | 5353 | 13398 | 1962 | 4175 | 0.00/0.00 |
| 10000 | 6493 | 11366 | 6429 | 9102 | 2050 | 2821 | 0.00/0.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 12 timer overhead) at N = 100, 500, 1000: too close to call there.

#### insert_end (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 379 | 997 | 374 | 1083 | 743 | 1051 | 0.00/0.00 |
| 500 | 619 | 1100 | 467 | 937 | 895 | 1960 | 0.00/0.00 |
| 1000 | 515 | 1235 | 662 | 1039 | 1190 | 1969 | 0.00/0.00 |
| 1500 | 696 | 1529 | 548 | 1013 | 1334 | 3386 | 0.00/0.00 |
| 2000 | 772 | 1829 | 733 | 1825 | 1488 | 2722 | 0.00/0.00 |
| 2500 | 681 | 1441 | 778 | 1352 | 1576 | 2178 | 0.00/0.00 |
| 3000 | 627 | 1586 | 842 | 1544 | 1618 | 3050 | 0.00/0.00 |
| 4000 | 591 | 1226 | 658 | 1048 | 1365 | 1931 | 0.00/0.00 |
| 5000 | 671 | 933 | 713 | 5605 | 1643 | 2139 | 0.00/0.00 |
| 7500 | 619 | 1184 | 544 | 1065 | 1431 | 1939 | 0.00/0.00 |
| 10000 | 688 | 1004 | 644 | 1102 | 1816 | 2940 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### remove_random (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 1246 | 1797 | 1307 | 2283 | 1310 | 2468 | 1.10/1.20 |
| 500 | 1813 | 6458 | 1628 | 3216 | 1777 | 3100 | 1.28/1.44 |
| 1000 | 2819 | 3658 | 2147 | 4242 | 2203 | 7236 | 1.50/1.42 |
| 1500 | 2268 | 4237 | 2290 | 3721 | 2522 | 3659 | 1.36/1.28 |
| 2000 | 3180 | 5462 | 3139 | 5224 | 3047 | 5081 | 1.66/1.82 |
| 2500 | 3056 | 8548 | 3397 | 5187 | 2960 | 6836 | 2.14/2.14 |
| 3000 | 3255 | 5722 | 3370 | 14072 | 3413 | 11171 | 1.62/1.66 |
| 4000 | 3094 | 4969 | 3192 | 6315 | 2775 | 4068 | 1.86/1.64 |
| 5000 | 3662 | 5458 | 3886 | 6212 | 2796 | 3965 | 2.18/2.00 |
| 7500 | 4288 | 7756 | 4345 | 9752 | 2951 | 4211 | 2.44/2.20 |
| 10000 | 5735 | 9878 | 5734 | 11161 | 3490 | 4804 | 2.92/2.56 |

Crossover: from N = 4000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 12 timer overhead) at N = 100, 500, 2000, 2500, 3000: too close to call there.

#### remove_first (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 1115 | 1780 | 1151 | 1775 | 1104 | 1369 | 1.00/1.00 |
| 500 | 1698 | 4061 | 1529 | 2229 | 1487 | 7570 | 3.00/3.00 |
| 1000 | 2172 | 3446 | 2417 | 7522 | 2136 | 2779 | 3.00/4.00 |
| 1500 | 2681 | 4707 | 2591 | 5029 | 2631 | 4319 | 4.00/5.00 |
| 2000 | 3721 | 4995 | 2810 | 4421 | 2728 | 3917 | 4.00/5.00 |
| 2500 | 3148 | 8148 | 4002 | 7895 | 2467 | 3567 | 5.00/5.00 |
| 3000 | 3420 | 7440 | 4016 | 10486 | 2671 | 4002 | 5.00/5.00 |
| 4000 | 4469 | 5821 | 3972 | 7652 | 2470 | 5270 | 5.00/7.00 |
| 5000 | 4669 | 6271 | 4834 | 7801 | 2410 | 4583 | 6.00/7.00 |
| 7500 | 5846 | 7371 | 6127 | 9413 | 2900 | 3605 | 7.00/7.00 |
| 10000 | 8237 | 21197 | 8100 | 10771 | 2655 | 3685 | 8.00/7.00 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 12 timer overhead) at N = 100, 1000, 1500: too close to call there.

#### remove_last (random, cold, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 858 | 1741 | 914 | 1459 | 1063 | 1619 | 1.00/1.00 |
| 500 | 883 | 1464 | 913 | 1382 | 1269 | 1713 | 1.00/1.00 |
| 1000 | 1042 | 1722 | 1106 | 1646 | 1405 | 2554 | 1.00/1.00 |
| 1500 | 1079 | 2251 | 1038 | 2374 | 1539 | 3656 | 1.00/1.00 |
| 2000 | 1027 | 2187 | 1134 | 1727 | 1721 | 2814 | 1.00/1.00 |
| 2500 | 1090 | 1977 | 1127 | 2664 | 1661 | 2400 | 1.00/1.00 |
| 3000 | 1239 | 1906 | 1490 | 3714 | 1532 | 8875 | 1.00/1.00 |
| 4000 | 1041 | 1503 | 1034 | 2027 | 1512 | 6958 | 1.00/2.00 |
| 5000 | 1283 | 1941 | 1306 | 2127 | 1877 | 2605 | 2.00/2.00 |
| 7500 | 988 | 1657 | 1138 | 2826 | 1799 | 2464 | 1.00/2.00 |
| 10000 | 1236 | 2197 | 1066 | 1405 | 1978 | 3179 | 1.00/1.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### find_hit (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 1164 | 1976 | 1007 | 1751 | 2.08/1.04 |
| 500 | 1395 | 7158 | 1951 | 3603 | 2.34/1.36 |
| 1000 | 1563 | 6058 | 1852 | 3631 | 2.50/1.60 |
| 1500 | 1623 | 3364 | 2705 | 4175 | 3.08/2.16 |
| 2000 | 2383 | 3621 | 2892 | 4073 | 2.98/1.92 |
| 2500 | 1755 | 3795 | 2583 | 4278 | 3.02/2.08 |
| 3000 | 1943 | 3537 | 2337 | 5550 | 3.20/2.16 |
| 4000 | 1881 | 4120 | 2400 | 3561 | 3.20/2.12 |
| 5000 | 1887 | 3749 | 2691 | 4197 | 3.04/2.08 |
| 7500 | 2135 | 6723 | 2614 | 5176 | 3.98/3.04 |
| 10000 | 1913 | 3903 | 2822 | 4212 | 4.10/3.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### find_miss (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 520 | 1688 | 704 | 1946 | 0.24/0.18 |
| 500 | 684 | 3134 | 1089 | 2539 | 0.80/0.60 |
| 1000 | 931 | 5331 | 1413 | 3131 | 0.64/0.52 |
| 1500 | 876 | 3331 | 2110 | 4249 | 1.02/0.92 |
| 2000 | 1104 | 3897 | 1691 | 4248 | 1.20/0.90 |
| 2500 | 1037 | 3064 | 1754 | 3531 | 1.26/1.12 |
| 3000 | 1021 | 3865 | 1730 | 3861 | 0.98/0.84 |
| 4000 | 945 | 8839 | 1620 | 3698 | 0.76/0.68 |
| 5000 | 1037 | 2667 | 1802 | 7410 | 1.00/0.82 |
| 7500 | 1071 | 3229 | 2041 | 3996 | 2.08/1.88 |
| 10000 | 1115 | 4278 | 2027 | 4255 | 1.26/1.22 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### update_message (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 1177 | 2006 | 989 | 1606 | 1.10/1.10 |
| 500 | 1458 | 2952 | 2069 | 3669 | 1.42/1.50 |
| 1000 | 1771 | 3872 | 1642 | 2720 | 1.56/1.48 |
| 1500 | 1658 | 3327 | 1873 | 3787 | 1.82/1.72 |
| 2000 | 1907 | 2998 | 2407 | 3795 | 1.32/1.46 |
| 2500 | 1933 | 4955 | 2095 | 4448 | 1.76/1.60 |
| 3000 | 1956 | 3637 | 1983 | 3601 | 1.98/1.82 |
| 4000 | 1529 | 3033 | 2315 | 3449 | 1.78/1.84 |
| 5000 | 1756 | 2732 | 2605 | 4123 | 1.84/1.88 |
| 7500 | 2061 | 4793 | 2492 | 7532 | 2.40/2.18 |
| 10000 | 2334 | 4450 | 2491 | 3985 | 2.14/1.92 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 12 timer overhead) at N = 1000, 2500, 3000, 10000: too close to call there.

#### traverse_all (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 470 | 3392 | 811 | 6938 | 0.00/0.00 |
| 500 | 1202 | 6602 | 3082 | 9506 | 0.00/0.00 |
| 1000 | 1941 | 8387 | 5719 | 27610 | 0.00/0.00 |
| 1500 | 2691 | 10021 | 7705 | 37648 | 0.00/0.00 |
| 2000 | 3336 | 12975 | 9873 | 25632 | 0.00/0.00 |
| 2500 | 4154 | 9884 | 12285 | 27169 | 0.00/0.00 |
| 3000 | 4827 | 11948 | 15515 | 46497 | 0.00/0.00 |
| 4000 | 6210 | 9825 | 23210 | 50562 | 0.00/0.00 |
| 5000 | 7563 | 13341 | 31138 | 64442 | 0.00/0.00 |
| 7500 | 11235 | 23817 | 55487 | 97813 | 0.00/0.00 |
| 10000 | 14497 | 32934 | 82019 | 93861 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### traverse_per_entry (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 4 | 33 | 8 | 69 | 0.00/0.00 |
| 500 | 2 | 13 | 6 | 19 | 0.00/0.00 |
| 1000 | 1 | 8 | 5 | 27 | 0.00/0.00 |
| 1500 | 1 | 6 | 5 | 25 | 0.00/0.00 |
| 2000 | 1 | 6 | 4 | 12 | 0.00/0.00 |
| 2500 | 1 | 3 | 4 | 10 | 0.00/0.00 |
| 3000 | 1 | 3 | 5 | 15 | 0.00/0.00 |
| 4000 | 1 | 2 | 5 | 12 | 0.00/0.00 |
| 5000 | 1 | 2 | 6 | 12 | 0.00/0.00 |
| 7500 | 1 | 3 | 7 | 13 | 0.00/0.00 |
| 10000 | 1 | 3 | 8 | 9 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N). Medians within 10% (or within the 12 timer overhead) at N = 100, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7500, 10000: too close to call there.

#### page_rank20 (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 312 | 568 | 729 | 1107 | 0.00/0.00 |
| 500 | 311 | 668 | 2283 | 3368 | 0.00/0.00 |
| 1000 | 273 | 505 | 3998 | 5763 | 0.00/0.00 |
| 1500 | 235 | 620 | 5653 | 9213 | 0.00/0.00 |
| 2000 | 240 | 430 | 8699 | 14458 | 0.00/0.00 |
| 2500 | 334 | 594 | 9487 | 16840 | 0.00/0.00 |
| 3000 | 230 | 614 | 12608 | 20274 | 0.00/0.00 |
| 4000 | 316 | 438 | 15456 | 27928 | 0.00/0.00 |
| 5000 | 322 | 531 | 19495 | 25480 | 0.00/0.00 |
| 7500 | 215 | 483 | 28086 | 50831 | 0.00/0.00 |
| 10000 | 316 | 552 | 42990 | 66426 | 0.00/0.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### page_name20 (random, cold, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 1610 | 3025 | 1372 | 6514 | 1.20/1.16 |
| 500 | 1664 | 3461 | 2522 | 3513 | 1.50/1.42 |
| 1000 | 1764 | 4221 | 2983 | 8735 | 1.62/1.74 |
| 1500 | 1822 | 8367 | 3264 | 6308 | 1.96/1.82 |
| 2000 | 1875 | 5005 | 4002 | 6659 | 1.68/1.66 |
| 2500 | 1668 | 7514 | 3874 | 4830 | 1.92/1.98 |
| 3000 | 1881 | 3483 | 4102 | 10036 | 2.22/2.22 |
| 4000 | 2042 | 3602 | 3991 | 6081 | 2.20/2.00 |
| 5000 | 1990 | 4416 | 3961 | 5763 | 2.30/2.38 |
| 7500 | 2165 | 3734 | 4108 | 6267 | 2.56/2.72 |
| 10000 | 2397 | 4426 | 4545 | 6155 | 2.42/2.32 |

Crossover: none (the AVL median is not below the array median at the largest N).

### Other distributions (host, warm)

The tables below cover the operations most sensitive to insertion order and key sharing. Full data is in the CSV.


#### bulk_insert (ascending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 1959 | 3440 | 1894 | 2417 | 3831 | 8472 | 16.00/17.00 |
| 500 | 17578 | 19885 | 16746 | 19829 | 28009 | 30742 | 158.00/196.00 |
| 1000 | 45599 | 48356 | 44423 | 49473 | 68255 | 74313 | 503.00/616.00 |
| 1500 | 80700 | 85645 | 78379 | 84169 | 114335 | 124511 | 908.00/1080.00 |
| 2000 | 117370 | 123000 | 109665 | 113395 | 162921 | 167332 | 1358.00/1663.00 |
| 2500 | 149705 | 157501 | 146927 | 151374 | 214797 | 224460 | 1949.00/2288.00 |
| 3000 | 185751 | 189082 | 179660 | 184906 | 268317 | 273265 | 2421.00/2908.00 |
| 4000 | 260275 | 315965 | 254298 | 261162 | 382547 | 421541 | 3583.00/4383.00 |
| 5000 | 457842 | 1103125 | 362261 | 400253 | 537105 | 550737 | 5161.00/6051.00 |
| 7500 | 604972 | 605887 | 594723 | 599440 | 824795 | 835089 | 8934.00/10486.00 |
| 10000 | 830669 | 837711 | 859766 | 869973 | 1169199 | 1194713 | 13665.00/15869.00 |

Crossover: none (the AVL median is not below the array median at the largest N).

#### insert_random (ascending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 71 | 256 | 83 | 271 | 88 | 230 | 0.16/0.15 |
| 500 | 120 | 290 | 147 | 1523 | 120 | 1656 | 0.48/0.46 |
| 1000 | 161 | 904 | 217 | 420 | 142 | 309 | 0.82/0.82 |
| 1500 | 192 | 390 | 269 | 525 | 163 | 592 | 1.12/1.08 |
| 2000 | 223 | 496 | 320 | 1657 | 158 | 346 | 1.15/1.13 |
| 2500 | 241 | 492 | 380 | 753 | 161 | 367 | 1.12/1.13 |
| 3000 | 262 | 1751 | 422 | 2531 | 176 | 396 | 1.28/1.22 |
| 4000 | 313 | 686 | 520 | 5400 | 177 | 440 | 1.44/1.42 |
| 5000 | 381 | 3783 | 656 | 2426 | 203 | 2549 | 1.58/1.57 |
| 7500 | 511 | 3052 | 900 | 2232 | 207 | 437 | 1.70/1.71 |
| 10000 | 682 | 4572 | 1206 | 7347 | 213 | 490 | 1.93/1.93 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500: too close to call there.

#### insert_front (ascending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 40 | 46 | 52 | 55 | 55 | 70 | 0.00/0.00 |
| 500 | 81 | 93 | 136 | 138 | 65 | 79 | 0.00/0.00 |
| 1000 | 120 | 1570 | 226 | 238 | 71 | 6312 | 0.00/0.00 |
| 1500 | 161 | 513 | 322 | 1742 | 71 | 88 | 0.00/0.00 |
| 2000 | 207 | 216 | 412 | 3695 | 76 | 149 | 0.00/0.00 |
| 2500 | 237 | 278 | 506 | 563 | 70 | 74 | 0.00/0.00 |
| 3000 | 275 | 1791 | 596 | 1916 | 76 | 95 | 0.00/0.00 |
| 4000 | 363 | 1647 | 784 | 2009 | 79 | 109 | 0.00/0.00 |
| 5000 | 541 | 1170 | 1052 | 4133 | 84 | 254 | 0.00/0.00 |
| 7500 | 978 | 3862 | 1584 | 6197 | 86 | 106 | 0.00/0.00 |
| 10000 | 1338 | 2782 | 2214 | 7230 | 84 | 103 | 0.00/0.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500: too close to call there.

#### remove_random (ascending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 107 | 259 | 116 | 259 | 142 | 283 | 1.12/1.13 |
| 500 | 159 | 355 | 180 | 1987 | 168 | 343 | 1.31/1.32 |
| 1000 | 191 | 384 | 239 | 1387 | 186 | 466 | 1.53/1.47 |
| 1500 | 224 | 2396 | 293 | 1969 | 201 | 1853 | 1.59/1.60 |
| 2000 | 261 | 505 | 336 | 675 | 202 | 393 | 1.69/1.68 |
| 2500 | 278 | 2310 | 395 | 2148 | 207 | 386 | 1.73/1.75 |
| 3000 | 289 | 1451 | 427 | 860 | 210 | 468 | 1.77/1.78 |
| 4000 | 341 | 1619 | 512 | 2067 | 227 | 473 | 1.93/1.94 |
| 5000 | 419 | 1697 | 656 | 3118 | 238 | 2431 | 2.13/2.07 |
| 7500 | 621 | 1538 | 872 | 2879 | 229 | 4921 | 2.10/2.15 |
| 10000 | 844 | 2274 | 1258 | 4611 | 239 | 533 | 2.32/2.32 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 500, 1000, 1500: too close to call there.

#### find_hit (ascending, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 137 | 386 | 95 | 239 | 2.18/1.16 |
| 500 | 181 | 386 | 136 | 4859 | 2.46/1.46 |
| 1000 | 197 | 416 | 153 | 309 | 2.69/1.68 |
| 1500 | 207 | 1116 | 160 | 339 | 2.82/1.79 |
| 2000 | 219 | 799 | 164 | 376 | 2.92/1.92 |
| 2500 | 222 | 2052 | 169 | 369 | 2.97/1.97 |
| 3000 | 219 | 492 | 174 | 1551 | 3.12/2.11 |
| 4000 | 221 | 480 | 187 | 446 | 3.23/2.25 |
| 5000 | 244 | 2377 | 198 | 731 | 3.40/2.40 |
| 7500 | 254 | 1099 | 191 | 1979 | 3.46/2.48 |
| 10000 | 296 | 3452 | 199 | 2150 | 3.65/2.60 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep.


#### bulk_insert (descending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 2264 | 2593 | 3477 | 4365 | 3696 | 3913 | 9.00/9.00 |
| 500 | 28154 | 31101 | 40521 | 48929 | 28279 | 32739 | 174.00/174.00 |
| 1000 | 87918 | 103368 | 136347 | 261040 | 71835 | 78766 | 566.00/570.00 |
| 1500 | 175672 | 179109 | 283930 | 294851 | 118324 | 121528 | 988.00/980.00 |
| 2000 | 270375 | 273261 | 452165 | 459584 | 160218 | 165500 | 1469.00/1486.00 |
| 2500 | 397345 | 402561 | 720270 | 1016680 | 222805 | 256559 | 2083.00/2048.00 |
| 3000 | 564811 | 570123 | 952296 | 973278 | 267059 | 271942 | 2769.00/2736.00 |
| 4000 | 947134 | 959470 | 1727839 | 1752063 | 408287 | 414436 | 4277.00/4242.00 |
| 5000 | 1701935 | 1770848 | 2794752 | 3069228 | 506533 | 512729 | 6003.00/5913.00 |
| 7500 | 4570682 | 4584581 | 6258005 | 6438176 | 824681 | 838344 | 10595.00/10510.00 |
| 10000 | 8548130 | 8556944 | 11402238 | 11967836 | 1235621 | 1245681 | 16128.00/15835.00 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 500: too close to call there.

#### insert_random (descending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 75 | 174 | 88 | 196 | 91 | 184 | 0.20/0.19 |
| 500 | 122 | 296 | 145 | 337 | 121 | 254 | 0.50/0.49 |
| 1000 | 156 | 487 | 219 | 1024 | 147 | 315 | 0.83/0.84 |
| 1500 | 192 | 439 | 278 | 572 | 168 | 353 | 0.93/0.98 |
| 2000 | 206 | 417 | 306 | 623 | 155 | 347 | 0.99/0.99 |
| 2500 | 251 | 3784 | 396 | 2549 | 174 | 418 | 1.27/1.21 |
| 3000 | 266 | 522 | 426 | 817 | 183 | 1395 | 1.24/1.19 |
| 4000 | 333 | 1043 | 572 | 2918 | 188 | 690 | 1.42/1.41 |
| 5000 | 371 | 1598 | 649 | 1382 | 198 | 1736 | 1.63/1.59 |
| 7500 | 544 | 1992 | 963 | 5544 | 214 | 538 | 1.95/1.91 |
| 10000 | 723 | 2484 | 1200 | 5091 | 231 | 1517 | 2.07/2.04 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500: too close to call there.

#### insert_front (descending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 41 | 52 | 52 | 55 | 49 | 100 | 0.00/0.00 |
| 500 | 81 | 85 | 136 | 1457 | 59 | 76 | 0.00/0.00 |
| 1000 | 120 | 153 | 237 | 950 | 69 | 82 | 0.00/0.00 |
| 1500 | 168 | 1967 | 338 | 342 | 75 | 80 | 0.00/0.00 |
| 2000 | 199 | 211 | 411 | 5055 | 71 | 88 | 0.00/0.00 |
| 2500 | 251 | 259 | 532 | 2139 | 79 | 99 | 0.00/0.00 |
| 3000 | 276 | 292 | 595 | 5197 | 79 | 258 | 0.00/0.00 |
| 4000 | 379 | 2591 | 820 | 3351 | 79 | 259 | 0.00/0.00 |
| 5000 | 540 | 730 | 1053 | 3911 | 80 | 110 | 0.00/0.00 |
| 7500 | 993 | 2955 | 1654 | 5901 | 77 | 258 | 0.00/0.00 |
| 10000 | 1395 | 3225 | 2214 | 6357 | 90 | 269 | 0.00/0.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500: too close to call there.

#### remove_random (descending, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 106 | 230 | 118 | 246 | 139 | 242 | 1.08/1.07 |
| 500 | 161 | 352 | 184 | 402 | 175 | 389 | 1.28/1.28 |
| 1000 | 191 | 498 | 250 | 1226 | 195 | 402 | 1.47/1.53 |
| 1500 | 229 | 4876 | 296 | 610 | 203 | 1591 | 1.54/1.54 |
| 2000 | 246 | 1316 | 335 | 647 | 202 | 406 | 1.61/1.64 |
| 2500 | 286 | 1091 | 421 | 3446 | 214 | 789 | 1.73/1.68 |
| 3000 | 287 | 608 | 414 | 1858 | 224 | 949 | 1.77/1.80 |
| 4000 | 366 | 1188 | 555 | 2820 | 227 | 818 | 1.85/1.83 |
| 5000 | 410 | 988 | 645 | 1451 | 227 | 2178 | 2.03/1.96 |
| 7500 | 671 | 2022 | 965 | 2070 | 248 | 818 | 2.26/2.22 |
| 10000 | 896 | 3103 | 1255 | 4724 | 259 | 2408 | 2.40/2.39 |

Crossover: from N = 1500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000, 1500: too close to call there.

#### find_hit (descending, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 134 | 269 | 101 | 198 | 2.13/1.13 |
| 500 | 180 | 394 | 138 | 302 | 2.44/1.40 |
| 1000 | 203 | 2342 | 160 | 386 | 2.64/1.67 |
| 1500 | 214 | 453 | 167 | 361 | 2.72/1.73 |
| 2000 | 207 | 482 | 162 | 388 | 2.85/1.84 |
| 2500 | 211 | 520 | 175 | 379 | 2.90/1.90 |
| 3000 | 217 | 1569 | 181 | 454 | 2.96/1.97 |
| 4000 | 227 | 857 | 184 | 447 | 3.00/1.99 |
| 5000 | 241 | 537 | 185 | 472 | 3.18/2.16 |
| 7500 | 258 | 1007 | 197 | 6758 | 3.56/2.52 |
| 10000 | 264 | 2379 | 212 | 775 | 3.62/2.60 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100: too close to call there.


#### bulk_insert (prefix, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 2354 | 3363 | 3101 | 6690 | 3527 | 6156 | 15.00/14.00 |
| 500 | 41513 | 48524 | 46945 | 55686 | 35535 | 49949 | 326.00/312.00 |
| 1000 | 120970 | 129261 | 145649 | 165104 | 116147 | 132178 | 935.00/927.00 |
| 1500 | 218469 | 229905 | 277013 | 288105 | 213100 | 225457 | 1752.00/1743.00 |
| 2000 | 334965 | 348671 | 433666 | 438099 | 304055 | 313266 | 2673.00/2669.00 |
| 2500 | 462535 | 471836 | 616586 | 629810 | 402094 | 414526 | 3672.00/3652.00 |
| 3000 | 602094 | 612942 | 826941 | 851986 | 501849 | 541007 | 4687.00/4667.00 |
| 4000 | 920617 | 936514 | 1314934 | 1337430 | 712301 | 722364 | 6700.00/6688.00 |
| 5000 | 1307433 | 1314276 | 1900810 | 1920089 | 884756 | 917921 | 8955.00/8912.00 |
| 7500 | 2754426 | 2790135 | 3945545 | 3959209 | 1466044 | 1557344 | 15061.00/15022.00 |
| 10000 | 4912165 | 4921083 | 6867209 | 7018838 | 2155918 | 2163452 | 21542.00/21648.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 1000, 1500, 2000: too close to call there.

#### insert_random (prefix, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 82 | 246 | 98 | 231 | 101 | 365 | 0.43/0.44 |
| 500 | 131 | 294 | 159 | 757 | 136 | 298 | 1.04/1.03 |
| 1000 | 167 | 527 | 227 | 847 | 157 | 366 | 1.36/1.39 |
| 1500 | 199 | 447 | 288 | 2219 | 165 | 572 | 1.66/1.64 |
| 2000 | 229 | 2001 | 341 | 2989 | 171 | 895 | 1.75/1.74 |
| 2500 | 256 | 2250 | 402 | 1179 | 180 | 874 | 1.97/1.93 |
| 3000 | 281 | 2505 | 461 | 1443 | 183 | 820 | 1.88/1.87 |
| 4000 | 346 | 1398 | 567 | 2464 | 201 | 934 | 2.26/2.31 |
| 5000 | 394 | 2055 | 669 | 3810 | 221 | 1956 | 2.35/2.32 |
| 7500 | 544 | 2213 | 936 | 2956 | 213 | 505 | 2.57/2.61 |
| 10000 | 704 | 1655 | 1276 | 3676 | 232 | 591 | 2.77/2.77 |

Crossover: from N = 1000 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500, 1000: too close to call there.

#### insert_front (prefix, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 42 | 143 | 55 | 2087 | 51 | 149 | 0.00/0.00 |
| 500 | 84 | 90 | 142 | 177 | 62 | 2194 | 0.00/0.00 |
| 1000 | 125 | 352 | 237 | 964 | 75 | 176 | 0.00/0.00 |
| 1500 | 169 | 1105 | 337 | 1352 | 74 | 91 | 0.00/0.00 |
| 2000 | 207 | 556 | 430 | 455 | 79 | 248 | 0.00/0.00 |
| 2500 | 248 | 668 | 529 | 3298 | 73 | 80 | 0.00/0.00 |
| 3000 | 288 | 298 | 623 | 4059 | 79 | 235 | 0.00/0.00 |
| 4000 | 377 | 973 | 821 | 3344 | 84 | 117 | 0.00/0.00 |
| 5000 | 539 | 1170 | 1052 | 2342 | 79 | 100 | 0.00/0.00 |
| 7500 | 991 | 4495 | 1580 | 2962 | 85 | 1333 | 0.00/0.00 |
| 10000 | 1339 | 2528 | 2212 | 6568 | 89 | 111 | 0.00/0.00 |

Crossover: from N = 500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 100, 500: too close to call there.

#### remove_random (prefix, warm, dcache n/a)

| N | array med | array max | array_wc med | array_wc max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 113 | 253 | 121 | 270 | 148 | 254 | 1.17/1.15 |
| 500 | 170 | 535 | 192 | 600 | 181 | 363 | 1.60/1.59 |
| 1000 | 199 | 772 | 249 | 1292 | 199 | 1448 | 1.80/1.84 |
| 1500 | 235 | 1737 | 304 | 2766 | 205 | 2412 | 2.09/2.10 |
| 2000 | 258 | 891 | 355 | 2315 | 212 | 904 | 2.20/2.17 |
| 2500 | 287 | 2049 | 415 | 830 | 220 | 463 | 2.39/2.38 |
| 3000 | 312 | 3959 | 467 | 2956 | 227 | 906 | 2.46/2.44 |
| 4000 | 358 | 766 | 553 | 2927 | 231 | 886 | 2.47/2.53 |
| 5000 | 427 | 2461 | 657 | 2728 | 239 | 2250 | 2.70/2.71 |
| 7500 | 666 | 1904 | 898 | 2431 | 242 | 511 | 2.90/2.92 |
| 10000 | 870 | 4603 | 1266 | 3902 | 263 | 3681 | 2.99/3.06 |

Crossover: from N = 1500 the AVL median is below the array median for every larger N in the sweep. Medians within 10% (or within the 34 timer overhead) at N = 500, 1000, 1500: too close to call there.

#### find_hit (prefix, warm, dcache n/a)

| N | array med | array max | avl med | avl max | tb avg (array/avl) |
|---:|---:|---:|---:|---:|---:|
| 100 | 144 | 318 | 102 | 218 | 2.22/1.19 |
| 500 | 187 | 2242 | 145 | 640 | 2.81/1.83 |
| 1000 | 207 | 2774 | 153 | 362 | 3.03/2.07 |
| 1500 | 218 | 1552 | 165 | 398 | 3.42/2.37 |
| 2000 | 212 | 915 | 170 | 736 | 3.44/2.44 |
| 2500 | 222 | 975 | 178 | 698 | 3.68/2.67 |
| 3000 | 228 | 544 | 184 | 5149 | 3.86/2.83 |
| 4000 | 235 | 554 | 189 | 826 | 3.88/2.88 |
| 5000 | 247 | 1042 | 193 | 873 | 4.07/3.06 |
| 7500 | 254 | 3592 | 196 | 528 | 4.16/3.17 |
| 10000 | 249 | 626 | 209 | 526 | 4.57/3.59 |

Crossover: from N = 100 the AVL median is below the array median for every larger N in the sweep.

Plots (log-log, median solid, max dashed): `plots/2026-10-14_host_O3_random_{warm,cold}_<operation>.svg`, generated by `scripts/plot.py`.

### Host observations

These are measurements only, on the host, and may not carry over to the target.

**Operations that move entries** (`insert_random`, `remove_random`, `bulk_insert`, warm, `random`):
- The AVL median is below the array median from N = 1,000 onwards.
- At N = 1,000 to 1,500, the two medians are within 10% of each other, so too close to call.
- At N = 10,000, the medians are 713 ns (array) vs 235 ns (AVL) for `insert_random`, and 905 ns vs 266 ns for `remove_random`.
- The array costs grow roughly linearly with N; the AVL costs grow roughly logarithmically.

**Worst-case positions:**
- `insert_front` and `remove_first`: the AVL median is lower from N = 500 and N = 100 respectively.
- `insert_end` and `remove_last`: the array is never slower in the sweep. The array then moves zero or almost zero entries.

**Bulk builds:**
- `bulk_rebuild` (append then sort) is within 10% between the two implementations from N = 500 upwards. At N = 100 the array is 16% lower.
- `bulk_insert` with `ascending` order is lower for the array, since every insert lands at the end. With `descending` order the array is about 7× higher than the AVL at N = 10,000 (8.5 ms vs 1.2 ms).

**Lookups** (`find_hit`, `find_miss`, `page_name20`):
- `find_hit`: the AVL median is 37 to 54 ns lower than the array median at every N.
- `find_miss`: the medians are within 10% of each other at every N.
- `page_name20`: the array median is lower at every N from 500 upwards (248 vs 364 ns at N = 10,000).
- The array calls the tie-break about one more time per `find_hit` than the AVL. That is the final equality check after `lower_bound`.

**Rank-based access and traversal:**
- `page_rank20` is constant at about 45 ns for the array. For the AVL it grows with N, reaching 8.4 µs at N = 10,000 warm, because the AVL walks from the start.
- `traverse_all` at N = 10,000 is 15 µs for the array vs 67 µs for the AVL (warm), and 14 µs vs 82 µs cold.
- In none of these does the AVL median fall below the array median in the sweep.

**Cold runs:**
- The `insert_random` and `remove_random` crossovers move up to N = 4,000.
- `find_hit` becomes lower for the array (1.9 µs vs 2.8 µs at N = 10,000). A binary search over the contiguous array touches fewer distinct lines than the tree walk.

**Tie-break calls:**
- They rise with N for both structures, reaching about 2.2 to 3.7 per single operation at N = 10,000.
- `bulk_insert` makes about 15,000 calls in total at N = 10,000.
- On the real system each call compares full names. If those names have to come from the SD card instead of RAM, this count matters more than the index cost itself.

**Noise:**
- Host max and p99 values are dominated by OS preemption: isolated maxima of 2 to 18 µs appear in operations whose p99 is well under 1 µs.
- They are not a measure of either structure's worst case. The target's PRIMASK-protected max values are.

## Caveats and deviations from the brief

1. **No target data yet.** This session had no hardware access. The target harness, the SD baseline and the run script are built and link at both `-Os` and `-O0`, but they have not been executed. Every target-specific statement above is either configuration or a prediction to check.
2. **Location.** The code lives in `source/benchmark/name_index/`, not `bench/`, as decided with the user. The benchmark has its own CMake targets and is never part of `app_logic`.
3. **N is capped at 10,000**, the design maximum, not 50,000. Nothing in the sweep was skipped for lack of space.
4. **DTCM placement was not measured.** Per the decision, everything sits in AXI SRAM (RAM_D1). DTCM is uncached, so the D-cache settings would not have applied there anyway.
5. **The D-cache is 32 KB, not 16 KB.** That is the STM32H723 datasheet value; the run reads the actual geometry from CCSIDR. The sweep is therefore dense around 2,700 entries, not 1,300.
6. **The benchmark runs as a FreeRTOS task**, not on bare metal before the scheduler starts, because the SD driver needs `osDelay`. Timed index regions have IRQs masked. Bulk builds at large N keep IRQs masked for up to roughly a second. That is fine for a benchmark, but the RTOS tick count drifts during the run.
7. **The AVL shape drifts.** Restoring the AVL after each insert or remove repetition keeps the same contents but not necessarily the same shape. Restoring an exact copy would need a second 180 KB arena, which does not fit. The array is restored exactly.
8. **The prefix distribution is not harder than random.** Realistic names already share keys (57% at N = 10,000), so `prefix` mostly re-tests the same tie-break path. A more extreme tie-break stress test (for example, 100% shared keys) was not run.
9. **The AVL page fetch by rank is O(k)** because nodes are not augmented with subtree sizes, as the brief's node layout specifies. `page_name20` measures the seek-by-name alternative.
10. **AVL rebuild uses a different algorithm than the brief's "n inserts".** It sorts the appended nodes in place and links a balanced tree. The n-insert build is reported separately as `bulk_insert` for both structures.
11. **`find` uses a probe sector.** It passes the reserved sector value `NI_PROBE_SECTOR` (0xFFFE) to the tie-break callback, which resolves it to the name being searched, so that the indexes never see full names. A Storage-backed callback in the real system has to support the same convention.
12. **Key folding is ASCII only.** UTF-8 names are compared bytewise.
13. **Host timing.** Timings are wall-clock ns with no IRQ control. Cold mode on the host uses only 50 repetitions per single operation and a 32 MiB eviction buffer. `traverse_per_entry` is integer ns, which is too coarse on the host to compare; use `traverse_all`.
14. **The word-copy variant was not timed for every operation.** It was timed only for `bulk_insert`, `insert_*` and `remove_*`.

## Not measured

- Every target number: index cycles in all cache modes at `-Os` and `-O0`, the SD baseline, and the D-cache geometry and MPU register dumps.
- DTCM placement (decided against).
- N above 10,000 (decided against).
- An exact-state restore for AVL repetitions (no memory for a second arena).
