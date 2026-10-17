#include <gtest/gtest.h>
#include <algorithm>
#include "test_support/message_ts.h"
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
 * entries, header.head is the newest entry in the sector. An entry is a copy
 * of the message plus the sector index (msgInd) of the message sector it was
 * copied from. The region is
 * kept small (kSectors) so wrapping can be exercised, and a guard sector
 * sits directly after the region to catch reads/writes past its end.
 */
class MessageHistoryTest : public ::testing::Test
{
protected:
    static constexpr uint16_t kCap = MESSAGE_HISTORY_SECTOR_CAPACITY;
    // At least 8 sectors, and large enough to hold 24 messages (without wrapping)
    // for the get_range paging tests, whatever the sector capacity is
    static constexpr uint16_t kMinMessages = 24;
    static constexpr uint16_t kSectors =
        ((kMinMessages + kCap - 1) / kCap) > 8 ? ((kMinMessages + kCap - 1) / kCap) : 8;
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

    /** @brief Message sector index stored with message `id`. Differs from the timestamp to catch mixed-up fields. */
    static MessageIndex msg_ind_for(uint16_t id)
    {
        return static_cast<MessageIndex>(id * 3 + 7);
    }

    static MessageHistoryEntry make_entry(uint16_t id)
    {
        MessageHistoryEntry e;
        std::memset(&e, 0, sizeof(e));
        e.msgInd = msg_ind_for(id);
        msg_set_ts(e.message, id);
        e.message.direction = (id % 2) != 0;
        std::snprintf(e.message.str, sizeof(e.message.str), "message %u", id);
        return e;
    }

    void add(int n = 1)
    {
        for (int i = 0; i < n; i++)
        {
            MessageHistoryEntry e = make_entry(added);
            ASSERT_EQ(message_history_add(&rb, &journal, &storage, &e), STRG_OK) << "add #" << added;
            added++;
        }
    }

    /** @brief Expect out[0..count) to be the newest `count` messages, newest first. */
    void expect_newest_first(const std::vector<MessageHistoryEntry> &out, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            MessageHistoryEntry want = make_entry(static_cast<uint16_t>(added - 1 - i));
            EXPECT_EQ(out[i].msgInd, want.msgInd) << "out[" << i << "]";
            EXPECT_EQ(msg_ts(out[i].message), msg_ts(want.message)) << "out[" << i << "]";
            EXPECT_EQ(out[i].message.direction, want.message.direction) << "out[" << i << "]";
            EXPECT_STREQ(out[i].message.str, want.message.str) << "out[" << i << "]";
        }
    }

    STRG_RET get_list(size_t n, std::vector<MessageHistoryEntry> &out, size_t &count)
    {
        out.assign(n + 1, MessageHistoryEntry{});
        // Sentinel past the requested range to catch overruns
        msg_set_ts(out[n].message, 0xBEEF);
        count = 0xFFFF;
        // get_list is get_range from the newest message
        STRG_RET ret = message_history_get_range(&rb, &storage, 0, n, &count, out.data());
        EXPECT_EQ(msg_ts(out[n].message), 0xBEEF) << "get_list wrote past n";
        return ret;
    }

    STRG_RET get_range(size_t start, size_t n, std::vector<MessageHistoryEntry> &out, size_t &count)
    {
        out.assign(n + 1, MessageHistoryEntry{});
        // Sentinel past the requested range to catch overruns
        msg_set_ts(out[n].message, 0xBEEF);
        count = 0xFFFF;
        STRG_RET ret = message_history_get_range(&rb, &storage, start, n, &count, out.data());
        EXPECT_EQ(msg_ts(out[n].message), 0xBEEF) << "get_range wrote past n";
        return ret;
    }

    /** @brief Expect out[0..count) to be messages start..start+count from newest, newest first. */
    void expect_range(const std::vector<MessageHistoryEntry> &out, size_t start, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            MessageHistoryEntry want = make_entry(static_cast<uint16_t>(added - 1 - start - i));
            EXPECT_EQ(out[i].msgInd, want.msgInd) << "start=" << start << " out[" << i << "]";
            EXPECT_EQ(msg_ts(out[i].message), msg_ts(want.message)) << "start=" << start << " out[" << i << "]";
            EXPECT_EQ(out[i].message.direction, want.message.direction) << "start=" << start << " out[" << i << "]";
            EXPECT_STREQ(out[i].message.str, want.message.str) << "start=" << start << " out[" << i << "]";
        }
    }
};

/* ---- get_list on an empty buffer ---------------------------------------- */

