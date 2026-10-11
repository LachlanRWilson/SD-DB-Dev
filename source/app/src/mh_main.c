/*
 * mh_main.c - on-hardware unit tests for the message history ring buffer
 * (message_history.c).
 *
 * Firmware-side port of tests/test_message_history.cpp (the
 * MessageHistoryTest suite): every TEST_F there has a test_*() function here
 * with the same name and the same checks, run against the physical SD card
 * (sd_storage.c) instead of the heap-backed FailableStorage mock.
 *
 * The ring buffer is kept small (MH_SECTORS) so wrapping can be exercised,
 * at the start of the real message history region:
 *
 *   raw sector  MH_START ...  MH_START + MH_SECTORS - 1 | MH_GUARD
 *               ring buffer sectors                    | guard sector
 *
 * The guard sector directly after the region catches reads/writes past its
 * end.
 *
 * Differences from the gtest suite (all forced by the target, not the logic):
 *   - The fixture rewrites every region sector (and the guard) on the card
 *     instead of zeroing a heap buffer.
 *   - The read-failure tests wrap sd_storage in a Storage that fails the Nth
 *     read, the same as FailableStorage does on the host.
 *   - Output buffers are static and hold MH_OUT_MAX messages, so the
 *     "request more than stored" tests ask for MH_OUT_MAX - 1 instead of
 *     1000. That is still more than the region can hold (MH_SECTORS * cap).
 *   - "Does not modify storage" compares the region and guard sectors
 *     before and after, not the whole card.
 *   - A failed check ends that test (gtest's EXPECT_* would carry on).
 *
 * ------------------------------------------------------------------------
 *  Reporting
 * ------------------------------------------------------------------------
 *   The LCD shows each test as it runs and the summary at the end
 *   (htest_ui.c). A failure shows the mh_main.c line of the failed check.
 *   g_mh_report (below) has the same results and can be read over SWD
 *   without halting the core, e.g.
 *
 *     STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <&g_mh_report> 0x100
 *
 *   Build with -DMH_TEST=ON to run this instead of db_main.c.
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "message_history.h"
#include "ring_buffer.h"
#include "journal.h"
#include "storage.h"
#include "mem_layout.h"
#include "sd_storage.h"
#include "htest_ui.h"
#include "mh_main.h"

#define MH_TASK_STACK_SIZE (8 * 1024)
#define MH_TASK_PRIORITY   osPriorityNormal

/* Large enough to hold 20+ messages for the get_range paging tests. */
#define MH_SECTORS   8
#define MH_CAP       MESSAGE_HISTORY_SECTOR_CAPACITY
#define MH_START     (DATA_REGION_START_SECTOR + MESSAGE_HISTORY_DATA_START_SECTOR)
#define MH_GUARD     (MH_START + MH_SECTORS)
#define MH_GUARD_SEQ 0xDEADBEEFu

/* Output buffer size; more than the region can ever hold. */
#define MH_OUT_MAX   64
#define MH_MANY      (MH_OUT_MAX - 1)

#define MH_MAX_TESTS    48
#define MH_REPORT_MAGIC 0x4D487E57u

_Static_assert(MH_SECTORS * MH_CAP < MH_MANY, "MH_OUT_MAX too small for the region");

extern Storage sd_storage;

/* ------------------------------------------------------------------ *
 *  Report (read over SWD)
 * ------------------------------------------------------------------ */
typedef enum
{
    TEST_NOT_RUN = 0,
    TEST_PASS,
    TEST_FAIL
} TestStatus;

typedef struct
{
    uint32_t magic;                    /* MH_REPORT_MAGIC once the suite has started   */
    uint32_t done;                     /* 1 when every test has run                    */
    uint32_t total;                    /* tests in this build                          */
    uint32_t passed;
    uint32_t failed;
    uint32_t current;                  /* 1-based number of the test running now       */
    uint32_t first_failed;             /* 1-based number of the first failure, 0 none  */
    uint8_t  status[MH_MAX_TESTS];     /* TestStatus per test                          */
    uint16_t fail_line[MH_MAX_TESTS];  /* mh_main.c line of the failed check           */
    uint32_t ms[MH_MAX_TESTS];         /* run time per test                            */
} MhTestReport;

volatile MhTestReport g_mh_report;

/* ------------------------------------------------------------------ *
 *  Storage that can fail the Nth read (FailableStorage on the host)
 * ------------------------------------------------------------------ */
