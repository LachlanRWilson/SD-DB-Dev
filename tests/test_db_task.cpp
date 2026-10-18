#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

extern "C"
{
#include "db_task.h"
#include "platform_os.h"
#include "superheader.h"
#include "usage_bitmap.h"
#include "mem_layout.h"

// database.c backing storage for the host build (not declared in a header)
extern uint8_t heap_mem[];
}

// Raw sectors spanned by the database (matches DATABASE_RAW_SECTOR_SIZE in database.c)
static constexpr size_t DB_RAW_SECTORS = DATA_REGION_START_SECTOR + TOTAL_DATABASE_SECTOR_SIZE;

// Max wait for a response before the test fails (the task should reply in well under this)
static constexpr uint32_t REPLY_TIMEOUT_MS = 2000;

/**
 * @brief Fixture for the database task (db_task.c): requests go through the
 * request queue to the task thread, responses come back on reply_q.
 *
 * Every test starts from a blank card and the task mounting it itself
 * (db_init(NULL) formats the blank card). TearDown stops the task, so the next
 * test can start it again.
 */
class DbTaskTest : public ::testing::Test
{
protected:
    static PlatformQueue *reply_q;  // shared: platform_os has no queue delete
    DatabaseTaskContext *ctx = nullptr;
    uint32_t next_tag = 1;

    static void SetUpTestSuite()
    {
        reply_q = platform_os_QueueCreate(4, sizeof(DbResponse));
        ASSERT_NE(reply_q, nullptr);
    }

    static void wipe_card()
    {
        std::memset(heap_mem, 0, DB_RAW_SECTORS * SECTOR_SIZE);
        std::memset(usage_bitmap, 0, USAGE_BITMAP_STORAGE_SIZE * sizeof(uint32_t));
    }

    void SetUp() override
    {
        wipe_card();
    }

    void TearDown() override
    {
        stop();
    }

    void start(Database *db = nullptr)
    {
        ctx = db_init(db);
        ASSERT_NE(ctx, nullptr);
    }

    // Stop the task and wait for its reply, so it has finished with the database
    void stop()
    {
        if (ctx == nullptr)
        {
            return;
        }
        DbRequest req{};
        req.type = DB_REQ_STOP;
        EXPECT_EQ(call(req).ret, STRG_OK);
        ctx = nullptr;
    }

    // Post a request and wait for its response
    DbResponse call(DbRequest req)
    {
        DbResponse rsp{};
        req.reply_q = reply_q;
        req.tag = next_tag++;
        EXPECT_TRUE(db_task_post(ctx, &req, REPLY_TIMEOUT_MS));
        EXPECT_TRUE(platform_os_QueueReceive(reply_q, &rsp, REPLY_TIMEOUT_MS)) << "no response";
        EXPECT_EQ(rsp.tag, req.tag);
        EXPECT_EQ(rsp.type, req.type);
        return rsp;
    }

    // Post a request without a reply queue (fire and forget)
    void post(DbRequest req)
    {
        req.reply_q = nullptr;
        EXPECT_TRUE(db_task_post(ctx, &req, REPLY_TIMEOUT_MS));
    }

    static DbRequest phone_req(DbRequestType type, const char *phone)
    {
        DbRequest req{};
        req.type = type;
        std::snprintf(req.args.phone.phone, sizeof(req.args.phone.phone), "%s", phone);
        return req;
    }

    static DbRequest add_req(const char *name, const char *phone)
    {
        DbRequest req{};
        req.type = DB_REQ_CONTACT_ADD;
        std::snprintf(req.args.contact_add.name, sizeof(req.args.contact_add.name), "%s", name);
        std::snprintf(req.args.contact_add.phone, sizeof(req.args.contact_add.phone), "%s", phone);
        return req;
    }

    static DbRequest message_req(const char *phone, uint32_t ts, bool out, const char *str)
    {
        DbRequest req{};
        req.type = DB_REQ_MESSAGE_ADD;
        std::snprintf(req.args.message_add.phone, sizeof(req.args.message_add.phone), "%s", phone);
        std::snprintf(req.args.message_add.str, sizeof(req.args.message_add.str), "%s", str);
        req.args.message_add.timestamp = ts;
        req.args.message_add.direction = out;
        return req;
    }

