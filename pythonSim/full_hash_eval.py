#!/usr/bin/env python3

import random
import statistics
import time
import zlib

import matplotlib.pyplot as plt
from collections import Counter


# ============================================================
# Configuration
# ============================================================

HASH_TABLE_SIZE = 14293
NUM_SAMPLES = 100_000

# Number of independent simulations at 70% load
NUM_SIMULATIONS = 100

LOAD_FACTOR = 0.70

RANDOM_SEED = 42


# ============================================================
# Hash functions
# ============================================================

def fnv1a(phone: str) -> int:
    """
    FNV-1a 32-bit.

    Equivalent to the style of hash currently being used
    in the C implementation.
    """

    hash_value = 2166136261

    for char in phone:

        if '0' <= char <= '9':

            hash_value ^= ord(char)

            hash_value = (
                hash_value * 16777619
            ) & 0xFFFFFFFF

    return hash_value


def fnv1a_folded(phone: str) -> int:
    """
    FNV-1a 32-bit followed by folding the upper and lower
    16 bits together.

    This is equivalent to the user's current hash_phone().
    """

    hash_value = fnv1a(phone)

    lower = hash_value & 0xFFFF
    upper = (hash_value >> 16) & 0xFFFF

    return (lower ^ upper) & 0xFFFF


def djb2(phone: str) -> int:
    """
    DJB2 hash.
    """

    hash_value = 5381

    for char in phone:

        if '0' <= char <= '9':

            hash_value = (
                ((hash_value << 5) + hash_value)
                + ord(char)
            ) & 0xFFFFFFFF

    return hash_value


def sdbm(phone: str) -> int:
    """
    SDBM hash.
    """

    hash_value = 0

    for char in phone:

        if '0' <= char <= '9':

            hash_value = (
                ord(char)
                + (hash_value << 6)
                + (hash_value << 16)
                - hash_value
            ) & 0xFFFFFFFF

    return hash_value


def murmur_finalizer(phone: str) -> int:
    """
    MurmurHash-style finalizer.

    The phone number is first accumulated into a simple
    32-bit value and then passed through Murmur3's strong
    avalanche finalizer.
    """

    hash_value = 0

    for char in phone:

        if '0' <= char <= '9':

            hash_value = (
                hash_value * 31
                + ord(char)
            ) & 0xFFFFFFFF

    # MurmurHash3 fmix32

    hash_value ^= hash_value >> 16

    hash_value = (
        hash_value * 0x85EBCA6B
    ) & 0xFFFFFFFF

    hash_value ^= hash_value >> 13

    hash_value = (
        hash_value * 0xC2B2AE35
    ) & 0xFFFFFFFF

    hash_value ^= hash_value >> 16

    return hash_value


def crc32_hash(phone: str) -> int:
    """
    CRC32-based hash.

    This is particularly interesting for STM32 because
    many STM32 devices provide a hardware CRC peripheral.

    The Python implementation uses zlib.crc32() as the
    reference implementation.
    """

    digits = "".join(
        char
        for char in phone
        if '0' <= char <= '9'
    )

    return zlib.crc32(
        digits.encode("ascii")
    ) & 0xFFFFFFFF


# ============================================================
# Hash function registry
# ============================================================

HASH_FUNCTIONS = {
    "FNV-1a": fnv1a,
    "FNV-1a 16-bit": fnv1a_folded,
    "DJB2": djb2,
    "SDBM": sdbm,
    "Murmur finalizer": murmur_finalizer,
    "CRC32": crc32_hash,
}


# ============================================================
# Australian phone number generation
# ============================================================

def generate_mobile_number():

    return (
        "04"
        + f"{random.randint(0, 99):02d}"
        + f"{random.randint(0, 999):03d}"
        + f"{random.randint(0, 999):03d}"
    )


def generate_landline_number():

    area_code = random.choice(
        ["02", "03", "07", "08"]
    )

    return (
        area_code
        + f"{random.randint(0, 9999):04d}"
        + f"{random.randint(0, 9999):04d}"
    )


