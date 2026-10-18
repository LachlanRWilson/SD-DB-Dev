/**
 * platform/host/src/platform_os_host.c
 * Brief: All file functions emulate (mock) FreeRTOS queues and tasks with pthreads. This will be
 * linked with main application logic when performing on PC unit testing.
 *
 */

#define _POSIX_C_SOURCE 200809L

#include "platform_os.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Ring buffer of fixed size items guarded by a mutex
struct PlatformQueue
{
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *buf;
    uint32_t length;
    uint32_t item_size;
    uint32_t head;      // index of the front item
    uint32_t count;     // items in the queue
};

typedef struct
{
    void (*fn)(void *arg);
    void *arg;
} ThreadStart;

/**
* deadline_after()
* ---------------------
* Absolute CLOCK_REALTIME time timeout_ms from now (pthread_cond_timedwait uses absolute time)
*/
static struct timespec deadline_after(uint32_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/**
* wait_on()
* ---------------------
* Wait on cond (lock held) until signalled or the deadline passes
*
* return: false on timeout
*/
static bool wait_on(pthread_cond_t *cond, pthread_mutex_t *lock, uint32_t timeout_ms, const struct timespec *deadline)
{
    if (timeout_ms == 0)
    {
        return false;
    }
    if (timeout_ms == PLATFORM_OS_WAIT_FOREVER)
    {
        return pthread_cond_wait(cond, lock) == 0;
    }
    return pthread_cond_timedwait(cond, lock, deadline) == 0;
}

/**
* platform_os_QueueCreate()
* ---------------------
* Create a queue of fixed size items. Items are copied in and out by value
*
* return: queue handle, NULL if out of memory
*/
PlatformQueue *platform_os_QueueCreate(uint32_t length, uint32_t item_size)
{
    if ((length == 0) || (item_size == 0))
    {
        return NULL;
    }

    PlatformQueue *q = calloc(1, sizeof(PlatformQueue));
    if (q == NULL)
    {
        return NULL;
    }
    q->buf = malloc((size_t)length * item_size);
    if (q->buf == NULL)
    {
        free(q);
        return NULL;
    }
    q->length = length;
    q->item_size = item_size;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

/**
* platform_os_QueueSend()
* ---------------------
* Copy an item to the back of the queue, waiting while the queue is full
*
* return: true if sent, false on timeout
*/
bool platform_os_QueueSend(PlatformQueue *queue, const void *item, uint32_t timeout_ms)
{
    if ((queue == NULL) || (item == NULL))
    {
        return false;
    }

    struct timespec deadline = deadline_after(timeout_ms);

    pthread_mutex_lock(&queue->lock);
    while (queue->count == queue->length)
    {
        if (!wait_on(&queue->not_full, &queue->lock, timeout_ms, &deadline))
        {
            pthread_mutex_unlock(&queue->lock);
            return false;
        }
    }

    uint32_t tail = (queue->head + queue->count) % queue->length;
    memcpy(&queue->buf[(size_t)tail * queue->item_size], item, queue->item_size);
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
    return true;
}

/**
* platform_os_QueueReceive()
* ---------------------
* Copy the item at the front of the queue out, waiting while the queue is empty
*
* return: true if received, false on timeout
*/
bool platform_os_QueueReceive(PlatformQueue *queue, void *item, uint32_t timeout_ms)
{
    if ((queue == NULL) || (item == NULL))
    {
        return false;
    }

    struct timespec deadline = deadline_after(timeout_ms);

    pthread_mutex_lock(&queue->lock);
    while (queue->count == 0)
    {
        if (!wait_on(&queue->not_empty, &queue->lock, timeout_ms, &deadline))
        {
            pthread_mutex_unlock(&queue->lock);
            return false;
        }
    }

    memcpy(item, &queue->buf[(size_t)queue->head * queue->item_size], queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
    return true;
}

// pthread entry: unpack the start args, then run the task function
static void *thread_entry(void *p)
{
    ThreadStart start = *(ThreadStart *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

/**
* platform_os_ThreadCreate()
* ---------------------
* Create and start a detached thread. stack_bytes and priority are ignored on host
*
* return: true if the thread started
*/
bool platform_os_ThreadCreate(const char *name, void (*fn)(void *arg), void *arg, uint32_t stack_bytes, int priority)
{
    pthread_t thread;
    (void)name;
    (void)stack_bytes;
    (void)priority;

    if (fn == NULL)
    {
        return false;
    }

    ThreadStart *start = malloc(sizeof(ThreadStart));
    if (start == NULL)
    {
        return false;
    }
    start->fn = fn;
    start->arg = arg;

    if (pthread_create(&thread, NULL, thread_entry, start) != 0)
    {
        free(start);
        return false;
    }
    // FreeRTOS tasks are never joined, so the thread cleans itself up on return
    pthread_detach(thread);
    return true;
}