    static DbRequest range_req(DbRequestType type, size_t start, size_t n, void *out)
    {
        DbRequest req{};
        req.type = type;
        req.args.range.start = start;
        req.args.range.n = n;
        req.args.range.out = out;
        return req;
    }

    size_t count()
    {
        DbRequest req{};
        req.type = DB_REQ_CONTACT_COUNT;
        DbResponse rsp = call(req);
        EXPECT_EQ(rsp.ret, STRG_OK);
        return rsp.result.range.count;
    }

    static std::string name_of(const Contact &c)
    {
        return std::string(c.name, c.name_len);
    }
};

PlatformQueue *DbTaskTest::reply_q = nullptr;

/**
 * @brief The task formats a blank card on start and reports zero contacts.
 */
TEST_F(DbTaskTest, MountsBlankCard)
{
    start();
    EXPECT_EQ(count(), 0u);
}

/**
 * @brief A contact added through the queue can be read back, and duplicates
 * report STRG_EXISTS.
 */
TEST_F(DbTaskTest, AddThenGet)
{
    start();
    EXPECT_EQ(call(add_req("Ada Lovelace", "0400000001")).ret, STRG_OK);
    EXPECT_EQ(call(add_req("Someone", "0400000001")).ret, STRG_EXISTS);

    DbResponse rsp = call(phone_req(DB_REQ_CONTACT_GET, "0400000001"));
    ASSERT_EQ(rsp.ret, STRG_OK);
    EXPECT_EQ(name_of(rsp.result.contact.contact), "Ada Lovelace");

    EXPECT_EQ(call(phone_req(DB_REQ_CONTACT_GET, "0499999999")).ret, STRG_EMPTY);
}

/**
 * @brief 100 fire and forget adds, then a count: the queue is FIFO so the
 * count runs after every add. The request queue (5) is shorter than the
 * burst, so posting also exercises the full queue wait.
 */
TEST_F(DbTaskTest, HundredAddsThenCount)
{
    char name[32];
    char phone[16];

    start();
    for (int i = 0; i < 100; i++)
    {
        std::snprintf(name, sizeof(name), "Contact %03d", i);
        std::snprintf(phone, sizeof(phone), "04%08d", i);
        post(add_req(name, phone));
    }
    EXPECT_EQ(count(), 100u);
}

/**
 * @brief Update renames and moves a contact, remove deletes it.
 */
TEST_F(DbTaskTest, UpdateAndRemove)
{
    start();
    ASSERT_EQ(call(add_req("Ada", "0400000001")).ret, STRG_OK);

    DbRequest upd{};
    upd.type = DB_REQ_CONTACT_UPDATE;
    std::snprintf(upd.args.contact_update.phone, sizeof(upd.args.contact_update.phone), "0400000001");
    std::snprintf(upd.args.contact_update.new_phone, sizeof(upd.args.contact_update.new_phone), "0400000002");
    std::snprintf(upd.args.contact_update.new_name, sizeof(upd.args.contact_update.new_name), "Ada L");
    EXPECT_EQ(call(upd).ret, STRG_OK);

    DbResponse rsp = call(phone_req(DB_REQ_CONTACT_GET, "0400000002"));
    ASSERT_EQ(rsp.ret, STRG_OK);
    EXPECT_EQ(name_of(rsp.result.contact.contact), "Ada L");

    EXPECT_EQ(call(phone_req(DB_REQ_CONTACT_REMOVE, "0400000002")).ret, STRG_OK);
    EXPECT_EQ(count(), 0u);
}

/**
 * @brief List writes into the caller's buffer in name order, and seek gives
 * the rank to list from.
 */
