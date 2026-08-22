#!/usr/bin/env python3
import matplotlib.pyplot as plt

"""
Cuckoo Hashing Performance Benchmark
=====================================

Benchmarks 2-choice cuckoo hashing using Australian phone numbers.

Maximum entries:
    10,000

The benchmark tests multiple table sizes to determine the smallest
table capable of reliably storing all entries while maintaining good
operational performance.

Author: Benchmark for RTDBMS/hash-table evaluation
"""

import random
import time
import sys
import statistics
from dataclasses import dataclass


# ============================================================
# Configuration
# ============================================================

NUM_KEYS = 10_000

# Your original table size
ORIGINAL_TABLE_SIZE = 14_293

# Candidate table sizes.
#
# These are deliberately around the region expected to be useful
# for 2-way cuckoo hashing.
#
# We use primes because they can give reasonable distribution
# when modulo arithmetic is used.
CANDIDATE_TABLE_SIZES = [
    14_293,
    16_007,
    17_003,
    18_001,
    19_003,
    20_011,
    21_011,
    22_003,
    23_009,
    24_007,
    25_007,
    26_003,
    28_001,
    30_001,
]

# Maximum number of cuckoo relocations allowed for one insertion.
#
# If this limit is reached, the insertion is considered failed.
MAX_KICKS_MULTIPLIER = 2

# Random seed so benchmark is reproducible
RANDOM_SEED = 42

# Number of lookup operations
NUM_LOOKUPS = 100_000

# Percentage of lookups that should be successful
SUCCESSFUL_LOOKUP_RATIO = 0.90


# ============================================================
# Hash functions
# ============================================================

