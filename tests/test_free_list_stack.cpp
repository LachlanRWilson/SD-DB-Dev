#include <gtest/gtest.h>

extern "C" {
#include "free_list_stack.h"
}

/**
 * @brief Test fixture for FreeList allocator tests.
 *
 * Provides a fresh allocator instance for each test and ensures
 * deterministic initialisation and cleanup.
 */
class FreeListTest : public ::testing::Test
{
protected:
    FreeList allocator_storage;
    FreeList *allocator = &allocator_storage;

    void SetUp() override
    {
        ASSERT_TRUE(free_list_init_software(allocator, 10));
    }

    void TearDown() override
    {
        free_list_reset(allocator);
    }
};

/**
 * @brief Verifies that sequential allocation returns valid sectors
 * until exhaustion, and then correctly returns UINT32_MAX when full.
 */
TEST_F(FreeListTest, AllocateSequentially)
{
    int test = 9;
    for (uint16_t i = 0; i < 10; i++)
    {
        uint16_t sector = free_list_allocate(allocator);
        // Every value should come out in decending order
        EXPECT_EQ(sector, test--);
        EXPECT_NE(sector, UINT32_MAX);
    }

    EXPECT_EQ(free_list_allocate(allocator), UINT32_MAX);
}

/**
 * @brief Ensures allocator behaves in LIFO order (stack-based allocation),
 * meaning the most recently freed/available sector is returned first.
 */
TEST_F(FreeListTest, AllocationIsLIFO)
{
    uint16_t first = free_list_allocate(allocator);
    uint16_t second = free_list_allocate(allocator);

    EXPECT_EQ(first, 9u);
    EXPECT_EQ(second, 8u);
}

/**
 * @brief Confirms that a freed sector is correctly returned to the pool
 * and can be reallocated in subsequent allocation calls.
 */
TEST_F(FreeListTest, FreeReusesSector)
{
    uint16_t a = free_list_allocate(allocator);
    uint16_t b = free_list_allocate(allocator);

    free_list_free(allocator, a);

    uint16_t c = free_list_allocate(allocator);

    EXPECT_EQ(c, a);
}

/**
 * @brief Tests full allocation cycle:
 * - allocate all sectors
 * - confirm exhaustion
 * - free a sector
 * - confirm it is reused on next allocation
 */
TEST_F(FreeListTest, FullCycleAllocateFreeAllocate)
{
    uint16_t sectors[10];

    for (uint16_t i = 0; i < 10; i++)
    {
        sectors[i] = free_list_allocate(allocator);
    }

    EXPECT_EQ(free_list_allocate(allocator), UINT32_MAX);

    free_list_free(allocator, sectors[5]);

    uint16_t reused = free_list_allocate(allocator);
    EXPECT_EQ(reused, sectors[5]);
}

/**
 * @brief Validates internal counters for correctness:
 * used_count and available count must reflect actual allocation state.
 */
TEST_F(FreeListTest, CountersTrackCorrectly)
{
    EXPECT_EQ(free_list_used(allocator), 0u);
    EXPECT_EQ(free_list_available(allocator), 10u);

    free_list_allocate(allocator);
    free_list_allocate(allocator);

    EXPECT_EQ(free_list_used(allocator), 2u);
    EXPECT_EQ(free_list_available(allocator), 8u);
}

/**
 * @brief Ensures reset restores allocator to initial state:
 * all sectors become available and counters return to zero state.
 */
TEST_F(FreeListTest, ResetRestoresAllSectors)
{
    for (int i = 0; i < 5; i++)
    {
        free_list_allocate(allocator);
    }

    free_list_reset(allocator);

    EXPECT_EQ(free_list_used(allocator), 0u);
    EXPECT_EQ(free_list_available(allocator), 10u);

    for (int i = 0; i < 10; i++)
    {
        EXPECT_NE(free_list_allocate(allocator), UINT32_MAX);
    }
}