typedef struct
{
    Storage *inner;
    int fail_after_read; /* -1 = never, otherwise the 1-based read that fails */
    int read_calls;
} FailCtx;

static bool fail_read_block(void *context, uint32_t index, uint8_t *out)
{
    FailCtx *ctx = (FailCtx *)context;
    ctx->read_calls++;
    if (ctx->fail_after_read == ctx->read_calls)
    {
        return false;
    }
    return ctx->inner->read_block(ctx->inner->context, index, out);
}

static bool fail_read_multiblock(void *context, uint32_t index, size_t n, uint8_t *out)
{
    FailCtx *ctx = (FailCtx *)context;
    ctx->read_calls++;
    if (ctx->fail_after_read == ctx->read_calls)
    {
        return false;
    }
    return ctx->inner->read_multiblock(ctx->inner->context, index, n, out);
}

static bool fail_write_block(void *context, uint32_t index, uint8_t *in)
{
    FailCtx *ctx = (FailCtx *)context;
    return ctx->inner->write_block(ctx->inner->context, index, in);
}

static bool fail_write_multiblock(void *context, uint32_t index, size_t n, uint8_t *in)
{
    FailCtx *ctx = (FailCtx *)context;
    return ctx->inner->write_multiblock(ctx->inner->context, index, n, in);
}

static uint32_t fail_capacity(void *context)
{
    FailCtx *ctx = (FailCtx *)context;
    return ctx->inner->capacity(ctx->inner->context);
}

static FailCtx g_fail_ctx = { .inner = &sd_storage, .fail_after_read = -1, .read_calls = 0 };

static Storage g_storage = {
    .context = &g_fail_ctx,
    .read_block = fail_read_block,
    .read_multiblock = fail_read_multiblock,
    .write_block = fail_write_block,
    .write_multiblock = fail_write_multiblock,
    .capacity = fail_capacity,
};

static Storage *const storage = &g_storage;

static SDStorageContext g_sd_ctx;
static Journal          g_journal;
static RingBuffer       g_rb;

/* Number of messages added so far (also used as the message's timestamp). */
static uint16_t g_added;

/* get_range() output; one extra slot for the overrun sentinel. */
__attribute__((section(".ram_d1")))
static Message g_out[MH_OUT_MAX + 1];
__attribute__((section(".ram_d1")))
static Message g_out2[MH_OUT_MAX + 1];

/* Region + guard as raw bytes, for the "does not modify storage" tests. */
__attribute__((section(".ram_d1")))
static uint8_t g_snapshot[MH_SECTORS + 1][SECTOR_SIZE];

/* ------------------------------------------------------------------ *
 *  Minimal test harness
 * ------------------------------------------------------------------ */
static int g_fail_line;

/* Fail the running test and return from it. */
#define CHECK(cond)                       \
    do                                    \
    {                                     \
        if (!(cond))                      \
        {                                 \
            if (g_fail_line == 0)         \
            {                             \
                g_fail_line = __LINE__;   \
            }                             \
            return;                       \
        }                                 \
    } while (0)

/* Record a failure at `line` but keep going (for helpers that return a value). */
static void fail_at(int line)
{
    if (g_fail_line == 0)
    {
        g_fail_line = line;
    }
}

typedef struct
{
    const char *name;
    void (*fn)(void);
} TestCase;

/* ------------------------------------------------------------------ *
 *  Fixture (mirrors MessageHistoryTest)
 * ------------------------------------------------------------------ */
static bool storage_bringup(void)
{
    if (!SDStorage_Init(&g_sd_ctx, 0, SECTOR_SIZE))
    {
        return false;
    }
    sd_storage.context = (void *)&g_sd_ctx;
    return true;
}

static bool write_header(uint16_t raw, uint32_t seq, RB_SECTOR_STATE state, uint8_t head)
{
    MessageHistorySectorB s;
    memset(&s, 0, sizeof(s));
    s.sector.header.seq = seq;
    s.sector.header.state = state;
    s.sector.header.head = head;
    return write_sector(storage, raw, s.buffer) == STRG_OK;
}

/* MessageHistoryTest::SetUp(). journal_init() runs first: it may roll back
 * an entry a previous test left active, which can rewrite a region sector. */
