#!/usr/bin/env python3

import random
import statistics
import matplotlib.pyplot as plt
from collections import Counter


# ============================================================
# Configuration
# ============================================================

HASH_TABLE_SIZE = 14293
NUM_SAMPLES = 100_000

# Seed makes experiments reproducible
RANDOM_SEED = 42


# ============================================================
# Hash function
# ============================================================

def hash_phone(phone: str) -> int:
    """
    Python equivalent of:

        uint16_t hash_phone(const char *phone)
        {
            uint32_t hash = 2166136261u;

            while (*phone)
            {
                if (*phone >= '0' && *phone <= '9')
                {
                    hash ^= (uint8_t)*phone;
                    hash *= 16777619u;
                }

                phone++;
            }

            return (uint16_t)hash ^ (hash >> 16);
        }

    Returns a 16-bit hash value.
    """

    # uint32_t
    hash_value = 2166136261

    for char in phone:

        if '0' <= char <= '9':

            # hash ^= (uint8_t)*phone
            hash_value ^= ord(char)

            # uint32_t multiplication
            # Python integers don't overflow, so explicitly
            # constrain the result to 32 bits.
            hash_value = (hash_value * 16777619) & 0xFFFFFFFF

    # Reproduce:
    #
    # return (uint16_t)hash ^ (hash >> 16);
    #
    # The result is eventually truncated to uint16_t.

    lower = hash_value & 0xFFFF
    upper = (hash_value >> 16) & 0xFFFF

    return (lower ^ upper) & 0xFFFF


# ============================================================
# Australian phone number generation
# ============================================================

def generate_mobile_number():
    """
    Generate an Australian mobile number.

    Australian mobile numbers have the form:

        04XX XXX XXX

    Example:

        0412 345 678
    """

    return (
        "04"
        + f"{random.randint(0, 99):02d}"
        + f"{random.randint(0, 999):03d}"
        + f"{random.randint(0, 999):03d}"
    )


def generate_landline_number():
    """
    Generate an Australian landline number.

    Simplified format:

        0X XXXX XXXX

    where X is the area code region.

    Examples:

        02 XXXX XXXX
        03 XXXX XXXX
        07 XXXX XXXX
        08 XXXX XXXX
    """

    area_code = random.choice(["02", "03", "07", "08"])

    return (
        area_code
        + f"{random.randint(0, 9999):04d}"
        + f"{random.randint(0, 9999):04d}"
    )


def generate_phone_number():
    """
    Generate either an Australian mobile or landline number.

    The ratio can be adjusted if desired.
    """

    # Approximately 70% mobile / 30% landline
    if random.random() < 0.7:
        return generate_mobile_number()

    return generate_landline_number()


# ============================================================
# Statistics
# ============================================================

def calculate_statistics(hash_values, bucket_values):

    hash_counts = Counter(hash_values)
    bucket_counts = Counter(bucket_values)

    total = len(hash_values)

    # Number of unique hash values
    unique_hashes = len(hash_counts)

    # Collisions at the 16-bit hash level
    hash_collisions = total - unique_hashes

    # Number of unique table buckets
    unique_buckets = len(bucket_counts)

    # Collisions after reducing hash to table size
    bucket_collisions = total - unique_buckets

    # Maximum number of entries in a bucket
    max_bucket_depth = max(bucket_counts.values())

    # Average number of entries per occupied bucket
    average_occupied_bucket_depth = (
        total / unique_buckets
    )

    # Number of empty buckets
    empty_buckets = HASH_TABLE_SIZE - unique_buckets

    # Load factor
    load_factor = total / HASH_TABLE_SIZE

    return {
        "total": total,
        "unique_hashes": unique_hashes,
        "hash_collisions": hash_collisions,
        "hash_collision_rate": hash_collisions / total,

        "unique_buckets": unique_buckets,
        "bucket_collisions": bucket_collisions,
        "bucket_collision_rate": bucket_collisions / total,

        "max_bucket_depth": max_bucket_depth,
        "average_occupied_bucket_depth":
            average_occupied_bucket_depth,

        "empty_buckets": empty_buckets,
        "load_factor": load_factor,
    }

def test_70_percent_load( num_simulations=100, hash_table_size=14293, load_factor=0.70):
    """
    Run multiple hash-table simulations at a specified load factor
    and plot the distribution of collision counts.

    Returns:
        results: List containing the collision count from each simulation.
    """

    num_entries = int(hash_table_size * load_factor)

    collision_results = []

    for _ in range(num_simulations):

        # Generate phone numbers
        phone_numbers = [
            generate_phone_number()
            for _ in range(num_entries)
        ]

        # Hash phone numbers
        hash_values = [
            hash_phone(phone)
            for phone in phone_numbers
        ]

        # Map hashes to hash-table buckets
        buckets = [
            hash_value % hash_table_size
            for hash_value in hash_values
        ]

        # Count entries in each bucket
        bucket_counts = [0] * hash_table_size

        for bucket in buckets:
            bucket_counts[bucket] += 1

        # Every entry after the first in a bucket is a collision
        collisions = sum(
            max(0, count - 1)
            for count in bucket_counts
        )

        collision_results.append(collisions)

    # --------------------------------------------------------
    # Plot results
    # --------------------------------------------------------

    plt.figure(figsize=(8, 6))

    plt.violinplot(
        collision_results,
        showmeans=True,
        showmedians=True,
        showextrema=True
    )

    plt.ylabel("Number of Collisions")
    plt.title(
        f"FNV-1a Collision Distribution at "
        f"{load_factor * 100:.0f}% Load "
        f"({num_simulations} Simulations)"
    )

    plt.tight_layout()
    plt.show()

    # --------------------------------------------------------
    # Print results
    # --------------------------------------------------------

    print()
    print("=" * 60)
    print("70% LOAD TEST")
    print("=" * 60)

    print(f"Hash table size:       {hash_table_size}")
    print(f"Load factor:           {load_factor:.0%}")
    print(f"Entries:               {num_entries}")
    print(f"Simulations:           {num_simulations}")
    print()
    print(f"Mean collisions:       {statistics.mean(collision_results):.2f}")
    print(f"Minimum collisions:    {min(collision_results)}")
    print(f"Maximum collisions:    {max(collision_results)}")
    print(
        f"Std deviation:         "
        f"{statistics.stdev(collision_results):.2f}"
    )

    return collision_results