def generate_phone_number():

    # Approximately 70% mobile
    if random.random() < 0.70:
        return generate_mobile_number()

    return generate_landline_number()


def generate_phone_numbers(count):

    return [
        generate_phone_number()
        for _ in range(count)
    ]


# ============================================================
# Distribution statistics
# ============================================================

def calculate_distribution_statistics(
    hash_values,
    table_size
):

    buckets = [
        value % table_size
        for value in hash_values
    ]

    bucket_counts = [
        0
        for _ in range(table_size)
    ]

    for bucket in buckets:
        bucket_counts[bucket] += 1

    total = len(hash_values)

    occupied = sum(
        1
        for count in bucket_counts
        if count > 0
    )

    collisions = total - occupied

    collision_rate = collisions / total

    maximum_bucket_depth = max(
        bucket_counts
    )

    occupied_depths = [
        count
        for count in bucket_counts
        if count > 0
    ]

    average_occupied_depth = (
        statistics.mean(occupied_depths)
    )

    std_deviation = statistics.pstdev(
        bucket_counts
    )

    # --------------------------------------------------------
    # Chi-squared statistic
    #
    # For a perfectly uniform distribution the expected
    # bucket occupancy is:
    #
    #       N / M
    #
    # --------------------------------------------------------

    expected = total / table_size

    chi_squared = sum(
        (
            (count - expected) ** 2
        ) / expected
        for count in bucket_counts
    )

    return {
        "buckets": buckets,
        "bucket_counts": bucket_counts,

        "occupied": occupied,

        "collisions": collisions,

        "collision_rate": collision_rate,

        "maximum_bucket_depth":
            maximum_bucket_depth,

        "average_occupied_depth":
            average_occupied_depth,

        "std_deviation":
            std_deviation,

        "chi_squared":
            chi_squared,
    }


# ============================================================
# Hash performance benchmark
# ============================================================

def benchmark_hash_function(
    hash_function,
    phone_numbers
):

    # --------------------------------------------------------
    # Time hash calculation
    # --------------------------------------------------------

    start = time.perf_counter()

    hash_values = [
        hash_function(phone)
        for phone in phone_numbers
    ]

    elapsed = (
        time.perf_counter() - start
    )

    # --------------------------------------------------------
    # Distribution
    # --------------------------------------------------------

    stats = calculate_distribution_statistics(
        hash_values,
        HASH_TABLE_SIZE
    )

    stats["hash_time_ms"] = (
        elapsed * 1000
    )

    stats["hash_time_us_per_key"] = (
        elapsed
        / len(phone_numbers)
        * 1_000_000
    )

    stats["hash_values"] = hash_values

    return stats


# ============================================================
# Collision simulation
# ============================================================

def collision_simulation(
    hash_function,
    num_simulations=NUM_SIMULATIONS
):

    num_entries = int(
        HASH_TABLE_SIZE * LOAD_FACTOR
    )

    results = []

    for simulation in range(
        num_simulations
    ):

        phones = generate_phone_numbers(
            num_entries
        )

        hash_values = [
            hash_function(phone)
            for phone in phones
        ]

        stats = calculate_distribution_statistics(
            hash_values,
            HASH_TABLE_SIZE
        )

        results.append(
            stats["collisions"]
        )

    return results


# ============================================================
# Print results
# ============================================================

def print_results(results):

    print()
    print("=" * 120)
    print("HASH FUNCTION COMPARISON")
    print("=" * 120)

    print(
        f"{'Hash Function':<20}"
        f"{'Collisions':>14}"
        f"{'Collision %':>14}"
        f"{'Max Bucket':>14}"
        f"{'Std Dev':>14}"
        f"{'Chi²':>16}"
        f"{'Time/key':>14}"
    )

    print("-" * 120)

    for name, stats in results.items():

        print(
            f"{name:<20}"
            f"{stats['collisions']:>14,}"
            f"{stats['collision_rate'] * 100:>13.3f}%"
            f"{stats['maximum_bucket_depth']:>14}"
            f"{stats['std_deviation']:>14.3f}"
            f"{stats['chi_squared']:>16.2f}"
            f"{stats['hash_time_us_per_key']:>13.3f} us"
        )