static bool fixture_setup(void)
{
    g_fail_ctx.fail_after_read = -1;
    g_fail_ctx.read_calls = 0;
    g_added = 0;

    memset(&g_journal, 0, sizeof(g_journal));
    if (!journal_init(&g_journal, storage)) { return false; }

    /* Format: every region sector empty with a valid CRC */
    for (uint16_t i = 0; i < MH_SECTORS; i++)
    {
        if (!write_header(MH_START + i, 0, RB_EMPTY, 0)) { return false; }
    }
    if (!write_header(MH_GUARD, MH_GUARD_SEQ, RB_OCCUPIED, MH_CAP - 1)) { return false; }

    memset(&g_rb, 0, sizeof(g_rb));
    return init_ring_buffer(storage, &g_rb, MH_SECTORS, MH_START);
}

/* Sector i of the region as stored on the card. */
static bool stored(uint16_t i, MessageHistorySectorB *s)
{
    return read_sector(storage, MH_START + i, s->buffer) == STRG_OK;
}

static Message make_message(uint16_t id)
{
    Message m;
    memset(&m, 0, sizeof(m));
    m.timestamp = id;
    m.direction = (id % 2) != 0;
    snprintf(m.str, sizeof(m.str), "message %u", id);
    return m;
}

static bool add(int n)
{
    for (int i = 0; i < n; i++)
    {
        Message m = make_message(g_added);
        if (message_history_add(&g_rb, &g_journal, storage, &m) != STRG_OK)
        {
            return false;
        }
        g_added++;
    }
    return true;
}

/* get_range into `out` (MH_OUT_MAX + 1 slots), with a sentinel past the
 * requested range to catch overruns. An overrun fails the test at `line`. */
static STRG_RET range_into(Message *out, size_t start, size_t n, size_t *count, int line)
{
    memset(out, 0, sizeof(Message) * (n + 1));
    out[n].timestamp = 0xBEEF;
    *count = 0xFFFF;
    STRG_RET ret = message_history_get_range(&g_rb, storage, start, n, count, out);
    if (out[n].timestamp != 0xBEEF)
    {
        fail_at(line);
    }
    return ret;
}

#define get_range(start, n, count)  range_into(g_out, (start), (n), (count), __LINE__)
/* get_list is get_range from the newest message */
#define get_list(n, count)          range_into(g_out, 0, (n), (count), __LINE__)

/* out[0..count) are messages start..start+count from newest, newest first. */
static bool range_is(const Message *out, size_t start, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        Message want = make_message((uint16_t)(g_added - 1 - start - i));
        if (out[i].timestamp != want.timestamp || strcmp(out[i].str, want.str) != 0)
        {
            return false;
        }
    }
    return true;
}

#define newest_first(out, count) range_is((out), 0, (count))

/*
 * Number of messages still held on the card after `added` adds.
 * Once wrapped, the current sector holds 1..cap and the rest are full.
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

static bool snapshot_region(void)
{
    for (uint16_t i = 0; i <= MH_SECTORS; i++)
    {
        if (read_sector_raw(storage, MH_START + i, g_snapshot[i]) != STRG_OK) { return false; }
    }
    return true;
}

static bool region_unchanged(void)
{
    uint8_t now[SECTOR_SIZE];
    for (uint16_t i = 0; i <= MH_SECTORS; i++)
    {
        if (read_sector_raw(storage, MH_START + i, now) != STRG_OK) { return false; }
        if (memcmp(now, g_snapshot[i], SECTOR_SIZE) != 0)          { return false; }
    }
    return true;
}

/* ================================================================== *
 *  get_list on an empty buffer
 * ================================================================== */
static void test_GetListOnEmptyBufferReturnsZero(void)
{
    size_t count;
    CHECK(get_list(5, &count) == STRG_OK);
    CHECK(count == 0);
}

static void test_GetListWithZeroRequestedReturnsZero(void)
{
    size_t count;
    CHECK(add(3));
    CHECK(get_list(0, &count) == STRG_OK);
    CHECK(count == 0);
}

/* ================================================================== *
 *  add: first sector
 * ================================================================== */
static void test_FirstAddClaimsSectorZero(void)
{
    MessageHistorySectorB s;
    CHECK(add(1));

    CHECK(g_rb.current_index == 0);
    CHECK(g_rb.occupancy == 1);
    CHECK(g_rb.seq == 0);

    CHECK(stored(0, &s));
    CHECK(s.sector.header.state == RB_OCCUPIED);
    CHECK(s.sector.header.head == 0);
    CHECK(s.sector.header.seq == 0);
    CHECK(s.sector.data.mh_entry[0].timestamp == 0);
}