# ============================================================
# Main experiment
# ============================================================

def main():
    test_70_percent_load();


    random.seed(RANDOM_SEED)

    print("Generating phone numbers...")

    phone_numbers = [
        generate_phone_number()
        for _ in range(NUM_SAMPLES)
    ]

    print("Hashing phone numbers...")

    hash_values = [
        hash_phone(phone)
        for phone in phone_numbers
    ]

    # Convert hash into an actual hash-table bucket
    bucket_values = [
        hash_value % HASH_TABLE_SIZE
        for hash_value in hash_values
    ]

    # --------------------------------------------------------
    # Statistics
    # --------------------------------------------------------

    stats = calculate_statistics(
        hash_values,
        bucket_values
    )

    print()
    print("=" * 60)
    print("HASH FUNCTION DISTRIBUTION")
    print("=" * 60)

    print(f"Samples:                    {stats['total']:,}")
    print(f"Hash table size:            {HASH_TABLE_SIZE:,}")
    print(f"Load factor:                {stats['load_factor']:.3f}")

    print()
    print("16-bit hash:")
    print(f"  Unique hash values:       {stats['unique_hashes']:,}")
    print(f"  Hash collisions:          {stats['hash_collisions']:,}")
    print(
        f"  Hash collision rate:      "
        f"{stats['hash_collision_rate'] * 100:.3f}%"
    )

    print()
    print("Hash table:")
    print(f"  Occupied buckets:         {stats['unique_buckets']:,}")
    print(f"  Empty buckets:             {stats['empty_buckets']:,}")
    print(f"  Bucket collisions:         {stats['bucket_collisions']:,}")
    print(
        f"  Bucket collision rate:     "
        f"{stats['bucket_collision_rate'] * 100:.3f}%"
    )

    print()
    print(f"Maximum bucket depth:        {stats['max_bucket_depth']}")
    print(
        f"Average occupied bucket:     "
        f"{stats['average_occupied_bucket_depth']:.3f}"
    )

    # ========================================================
    # Distribution of 16-bit hash values
    # ========================================================

    plt.figure(figsize=(12, 6))

    plt.hist(
        hash_values,
        bins=256
    )

    plt.xlabel("16-bit Hash Value")
    plt.ylabel("Number of Phone Numbers")
    plt.title("Distribution of FNV-1a 16-bit Hash Values")

    plt.tight_layout()
    plt.show()

    # ========================================================
    # Distribution across actual hash table buckets
    # ========================================================

    bucket_frequency = [
        bucket_values.count(bucket)
        for bucket in range(HASH_TABLE_SIZE)
    ]

    plt.figure(figsize=(14, 6))

    plt.bar(
        range(HASH_TABLE_SIZE),
        bucket_frequency,
        width=1.0
    )

    plt.xlabel("Hash Table Bucket")
    plt.ylabel("Number of Entries")
    plt.title(
        f"Hash Table Bucket Distribution "
        f"(N = {NUM_SAMPLES:,}, "
        f"Table Size = {HASH_TABLE_SIZE:,})"
    )

    plt.tight_layout()
    plt.show()

    # ========================================================
    # Bucket occupancy distribution
    # ========================================================

    occupancy_distribution = Counter(bucket_frequency)

    occupancies = sorted(occupancy_distribution.keys())
    number_of_buckets = [
        occupancy_distribution[x]
        for x in occupancies
    ]

    plt.figure(figsize=(10, 6))

    plt.bar(
        occupancies,
        number_of_buckets
    )

    plt.xlabel("Number of Entries in Bucket")
    plt.ylabel("Number of Buckets")
    plt.title("Hash Table Bucket Occupancy Distribution")

    plt.tight_layout()
    plt.show()

    # ========================================================
    # Print worst buckets
    # ========================================================

    bucket_counts = Counter(bucket_values)

    print()
    print("=" * 60)
    print("WORST 20 BUCKETS")
    print("=" * 60)

    print(
        f"{'Bucket':>10} {'Entries':>10}"
    )

    for bucket, count in bucket_counts.most_common(20):

        print(
            f"{bucket:>10} {count:>10}"
        )




if __name__ == "__main__":
    main()
