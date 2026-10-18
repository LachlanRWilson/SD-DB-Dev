#ifndef DB_TASK_H
#define DB_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include "database.h"
#include "platform_os.h"

// C Wrapper of C++ test file
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DB_REQ_CONTACT_ADD,
  DB_REQ_CONTACT_GET,
  DB_REQ_CONTACT_UPDATE,
  DB_REQ_CONTACT_REMOVE,
  DB_REQ_CONTACT_LIST,
  DB_REQ_CONTACT_SEEK,
  DB_REQ_CONTACT_COUNT,
  DB_REQ_MESSAGE_ADD,
  DB_REQ_MESSAGE_GET_CHAT,
  DB_REQ_MESSAGE_REMOVE_CHAT,
  DB_REQ_MESSAGE_RECENT,
  DB_REQ_CHAT_LIST,
  DB_REQ_CALL_ADD,
  DB_REQ_CALL_RECENT,
  DB_REQ_STOP, // host: end the loop so the thread can be joined
  DB_REQ_COUNT // number of request types (keep last)
} DbRequestType;

typedef struct
{
    char phone[MAX_PHONE_LEN + 1];
} DbPhoneArgs;                              // CONTACT_GET, CONTACT_REMOVE, MESSAGE_REMOVE_CHAT, CALL_ADD

typedef struct {
    char phone[MAX_PHONE_LEN + 1];
    char name[MAX_NAME_LEN + 1];
} DbContactAddArgs;

typedef struct
{
    char phone[MAX_PHONE_LEN + 1];
    char new_phone[MAX_PHONE_LEN + 1];
    char new_name[MAX_NAME_LEN + 1];
} DbContactUpdateArgs;

typedef struct
{
    char prefix[MAX_NAME_LEN + 1];
} DbContactSeekArgs;

typedef struct
{
    size_t start;
    size_t n;
    void *out;                              // caller owned: ContactBuffer*, MessageHistoryEntry*, ChatSummary*, CallHistoryEntry*
} DbRangeArgs;                              // CONTACT_LIST, MESSAGE_RECENT, CHAT_LIST, CALL_RECENT

typedef struct
{
    char phone[MAX_PHONE_LEN + 1];
    size_t start;
    size_t n;
    Message *out;                           // caller owned, n long
} DbChatArgs;                               // MESSAGE_GET_CHAT

typedef struct
{
    char phone[MAX_PHONE_LEN + 1];
    uint32_t timestamp;
    bool direction;
    char str[SMS_MAX_MESSAGE_LENGTH + 1];
} DbMessageAddArgs;

typedef union {
    DbPhoneArgs phone;                      // CONTACT_GET, CONTACT_REMOVE, REMOVE_CHAT, CALL_ADD
    DbContactAddArgs contact_add;           // CONTACT_ADD
    DbContactUpdateArgs contact_update;     // CONTACT_UPDATE
    DbContactSeekArgs contact_seek;         // CONTACT_SEEK
    DbRangeArgs range;                      // CONTACT_LIST, MESSAGE_RECENT, CHAT_LIST, CALL_RECENT
    DbChatArgs chat;                        // MESSAGE_GET_CHAT
    DbMessageAddArgs message_add;           // MESSAGE_ADD
} U_DbRequestArgs;

typedef struct DBRequest
{
    DbRequestType type;                     // Request Type
    PlatformQueue *reply_q;                 // Reply Queue
    uint32_t tag;                           // Request sequence tag
    U_DbRequestArgs args;                   // Request Arguments
} DbRequest;

typedef struct
{
    size_t rank;                            // first name >= prefix
} DbSeekResult;                             // CONTACT_SEEK

typedef struct
{
    size_t count;                           // rows written to the request's out buffer
} DbRangeResult;                            // CONTACT_LIST, MESSAGE_GET_CHAT, MESSAGE_RECENT, CALL_RECENT, CONTACT_COUNT
                                            //
typedef union
{
    ContactBuffer contact;                  // CONTACT_GET
    DbSeekResult seek;                      // CONTACT_SEEK
    DbRangeResult range;                    // CONTACT_LIST, MESSAGE_RECENT, CHAT_LIST, CALL_RECENT
} DbResponseResult;

typedef struct
{
    DbRequestType type;                     // copied from the request, selects the union member
    uint32_t tag;                           // copied from the request
    STRG_RET ret;                           // every request sets this
    DbResponseResult result;                // Result of the response (note output of lists are stored in args.out)
} DbResponse;


// Opaque Declaration
typedef struct DatabaseTaskContext DatabaseTaskContext;

/**
 * @brief Create the request queue and start the database task. The task owns
 * the database: only post requests to it while it runs
 *
 * @param db NULL for the task to own and mount its own database, else an
 * already mounted database for the task to use (host tools and tests)
 * @retval database task context, NULL if the queue or task couldn't be created
 */
DatabaseTaskContext *db_init(Database *db);

/**
 * @brief Post a request to the database task. The response is sent to
 * req->reply_q (if not NULL) once handled
 *
 * @param ctx database task context
 * @param req request (copied into the queue)
 * @param timeout_ms max wait while the queue is full
 * @retval true if posted, false on timeout
 */
bool db_task_post(DatabaseTaskContext *ctx, const DbRequest *req, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DB_TASK_H */