static void test_AddsWithinSectorAdvanceHead(void)
{
    MessageHistorySectorB s;
    CHECK(add(MH_CAP));

    CHECK(g_rb.current_index == 0);
    CHECK(g_rb.occupancy == 1);

    CHECK(stored(0, &s));
    CHECK(s.sector.header.state == RB_OCCUPIED);
    CHECK(s.sector.header.head == MH_CAP - 1);
    for (uint16_t i = 0; i < MH_CAP; i++)
    {
        CHECK(s.sector.data.mh_entry[i].timestamp == i);
    }
}

/* ================================================================== *
 *  add: moving between sectors
 * ================================================================== */
static void test_AddPastFullSectorMovesToNextSector(void)
{
    MessageHistorySectorB s0;
    MessageHistorySectorB s1;
    CHECK(add(MH_CAP + 1));

    CHECK(g_rb.current_index == 1);
    CHECK(g_rb.occupancy == 2);
    CHECK(g_rb.seq == 1);

    CHECK(stored(0, &s0));
    CHECK(s0.sector.header.state == RB_FULL);
    CHECK(s0.sector.header.head == MH_CAP - 1);

    CHECK(stored(1, &s1));
    CHECK(s1.sector.header.state == RB_OCCUPIED);
    CHECK(s1.sector.header.head == 0);
    CHECK(s1.sector.header.seq == 1);
    CHECK(s1.sector.data.mh_entry[0].timestamp == MH_CAP);
}

static void test_SectorsFillSequentiallyWithIncreasingSeq(void)
{
    MessageHistorySectorB s;
    CHECK(add(MH_SECTORS * MH_CAP));

    CHECK(g_rb.current_index == MH_SECTORS - 1);
    CHECK(g_rb.occupancy == MH_SECTORS);
    CHECK(g_rb.seq == MH_SECTORS - 1);

    for (uint16_t i = 0; i < MH_SECTORS; i++)
    {
        CHECK(stored(i, &s));
        CHECK(s.sector.header.seq == i);
        CHECK(s.sector.header.state == (i == MH_SECTORS - 1 ? RB_OCCUPIED : RB_FULL));
        CHECK(s.sector.data.mh_entry[0].timestamp == i * MH_CAP);
    }
}

/* ================================================================== *
 *  add: wrapping
 * ================================================================== */
static void test_WrapOverwritesOldestSector(void)
{
    MessageHistorySectorB s0;
    CHECK(add(MH_SECTORS * MH_CAP + 1));

    CHECK(g_rb.current_index == 0);
    CHECK(g_rb.occupancy == MH_SECTORS);
    CHECK(g_rb.seq == MH_SECTORS);

    CHECK(stored(0, &s0));
    CHECK(s0.sector.header.state == RB_OCCUPIED);
    CHECK(s0.sector.header.head == 0);
    CHECK(s0.sector.header.seq == MH_SECTORS);
    CHECK(s0.sector.data.mh_entry[0].timestamp == MH_SECTORS * MH_CAP);
}

static void test_AddsAfterWrapKeepFillingReusedSector(void)
{
    MessageHistorySectorB s0;

    /* After reusing a FULL sector the next add must append, not reset head */
    CHECK(add(MH_SECTORS * MH_CAP + 3));

    CHECK(stored(0, &s0));
    CHECK(s0.sector.header.state == RB_OCCUPIED);
    CHECK(s0.sector.header.head == 2);
    for (uint16_t i = 0; i < 3; i++)
    {
        CHECK(s0.sector.data.mh_entry[i].timestamp == MH_SECTORS * MH_CAP + i);
    }
}

static void test_ManyWrapsStayInsideRegion(void)
{
    MessageHistorySectorB g;
    CHECK(add(3 * MH_SECTORS * MH_CAP + 2));

    CHECK(g_rb.current_index < MH_SECTORS);
    CHECK(g_rb.occupancy == MH_SECTORS);

    /* Guard sector untouched */
    CHECK(read_sector(storage, MH_GUARD, g.buffer) == STRG_OK);
    CHECK(g.sector.header.seq == MH_GUARD_SEQ);
}

/* ================================================================== *
 *  add: error handling
 * ================================================================== */
static void test_AddRejectsCorruptSectorState(void)
{
    MessageHistorySectorB s;
    Message m = make_message(0);

    CHECK(write_header(MH_START, 0, 0x7F, 200));
    CHECK(message_history_add(&g_rb, &g_journal, storage, &m) != STRG_OK);

    /* Sector not rewritten */
    CHECK(stored(0, &s));
    CHECK(s.sector.header.state == 0x7F);
}

