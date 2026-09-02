import random
import math

# ============================================================
# Configuration
# ============================================================

SECTOR_SIZE = 512
BITS_PER_ELEMENT = 32
BYTES_PER_ELEMENT = 4
ELEMENTS_PER_SECTOR = SECTOR_SIZE // BYTES_PER_ELEMENT

# Change this to your actual number of data sectors
TOTAL_DATA_SECTOR_SIZE = 44800

USAGE_BITMAP_START_SECTOR = 1


# ============================================================
# Equivalent to the C macros
# ============================================================

USAGE_BITMAP_SIZE = (
    TOTAL_DATA_SECTOR_SIZE + BITS_PER_ELEMENT - 1
) // BITS_PER_ELEMENT

USAGE_BITMAP_SECTOR_SIZE = (
    USAGE_BITMAP_SIZE + ELEMENTS_PER_SECTOR - 1
) // ELEMENTS_PER_SECTOR

USAGE_BITMAP_STORAGE_SIZE = (
    USAGE_BITMAP_SECTOR_SIZE * ELEMENTS_PER_SECTOR
)


def find_bitmap_sector(sector_index):
    return sector_index // (SECTOR_SIZE * 8)


def find_bitmap_element(sector_index):
    return (
        (sector_index % (SECTOR_SIZE * 8))
        // BITS_PER_ELEMENT
    )


def find_bitmap_index(sector_index):
    return sector_index // BITS_PER_ELEMENT


def find_bitmap_bit(sector_index):
    return sector_index % BITS_PER_ELEMENT


# ============================================================
# Simulated SD card
# ============================================================

class FakeSDCard:

    def __init__(self):
        self.writes = []
        self.reads = []

    def read_multiblock(self, start_sector, sector_count):
        # Record read
        self.reads.append(
            (start_sector, sector_count)
        )

        # Return simulated sector data
        return bytearray(
            sector_count * SECTOR_SIZE
        )

    def write_block(self, sector, data):
        # Record write
        self.writes.append(
            (sector, len(data))
        )

        # Verify write is exactly one sector
        assert len(data) == SECTOR_SIZE, (
            f"Write size is {len(data)}, "
            f"expected {SECTOR_SIZE}"
        )

        # Verify sector number is valid
        assert sector >= 0, (
            f"Invalid sector {sector}"
        )

        return True


# ============================================================
# Simulated usage bitmap
# ============================================================

