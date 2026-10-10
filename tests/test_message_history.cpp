#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C"
{
#include "message_history.h"
#include "ring_buffer.h"
#include "journal.h"
#include "storage.h"
#include "mem_layout.h"
}

#include "test_support/failable_storage.h"

/**
 * @brief Behaviour of the message history ring buffer (message_history.c).
 *
 * Each ring buffer slot is one sector holding MESSAGE_HISTORY_SECTOR_CAPACITY
 * messages, header.head is the newest message in the sector. The region is
 * kept small (kSectors) so wrapping can be exercised, and a guard sector
 * sits directly after the region to catch reads/writes past its end.
 */
class MessageHistoryTest : public ::testing::Test
{
protected:
    static constexpr uint16_t kSectors = 4;
    static constexpr uint16_t kCap = MESSAGE_HISTORY_SECTOR_CAPACITY;
    static constexpr uint16_t kStart = DATA_REGION_START_SECTOR + MESSAGE_HISTORY_DATA_START_SECTOR;
    static constexpr uint16_t kGuard = kStart + kSectors;
    static constexpr uint32_t kTotalSectors = kGuard + 1;
    static constexpr uint32_t kGuardSeq = 0xDEADBEEF;

    FailableStorageCtx ctx{};
    Storage storage{};
    std::vector<uint8_t> mem;
    Journal journal{};
    RingBuffer rb{};

    // Number of messages added so far (also used as the message's timestamp)
    uint16_t added = 0;

    void SetUp() override
    {
        mem.assign(static_cast<size_t>(SECTOR_SIZE) * kTotalSectors, 0);
        ASSERT_TRUE(FailableStorage_Init(&ctx, mem.data(), SECTOR_SIZE, kTotalSectors));
        storage = FailableStorage_Make(&ctx);

        std::memset(&journal, 0, sizeof(Journal));
        journal.storage = &storage;

        // Format: every region sector empty with a valid CRC
        for (uint16_t i = 0; i < kSectors; i++)
        {
            write_header(kStart + i, 0, RB_EMPTY, 0);
        }
        write_header(kGuard, kGuardSeq, RB_OCCUPIED, kCap - 1);

        ASSERT_TRUE(init_ring_buffer(&storage, &rb, kSectors, kStart));
    }

    void write_header(uint16_t raw, uint32_t seq, RB_SECTOR_STATE state, uint8_t head)
    {
        MessageHistorySectorB s;
        std::memset(&s, 0, sizeof(s));
        s.sector.header.seq = seq;
        s.sector.header.state = state;
        s.sector.header.head = head;
        ASSERT_EQ(write_sector(&storage, raw, s.buffer), STRG_OK);
    }

    /** @brief Sector i of the region as stored on the card. */
    MessageHistorySectorB stored(uint16_t i)
    {
        MessageHistorySectorB s;
        EXPECT_EQ(read_sector(&storage, kStart + i, s.buffer), STRG_OK);
        return s;
    }

    static Message make_message(uint16_t id)
    {
        Message m;
        std::memset(&m, 0, sizeof(m));
        m.timestamp = id;
        m.direction = (id % 2) != 0;
        std::snprintf(m.str, sizeof(m.str), "message %u", id);
        return m;
    }

    void add(int n = 1)
    {
        for (int i = 0; i < n; i++)
        {
            Message m = make_message(added);
            ASSERT_EQ(message_history_add(&rb, &journal, &storage, &m), STRG_OK) << "add #" << added;
            added++;
        }
    }

    /** @brief Expect out[0..count) to be the newest `count` messages, newest first. */
    void expect_newest_first(const std::vector<Message> &out, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            Message want = make_message(static_cast<uint16_t>(added - 1 - i));
            EXPECT_EQ(out[i].timestamp, want.timestamp) << "out[" << i << "]";
            EXPECT_STREQ(out[i].str, want.str) << "out[" << i << "]";
        }
    }

    STRG_RET get_list(size_t n, std::vector<Message> &out, size_t &count)
    {
        out.assign(n + 1, Message{});
        // Sentinel past the requested range to catch overruns
        out[n].timestamp = 0xBEEF;
        count = 0xFFFF;
        STRG_RET ret = message_history_get_list(&rb, &storage, n, &count, out.data());
        EXPECT_EQ(out[n].timestamp, 0xBEEF) << "get_list wrote past n";
        return ret;
    }
};

/* ---- get_list on an empty buffer ---------------------------------------- */