static void test_AddPropagatesReadFailure(void)
{
    Message m = make_message(0);

    g_fail_ctx.read_calls = 0;
    g_fail_ctx.fail_after_read = 1;
    STRG_RET ret = message_history_add(&g_rb, &g_journal, storage, &m);
    g_fail_ctx.fail_after_read = -1;
    CHECK(ret == STRG_FAIL);
}

/* ================================================================== *
 *  get_list
 * ================================================================== */
static void test_GetListWithinOneSectorIsNewestFirst(void)
{
    size_t count;
    CHECK(add(3));
    CHECK(get_list(3, &count) == STRG_OK);
    CHECK(count == 3);
    CHECK(newest_first(g_out, count));
}

static void test_GetListFewerThanStoredReturnsNewest(void)
{
    size_t count;
    CHECK(add(MH_CAP + 2));
    CHECK(get_list(4, &count) == STRG_OK);
    CHECK(count == 4);
    CHECK(newest_first(g_out, count));
}

static void test_GetListSpansSectors(void)
{
    size_t count;
    CHECK(add(2 * MH_CAP + 1));
    CHECK(get_list(2 * MH_CAP + 1, &count) == STRG_OK);
    CHECK(count == 2 * MH_CAP + 1);
    CHECK(newest_first(g_out, count));
}

static void test_GetListMoreThanStoredReturnsStoredCountNoDuplicates(void)
{
    size_t count;
    CHECK(add(MH_CAP + 1));
    CHECK(get_list(MH_MANY, &count) == STRG_OK);
    CHECK(count == MH_CAP + 1);
    CHECK(newest_first(g_out, count));
}

static void test_GetListOnWrappedBufferReturnsNewestAndStops(void)
{
    /* Wrapped once with 2 messages in the reused sector 0:
     * sector 0 holds 2 newest, sectors 1..MH_SECTORS-1 are full */
    const size_t stored_count = 2 + (MH_SECTORS - 1) * MH_CAP;
    size_t count;

    CHECK(add(MH_SECTORS * MH_CAP + 2));
    CHECK(get_list(MH_MANY, &count) == STRG_OK);
    CHECK(count == stored_count);
    CHECK(newest_first(g_out, count));
}

static void test_GetListOnFullUnwrappedBufferReturnsAll(void)
{
    size_t count;
    CHECK(add(MH_SECTORS * MH_CAP));
    CHECK(get_list(MH_MANY, &count) == STRG_OK);
    CHECK(count == MH_SECTORS * MH_CAP);
    CHECK(newest_first(g_out, count));
}

static void test_GetListDoesNotModifyStorage(void)
{
    size_t count;
    CHECK(add(MH_CAP + 2));
    CHECK(snapshot_region());
    CHECK(get_list(MH_MANY, &count) == STRG_OK);
    CHECK(region_unchanged());
}

static void test_GetListPropagatesReadFailure(void)
{
    size_t count;
    CHECK(add(2));

    g_fail_ctx.read_calls = 0;
    g_fail_ctx.fail_after_read = 1;
    STRG_RET ret = get_list(2, &count);
    g_fail_ctx.fail_after_read = -1;
    CHECK(ret == STRG_FAIL);
}

/* ================================================================== *
 *  get_range
 * ================================================================== */

/* get_range(0, 10) returns the 10 newest messages, newest first. */
static void test_GetRangeReturnsFirstTenMessages(void)
{
    size_t count;
    CHECK(add(10));
    CHECK(get_range(0, 10, &count) == STRG_OK);
    CHECK(count == 10);
    CHECK(range_is(g_out, 0, count));
}

/* With 20 messages stored, get_range(10, 10) returns the second 10. Together
 * the two pages cover every added message once, newest first. */
static void test_GetRangeReturnsSecondTenMessages(void)
{
    size_t first_count;
    size_t second_count;

    CHECK(add(20));

    CHECK(range_into(g_out, 0, 10, &first_count, __LINE__) == STRG_OK);
    CHECK(first_count == 10);
    CHECK(range_into(g_out2, 10, 10, &second_count, __LINE__) == STRG_OK);
    CHECK(second_count == 10);

    CHECK(range_is(g_out, 0, first_count));
    CHECK(range_is(g_out2, 10, second_count));

    /* The two pages are a partition of every added message:
     * no duplicates between them and nothing missing. */
    for (size_t i = 0; i < 20; i++)
    {
        const Message *m = (i < 10) ? &g_out[i] : &g_out2[i - 10];
        CHECK(m->timestamp == 19 - i);
    }
}