def splitmix64(x: int) -> int:
    """
    SplitMix64 integer hash.

    Provides good bit mixing and is deterministic.
    """

    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF

    x = ((x ^ (x >> 30)) *
         0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF

    x = ((x ^ (x >> 27)) *
         0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF

    return x ^ (x >> 31)


def hash1(key: int, table_size: int) -> int:
    """
    First cuckoo hash function.
    """
    return splitmix64(key) % table_size


def hash2(key: int, table_size: int) -> int:
    """
    Second cuckoo hash function.

    Different seed ensures that hash1 and hash2 produce
    independent positions for the same key.
    """
    return splitmix64(
        key ^ 0xD6E8FEB86659FD93
    ) % table_size


# ============================================================
# Australian phone number generation
# ============================================================

def generate_australian_phone_numbers(count: int,
                                      seed: int = RANDOM_SEED):
    """
    Generate unique Australian phone numbers.

    Includes:
        04xxxxxxxx  - mobile
        02xxxxxxxx  - NSW/ACT landline
        03xxxxxxxx  - VIC/TAS landline
        07xxxxxxxx  - QLD landline
        08xxxxxxxx  - SA/WA/NT landline

    Numbers are represented internally as integers rather
    than strings to better represent a C implementation where
    the phone number would likely be stored as an integer.
    """

    rng = random.Random(seed)

    numbers = set()

    prefixes = [
        "04",   # mobile
        "02",   # NSW / ACT
        "03",   # VIC / TAS
        "07",   # QLD
        "08",   # SA / WA / NT
    ]

    while len(numbers) < count:

        prefix = rng.choice(prefixes)

        suffix = rng.randint(0, 99_999_999)

        number = int(prefix + f"{suffix:08d}")

        numbers.add(number)

    return list(numbers)


# ============================================================
# Cuckoo hash table
# ============================================================

class CuckooHashTable:
    """
    2-choice cuckoo hash table.

    Each key can occupy one of two locations:

        hash1(key)
        hash2(key)

    Insertion may evict an existing key and move it to its
    alternate position.
    """

    EMPTY = None

    def __init__(self, capacity: int):
        self.capacity = capacity

        self.table = [self.EMPTY] * capacity

        # Statistics
        self.total_relocations = 0
        self.max_relocations = 0

        self.failed_insertions = 0

    # --------------------------------------------------------
    # Hash positions
    # --------------------------------------------------------

    def positions(self, key):
        return (
            hash1(key, self.capacity),
            hash2(key, self.capacity)
        )

    # --------------------------------------------------------
    # Insert
    # --------------------------------------------------------

    def insert(self, key: int) -> bool:

        pos1, pos2 = self.positions(key)

        # First position empty
        if self.table[pos1] is None:
            self.table[pos1] = key
            return True

        # Second position empty
        if self.table[pos2] is None:
            self.table[pos2] = key
            return True

        # Both positions occupied.
        #
        # Start by replacing position 1.
        current = key
        position = pos1

        relocations = 0

        max_kicks = max(
            1,
            self.capacity * MAX_KICKS_MULTIPLIER
        )

        for _ in range(max_kicks):

            # Evict current key
            current, self.table[position] = (
                self.table[position],
                current
            )

            relocations += 1

            # Find the two possible locations for
            # the evicted key.
            p1, p2 = self.positions(current)

            # Move to the alternate location
            if position == p1:
                position = p2
            else:
                position = p1

            # If alternate position is free,
            # insertion succeeded.
            if self.table[position] is None:

                self.table[position] = current

                self.total_relocations += relocations

                if relocations > self.max_relocations:
                    self.max_relocations = relocations

                return True

        # Cycle / excessive relocation detected
        self.failed_insertions += 1

        self.total_relocations += relocations

        if relocations > self.max_relocations:
            self.max_relocations = relocations

        return False

    # --------------------------------------------------------
    # Lookup
    # --------------------------------------------------------

    def lookup(self, key: int):

        p1, p2 = self.positions(key)

        # Probe first position
        if self.table[p1] == key:
            return True, 1

        # Probe second position
        if self.table[p2] == key:
            return True, 2

        return False, 2

    # --------------------------------------------------------
    # Current load
    # --------------------------------------------------------

    def size(self):
        return sum(
            1 for item in self.table
            if item is not None
        )

    def load_factor(self):
        return self.size() / self.capacity


# ============================================================
# Benchmark result
# ============================================================

@dataclass
class BenchmarkResult:

    table_size: int
    entries: int
    load_factor: float

    insertion_time_ms: float

    lookup_time_ms: float

    average_lookup_probes: float
    maximum_lookup_probes: int

    average_relocations: float
    maximum_relocations: int

    failed_insertions: int

    memory_bytes: int

    successful_insertions: bool


# ============================================================
# Memory calculation
# ============================================================

def estimate_memory_bytes(table_size: int,
                           bytes_per_entry: int = 8):
    """
    Estimate memory used by the hash table.

    Default:

        8 bytes per slot

    This corresponds to storing a uint64_t / 64-bit phone
    number or equivalent key representation.

    Change bytes_per_entry to match your actual C structure.

    Examples:

        uint32_t key              -> 4
        uint64_t key              -> 8
        key + metadata (16 bytes) -> 16
    """

    return table_size * bytes_per_entry


# ============================================================
# Benchmark one table size
# ============================================================

def benchmark_table(table_size: int,
                    keys,
                    lookup_keys):

    table = CuckooHashTable(table_size)

    # --------------------------------------------------------
    # INSERTION BENCHMARK
    # --------------------------------------------------------

    start = time.perf_counter()

    for key in keys:

        success = table.insert(key)

        if not success:
            # Stop once the table can no longer insert.
            break

    insertion_time = time.perf_counter() - start

    successful = (
        table.size() == len(keys)
        and table.failed_insertions == 0
    )

    # --------------------------------------------------------
    # LOOKUP BENCHMARK
    # --------------------------------------------------------

    lookup_probes = []
    successful_lookups = 0

    start = time.perf_counter()

    for key in lookup_keys:

        found, probes = table.lookup(key)

        lookup_probes.append(probes)

        if found:
            successful_lookups += 1

    lookup_time = time.perf_counter() - start

    # --------------------------------------------------------
    # Statistics
    # --------------------------------------------------------

    average_relocations = 0.0

    if table.size() > 0:

        average_relocations = (
            table.total_relocations /
            table.size()
        )

    average_lookup_probes = statistics.mean(
        lookup_probes
    )

    memory_bytes = estimate_memory_bytes(
        table_size
    )

    return BenchmarkResult(
        table_size=table_size,
        entries=table.size(),
        load_factor=table.load_factor(),

        insertion_time_ms=insertion_time * 1000,

        lookup_time_ms=lookup_time * 1000,

        average_lookup_probes=average_lookup_probes,

        maximum_lookup_probes=max(lookup_probes),

        average_relocations=average_relocations,

        maximum_relocations=table.max_relocations,

        failed_insertions=table.failed_insertions,

        memory_bytes=memory_bytes,

        successful_insertions=successful
    )


# ============================================================
# Lookup workload
# ============================================================

def create_lookup_workload(keys,
                           count=NUM_LOOKUPS,
                           seed=1234):

    rng = random.Random(seed)

    successful_count = int(
        count * SUCCESSFUL_LOOKUP_RATIO
    )

    unsuccessful_count = count - successful_count

    # Successful lookups
    workload = [
        rng.choice(keys)
        for _ in range(successful_count)
    ]

    # Generate numbers that aren't in the table
    key_set = set(keys)

    while len(workload) < count:

        candidate = rng.randint(
            20_000_000,
            99_999_999
        )

        if candidate not in key_set:
            workload.append(candidate)

    rng.shuffle(workload)

    return workload


# ============================================================
# Formatting
# ============================================================

def format_bytes(num_bytes):

    if num_bytes < 1024:
        return f"{num_bytes} B"

    if num_bytes < 1024 ** 2:
        return f"{num_bytes / 1024:.2f} KiB"

    return f"{num_bytes / (1024 ** 2):.2f} MiB"


def print_results(results):

    print()
    print("=" * 120)
    print("CUCKOO HASHING BENCHMARK")
    print("=" * 120)

    print(
        f"{'Size':>8} "
        f"{'Load':>8} "
        f"{'Entries':>9} "
        f"{'Insert ms':>12} "
        f"{'Lookup ms':>12} "
        f"{'Avg Probes':>12} "
        f"{'Max Probe':>11} "
        f"{'Avg Reloc':>12} "
        f"{'Max Reloc':>11} "
        f"{'Failures':>10} "
        f"{'Memory':>12}"
    )

    print("-" * 120)

    for r in results:

        print(
            f"{r.table_size:>8} "
            f"{r.load_factor * 100:>7.2f}% "
            f"{r.entries:>9} "
            f"{r.insertion_time_ms:>12.3f} "
            f"{r.lookup_time_ms:>12.3f} "
            f"{r.average_lookup_probes:>12.3f} "
            f"{r.maximum_lookup_probes:>11} "
            f"{r.average_relocations:>12.3f} "
            f"{r.maximum_relocations:>11} "
            f"{r.failed_insertions:>10} "
            f"{format_bytes(r.memory_bytes):>12}"
        )

    print("-" * 120)


# ============================================================
# Find optimal table size
# ============================================================

def find_optimal(results):

    successful = [
        r for r in results
        if r.successful_insertions
    ]

    if not successful:
        return None

    # Primary objective:
    #     minimum memory
    #
    # Secondary objectives:
    #     low lookup probes
    #     low relocations
    #
    # Therefore choose the smallest table size which
    # successfully stores every key.

    successful.sort(
        key=lambda r: (
            r.memory_bytes,
            r.average_lookup_probes,
            r.average_relocations
        )
    )

    return successful[0]

def plot_results(results):
    """
    Generate a single figure containing all cuckoo hashing
    performance metrics.
    """

    successful = sorted(
        [
            r for r in results
            if r.successful_insertions
        ],
        key=lambda r: r.load_factor
    )

    if not successful:
        print("No successful results available for plotting.")
        return

    # --------------------------------------------------------
    # Extract data
    # --------------------------------------------------------

    load_factors = [
        r.load_factor * 100
        for r in successful
    ]

    table_sizes = [
        r.table_size
        for r in successful
    ]

    avg_relocations = [
        r.average_relocations
        for r in successful
    ]

    max_relocations = [
        r.maximum_relocations
        for r in successful
    ]

    avg_probes = [
        r.average_lookup_probes
        for r in successful
    ]

    insertion_times = [
        r.insertion_time_ms
        for r in successful
    ]

    lookup_times = [
        r.lookup_time_ms
        for r in successful
    ]

    memory_kib = [
        r.memory_bytes / 1024
        for r in successful
    ]

    # --------------------------------------------------------
    # Create single figure
    # --------------------------------------------------------

    fig, axes = plt.subplots(
        2,
        3,
        figsize=(16, 9)
    )

    # ========================================================
    # 1. Load factor vs average relocations
    # ========================================================

    ax = axes[0, 0]

    ax.plot(
        load_factors,
        avg_relocations,
        marker="o"
    )

    ax.set_title(
        "Average Relocations vs Load Factor"
    )

    ax.set_xlabel(
        "Load factor (%)"
    )

    ax.set_ylabel(
        "Average relocations"
    )

    ax.grid(True, alpha=0.3)

    # ========================================================
    # 2. Load factor vs maximum relocations
    # ========================================================

    ax = axes[0, 1]

    ax.plot(
        load_factors,
        max_relocations,
        marker="o"
    )

    ax.set_title(
        "Maximum Relocations vs Load Factor"
    )

    ax.set_xlabel(
        "Load factor (%)"
    )

    ax.set_ylabel(
        "Maximum relocations"
    )

    ax.grid(True, alpha=0.3)

    # ========================================================
    # 3. Load factor vs average lookup probes
    # ========================================================

    ax = axes[0, 2]

    ax.plot(
        load_factors,
        avg_probes,
        marker="o"
    )

    ax.set_title(
        "Average Lookup Probes vs Load Factor"
    )

    ax.set_xlabel(
        "Load factor (%)"
    )

    ax.set_ylabel(
        "Average probes"
    )

    # Maximum possible probes for 2-choice
    # cuckoo hashing is two.
    ax.set_ylim(
        0,
        2.1
    )

    ax.grid(True, alpha=0.3)

    # ========================================================
    # 4. Table size vs memory
    # ========================================================

    ax = axes[1, 0]

    ax.plot(
        table_sizes,
        memory_kib,
        marker="o"
    )

    ax.set_title(
        "Memory Consumption vs Table Size"
    )

    ax.set_xlabel(
        "Table size (slots)"
    )

    ax.set_ylabel(
        "Memory (KiB)"
    )

    ax.grid(True, alpha=0.3)

    # ========================================================
    # 5. Table size vs insertion time
    # ========================================================

    ax = axes[1, 1]

    ax.plot(
        table_sizes,
        insertion_times,
        marker="o"
    )

    ax.set_title(
        "Insertion Time vs Table Size"
    )

    ax.set_xlabel(
        "Table size (slots)"
    )

    ax.set_ylabel(
        "Insertion time (ms)"
    )

    ax.grid(True, alpha=0.3)

    # ========================================================
    # 6. Table size vs lookup time
    # ========================================================

    ax = axes[1, 2]

    ax.plot(
        table_sizes,
        lookup_times,
        marker="o"
    )

    ax.set_title(
        "Lookup Time vs Table Size"
    )

    ax.set_xlabel(
        "Table size (slots)"
    )

    ax.set_ylabel(
        "Lookup time (ms)"
    )

    ax.grid(True, alpha=0.3)

    # --------------------------------------------------------
    # Overall figure formatting
    # --------------------------------------------------------

    fig.suptitle(
        "Cuckoo Hashing Performance Analysis",
        fontsize=16
    )

    fig.tight_layout(
        rect=[0, 0, 1, 0.96]
    )

    # Save high-resolution figure
    fig.savefig(
        "cuckoo_hashing_performance.png",
        dpi=300,
        bbox_inches="tight"
    )

    plt.show()

# ============================================================
# Main
# ============================================================

def main():

    print("=" * 80)
    print("AUSTRALIAN PHONE NUMBER CUCKOO HASH BENCHMARK")
    print("=" * 80)

    print()
    print(f"Number of entries:     {NUM_KEYS:,}")
    print(f"Original table size:   {ORIGINAL_TABLE_SIZE:,}")
    print(
        f"Original load factor:  "
        f"{NUM_KEYS / ORIGINAL_TABLE_SIZE * 100:.2f}%"
    )

    print()
    print("Generating Australian phone numbers...")

    keys = generate_australian_phone_numbers(
        NUM_KEYS
    )

    print(f"Generated {len(keys):,} unique numbers.")

    # Generate lookup workload once so that every
    # table size is tested using exactly the same workload.
    lookup_keys = create_lookup_workload(keys)

    print()
    print(
        f"Running {len(CANDIDATE_TABLE_SIZES)} "
        f"table-size benchmarks..."
    )

    results = []

    for table_size in CANDIDATE_TABLE_SIZES:

        print(
            f"  Testing table size {table_size:,}...",
            end=" ",
            flush=True
        )

        result = benchmark_table(
            table_size,
            keys,
            lookup_keys
        )

        results.append(result)

        if result.successful_insertions:
            print(
                f"SUCCESS "
                f"(load={result.load_factor * 100:.2f}%, "
                f"relocations={result.average_relocations:.2f})"
            )
        else:
            print(
                f"FAILED "
                f"(inserted={result.entries:,}, "
                f"failures={result.failed_insertions})"
            )

    # Print complete benchmark table
    print_results(results)

    # --------------------------------------------------------
    # Original table
    # --------------------------------------------------------

    original = next(
        (
            r for r in results
            if r.table_size == ORIGINAL_TABLE_SIZE
        ),
        None
    )

    print()
    print("=" * 80)
    print("ORIGINAL TABLE ANALYSIS")
    print("=" * 80)

    if original:

        print(
            f"Table size:          {original.table_size:,}"
        )

        print(
            f"Load factor:         "
            f"{original.load_factor * 100:.2f}%"
        )

        print(
            f"Entries inserted:    "
            f"{original.entries:,}"
        )

        print(
            f"Failed insertions:   "
            f"{original.failed_insertions}"
        )

        print(
            f"Memory:              "
            f"{format_bytes(original.memory_bytes)}"
        )

        if original.successful_insertions:
            print()
            print(
                "The 14,293-slot table successfully stored "
                "all entries."
            )
        else:
            print()
            print(
                "The 14,293-slot table could not reliably "
                "store all entries using 2-way cuckoo hashing."
            )

    # --------------------------------------------------------
    # Optimal size
    # --------------------------------------------------------

    optimal = find_optimal(results)

    print()
    print("=" * 80)
    print("RECOMMENDED TABLE SIZE")
    print("=" * 80)

    if optimal:

        print(
            f"Recommended size:       "
            f"{optimal.table_size:,} slots"
        )

        print(
            f"Load factor:            "
            f"{optimal.load_factor * 100:.2f}%"
        )

        print(
            f"Memory:                 "
            f"{format_bytes(optimal.memory_bytes)}"
        )

        print(
            f"Average relocations:    "
            f"{optimal.average_relocations:.3f}"
        )

        print(
            f"Maximum relocations:    "
            f"{optimal.maximum_relocations}"
        )

        print(
            f"Average lookup probes:  "
            f"{optimal.average_lookup_probes:.3f}"
        )

        print(
            f"Maximum lookup probes:  "
            f"{optimal.maximum_lookup_probes}"
        )

        print(
            f"Insertion time:         "
            f"{optimal.insertion_time_ms:.3f} ms"
        )

        print(
            f"Lookup time:            "
            f"{optimal.lookup_time_ms:.3f} ms"
        )

        print()
        print(
            "This is the smallest tested table that "
            "successfully stores all 10,000 keys."
        )

    else:

        print(
            "No tested table size successfully stored "
            "all keys."
        )

        print(
            "Increase the candidate table sizes."
        )

    # --------------------------------------------------------
    # Memory comparison
    # --------------------------------------------------------

    print()
    print("=" * 80)
    print("MEMORY COMPARISON")
    print("=" * 80)

    print(
        "Assuming 8 bytes per hash-table slot:"
    )

    for size in [
        ORIGINAL_TABLE_SIZE,
        16_007,
        18_001,
        20_011,
        22_003,
        24_007,
    ]:

        print(
            f"{size:>8,} slots -> "
            f"{format_bytes(estimate_memory_bytes(size))}"
        )

    print()
    print("Benchmark complete.")

    print()
    print("Generating performance plots...")

    plot_results(results)

    print("Plots generated.")


if __name__ == "__main__":
    main()