TEST_F(MessageHistoryTest, GetListOnEmptyBufferReturnsZero)
{
    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_list(5, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageHistoryTest, GetListWithZeroRequestedReturnsZero)
{
    add(3);
    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_list(0, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageHistoryTest, GetListRejectsNullArguments)
{
    Message out[1];
    size_t count;
    EXPECT_EQ(message_history_get_list(nullptr, &storage, 1, &count, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_list(&rb, nullptr, 1, &count, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_list(&rb, &storage, 1, nullptr, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_list(&rb, &storage, 1, &count, nullptr), STRG_FAIL);
}

/* ---- add: first sector --------------------------------------------------- */

TEST_F(MessageHistoryTest, FirstAddClaimsSectorZero)
{
    add();

    EXPECT_EQ(rb.current_index, 0);
    EXPECT_EQ(rb.occupancy, 1);
    EXPECT_EQ(rb.seq, 0u);

    MessageHistorySectorB s = stored(0);
    EXPECT_EQ(s.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s.sector.header.head, 0);
    EXPECT_EQ(s.sector.header.seq, 0u);
    EXPECT_EQ(s.sector.data.mh_entry[0].timestamp, 0);
}

TEST_F(MessageHistoryTest, AddsWithinSectorAdvanceHead)
{
    add(kCap);

    EXPECT_EQ(rb.current_index, 0);
    EXPECT_EQ(rb.occupancy, 1);

    MessageHistorySectorB s = stored(0);
    EXPECT_EQ(s.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s.sector.header.head, kCap - 1);
    for (uint16_t i = 0; i < kCap; i++)
    {
        EXPECT_EQ(s.sector.data.mh_entry[i].timestamp, i);
    }
}

/* ---- add: moving between sectors ---------------------------------------- */

TEST_F(MessageHistoryTest, AddPastFullSectorMovesToNextSector)
{
    add(kCap + 1);

    EXPECT_EQ(rb.current_index, 1);
    EXPECT_EQ(rb.occupancy, 2);
    EXPECT_EQ(rb.seq, 1u);

    MessageHistorySectorB s0 = stored(0);
    EXPECT_EQ(s0.sector.header.state, RB_FULL);
    EXPECT_EQ(s0.sector.header.head, kCap - 1);

    MessageHistorySectorB s1 = stored(1);
    EXPECT_EQ(s1.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s1.sector.header.head, 0);
    EXPECT_EQ(s1.sector.header.seq, 1u);
    EXPECT_EQ(s1.sector.data.mh_entry[0].timestamp, kCap);
}

TEST_F(MessageHistoryTest, SectorsFillSequentiallyWithIncreasingSeq)
{
    add(kSectors * kCap);

    EXPECT_EQ(rb.current_index, kSectors - 1);
    EXPECT_EQ(rb.occupancy, kSectors);
    EXPECT_EQ(rb.seq, kSectors - 1u);

    for (uint16_t i = 0; i < kSectors; i++)
    {
        MessageHistorySectorB s = stored(i);
        EXPECT_EQ(s.sector.header.seq, i) << "sector " << i;
        EXPECT_EQ(s.sector.header.state, i == kSectors - 1 ? RB_OCCUPIED : RB_FULL) << "sector " << i;
        EXPECT_EQ(s.sector.data.mh_entry[0].timestamp, i * kCap) << "sector " << i;
    }
}

/* ---- add: wrapping ------------------------------------------------------- */

TEST_F(MessageHistoryTest, WrapOverwritesOldestSector)
{
    add(kSectors * kCap + 1);

    EXPECT_EQ(rb.current_index, 0);
    EXPECT_EQ(rb.occupancy, kSectors);
    EXPECT_EQ(rb.seq, kSectors);

    MessageHistorySectorB s0 = stored(0);
    EXPECT_EQ(s0.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s0.sector.header.head, 0);
    EXPECT_EQ(s0.sector.header.seq, kSectors);
    EXPECT_EQ(s0.sector.data.mh_entry[0].timestamp, kSectors * kCap);
}

TEST_F(MessageHistoryTest, AddsAfterWrapKeepFillingReusedSector)
{
    // After reusing a FULL sector the next add must append, not reset head
    add(kSectors * kCap + 3);

    MessageHistorySectorB s0 = stored(0);
    EXPECT_EQ(s0.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s0.sector.header.head, 2);
    for (uint16_t i = 0; i < 3; i++)
    {
        EXPECT_EQ(s0.sector.data.mh_entry[i].timestamp, kSectors * kCap + i);
    }
}

TEST_F(MessageHistoryTest, ManyWrapsStayInsideRegion)
{
    add(3 * kSectors * kCap + 2);

    EXPECT_LT(rb.current_index, kSectors);
    EXPECT_EQ(rb.occupancy, kSectors);

    // Guard sector untouched
    MessageHistorySectorB g;
    ASSERT_EQ(read_sector(&storage, kGuard, g.buffer), STRG_OK);
    EXPECT_EQ(g.sector.header.seq, kGuardSeq);
}

/* ---- add: error handling ------------------------------------------------- */

TEST_F(MessageHistoryTest, AddRejectsCorruptSectorState)
{
    write_header(kStart, 0, 0x7F, 200);
    Message m = make_message(0);
    EXPECT_NE(message_history_add(&rb, &journal, &storage, &m), STRG_OK);

    // Sector not rewritten
    MessageHistorySectorB s = stored(0);
    EXPECT_EQ(s.sector.header.state, 0x7F);
}

TEST_F(MessageHistoryTest, AddPropagatesReadFailure)
{
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    Message m = make_message(0);
    EXPECT_EQ(message_history_add(&rb, &journal, &storage, &m), STRG_FAIL);
}

/* ---- get_list ------------------------------------------------------------ */

TEST_F(MessageHistoryTest, GetListWithinOneSectorIsNewestFirst)
{
    add(3);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(3, out, count), STRG_OK);
    EXPECT_EQ(count, 3u);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListFewerThanStoredReturnsNewest)
{
    add(kCap + 2);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(4, out, count), STRG_OK);
    EXPECT_EQ(count, 4u);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListSpansSectors)
{
    add(2 * kCap + 1);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(2 * kCap + 1, out, count), STRG_OK);
    EXPECT_EQ(count, 2u * kCap + 1);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListMoreThanStoredReturnsStoredCountNoDuplicates)
{
    add(kCap + 1);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(50, out, count), STRG_OK);
    EXPECT_EQ(count, kCap + 1u);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListOnWrappedBufferReturnsNewestAndStops)
{
    // Wrapped once with 2 messages in the reused sector 0:
    // sector 0 holds 2 newest, sectors 1..kSectors-1 are full
    add(kSectors * kCap + 2);
    const size_t stored_count = 2 + (kSectors - 1) * kCap;

    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(1000, out, count), STRG_OK);
    EXPECT_EQ(count, stored_count);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListOnFullUnwrappedBufferReturnsAll)
{
    add(kSectors * kCap);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(1000, out, count), STRG_OK);
    EXPECT_EQ(count, static_cast<size_t>(kSectors * kCap));
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListDoesNotModifyStorage)
{
    add(kCap + 2);
    std::vector<uint8_t> before = mem;
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_list(100, out, count), STRG_OK);
    EXPECT_EQ(mem, before);
}