static void test_GetRangeOnEmptyBufferReturnsZero(void)
{
    size_t count;
    CHECK(get_range(0, 5, &count) == STRG_OK);
    CHECK(count == 0);
}

static void test_GetRangeWithZeroRequestedReturnsZero(void)
{
    size_t count;
    CHECK(add(5));
    CHECK(get_range(2, 0, &count) == STRG_OK);
    CHECK(count == 0);
}

static void test_GetRangeRejectsNullArguments(void)
{
    size_t count;
    CHECK(message_history_get_range(NULL, storage, 0, 1, &count, g_out) == STRG_FAIL);
    CHECK(message_history_get_range(&g_rb, NULL, 0, 1, &count, g_out) == STRG_FAIL);
    CHECK(message_history_get_range(&g_rb, storage, 0, 1, NULL, g_out) == STRG_FAIL);
    CHECK(message_history_get_range(&g_rb, storage, 0, 1, &count, NULL) == STRG_FAIL);
}

static void test_GetRangeStartInsidePartialCurrentSector(void)
{
    /* Current sector holds 2 messages, start at the older of the two */
    size_t count;
    CHECK(add(MH_CAP + 2));
    CHECK(get_range(1, 3, &count) == STRG_OK);
    CHECK(count == 3);
    CHECK(range_is(g_out, 1, count));
}

static void test_GetRangeStartOnSectorBoundary(void)
{
    /* start == messages in current sector: newest message of the previous sector */
    size_t count;
    CHECK(add(MH_CAP + 2));
    CHECK(get_range(2, 2, &count) == STRG_OK);
    CHECK(count == 2);
    CHECK(range_is(g_out, 2, count));
}

static void test_GetRangeStartOnOldestSlotOfFullSector(void)
{
    /* start lands on slot 0 of the sector behind the current one */
    size_t count;
    CHECK(add(2 * MH_CAP + 1));
    CHECK(get_range(MH_CAP, 2, &count) == STRG_OK);
    CHECK(count == 2);
    CHECK(range_is(g_out, MH_CAP, count));
}

static void test_GetRangeWithFullCurrentSector(void)
{
    size_t count;
    CHECK(add(3 * MH_CAP));
    CHECK(get_range(MH_CAP, MH_CAP, &count) == STRG_OK);
    CHECK(count == MH_CAP);
    CHECK(range_is(g_out, MH_CAP, count));
}

static void test_GetRangePastOldestIsTruncatedNoDuplicates(void)
{
    size_t count;
    CHECK(add(10));
    CHECK(get_range(8, 10, &count) == STRG_OK);
    CHECK(count == 2);
    CHECK(range_is(g_out, 8, count));
}

static void test_GetRangeStartOnOldestMessageReturnsOne(void)
{
    size_t count;
    CHECK(add(10));
    CHECK(get_range(9, 5, &count) == STRG_OK);
    CHECK(count == 1);
    CHECK(range_is(g_out, 9, count));
}

static void test_GetRangeStartPastStoredFails(void)
{
    /* Partial current sector and full current sector */
    const int counts[] = { 10, 9 };
    size_t count;

    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++)
    {
        int n = counts[k];
        CHECK(fixture_setup());
        CHECK(add(n));
        CHECK(get_range((size_t)n, 5, &count) == STRG_FAIL);
        CHECK(get_range((size_t)n + 50, 5, &count) == STRG_FAIL);
    }
}

static void test_GetRangeOnWrappedBufferCrossesWrap(void)
{
    size_t count;

    /* current_index == 0 with 2 messages, older sectors are MH_SECTORS-1..1 */
    CHECK(add(MH_SECTORS * MH_CAP + 2));
    CHECK(g_rb.current_index == 0);
    const size_t stored_count = stored_after(g_added, MH_SECTORS, MH_CAP);

    /* Starts in sector 0 and crosses back into sector MH_SECTORS-1 */
    CHECK(get_range(1, 4, &count) == STRG_OK);
    CHECK(count == 4);
    CHECK(range_is(g_out, 1, count));

    /* Starts behind the wrap (negative index handling) */
    CHECK(get_range(2 + MH_CAP + 1, 4, &count) == STRG_OK);
    CHECK(count == 4);
    CHECK(range_is(g_out, 2 + MH_CAP + 1, count));

    /* Reads to the oldest message and stops */
    CHECK(get_range(10, MH_MANY, &count) == STRG_OK);
    CHECK(count == stored_count - 10);
    CHECK(range_is(g_out, 10, count));
}