TEST_F(MessageHistoryTest, GetListOnEmptyBufferReturnsZero)
{
    std::vector<MessageHistoryEntry> out;
    size_t count;
    EXPECT_EQ(get_list(5, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageHistoryTest, GetListWithZeroRequestedReturnsZero)
{
    add(3);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    EXPECT_EQ(get_list(0, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
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
    EXPECT_EQ(msg_ts(s.sector.data.mh_entry[0].message), 0);
    EXPECT_EQ(s.sector.data.mh_entry[0].msgInd, msg_ind_for(0));
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
        EXPECT_EQ(msg_ts(s.sector.data.mh_entry[i].message), i);
        EXPECT_EQ(s.sector.data.mh_entry[i].msgInd, msg_ind_for(i));
    }
}

TEST_F(MessageHistoryTest, AddStoresWholeEntry)
{
    // Fill a sector so the last slot (next to the padding) is checked too
    add(kCap);

    MessageHistorySectorB s = stored(0);
    for (uint16_t i = 0; i < kCap; i++)
    {
        MessageHistoryEntry want = make_entry(i);
        EXPECT_EQ(std::memcmp(&s.sector.data.mh_entry[i], &want, sizeof(MessageHistoryEntry)), 0) << "slot " << i;
    }
}

TEST_F(MessageHistoryTest, AddKeepsFullRangeMessageIndex)
{
    // msgInd is independent of the message contents, check the extremes survive
    const MessageIndex inds[] = {0, 1, 0x00FF, 0x0100, 0xFFFF};
    for (MessageIndex ind : inds)
    {
        MessageHistoryEntry e = make_entry(added);
        e.msgInd = ind;
        ASSERT_EQ(message_history_add(&rb, &journal, &storage, &e), STRG_OK);
        added++;
    }

    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(5, out, count), STRG_OK);
    ASSERT_EQ(count, 5u);
    for (size_t i = 0; i < count; i++)
    {
        EXPECT_EQ(out[i].msgInd, inds[4 - i]) << "out[" << i << "]";
        EXPECT_EQ(msg_ts(out[i].message), 4 - i) << "out[" << i << "]";
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
    EXPECT_EQ(msg_ts(s1.sector.data.mh_entry[0].message), kCap);
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
        EXPECT_EQ(msg_ts(s.sector.data.mh_entry[0].message), i * kCap) << "sector " << i;
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
    EXPECT_EQ(msg_ts(s0.sector.data.mh_entry[0].message), kSectors * kCap);
}

TEST_F(MessageHistoryTest, AddsAfterWrapKeepFillingReusedSector)
{
    // After reusing a FULL sector the next add must append, not reset head.
    // Fill the reused sector with up to 3 entries (but not past its capacity)
    static_assert(kCap >= 2, "need at least 2 entries per sector to test appending");
    constexpr uint16_t kExtra = kCap < 3 ? kCap : 3;
    add(kSectors * kCap + kExtra);

    MessageHistorySectorB s0 = stored(0);
    EXPECT_EQ(s0.sector.header.state, RB_OCCUPIED);
    EXPECT_EQ(s0.sector.header.head, kExtra - 1);
    for (uint16_t i = 0; i < kExtra; i++)
    {
        EXPECT_EQ(msg_ts(s0.sector.data.mh_entry[i].message), kSectors * kCap + i);
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
    MessageHistoryEntry e = make_entry(0);
    EXPECT_NE(message_history_add(&rb, &journal, &storage, &e), STRG_OK);

    // Sector not rewritten
    MessageHistorySectorB s = stored(0);
    EXPECT_EQ(s.sector.header.state, 0x7F);
}

TEST_F(MessageHistoryTest, AddPropagatesReadFailure)
{
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    MessageHistoryEntry e = make_entry(0);
    EXPECT_EQ(message_history_add(&rb, &journal, &storage, &e), STRG_FAIL);
}

/* ---- get_list ------------------------------------------------------------ */

TEST_F(MessageHistoryTest, GetListWithinOneSectorIsNewestFirst)
{
    add(3);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(3, out, count), STRG_OK);
    EXPECT_EQ(count, 3u);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListFewerThanStoredReturnsNewest)
{
    add(kCap + 2);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(4, out, count), STRG_OK);
    EXPECT_EQ(count, 4u);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListSpansSectors)
{
    add(2 * kCap + 1);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(2 * kCap + 1, out, count), STRG_OK);
    EXPECT_EQ(count, 2u * kCap + 1);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListMoreThanStoredReturnsStoredCountNoDuplicates)
{
    add(kCap + 1);
    std::vector<MessageHistoryEntry> out;
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

    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(1000, out, count), STRG_OK);
    EXPECT_EQ(count, stored_count);
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListOnFullUnwrappedBufferReturnsAll)
{
    add(kSectors * kCap);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(1000, out, count), STRG_OK);
    EXPECT_EQ(count, static_cast<size_t>(kSectors * kCap));
    expect_newest_first(out, count);
}

