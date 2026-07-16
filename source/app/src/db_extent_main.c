#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#include "message_extent.h"
#include "hash_table.h"
#include "free_list_stack.h"
#include "sd_storage.h"
#include "test_db_extent.h"

extern Storage sd_storage;

// Store hash table entries and free stack in RAM D1 (doesn't fit in DCTMRAM)
__attribute__((section(".ram_d1")))
uint16_t message_fls_mem[2 * HASH_TABLE_SIZE];

#define DATABASE_TASK_STACK_SIZE 8192 * 2
#define HASH_TABLE_START_SECTOR 0
#define DATABASE_TASK_PRIORITY  osPriorityNormal

// SD Card Indexes
#define MESSAGE_EXTENT_INDEX 0

void dbTask(void* arg)
{
    FreeList message_fls;
    free_list_init(&message_fls, message_fls_mem, 2 * HASH_TABLE_SIZE);

    // Storage Context
    SDStorageContext sd_ctx;
    SDStorage_Init(&sd_ctx, 2 * MESSAGE_EXTENT_INDEX, sizeof(MessageBlockBuffer));

    // Adding context to struct 
    sd_storage.context = (void*) &sd_ctx;
    
    MessageExtent extent;

    // Create Hash Table
    message_extent_init(&extent, &sd_storage, &message_fls, 2* HASH_TABLE_SIZE);

    if (!test_db_extents(&extent))
    {
        // slow flash for fail
        for (;;)
        {
            GPIOB->ODR ^= (1 << 0);
            osDelay(2000);
        }
    }

    // Fast flash for pass
    for (;;) 
    {
        GPIOB->ODR ^= (1 << 0);
        osDelay(300);
    }
}

void DB_Extent_Init(void)
{
    // Create and start the task
    osThreadAttr_t task_attr = {
        .name = "Database Task",
        .stack_size = DATABASE_TASK_STACK_SIZE, // Increased stack size for display operations
        .priority = DATABASE_TASK_PRIORITY};
    osThreadId_t thread_id = osThreadNew(dbTask, NULL, &task_attr);

}