# ============================================================
# Plot results
# ============================================================

def plot_results(
    results,
    collision_results
):

    names = list(results.keys())

    # ========================================================
    # Extract data
    # ========================================================

    collision_rates = [
        results[name]["collision_rate"] * 100
        for name in names
    ]

    max_bucket_depths = [
        results[name]["maximum_bucket_depth"]
        for name in names
    ]

    std_deviations = [
        results[name]["std_deviation"]
        for name in names
    ]

    chi_squared = [
        results[name]["chi_squared"]
        for name in names
    ]

    hash_times = [
        results[name]["hash_time_us_per_key"]
        for name in names
    ]

    # ========================================================
    # Single figure
    # ========================================================

    fig, axes = plt.subplots(
        2,
        3,
        figsize=(18, 10)
    )

    # ========================================================
    # 1. Collision rate
    # ========================================================

    axes[0, 0].bar(
        names,
        collision_rates
    )

    axes[0, 0].set_title(
        "Collision Rate"
    )

    axes[0, 0].set_ylabel(
        "Collision rate (%)"
    )

    axes[0, 0].tick_params(
        axis="x",
        rotation=35
    )

    axes[0, 0].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # 2. Maximum bucket depth
    # ========================================================

    axes[0, 1].bar(
        names,
        max_bucket_depths
    )

    axes[0, 1].set_title(
        "Maximum Bucket Depth"
    )

    axes[0, 1].set_ylabel(
        "Entries in largest bucket"
    )

    axes[0, 1].tick_params(
        axis="x",
        rotation=35
    )

    axes[0, 1].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # 3. Bucket distribution standard deviation
    # ========================================================

    axes[0, 2].bar(
        names,
        std_deviations
    )

    axes[0, 2].set_title(
        "Bucket Occupancy Standard Deviation"
    )

    axes[0, 2].set_ylabel(
        "Standard deviation"
    )

    axes[0, 2].tick_params(
        axis="x",
        rotation=35
    )

    axes[0, 2].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # 4. Chi-squared
    # ========================================================

    axes[1, 0].bar(
        names,
        chi_squared
    )

    axes[1, 0].set_title(
        "Hash Distribution χ² Statistic"
    )

    axes[1, 0].set_ylabel(
        "χ² statistic"
    )

    axes[1, 0].tick_params(
        axis="x",
        rotation=35
    )

    axes[1, 0].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # 5. Hash execution time
    # ========================================================

    axes[1, 1].bar(
        names,
        hash_times
    )

    axes[1, 1].set_title(
        "Hash Execution Time"
    )

    axes[1, 1].set_ylabel(
        "Time per key (µs)"
    )

    axes[1, 1].tick_params(
        axis="x",
        rotation=35
    )

    axes[1, 1].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # 6. Collision distribution across simulations
    # ========================================================

    collision_data = [
        collision_results[name]
        for name in names
    ]

    axes[1, 2].boxplot(
        collision_data,
        tick_labels=names,
        showmeans=True
    )
    axes[1, 2].set_title(
        f"Collision Distribution at "
        f"{LOAD_FACTOR:.0%} Load"
    )

    axes[1, 2].set_ylabel(
        "Number of collisions"
    )

    axes[1, 2].tick_params(
        axis="x",
        rotation=35
    )

    axes[1, 2].grid(
        axis="y",
        alpha=0.3
    )

    # ========================================================
    # Overall formatting
    # ========================================================

    fig.suptitle(
        "Australian Phone Number Hash Function Comparison",
        fontsize=16
    )

    fig.tight_layout()

    fig.subplots_adjust(
        top=0.92
    )

    fig.savefig(
        "hash_function_comparison.png",
        dpi=300,
        bbox_inches="tight"
    )

    plt.show()


# ============================================================
# Detailed distribution plot
# ============================================================