TEST_F(MessageHistoryTest, GetListDoesNotModifyStorage)
{
    add(kCap + 2);
    std::vector<uint8_t> before = mem;
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_list(100, out, count), STRG_OK);
    EXPECT_EQ(mem, before);
}

TEST_F(MessageHistoryTest, GetListPropagatesReadFailure)
{
    add(2);
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    std::vector<MessageHistoryEntry> out;
    size_t count;
    EXPECT_EQ(get_list(2, out, count), STRG_FAIL);
}

/* ---- get_range ----------------------------------------------------------- */

/**
 * @brief Number of messages still held on the card after `added` adds.
 *        Once wrapped, the current sector holds 1..kCap and the rest are full.
 */
static size_t stored_after(size_t added, size_t sectors, size_t cap)
{
    if (added <= sectors * cap)
    {
        return added;
    }
    size_t cur_cnt = ((added - 1) % cap) + 1;
    return cur_cnt + (sectors - 1) * cap;
}

/**
 * @brief get_range(0, 10) returns the 10 newest messages, newest first.
 */
TEST_F(MessageHistoryTest, GetRangeReturnsFirstTenMessages)
{
    add(10);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(0, 10, out, count), STRG_OK);
    EXPECT_EQ(count, 10u);
    expect_range(out, 0, count);
}

/**
 * @brief With 20 messages stored, get_range(10, 10) returns the second 10 -
 *        disjoint from the first 10, and together the two pages cover every
 *        added message in newest-first order.
 */
TEST_F(MessageHistoryTest, GetRangeReturnsSecondTenMessages)
{
    add(20);

    std::vector<MessageHistoryEntry> first_page;
    size_t first_count;
    ASSERT_EQ(get_range(0, 10, first_page, first_count), STRG_OK);
    ASSERT_EQ(first_count, 10u);

    std::vector<MessageHistoryEntry> second_page;
    size_t second_count;
    ASSERT_EQ(get_range(10, 10, second_page, second_count), STRG_OK);
    ASSERT_EQ(second_count, 10u);

    expect_range(first_page, 0, first_count);
    expect_range(second_page, 10, second_count);

    // The two pages together must be a partition of every added message:
    // no duplicates between them and nothing missing.
    std::vector<uint16_t> returned;
    for (size_t i = 0; i < first_count; i++)
    {
        returned.push_back(msg_ts(first_page[i].message));
    }
    for (size_t i = 0; i < second_count; i++)
    {
        returned.push_back(msg_ts(second_page[i].message));
    }
    std::vector<uint16_t> expected;
    for (int i = 19; i >= 0; i--)
    {
        expected.push_back(static_cast<uint16_t>(i));
    }
    EXPECT_EQ(returned, expected);
}