TEST_F(DbTaskTest, ListAndSeek)
{
    ContactBuffer page[10];

    start();
    ASSERT_EQ(call(add_req("Zara", "0400000003")).ret, STRG_OK);
    ASSERT_EQ(call(add_req("Ada", "0400000001")).ret, STRG_OK);
    ASSERT_EQ(call(add_req("Bob", "0400000002")).ret, STRG_OK);

    DbResponse rsp = call(range_req(DB_REQ_CONTACT_LIST, 0, 10, page));
    ASSERT_EQ(rsp.ret, STRG_OK);
    ASSERT_EQ(rsp.result.range.count, 3u);
    EXPECT_EQ(name_of(page[0].contact), "Ada");
    EXPECT_EQ(name_of(page[1].contact), "Bob");
    EXPECT_EQ(name_of(page[2].contact), "Zara");

    DbRequest seek{};
    seek.type = DB_REQ_CONTACT_SEEK;
    std::snprintf(seek.args.contact_seek.prefix, sizeof(seek.args.contact_seek.prefix), "b");
    rsp = call(seek);
    ASSERT_EQ(rsp.ret, STRG_OK);
    EXPECT_EQ(rsp.result.seek.rank, 1u);
}

/**
 * @brief Messages: add, read the chat into the caller's buffer, recent
 * history, then remove the chat.
 */
TEST_F(DbTaskTest, MessagesAndChat)
{
    Message chat[4];
    MessageHistoryEntry recent[4];

    start();
    ASSERT_EQ(call(add_req("Ada", "0400000001")).ret, STRG_OK);
    ASSERT_EQ(call(message_req("0400000001", 100, true, "hello")).ret, STRG_OK);
    ASSERT_EQ(call(message_req("0400000001", 101, false, "hi back")).ret, STRG_OK);

    DbRequest get{};
    get.type = DB_REQ_MESSAGE_GET_CHAT;
    std::snprintf(get.args.chat.phone, sizeof(get.args.chat.phone), "0400000001");
    get.args.chat.start = 0;
    get.args.chat.n = 4;
    get.args.chat.out = chat;
    DbResponse rsp = call(get);
    ASSERT_EQ(rsp.ret, STRG_OK);
    ASSERT_EQ(rsp.result.range.count, 2u);
    // newest first
    EXPECT_STREQ(chat[0].str, "hi back");
    EXPECT_STREQ(chat[1].str, "hello");

    rsp = call(range_req(DB_REQ_MESSAGE_RECENT, 0, 4, recent));
    ASSERT_EQ(rsp.ret, STRG_OK);
    EXPECT_EQ(rsp.result.range.count, 2u);

    EXPECT_EQ(call(phone_req(DB_REQ_MESSAGE_REMOVE_CHAT, "0400000001")).ret, STRG_OK);
    EXPECT_EQ(call(get).ret, STRG_EMPTY);
}

/**
 * @brief Calls: a known and an unknown number, newest first.
 */
TEST_F(DbTaskTest, Calls)
{
    CallHistoryEntry calls[4];

    start();
    ASSERT_EQ(call(add_req("Ada", "0400000001")).ret, STRG_OK);
    ASSERT_EQ(call(phone_req(DB_REQ_CALL_ADD, "0400000001")).ret, STRG_OK);
    ASSERT_EQ(call(phone_req(DB_REQ_CALL_ADD, "0499999999")).ret, STRG_OK);

    DbResponse rsp = call(range_req(DB_REQ_CALL_RECENT, 0, 4, calls));
    ASSERT_EQ(rsp.ret, STRG_OK);
    ASSERT_EQ(rsp.result.range.count, 2u);
    EXPECT_EQ(std::string(calls[0].contact.phone, calls[0].contact.phone_len), "0499999999");
    EXPECT_EQ(name_of(calls[1].contact), "Ada");
}

/**
 * @brief Types without a handler reply STRG_FAIL instead of crashing: one
 * past the table.
 */
TEST_F(DbTaskTest, UnknownTypeFails)
{
    DbRequest req{};

    start();
    req.type = DB_REQ_COUNT;
    EXPECT_EQ(call(req).ret, STRG_FAIL);

    // the task still serves requests afterwards
    EXPECT_EQ(count(), 0u);
}

