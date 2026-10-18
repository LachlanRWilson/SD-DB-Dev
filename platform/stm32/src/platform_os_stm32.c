/**
 * platform/stm32/src/platform_os_stm32.c
 * Brief: All file functions perform FreeRTOS queue and task operations through CMSIS-OS2. This
 * will be linked with main application logic when performing on STM32 binary file compilation.
 *
 */

#include "platform_os.h"

#include "cmsis_os2.h"

/**
* ms_to_ticks()
* ---------------------
* Convert a timeout in ms to kernel ticks
*/
static uint32_t ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == PLATFORM_OS_WAIT_FOREVER)
    {
        return osWaitForever;
    }
    return (uint32_t)(((uint64_t)timeout_ms * osKernelGetTickFreq()) / 1000U);
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
    // the handle is the CMSIS queue id
    return (PlatformQueue *)osMessageQueueNew(length, item_size, NULL);
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
    return osMessageQueuePut((osMessageQueueId_t)queue, item, 0U, ms_to_ticks(timeout_ms)) == osOK;
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
    return osMessageQueueGet((osMessageQueueId_t)queue, item, NULL, ms_to_ticks(timeout_ms)) == osOK;
}

/**
* platform_os_ThreadCreate()
* ---------------------
* Create and start a FreeRTOS task
*
* return: true if the task started
*/
bool platform_os_ThreadCreate(const char *name, void (*fn)(void *arg), void *arg, uint32_t stack_bytes, int priority)
{
    const osThreadAttr_t attr = {
        .name = name,
        .stack_size = stack_bytes,
        .priority = (osPriority_t)priority,
    };

    return osThreadNew(fn, arg, &attr) != NULL;
}
