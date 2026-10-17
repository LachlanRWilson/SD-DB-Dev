#include <gtest/gtest.h>
#include "test_support/message_ts.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C"
{
#include "message.h"
#include "journal.h"
#include "usage_bitmap.h"
#include "storage.h"
#include "mem_layout.h"
}

#include "test_support/failable_storage.h"

/**
 * @brief Behaviour of read_n_messages() (message.c), the message range read
 *        for a single chat.
 *
 * A chat is a linked list of message sectors, each holding
 * MESSAGE_SECTOR_CAPACITY messages. startIndex is the latest sector and
 * header.prev walks back to older sectors. Only the latest sector can be
 * partially full. Sectors are allocated at non-sequential indices so the
 * read has to follow the prev chain rather than assume index - 1.
 */
class MessageRangeTest : public ::testing::Test
{
protected:
    static constexpr uint32_t kTotalSectors = DATA_REGION_START_SECTOR + TOTAL_DATA_SECTOR_SIZE;
    static constexpr uint16_t kCap = MESSAGE_SECTOR_CAPACITY;
    static constexpr uint16_t kContactIndex = 0;

    FailableStorageCtx ctx{};
    Storage storage{};
    std::vector<uint8_t> mem;
    Journal journal{};

    // Latest message sector of the chat (UINT16_MAX = empty chat)
    uint16_t latest = UINT16_MAX;
    // Number of sectors in the chat so far
    uint16_t sectors = 0;
    // Number of messages added so far (also used as the message's timestamp)
    uint16_t added = 0;

    void SetUp() override
    {
        mem.assign(static_cast<size_t>(SECTOR_SIZE) * kTotalSectors, 0);
        ASSERT_TRUE(FailableStorage_Init(&ctx, mem.data(), SECTOR_SIZE, kTotalSectors));
        storage = FailableStorage_Make(&ctx);

        ASSERT_TRUE(init_usage_bitmap(&storage));

        std::memset(&journal, 0, sizeof(Journal));
        ASSERT_TRUE(journal_init(&journal, &storage));

        latest = UINT16_MAX;
        sectors = 0;
        added = 0;
    }

    /** @brief Message sector index of the i-th sector in the chat. Spread out to catch index - 1 assumptions. */
    static uint16_t sector_index(uint16_t i)
    {
        return static_cast<uint16_t>((i * 977u + 5u) % TOTAL_MESSAGE_SECTOR_SIZE);
    }

    static MessageBuffer make_message(uint16_t id)
    {
        MessageBuffer m;
        std::memset(&m, 0, sizeof(m));
        msg_set_ts(m.msg, id);
        m.msg.direction = (id % 2) != 0;
        std::snprintf(m.msg.str, sizeof(m.msg.str), "message %u", id);
        return m;
    }

    void add(int n = 1)
    {
        for (int i = 0; i < n; i++)
        {
            MessageBuffer m = make_message(added);
            if (latest == UINT16_MAX)
            {
                uint16_t next = sector_index(sectors);
                ASSERT_EQ(write_new_message_sector(&storage, &journal, kContactIndex, next, &m), STRG_OK) << "add #" << added;
                latest = next;
                sectors++;
            }
            else
            {
                STRG_RET ret = write_message(&storage, &journal, latest, &m);
                if (ret == STRG_FULL)
                {
                    uint16_t next = sector_index(sectors);
                    ret = write_next_message_sector(&storage, &journal, kContactIndex, latest, next, &m);
                    latest = next;
                    sectors++;
                }
                ASSERT_EQ(ret, STRG_OK) << "add #" << added;
            }
            added++;
        }
    }

    STRG_RET get_range(size_t start, size_t n, std::vector<Message> &out, size_t &count)
    {
        out.assign(n + 1, Message{});
        // Sentinel past the requested range to catch overruns
        msg_set_ts(out[n], 0xBEEF);
        count = 0xFFFF;
        STRG_RET ret = read_n_messages(&storage, latest, start, n, &count, out.data());
        EXPECT_EQ(msg_ts(out[n]), 0xBEEF) << "get_range wrote past n";
        return ret;
    }