class UsageBitmap:

    def __init__(self, storage):
        self.storage = storage

        # Equivalent to:
        #
        # uint32_t usage_bitmap[
        #     USAGE_BITMAP_STORAGE_SIZE
        # ];
        self.bitmap = [
            0
        ] * USAGE_BITMAP_STORAGE_SIZE

    def initial_read(self):

        data = self.storage.read_multiblock(
            USAGE_BITMAP_START_SECTOR,
            USAGE_BITMAP_SECTOR_SIZE
        )

        # Verify read alignment
        assert len(data) % SECTOR_SIZE == 0

        assert len(data) == (
            USAGE_BITMAP_SECTOR_SIZE * SECTOR_SIZE
        )

        # Simulate conversion from bytes -> uint32_t
        #
        # The actual contents don't matter for this
        # alignment test.
        return True

    def check_usage_bit(self, index):

        assert 0 <= index < TOTAL_DATA_SECTOR_SIZE

        word = find_bitmap_index(index)
        bit = find_bitmap_bit(index)

        assert word < USAGE_BITMAP_STORAGE_SIZE

        return (
            (self.bitmap[word] >> bit)
            & 1
        )

    def update_usage_bit(self, index, used_state):

        # ----------------------------------------------------
        # Validate logical sector index
        # ----------------------------------------------------

        assert 0 <= index < TOTAL_DATA_SECTOR_SIZE, (
            f"Invalid data sector index: {index}"
        )

        # ----------------------------------------------------
        # Equivalent to C:
        #
        # bitmap_sector =
        #     USAGE_BITMAP_FIND_SECTOR(index);
        # ----------------------------------------------------

        bitmap_sector = find_bitmap_sector(index)

        assert bitmap_sector < USAGE_BITMAP_SECTOR_SIZE, (
            f"Bitmap sector {bitmap_sector} "
            f"outside bitmap"
        )

        # ----------------------------------------------------
        # Equivalent to:
        #
        # &usage_bitmap[
        #     bitmap_sector * ELEMENTS_PER_SECTOR
        # ]
        # ----------------------------------------------------

        sector_start_element = (
            bitmap_sector *
            ELEMENTS_PER_SECTOR
        )

        sector_end_element = (
            sector_start_element +
            ELEMENTS_PER_SECTOR
        )

        # Verify RAM sector alignment
        assert (
            sector_start_element %
            ELEMENTS_PER_SECTOR
        ) == 0

        assert (
            sector_end_element <=
            USAGE_BITMAP_STORAGE_SIZE
        )

        # ----------------------------------------------------
        # Find word and bit
        # ----------------------------------------------------

        element = find_bitmap_element(index)
        bit = find_bitmap_bit(index)

        assert 0 <= element < ELEMENTS_PER_SECTOR
        assert 0 <= bit < BITS_PER_ELEMENT

        # Absolute RAM word
        absolute_element = (
            sector_start_element +
            element
        )

        assert (
            absolute_element <
            USAGE_BITMAP_STORAGE_SIZE
        )

        # ----------------------------------------------------
        # Modify bit
        # ----------------------------------------------------

        if used_state:
            self.bitmap[absolute_element] |= (
                1 << bit
            )
        else:
            self.bitmap[absolute_element] &= ~(
                1 << bit
            )

        # ----------------------------------------------------
        # Equivalent to:
        #
        # write_block(
        #     USAGE_BITMAP_START_SECTOR +
        #     bitmap_sector,
        #     write_sector
        # )
        # ----------------------------------------------------

        sd_sector = (
            USAGE_BITMAP_START_SECTOR +
            bitmap_sector
        )

        # Extract exactly one 512-byte sector
        write_start = sector_start_element
        write_end = sector_end_element

        sector_words = self.bitmap[
            write_start:write_end
        ]

        assert len(sector_words) == ELEMENTS_PER_SECTOR

        # Convert uint32_t words into bytes
        write_data = bytearray(
            len(sector_words) *
            BYTES_PER_ELEMENT
        )

        # We don't actually care about endian ordering
        # for this alignment test.
        for i, word in enumerate(sector_words):

            write_data[
                i * 4:
                i * 4 + 4
            ] = int(word & 0xFFFFFFFF).to_bytes(
                4,
                byteorder="little"
            )

        # ----------------------------------------------------
        # Final alignment checks
        # ----------------------------------------------------

        assert len(write_data) == SECTOR_SIZE

        assert (
            len(write_data) %
            SECTOR_SIZE
        ) == 0

        # Write
        self.storage.write_block(
            sd_sector,
            write_data
        )


# ============================================================
# Mathematical tests
# ============================================================

def test_bitmap_math():

    print("Testing bitmap maths...")

    assert ELEMENTS_PER_SECTOR == 128

    # Bitmap must contain enough bits
    assert (
        USAGE_BITMAP_SIZE *
        BITS_PER_ELEMENT
    ) >= TOTAL_DATA_SECTOR_SIZE

    # Bitmap must not be unnecessarily large
    assert (
        (USAGE_BITMAP_SIZE - 1) *
        BITS_PER_ELEMENT
    ) < TOTAL_DATA_SECTOR_SIZE

    # Storage must contain whole sectors
    assert (
        USAGE_BITMAP_STORAGE_SIZE %
        ELEMENTS_PER_SECTOR
    ) == 0

    # Storage must be large enough
    assert (
        USAGE_BITMAP_STORAGE_SIZE >=
        USAGE_BITMAP_SIZE
    )

    print("  PASS")


# ============================================================
# Test every possible logical sector
# ============================================================

def test_every_sector():

    print(
        f"Testing every data sector "
        f"(0 -> {TOTAL_DATA_SECTOR_SIZE - 1})..."
    )

    storage = FakeSDCard()
    bitmap = UsageBitmap(storage)

    bitmap.initial_read()

    for index in range(TOTAL_DATA_SECTOR_SIZE):

        bitmap.update_usage_bit(
            index,
            True
        )

        # Check it actually became set
        assert bitmap.check_usage_bit(index)

        bitmap.update_usage_bit(
            index,
            False
        )

        # Check it actually became clear
        assert not bitmap.check_usage_bit(index)

    print("  PASS")


# ============================================================
# Test bitmap sector boundaries
# ============================================================

def test_boundaries():

    print("Testing bitmap sector boundaries...")

    storage = FakeSDCard()
    bitmap = UsageBitmap(storage)

    bitmap.initial_read()

    # Each bitmap sector represents:
    #
    # 512 * 8 = 4096 data sectors

    sectors_per_bitmap_sector = (
        SECTOR_SIZE * 8
    )

    for bitmap_sector in range(
        USAGE_BITMAP_SECTOR_SIZE
    ):

        first = (
            bitmap_sector *
            sectors_per_bitmap_sector
        )

        last = (
            first +
            sectors_per_bitmap_sector -
            1
        )

        # Don't test beyond logical bitmap
        if first >= TOTAL_DATA_SECTOR_SIZE:
            continue

        last = min(
            last,
            TOTAL_DATA_SECTOR_SIZE - 1
        )

        indices = {
            first,
            last,
        }

        # Also test the boundary before/after
        if first > 0:
            indices.add(first - 1)

        if last + 1 < TOTAL_DATA_SECTOR_SIZE:
            indices.add(last + 1)

        for index in indices:

            bitmap.update_usage_bit(
                index,
                True
            )

            assert bitmap.check_usage_bit(index)

    print("  PASS")