TEST_F(MessageHistoryTest, GetListPropagatesReadFailure)
{
    add(2);
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_list(2, out, count), STRG_FAIL);
}

/* ---- persistence --------------------------------------------------------- */

/**
 * @brief Reconstructing the ring buffer from the card after a series of adds
 *        gives the same RAM state, and adding/listing continues seamlessly.
 */
TEST_F(MessageHistoryTest, ReconstructAfterAddsMatchesRamState)
{
    const int counts[] = {1, kCap, kCap + 1, kSectors * kCap, kSectors * kCap + 1, 2 * kSectors * kCap + 3};
    for (int n : counts)
    {
        SetUp();
        added = 0;
        add(n);
        RingBuffer before = rb;

        RingBuffer after{};
        ASSERT_TRUE(init_ring_buffer(&storage, &after, kSectors, kStart)) << "n=" << n;
        EXPECT_EQ(after.current_index, before.current_index) << "n=" << n;
        EXPECT_EQ(after.occupancy, before.occupancy) << "n=" << n;
        EXPECT_EQ(after.seq, before.seq) << "n=" << n;

        // Continue on the reconstructed state
        rb = after;
        add(kCap + 1);
        std::vector<Message> out;
        size_t count;
        ASSERT_EQ(get_list(5, out, count), STRG_OK) << "n=" << n;
        EXPECT_EQ(count, 5u) << "n=" << n;
        expect_newest_first(out, count);
    }
}