    /** @brief Expect out[0..count) to be messages start..start+count from newest, newest first. */
    void expect_range(const std::vector<Message> &out, size_t start, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            MessageBuffer want = make_message(static_cast<uint16_t>(added - 1 - start - i));
            EXPECT_EQ(msg_ts(out[i]), msg_ts(want.msg)) << "start=" << start << " out[" << i << "]";
            EXPECT_EQ(out[i].direction, want.msg.direction) << "start=" << start << " out[" << i << "]";
            EXPECT_STREQ(out[i].str, want.msg.str) << "start=" << start << " out[" << i << "]";
        }
    }

    /** @brief Read-modify-write a message sector header (CRC is re-stamped on write). */
    template <typename F>
    void corrupt_header(uint16_t index, F edit)
    {
        MessageSectorBuffer s;
        ASSERT_EQ(read_message_sector(&storage, index, &s), STRG_OK);
        edit(s.var.header);
        ASSERT_EQ(write_message_sector(&storage, index, &s), STRG_OK);
    }
};

/* ---- get_range ----------------------------------------------------------- */

/**
 * @brief get_range(0, 10) returns the 10 newest messages, newest first.
 */
TEST_F(MessageRangeTest, GetRangeReturnsFirstTenMessages)
{
    add(10);
    std::vector<Message> out;
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
TEST_F(MessageRangeTest, GetRangeReturnsSecondTenMessages)
{
    add(20);

    std::vector<Message> first_page;
    size_t first_count;
    ASSERT_EQ(get_range(0, 10, first_page, first_count), STRG_OK);
    ASSERT_EQ(first_count, 10u);

    std::vector<Message> second_page;
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
        returned.push_back(msg_ts(first_page[i]));
    }
    for (size_t i = 0; i < second_count; i++)
    {
        returned.push_back(msg_ts(second_page[i]));
    }
    std::vector<uint16_t> expected;
    for (int i = 19; i >= 0; i--)
    {
        expected.push_back(static_cast<uint16_t>(i));
    }
    EXPECT_EQ(returned, expected);
}

TEST_F(MessageRangeTest, GetRangeOnEmptyChatReturnsZero)
{
    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_range(0, 5, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageRangeTest, GetRangeWithZeroRequestedReturnsZero)
{
    add(5);
    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_range(2, 0, out, count), STRG_OK);
    EXPECT_EQ(count, 0u);
}

TEST_F(MessageRangeTest, GetRangeRejectsNullArguments)
{
    add(1);
    Message out[1];
    size_t count;
    EXPECT_EQ(read_n_messages(nullptr, latest, 0, 1, &count, out), STRG_FAIL);
    EXPECT_EQ(read_n_messages(&storage, latest, 0, 1, nullptr, out), STRG_FAIL);
    EXPECT_EQ(read_n_messages(&storage, latest, 0, 1, &count, nullptr), STRG_FAIL);
}

TEST_F(MessageRangeTest, GetRangeStartInsideFullLatestSector)
{
    // Latest sector is full, start at its older message
    add(2 * kCap);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(1, 3, out, count), STRG_OK);
    EXPECT_EQ(count, 3u);
    expect_range(out, 1, count);
}

TEST_F(MessageRangeTest, GetRangeStartOnSectorBoundary)
{
    // start == messages in latest sector: newest message of the previous sector
    add(kCap + 1);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(1, 2, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, 1, count);
}

TEST_F(MessageRangeTest, GetRangeStartOnOldestSlotOfFullSector)
{
    // start lands on slot 0 of the sector behind the latest one
    add(2 * kCap + 1);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(kCap, 2, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, kCap, count);
}

TEST_F(MessageRangeTest, GetRangeWithFullLatestSector)
{
    add(3 * kCap);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(kCap, kCap, out, count), STRG_OK);
    EXPECT_EQ(count, static_cast<size_t>(kCap));
    expect_range(out, kCap, count);
}

TEST_F(MessageRangeTest, GetRangePastOldestIsTruncatedNoDuplicates)
{
    add(10);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(8, 10, out, count), STRG_OK);
    EXPECT_EQ(count, 2u);
    expect_range(out, 8, count);
}

TEST_F(MessageRangeTest, GetRangeStartOnOldestMessageReturnsOne)
{
    add(10);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(9, 5, out, count), STRG_OK);
    EXPECT_EQ(count, 1u);
    expect_range(out, 9, count);
}

