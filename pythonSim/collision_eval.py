#!/usr/bin/env python3

import random
import statistics
import matplotlib.pyplot as plt


# ============================================================
# Configuration
# ============================================================

HASH_TABLE_SIZE = 14293
NUM_ENTRIES = int(HASH_TABLE_SIZE * 0.70)

NUM_SIMULATIONS = 100

RANDOM_SEED = 42


# ============================================================
# Hash function
# ============================================================

def hash_phone(phone: str) -> int:
    """
    Python equivalent of the STM32 FNV-1a hash function.

    Returns a 16-bit hash value.
    """

    hash_value = 2166136261

    for char in phone:

        if '0' <= char <= '9':

            hash_value ^= ord(char)

            hash_value = (
                hash_value * 16777619
            ) & 0xFFFFFFFF

    lower = hash_value & 0xFFFF
    upper = (hash_value >> 16) & 0xFFFF

    return (lower ^ upper) & 0xFFFF


# ============================================================
# Australian phone number generation
# ============================================================

def generate_mobile_number():
    """Generate an Australian mobile number."""

    return (
        "04"
        + f"{random.randint(0, 99):02d}"
        + f"{random.randint(0, 999):03d}"
        + f"{random.randint(0, 999):03d}"
    )


def generate_landline_number():
    """Generate an Australian landline number."""

    area_code = random.choice([
        "02",
        "03",
        "07",
        "08"
    ])

    return (
        area_code
        + f"{random.randint(0, 9999):04d}"
        + f"{random.randint(0, 9999):04d}"
    )


def generate_phone_number():
    """
    Generate an Australian mobile or landline number.

    Approximately 70% are mobile numbers.
    """

    if random.random() < 0.7:
        return generate_mobile_number()

    return generate_landline_number()


# ============================================================
# Linear probing
# ============================================================

def insert_linear_probing(
    phone,
    table
):
    """
    Insert using linear probing.

    Returns:
        probes: Number of occupied slots encountered.
    """

    hash_value = hash_phone(phone)

    index = hash_value % len(table)

    probes = 0

    for _ in range(len(table)):

        if table[index] is None:

            table[index] = phone

            return probes

        probes += 1

        index = (index + 1) % len(table)

    return None


# ============================================================
# Double hashing
# ============================================================

def secondary_hash(hash_value, table_size):
    """
    Secondary hash function.

    The step size must never be zero.

    Using a prime table size allows:

        step = prime - (hash % prime)

    to generate a valid probe sequence.
    """

    prime = 14281

    return prime - (hash_value % prime)


def insert_double_hashing(
    phone,
    table
):
    """
    Insert using double hashing.

    Returns:
        probes: Number of occupied slots encountered.
    """

    hash_value = hash_phone(phone)

    index = hash_value % len(table)

    step = secondary_hash(
        hash_value,
        len(table)
    )

    probes = 0

    for _ in range(len(table)):

        if table[index] is None:

            table[index] = phone

            return probes

        probes += 1

        index = (
            index + step
        ) % len(table)

    return None


# ============================================================
# Cuckoo hashing
# ============================================================

class CuckooHashTable:

    def __init__(self, size):

        self.size = size

        self.table1 = [None] * size
        self.table2 = [None] * size

        self.relocations = 0

    def hash1(self, phone):

        return (
            hash_phone(phone)
            % self.size
        )

    def hash2(self, phone):

        hash_value = hash_phone(phone)

        return (
            (hash_value * 31 + 17)
            % self.size
        )

    def insert(self, phone):

        current = phone

        table_number = 1

        max_relocations = self.size

        for _ in range(max_relocations):

            if table_number == 1:

                index = self.hash1(current)

                if self.table1[index] is None:

                    self.table1[index] = current

                    return True

                current, self.table1[index] = (
                    self.table1[index],
                    current
                )

                table_number = 2

            else:

                index = self.hash2(current)

                if self.table2[index] is None:

                    self.table2[index] = current

                    return True

                current, self.table2[index] = (
                    self.table2[index],
                    current
                )

                table_number = 1

            self.relocations += 1

        return False


# ============================================================
# Run one simulation
# ============================================================

def run_simulation():

    phone_numbers = [
        generate_phone_number()
        for _ in range(NUM_ENTRIES)
    ]

    # --------------------------------------------------------
    # Linear probing
    # --------------------------------------------------------

    linear_table = [
        None
    ] * HASH_TABLE_SIZE

    linear_probes = []

    linear_failed = 0

    for phone in phone_numbers:

        probes = insert_linear_probing(
            phone,
            linear_table
        )

        if probes is None:

            linear_failed += 1

        else:

            linear_probes.append(probes)

    # --------------------------------------------------------
    # Double hashing
    # --------------------------------------------------------

    double_table = [
        None
    ] * HASH_TABLE_SIZE

    double_probes = []

    double_failed = 0

    for phone in phone_numbers:

        probes = insert_double_hashing(
            phone,
            double_table
        )

        if probes is None:

            double_failed += 1

        else:

            double_probes.append(probes)

    # --------------------------------------------------------
    # Cuckoo hashing
    # --------------------------------------------------------

    cuckoo = CuckooHashTable(
        HASH_TABLE_SIZE
    )

    cuckoo_failed = 0

    for phone in phone_numbers:

        success = cuckoo.insert(
            phone
        )

        if not success:

            cuckoo_failed += 1

            break

    return {
        "linear": {
            "probes": linear_probes,
            "failed": linear_failed
        },

        "double": {
            "probes": double_probes,
            "failed": double_failed
        },

        "cuckoo": {
            "relocations": cuckoo.relocations,
            "failed": cuckoo_failed
        }
    }