/**
 * @brief Each response carries its request's tag, so several outstanding
 * requests on one reply queue can be matched up.
 */
TEST_F(DbTaskTest, ResponsesMatchTags)
{
    DbResponse rsp{};

    start();
    DbRequest a = add_req("Ada", "0400000001");
    DbRequest b = phone_req(DB_REQ_CONTACT_GET, "0400000001");
    a.reply_q = reply_q;
    a.tag = 7;
    b.reply_q = reply_q;
    b.tag = 8;
    ASSERT_TRUE(db_task_post(ctx, &a, REPLY_TIMEOUT_MS));
    ASSERT_TRUE(db_task_post(ctx, &b, REPLY_TIMEOUT_MS));

    ASSERT_TRUE(platform_os_QueueReceive(reply_q, &rsp, REPLY_TIMEOUT_MS));
    EXPECT_EQ(rsp.tag, 7u);
    EXPECT_EQ(rsp.type, DB_REQ_CONTACT_ADD);
    EXPECT_EQ(rsp.ret, STRG_OK);

    ASSERT_TRUE(platform_os_QueueReceive(reply_q, &rsp, REPLY_TIMEOUT_MS));
    EXPECT_EQ(rsp.tag, 8u);
    EXPECT_EQ(rsp.type, DB_REQ_CONTACT_GET);
    EXPECT_EQ(rsp.ret, STRG_OK);
}

/**
 * @brief A corrupted superheader is reported for every request (the task keeps
 * running rather than exiting), and stop still works.
 */
TEST_F(DbTaskTest, MountFailureReported)
{
    Database db{};

    // a formatted card with a bad superheader byte
    ASSERT_EQ(database_init(&db), STRG_OK);
    heap_mem[SUPERHEADER_SECTOR * SECTOR_SIZE + 8] ^= 0xFF;

    start();
    DbRequest req{};
    req.type = DB_REQ_CONTACT_COUNT;
    EXPECT_EQ(call(req).ret, STRG_CORRUPT);
    EXPECT_EQ(call(add_req("Ada", "0400000001")).ret, STRG_CORRUPT);
}

/**
 * @brief With a caller supplied (already mounted) database the task skips
 * database_init and works on that database, which the caller sees after stop.
 */
TEST_F(DbTaskTest, UsesCallerDatabase)
{
    Database db{};

    ASSERT_EQ(database_init(&db), STRG_OK);
    ASSERT_EQ(database_contact_add(&db, "Ada", "0400000001"), STRG_OK);

    start(&db);
    EXPECT_EQ(count(), 1u);
    EXPECT_EQ(call(add_req("Bob", "0400000002")).ret, STRG_OK);
    stop();

    EXPECT_EQ(database_contact_count(&db), 2u);
}

/**
 * @brief Posting before the task has run is safe: the queue exists as soon as
 * db_init returns, and the requests wait until the task has mounted.
 */
TEST_F(DbTaskTest, PostRightAfterInit)
{
    start();
    post(add_req("Ada", "0400000001"));
    EXPECT_EQ(count(), 1u);
}

/**
 * @brief The chat list comes back most recent first in the caller's buffer.
 */
TEST_F(DbTaskTest, ChatList)
{
    ChatSummary chats[4];

    start();
    ASSERT_EQ(call(message_req("0400000001", 100, true, "a")).ret, STRG_OK);
    ASSERT_EQ(call(message_req("0400000002", 101, true, "b")).ret, STRG_OK);

    DbResponse rsp = call(range_req(DB_REQ_CHAT_LIST, 0, 4, chats));
    ASSERT_EQ(rsp.ret, STRG_OK);
    ASSERT_EQ(rsp.result.range.count, 2u);
    EXPECT_EQ(std::string(chats[0].contact.phone, chats[0].contact.phone_len), "0400000002");
    EXPECT_STREQ(chats[0].latest.str, "b");
    EXPECT_EQ(std::string(chats[1].contact.phone, chats[1].contact.phone_len), "0400000001");
}