# ============================================================
# Random stress test
# ============================================================

def test_random_updates(iterations=100000):

    print(
        f"Running {iterations:,} random updates..."
    )

    storage = FakeSDCard()
    bitmap = UsageBitmap(storage)

    bitmap.initial_read()

    expected = [
        False
    ] * TOTAL_DATA_SECTOR_SIZE

    for _ in range(iterations):

        index = random.randrange(
            TOTAL_DATA_SECTOR_SIZE
        )

        state = bool(
            random.getrandbits(1)
        )

        bitmap.update_usage_bit(
            index,
            state
        )

        expected[index] = state

        assert (
            bitmap.check_usage_bit(index)
            == expected[index]
        )

    print("  PASS")


# ============================================================
# Verify every SD write was aligned
# ============================================================

def test_write_alignment(storage):

    print("Checking all SD writes...")

    for i, (sector, size) in enumerate(
        storage.writes
    ):

        # Every write must be exactly one sector
        assert size == SECTOR_SIZE, (
            f"Write {i}: "
            f"{size} bytes"
        )

        # Sector address itself must be valid
        assert sector >= (
            USAGE_BITMAP_START_SECTOR
        )

        # Every write must fall within bitmap
        assert sector < (
            USAGE_BITMAP_START_SECTOR +
            USAGE_BITMAP_SECTOR_SIZE
        )

    print(
        f"  PASS "
        f"({len(storage.writes):,} writes checked)"
    )


# ============================================================
# Verify initial read alignment
# ============================================================

def test_read_alignment(storage):

    print("Checking initial read...")

    assert len(storage.reads) == 1

    start_sector, sector_count = (
        storage.reads[0]
    )

    assert (
        start_sector ==
        USAGE_BITMAP_START_SECTOR
    )

    assert (
        sector_count ==
        USAGE_BITMAP_SECTOR_SIZE
    )

    # Must represent whole sectors
    assert (
        sector_count * SECTOR_SIZE
    ) % SECTOR_SIZE == 0

    print(
        f"  PASS "
        f"({sector_count} sectors / "
        f"{sector_count * SECTOR_SIZE} bytes)"
    )


# ============================================================
# Main
# ============================================================

def main():

    print("=" * 60)
    print("USAGE BITMAP ALIGNMENT TEST")
    print("=" * 60)

    print()
    print("Configuration:")
    print(
        f"  Data sectors:           "
        f"{TOTAL_DATA_SECTOR_SIZE:,}"
    )
    print(
        f"  Bitmap words:           "
        f"{USAGE_BITMAP_SIZE:,}"
    )
    print(
        f"  Bitmap SD sectors:      "
        f"{USAGE_BITMAP_SECTOR_SIZE:,}"
    )
    print(
        f"  RAM words allocated:    "
        f"{USAGE_BITMAP_STORAGE_SIZE:,}"
    )
    print(
        f"  RAM bytes allocated:    "
        f"{USAGE_BITMAP_STORAGE_SIZE * 4:,}"
    )
    print(
        f"  SD bytes allocated:     "
        f"{USAGE_BITMAP_SECTOR_SIZE * 512:,}"
    )
    print(
        f"  Data sectors/bit:       1"
    )
    print(
        f"  Data sectors/bitmap sector: "
        f"{SECTOR_SIZE * 8:,}"
    )

    print()

    # Mathematical validation
    test_bitmap_math()

    # Every possible index
    test_every_sector()

    # Explicit boundary testing
    test_boundaries()

    # Random stress
    test_random_updates()

    # Get storage used by the random test
    storage = FakeSDCard()
    bitmap = UsageBitmap(storage)

    bitmap.initial_read()

    # Do some writes so we can validate them
    for _ in range(10000):

        index = random.randrange(
            TOTAL_DATA_SECTOR_SIZE
        )

        bitmap.update_usage_bit(
            index,
            bool(random.getrandbits(1))
        )

    test_read_alignment(storage)
    test_write_alignment(storage)

    print()
    print("=" * 60)
    print("ALL TESTS PASSED")
    print("=" * 60)


if __name__ == "__main__":
    main()