/* Sweep every valid start for a spread of fill levels (partial, full,
 * wrapped) and check each page against the expected newest-first slice. */
static void test_GetRangeEveryStartMatchesExpectedSlice(void)
{
    const int counts[] = { 1, MH_CAP, MH_CAP + 1, 20, MH_SECTORS * MH_CAP, MH_SECTORS * MH_CAP + 1,
                           MH_SECTORS * MH_CAP + MH_CAP, 2 * MH_SECTORS * MH_CAP + 2 };
    const size_t page = 4;
    size_t count;

    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++)
    {
        CHECK(fixture_setup());
        CHECK(add(counts[k]));
        const size_t stored_count = stored_after(g_added, MH_SECTORS, MH_CAP);

        for (size_t start = 0; start < stored_count; start++)
        {
            size_t want = (stored_count - start < page) ? stored_count - start : page;
            CHECK(get_range(start, page, &count) == STRG_OK);
            CHECK(count == want);
            CHECK(range_is(g_out, start, count));
        }
    }
}

static void test_GetRangeDoesNotModifyStorage(void)
{
    size_t count;
    CHECK(add(20));
    CHECK(snapshot_region());
    CHECK(get_range(10, 10, &count) == STRG_OK);
    CHECK(region_unchanged());
}

static void test_GetRangePropagatesReadFailure(void)
{
    size_t count;
    STRG_RET ret;
    CHECK(add(10));

    /* Failure on the current-sector read */
    g_fail_ctx.read_calls = 0;
    g_fail_ctx.fail_after_read = 1;
    ret = get_range(5, 5, &count);
    g_fail_ctx.fail_after_read = -1;
    CHECK(ret == STRG_FAIL);

    /* Failure on a later sector read */
    g_fail_ctx.read_calls = 0;
    g_fail_ctx.fail_after_read = 3;
    ret = get_range(0, 10, &count);
    g_fail_ctx.fail_after_read = -1;
    CHECK(ret == STRG_FAIL);
}

/* ================================================================== *
 *  Persistence
 * ================================================================== */

/* Reconstructing the ring buffer from the card after a series of adds gives
 * the same RAM state, and adding/listing continues seamlessly. */
static void test_ReconstructAfterAddsMatchesRamState(void)
{
    const int counts[] = { 1, MH_CAP, MH_CAP + 1, MH_SECTORS * MH_CAP, MH_SECTORS * MH_CAP + 1,
                           2 * MH_SECTORS * MH_CAP + 3 };
    size_t count;

    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++)
    {
        CHECK(fixture_setup());
        CHECK(add(counts[k]));
        RingBuffer before = g_rb;

        RingBuffer after;
        memset(&after, 0, sizeof(after));
        CHECK(init_ring_buffer(storage, &after, MH_SECTORS, MH_START));
        CHECK(after.current_index == before.current_index);
        CHECK(after.occupancy == before.occupancy);
        CHECK(after.seq == before.seq);

        /* Continue on the reconstructed state */
        g_rb = after;
        CHECK(add(MH_CAP + 1));
        CHECK(get_list(5, &count) == STRG_OK);
        CHECK(count == 5);
        CHECK(newest_first(g_out, count));
    }
}

/* ================================================================== *
 *  Test table
 * ================================================================== */
#define TEST(fn) { #fn, test_##fn }

