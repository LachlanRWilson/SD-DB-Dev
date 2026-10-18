/**
 * platform/stm32/include/platform_os.h
 * Brief: All file functions for FreeRTOS (CMSIS-OS2) queue and task operations.
 * This will be linked with main application logic when creating binary files.
 *
*/

#ifndef PLATFORM_OS_H
#define PLATFORM_OS_H

#include <stdbool.h>
#include <stdint.h>

// C Wrapper of C++ test file
#ifdef __cplusplus
extern "C" {
#endif

// Block until the operation completes
#define PLATFORM_OS_WAIT_FOREVER UINT32_MAX

// Thread priority (CMSIS-OS2 osPriorityNormal)
#define PLATFORM_OS_PRIORITY_NORMAL 24

// Opaque Declaration
typedef struct PlatformQueue PlatformQueue;

/**
* platform_os_QueueCreate()
* ---------------------
* Create a queue of fixed size items. Items are copied in and out by value
*
* length: max number of items in the queue
* item_size: size of one item in bytes
*
* return: queue handle, NULL if out of memory
*/
PlatformQueue *platform_os_QueueCreate(uint32_t length, uint32_t item_size);

/**
* platform_os_QueueSend()
* ---------------------
* Copy an item to the back of the queue, waiting while the queue is full
*
* queue: queue handle
* item: item to copy (item_size bytes)
* timeout_ms: max wait (0 = don't wait, PLATFORM_OS_WAIT_FOREVER = block)
*
* return: true if sent, false on timeout
*/
bool platform_os_QueueSend(PlatformQueue *queue, const void *item, uint32_t timeout_ms);

/**
* platform_os_QueueReceive()
* ---------------------
* Copy the item at the front of the queue out, waiting while the queue is empty
*
* queue: queue handle
* item: destination (item_size bytes)
* timeout_ms: max wait (0 = don't wait, PLATFORM_OS_WAIT_FOREVER = block)
*
* return: true if received, false on timeout
*/
bool platform_os_QueueReceive(PlatformQueue *queue, void *item, uint32_t timeout_ms);

/**
* platform_os_ThreadCreate()
* ---------------------
* Create and start a thread (FreeRTOS task)
*
* name: thread name
* fn: thread function
* arg: argument passed to fn
* stack_bytes: stack size in bytes
* priority: PLATFORM_OS_PRIORITY_* (osPriority_t value)
*
* return: true if the thread started
*/
bool platform_os_ThreadCreate(const char *name, void (*fn)(void *arg), void *arg, uint32_t stack_bytes, int priority);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_OS_H */