TEST_F(MessageRangeTest, GetRangeStartPastStoredFails)
{
    // Full and partial latest sector
    for (int n : {10, 9})
    {
        SetUp();
        add(n);
        std::vector<Message> out;
        size_t count;
        EXPECT_EQ(get_range(n, 5, out, count), STRG_FAIL) << "n=" << n;
        EXPECT_EQ(get_range(n + 50, 5, out, count), STRG_FAIL) << "n=" << n;
    }
}

TEST_F(MessageRangeTest, GetRangeReadsToOldestAndStops)
{
    add(25);
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(10, 1000, out, count), STRG_OK);
    EXPECT_EQ(count, 15u);
    expect_range(out, 10, count);
}

/**
 * @brief Sweep every valid start for a spread of fill levels (single message,
 *        full/partial latest sector, many sectors) and check each page
 *        against the expected newest-first slice.
 */
TEST_F(MessageRangeTest, GetRangeEveryStartMatchesExpectedSlice)
{
    const int counts[] = {1, kCap, kCap + 1, 2 * kCap + 1, 20, 31};
    const size_t page = 4;
    for (int n : counts)
    {
        SetUp();
        add(n);

        for (size_t start = 0; start < static_cast<size_t>(n); start++)
        {
            std::vector<Message> out;
            size_t count;
            ASSERT_EQ(get_range(start, page, out, count), STRG_OK) << "n=" << n << " start=" << start;
            EXPECT_EQ(count, std::min(page, n - start)) << "n=" << n << " start=" << start;
            expect_range(out, start, count);
        }
    }
}

TEST_F(MessageRangeTest, GetRangeDoesNotModifyStorage)
{
    add(20);
    std::vector<uint8_t> before = mem;
    std::vector<Message> out;
    size_t count;
    ASSERT_EQ(get_range(10, 10, out, count), STRG_OK);
    EXPECT_EQ(mem, before);
}

TEST_F(MessageRangeTest, GetRangePropagatesReadFailure)
{
    add(10);
    std::vector<Message> out;
    size_t count;

    // Failure on the latest-sector read
    ctx.read_calls = 0;
    ctx.fail_after_read = 1;
    EXPECT_EQ(get_range(5, 5, out, count), STRG_FAIL);

    // Failure while walking back to the start sector
    ctx.read_calls = 0;
    ctx.fail_after_read = 2;
    EXPECT_EQ(get_range(5, 5, out, count), STRG_FAIL);

    // Failure on a later sector read
    ctx.read_calls = 0;
    ctx.fail_after_read = 3;
    EXPECT_EQ(get_range(0, 10, out, count), STRG_FAIL);
}

/* ---- corruption ---------------------------------------------------------- */

TEST_F(MessageRangeTest, GetRangeOnUnusedSectorReturnsEmpty)
{
    std::vector<Message> out(2);
    size_t count;
    EXPECT_EQ(read_n_messages(&storage, sector_index(0), 0, 1, &count, out.data()), STRG_EMPTY);
}

TEST_F(MessageRangeTest, GetRangeRejectsCorruptMessageCount)
{
    add(3 * kCap);
    // Sector behind the latest claims more messages than it can hold
    corrupt_header(sector_index(1), [](MessageSectorHeader &h) { h.msg_count = kCap + 5; });

    std::vector<Message> out;
    size_t count;
    EXPECT_EQ(get_range(0, 10, out, count), STRG_FAIL);
    EXPECT_EQ(get_range(kCap + 1, 1, out, count), STRG_FAIL);
}

TEST_F(MessageRangeTest, GetRangeStopsOnLoopingPrevChain)
{
    add(2 * kCap);
    // Oldest sector points back at the latest one
    corrupt_header(sector_index(0), [this](MessageSectorHeader &h) { h.prev = latest; });

    std::vector<Message> out;
    size_t count;
    // Walking to the start and reading onwards must both terminate. Both
    // ranges go past what TOTAL_MESSAGE_SECTOR_SIZE sectors could hold.
    const size_t past_all = static_cast<size_t>(TOTAL_MESSAGE_SECTOR_SIZE) * kCap + 1;
    EXPECT_EQ(get_range(past_all, 1, out, count), STRG_FAIL);
    EXPECT_EQ(get_range(0, past_all, out, count), STRG_FAIL);
}
