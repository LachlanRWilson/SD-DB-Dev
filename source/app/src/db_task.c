#include "db_task.h"

#define DB_TASK_STACK_SIZE 4096

// 5 requests (~1 KB) of the 15 KB FreeRTOS heap
#define DB_TASK_QUEUE_SIZE 5

struct DatabaseTaskContext
{
    PlatformQueue *request_q;   // requests posted to the task
    Database *db;               // database the task uses (own_db or the caller's)
    Database own_db;            // task owned database (too large for the task stack)
    bool mount;                 // run database_init in the task
    bool running;               // task started and not stopped
    STRG_RET mount_ret;         // database_init result, returned for every request if not STRG_OK
};

// Request handler: runs one request on the database and fills its result
typedef STRG_RET (*DbHandler)(Database *db, const U_DbRequestArgs *args, DbResponseResult *result);

static STRG_RET handle_contact_add(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    return database_contact_add(db, args->contact_add.name, args->contact_add.phone);
}

static STRG_RET handle_contact_get(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_contact_get(db, args->phone.phone, &result->contact);
}

static STRG_RET handle_contact_update(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    return database_contact_update(db, args->contact_update.phone, args->contact_update.new_name,
                                   args->contact_update.new_phone);
}

static STRG_RET handle_contact_remove(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    return database_contact_remove(db, args->phone.phone);
}

static STRG_RET handle_contact_list(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_contact_list(db, args->range.start, args->range.n, &result->range.count,
                                 (ContactBuffer *)args->range.out);
}

static STRG_RET handle_contact_seek(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_contact_seek(db, args->contact_seek.prefix, &result->seek.rank);
}

static STRG_RET handle_contact_count(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)args;
    result->range.count = database_contact_count(db);
    return STRG_OK;
}

static STRG_RET handle_message_add(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    // database_message_add takes a non-const str but doesn't modify it
    return database_message_add(db, args->message_add.phone, args->message_add.timestamp,
                                args->message_add.direction, (char *)args->message_add.str);
}

static STRG_RET handle_message_get_chat(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_message_get_chat(db, args->chat.phone, args->chat.start, args->chat.n,
                                     &result->range.count, args->chat.out);
}

static STRG_RET handle_message_remove_chat(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    return database_message_remove_chat(db, args->phone.phone);
}

static STRG_RET handle_message_recent(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_message_recent(db, args->range.start, args->range.n, &result->range.count,
                                   (MessageHistoryEntry *)args->range.out);
}

static STRG_RET handle_call_add(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)result;
    return database_call_add(db, args->phone.phone);
}

static STRG_RET handle_chat_list(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_chat_list(db, args->range.start, args->range.n, &result->range.count,
                              (ChatSummary *)args->range.out);
}

static STRG_RET handle_call_recent(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    return database_call_recent(db, args->range.start, args->range.n, &result->range.count,
                                (CallHistoryEntry *)args->range.out);
}

// the task loop exits after this reply is sent
static STRG_RET handle_stop(Database *db, const U_DbRequestArgs *args, DbResponseResult *result)
{
    (void)db;
    (void)args;
    (void)result;
    return STRG_OK;
}

// Request type -> handler. Types without a handler reply STRG_FAIL
static const DbHandler db_handlers[DB_REQ_COUNT] = {
    [DB_REQ_CONTACT_ADD]         = handle_contact_add,
    [DB_REQ_CONTACT_GET]         = handle_contact_get,
    [DB_REQ_CONTACT_UPDATE]      = handle_contact_update,
    [DB_REQ_CONTACT_REMOVE]      = handle_contact_remove,
    [DB_REQ_CONTACT_LIST]        = handle_contact_list,
    [DB_REQ_CONTACT_SEEK]        = handle_contact_seek,
    [DB_REQ_CONTACT_COUNT]       = handle_contact_count,
    [DB_REQ_MESSAGE_ADD]         = handle_message_add,
    [DB_REQ_MESSAGE_GET_CHAT]    = handle_message_get_chat,
    [DB_REQ_MESSAGE_REMOVE_CHAT] = handle_message_remove_chat,
    [DB_REQ_MESSAGE_RECENT]      = handle_message_recent,
    [DB_REQ_CHAT_LIST]           = handle_chat_list,
    [DB_REQ_CALL_ADD]            = handle_call_add,
    [DB_REQ_CALL_RECENT]         = handle_call_recent,
    [DB_REQ_STOP]                = handle_stop,
};
static DatabaseTaskContext db_ctx;