def plot_bucket_distributions(
    phone_numbers,
    results
):

    """
    Plot bucket distributions for each hash function.

    This produces a separate figure because putting all
    bucket distributions into the main performance figure
    would make it unreadable.
    """

    names = list(results.keys())

    fig, axes = plt.subplots(
        len(names),
        1,
        figsize=(14, 14),
        sharex=True
    )

    for ax, name in zip(
        axes,
        names
    ):

        bucket_counts = results[name][
            "bucket_counts"
        ]

        ax.plot(
            range(HASH_TABLE_SIZE),
            bucket_counts,
            linewidth=0.7
        )

        ax.set_ylabel(
            "Entries"
        )

        ax.set_title(
            name
        )

        ax.grid(
            alpha=0.2
        )

    axes[-1].set_xlabel(
        "Hash table bucket"
    )

    fig.suptitle(
        "Hash Table Bucket Distribution",
        fontsize=16
    )

    fig.tight_layout()

    fig.subplots_adjust(
        top=0.96
    )

    fig.savefig(
        "hash_bucket_distributions.png",
        dpi=300,
        bbox_inches="tight"
    )

    plt.show()


# ============================================================
# Main
# ============================================================

def main():

    random.seed(
        RANDOM_SEED
    )

    print("=" * 80)
    print(
        "AUSTRALIAN PHONE NUMBER HASH FUNCTION BENCHMARK"
    )
    print("=" * 80)

    print()
    print(
        f"Hash table size:       "
        f"{HASH_TABLE_SIZE:,}"
    )

    print(
        f"Samples:               "
        f"{NUM_SAMPLES:,}"
    )

    print(
        f"Load factor:           "
        f"{LOAD_FACTOR:.0%}"
    )

    print(
        f"Simulations:           "
        f"{NUM_SIMULATIONS}"
    )

    # --------------------------------------------------------
    # Generate common dataset
    # --------------------------------------------------------

    print()
    print("Generating Australian phone numbers...")

    phone_numbers = generate_phone_numbers(
        NUM_SAMPLES
    )

    print(
        f"Generated "
        f"{len(phone_numbers):,} numbers."
    )

    # --------------------------------------------------------
    # Benchmark each hash function
    # --------------------------------------------------------

    results = {}

    for name, function in HASH_FUNCTIONS.items():

        print(
            f"Testing {name}..."
        )

        results[name] = benchmark_hash_function(
            function,
            phone_numbers
        )

    # --------------------------------------------------------
    # Collision simulations
    # --------------------------------------------------------

    print()
    print(
        "Running collision simulations..."
    )

    collision_results = {}

    for name, function in HASH_FUNCTIONS.items():

        print(
            f"  {name}..."
        )

        collision_results[name] = (
            collision_simulation(
                function
            )
        )

    # --------------------------------------------------------
    # Print results
    # --------------------------------------------------------

    print_results(
        results
    )

    # --------------------------------------------------------
    # Plot performance comparison
    # --------------------------------------------------------

    print()
    print(
        "Generating performance plots..."
    )

    plot_results(
        results,
        collision_results
    )

    # --------------------------------------------------------
    # Plot bucket distributions
    # --------------------------------------------------------

    print(
        "Generating bucket distribution plots..."
    )

    plot_bucket_distributions(
        phone_numbers,
        results
    )

    # --------------------------------------------------------
    # Best functions
    # --------------------------------------------------------

    print()
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)

    best_collision = min(
        names := results.keys(),
        key=lambda name:
            results[name]["collision_rate"]
    )

    best_std = min(
        results.keys(),
        key=lambda name:
            results[name]["std_deviation"]
    )

    best_max_bucket = min(
        results.keys(),
        key=lambda name:
            results[name]["maximum_bucket_depth"]
    )

    fastest = min(
        results.keys(),
        key=lambda name:
            results[name]["hash_time_us_per_key"]
    )

    print(
        f"Lowest collision rate:     "
        f"{best_collision}"
    )

    print(
        f"Lowest bucket deviation:   "
        f"{best_std}"
    )

    print(
        f"Smallest max bucket:       "
        f"{best_max_bucket}"
    )

    print(
        f"Fastest hash:               "
        f"{fastest}"
    )

    print()
    print(
        "Benchmark complete."
    )


if __name__ == "__main__":
    main()