TEST_F(MessageHistoryTest, GetRangeOnEmptyBufferReturnsZero)
{
    std::vector<MessageHistoryEntry> out;
    size_t count;
    EXPECT_EQ(get_range(0, 5, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageHistoryTest, GetRangeWithZeroRequestedReturnsZero)
{
    add(5);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    EXPECT_EQ(get_range(2, 0, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageHistoryTest, GetRangeRejectsNullArguments)
{
    MessageHistoryEntry out[1];
    size_t count;
    EXPECT_EQ(message_history_get_range(nullptr, &storage, 0, 1, &count, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_range(&rb, nullptr, 0, 1, &count, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_range(&rb, &storage, 0, 1, nullptr, out), STRG_FAIL);
    EXPECT_EQ(message_history_get_range(&rb, &storage, 0, 1, &count, nullptr), STRG_FAIL);
}

TEST_F(MessageHistoryTest, GetRangeStartInsidePartialCurrentSector)
{
    // Current sector holds 2 messages, start at the older of the two
    add(kCap + 2);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(1, 3, out, count), STRG_OK);
    EXPECT_EQ(count, 3u);
    expect_range(out, 1, count);
}

TEST_F(MessageHistoryTest, GetRangeStartOnSectorBoundary)
{
    // start == messages in current sector: first (newest) message of previous sector
    add(kCap + 2);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(2, 2, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, 2, count);
}

TEST_F(MessageHistoryTest, GetRangeStartOnOldestSlotOfFullSector)
{
    // start lands on slot 0 of the sector behind the current one
    add(2 * kCap + 1);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(kCap, 2, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, kCap, count);
}

TEST_F(MessageHistoryTest, GetRangeWithFullCurrentSector)
{
    add(3 * kCap);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(kCap, kCap, out, count), STRG_OK);
    EXPECT_EQ(count, static_cast<size_t>(kCap));
    expect_range(out, kCap, count);
}

TEST_F(MessageHistoryTest, GetRangePastOldestIsTruncatedNoDuplicates)
{
    add(10);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(8, 10, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, 8, count);
}

TEST_F(MessageHistoryTest, GetRangeStartOnOldestMessageReturnsOne)
{
    add(10);
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(9, 5, out, count), STRG_OK);
    EXPECT_EQ(count, 1u);
    expect_range(out, 9, count);
}

TEST_F(MessageHistoryTest, GetRangeStartPastStoredFails)
{
    // Partial current sector and full current sector
    for (int n : {10, 9})
    {
        SetUp();
        added = 0;
        add(n);
        std::vector<MessageHistoryEntry> out;
        size_t count;
        EXPECT_EQ(get_range(n, 5, out, count), STRG_FAIL) << "n=" << n;
        EXPECT_EQ(get_range(n + 50, 5, out, count), STRG_FAIL) << "n=" << n;
    }
}

TEST_F(MessageHistoryTest, GetRangeOnWrappedBufferCrossesWrap)
{
    // current_index == 0 with 2 messages, older sectors are kSectors-1..1
    add(kSectors * kCap + 2);
    ASSERT_EQ(rb.current_index, 0);
    const size_t stored_count = stored_after(added, kSectors, kCap);

    std::vector<MessageHistoryEntry> out;
    size_t count;

    // Starts in sector 0 and crosses back into sector kSectors-1
    ASSERT_EQ(get_range(1, 4, out, count), STRG_OK);
    EXPECT_EQ(count, 4u);
    expect_range(out, 1, count);

    // Starts behind the wrap (negative index handling)
    ASSERT_EQ(get_range(2 + kCap + 1, 4, out, count), STRG_OK);
    EXPECT_EQ(count, 4u);
    expect_range(out, 2 + kCap + 1, count);

    // Reads to the oldest message and stops
    ASSERT_EQ(get_range(10, 1000, out, count), STRG_OK);
    EXPECT_EQ(count, stored_count - 10);
    expect_range(out, 10, count);
}

/**
 * @brief Sweep every valid start for a spread of fill levels (partial, full,
 *        wrapped) and check each page against the expected newest-first slice.
 */
TEST_F(MessageHistoryTest, GetRangeEveryStartMatchesExpectedSlice)
{
    const int counts[] = {1, kCap, kCap + 1, 20, kSectors * kCap, kSectors * kCap + 1,
                          kSectors * kCap + kCap, 2 * kSectors * kCap + 2};
    const size_t page = 4;
    for (int n : counts)
    {
        SetUp();
        added = 0;
        add(n);
        const size_t stored_count = stored_after(added, kSectors, kCap);

        for (size_t start = 0; start < stored_count; start++)
        {
            std::vector<MessageHistoryEntry> out;
            size_t count;
            ASSERT_EQ(get_range(start, page, out, count), STRG_OK) << "n=" << n << " start=" << start;
            EXPECT_EQ(count, std::min(page, stored_count - start)) << "n=" << n << " start=" << start;
            expect_range(out, start, count);
        }
    }
}

TEST_F(MessageHistoryTest, GetRangeDoesNotModifyStorage)
{
    add(20);
    std::vector<uint8_t> before = mem;
    std::vector<MessageHistoryEntry> out;
    size_t count;
    ASSERT_EQ(get_range(10, 10, out, count), STRG_OK);
    EXPECT_EQ(mem, before);
}

TEST_F(MessageHistoryTest, GetRangePropagatesReadFailure)
{
    add(10);
    std::vector<MessageHistoryEntry> out;
    size_t count;

    // Failure on the current-sector read
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    EXPECT_EQ(get_range(5, 5, out, count), STRG_FAIL);

    // Failure on a later sector read
    ctx.read_calls = 0;
    ctx.fail_after_read = 3;
    EXPECT_EQ(get_range(0, 10, out, count), STRG_FAIL);
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
        std::vector<MessageHistoryEntry> out;
        size_t count;
        ASSERT_EQ(get_list(5, out, count), STRG_OK) << "n=" << n;
        EXPECT_EQ(count, std::min<size_t>(5, stored_after(added, kSectors, kCap))) << "n=" << n;
        expect_newest_first(out, count);
    }
}