/**
 * @brief Run one request through the handler table and post the response to
 * the request's reply queue. Unknown or unhandled types reply STRG_FAIL
 *
 * @param ctx database task context
 * @param req request to run
 */
static void db_handle(DatabaseTaskContext *ctx, const DbRequest *req)
{
    DbResponse rsp = { .type = req->type, .tag = req->tag, .ret = STRG_FAIL };

    if ((ctx->mount_ret != STRG_OK) && (req->type != DB_REQ_STOP))
    {
        // not mounted: report why instead of touching the database
        rsp.ret = ctx->mount_ret;
    } else if (((unsigned)req->type < DB_REQ_COUNT) && (db_handlers[req->type] != NULL)) {
        // type comes from another task, so bounds check before indexing
        rsp.ret = db_handlers[req->type](ctx->db, &req->args, &rsp.result);
    }

    if (req->reply_q != NULL)
    {
        platform_os_QueueSend(req->reply_q, &rsp, PLATFORM_OS_WAIT_FOREVER);
    }
}

/**
 * @brief Database task. Mounts the database, then blocks on the request queue
 * and handles one request at a time. Never returns on target
 *
 * @param arg database task context
 */
static void dbTask(void *arg)
{
    DatabaseTaskContext *ctx = (DatabaseTaskContext *)arg;
    DbRequest req;

    ctx->mount_ret = ctx->mount ? database_init(ctx->db) : STRG_OK;

    for (;;)
    {
        if (!platform_os_QueueReceive(ctx->request_q, &req, PLATFORM_OS_WAIT_FOREVER))
        {
            continue;
        }

#if defined(HOST_BUILD)
        // host: end the thread (a FreeRTOS task must never return). Cleared
        // before the reply so db_init can restart once the caller has it
        if (req.type == DB_REQ_STOP)
        {
            ctx->running = false;
            db_handle(ctx, &req);
            return;
        }
#endif

        db_handle(ctx, &req);
    }
}

DatabaseTaskContext *db_init(Database *db)
{
    // already started
    if (db_ctx.running)
    {
        return &db_ctx;
    }

    db_ctx.db = (db != NULL) ? db : &db_ctx.own_db;
    db_ctx.mount = (db == NULL);
    db_ctx.mount_ret = STRG_FAIL;

    // Create the DB Request Queue before the task, so requests can be posted straight away (kept across host restarts)
    if (db_ctx.request_q == NULL)
    {
        db_ctx.request_q = platform_os_QueueCreate(DB_TASK_QUEUE_SIZE, sizeof(DbRequest));
        if (db_ctx.request_q == NULL)
        {
            return NULL;
        }
    }

    // Create the database task
    db_ctx.running = true;
    if (!platform_os_ThreadCreate("DB Task", dbTask, &db_ctx, DB_TASK_STACK_SIZE, PLATFORM_OS_PRIORITY_NORMAL))
    {
        db_ctx.running = false;
        return NULL;
    }
    return &db_ctx;
}

bool db_task_post(DatabaseTaskContext *ctx, const DbRequest *req, uint32_t timeout_ms)
{
    if ((ctx == NULL) || (req == NULL))
    {
        return false;
    }
    return platform_os_QueueSend(ctx->request_q, req, timeout_ms);
}