static const TestCase g_tests[] = {
    TEST(GetListOnEmptyBufferReturnsZero),
    TEST(GetListWithZeroRequestedReturnsZero),
    TEST(FirstAddClaimsSectorZero),
    TEST(AddsWithinSectorAdvanceHead),
    TEST(AddPastFullSectorMovesToNextSector),
    TEST(SectorsFillSequentiallyWithIncreasingSeq),
    TEST(WrapOverwritesOldestSector),
    TEST(AddsAfterWrapKeepFillingReusedSector),
    TEST(ManyWrapsStayInsideRegion),
    TEST(AddRejectsCorruptSectorState),
    TEST(AddPropagatesReadFailure),
    TEST(GetListWithinOneSectorIsNewestFirst),
    TEST(GetListFewerThanStoredReturnsNewest),
    TEST(GetListSpansSectors),
    TEST(GetListMoreThanStoredReturnsStoredCountNoDuplicates),
    TEST(GetListOnWrappedBufferReturnsNewestAndStops),
    TEST(GetListOnFullUnwrappedBufferReturnsAll),
    TEST(GetListDoesNotModifyStorage),
    TEST(GetListPropagatesReadFailure),
    TEST(GetRangeReturnsFirstTenMessages),
    TEST(GetRangeReturnsSecondTenMessages),
    TEST(GetRangeOnEmptyBufferReturnsZero),
    TEST(GetRangeWithZeroRequestedReturnsZero),
    TEST(GetRangeRejectsNullArguments),
    TEST(GetRangeStartInsidePartialCurrentSector),
    TEST(GetRangeStartOnSectorBoundary),
    TEST(GetRangeStartOnOldestSlotOfFullSector),
    TEST(GetRangeWithFullCurrentSector),
    TEST(GetRangePastOldestIsTruncatedNoDuplicates),
    TEST(GetRangeStartOnOldestMessageReturnsOne),
    TEST(GetRangeStartPastStoredFails),
    TEST(GetRangeOnWrappedBufferCrossesWrap),
    TEST(GetRangeEveryStartMatchesExpectedSlice),
    TEST(GetRangeDoesNotModifyStorage),
    TEST(GetRangePropagatesReadFailure),
    TEST(ReconstructAfterAddsMatchesRamState),
};

#define N_TESTS (sizeof(g_tests) / sizeof(g_tests[0]))

_Static_assert(N_TESTS <= MH_MAX_TESTS, "raise MH_MAX_TESTS");

/* ------------------------------------------------------------------ *
 *  Runner
 * ------------------------------------------------------------------ */
static void run_test(size_t i)
{
    uint32_t start = osKernelGetTickCount();

    g_fail_line = 0;
    g_mh_report.current = (uint32_t)(i + 1);
    htest_ui_start((uint32_t)i, g_tests[i].name);

    if (!fixture_setup())
    {
        g_fail_line = __LINE__;
    }
    else
    {
        g_tests[i].fn();
    }

    g_mh_report.ms[i] = osKernelGetTickCount() - start;
    g_mh_report.fail_line[i] = (uint16_t)g_fail_line;
    htest_ui_result((uint32_t)i, g_fail_line == 0, (uint32_t)g_fail_line);

    if (g_fail_line == 0)
    {
        g_mh_report.status[i] = TEST_PASS;
        g_mh_report.passed++;
    }
    else
    {
        g_mh_report.status[i] = TEST_FAIL;
        g_mh_report.failed++;
        if (g_mh_report.first_failed == 0)
        {
            g_mh_report.first_failed = (uint32_t)(i + 1);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Task
 * ------------------------------------------------------------------ */
static void mhTask(void *arg)
{
    (void)arg;

    memset((void *)&g_mh_report, 0, sizeof(g_mh_report));
    g_mh_report.total = (uint32_t)N_TESTS;
    g_mh_report.magic = MH_REPORT_MAGIC;

    htest_ui_init("MSG HISTORY", (uint32_t)N_TESTS);

    if (!storage_bringup())
    {
        /* Nothing can run without the card: report it as test 1 failing. */
        g_mh_report.first_failed = 1;
        g_mh_report.failed = 1;
        g_mh_report.status[0] = TEST_FAIL;
        g_mh_report.fail_line[0] = (uint16_t)__LINE__;
        htest_ui_start(0, "SD card init");
        htest_ui_result(0, false, g_mh_report.fail_line[0]);
    }
    else
    {
        for (size_t i = 0; i < N_TESTS; i++)
        {
            run_test(i);
        }
    }

    g_mh_report.current = 0;
    g_mh_report.done = 1;
    htest_ui_done(g_mh_report.passed, g_mh_report.failed);

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_mh_task_cb;
static uint8_t g_mh_task_stack[MH_TASK_STACK_SIZE] __attribute__((aligned(8)));

void MH_Test_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "MH Test Task",
        .cb_mem = &g_mh_task_cb,
        .cb_size = sizeof(g_mh_task_cb),
        .stack_mem = g_mh_task_stack,
        .stack_size = sizeof(g_mh_task_stack),
        .priority = MH_TASK_PRIORITY};

    osThreadNew(mhTask, NULL, &task_attr);
}