# ============================================================
# Run experiments
# ============================================================

def main():

    random.seed(RANDOM_SEED)

    linear_average_probes = []
    double_average_probes = []
    cuckoo_relocations = []

    linear_max_probes = []
    double_max_probes = []

    linear_failures = []
    double_failures = []
    cuckoo_failures = []

    print()
    print("=" * 70)
    print("HASH COLLISION MANAGEMENT COMPARISON")
    print("=" * 70)

    print(
        f"Hash table size:       {HASH_TABLE_SIZE:,}"
    )

    print(
        f"Entries:               {NUM_ENTRIES:,}"
    )

    print(
        f"Load factor:           "
        f"{NUM_ENTRIES / HASH_TABLE_SIZE:.0%}"
    )

    print(
        f"Simulations:           {NUM_SIMULATIONS}"
    )

    print()

    # --------------------------------------------------------
    # Simulations
    # --------------------------------------------------------

    for simulation in range(NUM_SIMULATIONS):

        results = run_simulation()

        # Linear probing

        linear_probes = (
            results["linear"]["probes"]
        )

        linear_average_probes.append(
            statistics.mean(linear_probes)
        )

        linear_max_probes.append(
            max(linear_probes)
        )

        linear_failures.append(
            results["linear"]["failed"]
        )

        # Double hashing

        double_probes = (
            results["double"]["probes"]
        )

        double_average_probes.append(
            statistics.mean(double_probes)
        )

        double_max_probes.append(
            max(double_probes)
        )

        double_failures.append(
            results["double"]["failed"]
        )

        # Cuckoo hashing

        cuckoo_relocations.append(
            results["cuckoo"]["relocations"]
        )

        cuckoo_failures.append(
            results["cuckoo"]["failed"]
        )

    # ========================================================
    # Print results
    # ========================================================

    print("=" * 70)
    print("RESULTS")
    print("=" * 70)

    print()

    print("LINEAR PROBING")
    print("-" * 70)

    print(
        f"Average probes:        "
        f"{statistics.mean(linear_average_probes):.3f}"
    )

    print(
        f"Maximum probes:        "
        f"{max(linear_max_probes)}"
    )

    print(
        f"Average failed:        "
        f"{statistics.mean(linear_failures):.3f}"
    )

    print()

    print("DOUBLE HASHING")
    print("-" * 70)

    print(
        f"Average probes:        "
        f"{statistics.mean(double_average_probes):.3f}"
    )

    print(
        f"Maximum probes:        "
        f"{max(double_max_probes)}"
    )

    print(
        f"Average failed:        "
        f"{statistics.mean(double_failures):.3f}"
    )

    print()

    print("CUCKOO HASHING")
    print("-" * 70)

    print(
        f"Average relocations:   "
        f"{statistics.mean(cuckoo_relocations):.3f}"
    )

    print(
        f"Maximum relocations:   "
        f"{max(cuckoo_relocations)}"
    )

    print(
        f"Average failed:        "
        f"{statistics.mean(cuckoo_failures):.3f}"
    )

    # ========================================================
    # Plot
    # ========================================================

    fig, axes = plt.subplots(
        2,
        2,
        figsize=(15, 10)
    )

    # --------------------------------------------------------
    # Plot 1 - Average probes
    # --------------------------------------------------------

    methods = [
        "Linear Probing",
        "Double Hashing"
    ]

    average_probes = [
        statistics.mean(linear_average_probes),
        statistics.mean(double_average_probes)
    ]

    axes[0, 0].bar(
        methods,
        average_probes
    )

    axes[0, 0].set_ylabel(
        "Average Probes"
    )

    axes[0, 0].set_title(
        "Average Probes per Insertion"
    )

    # --------------------------------------------------------
    # Plot 2 - Maximum probe length
    # --------------------------------------------------------

    maximum_probes = [
        max(linear_max_probes),
        max(double_max_probes)
    ]

    axes[0, 1].bar(
        methods,
        maximum_probes
    )

    axes[0, 1].set_ylabel(
        "Maximum Probes"
    )

    axes[0, 1].set_title(
        "Maximum Probe Length"
    )

    # --------------------------------------------------------
    # Plot 3 - Cuckoo relocations
    # --------------------------------------------------------

    axes[1, 0].hist(
        cuckoo_relocations,
        bins=20
    )

    axes[1, 0].set_xlabel(
        "Number of Relocations"
    )

    axes[1, 0].set_ylabel(
        "Number of Simulations"
    )

    axes[1, 0].set_title(
        "Cuckoo Hashing Relocations"
    )

    # --------------------------------------------------------
    # Plot 4 - Probe distribution
    # --------------------------------------------------------

    axes[1, 1].boxplot(
    [
        linear_average_probes,
        double_average_probes,
        cuckoo_relocations
    ],
    tick_labels=[
        "Linear",
        "Double",
        "Cuckoo"
    ]
)
    axes[1, 1].set_ylabel(
        "Operations"
    )

    axes[1, 1].set_title(
        "Distribution Across Simulations"
    )

    # --------------------------------------------------------
    # Figure formatting
    # --------------------------------------------------------

    fig.suptitle(
        "Hash Collision Management Comparison",
        fontsize=16
    )

    plt.tight_layout()

    plt.subplots_adjust(
        top=0.92
    )

    plt.show()


# ============================================================
# Main
# ============================================================

if __name__ == "__main__":
    main()
