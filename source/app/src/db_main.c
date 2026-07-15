#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#include "hash_table.h"
#include "free_list_stack.h"
#include "sd_storage.h"
#include "message_extent.h"
#include "test_db_main.h"

extern Storage sd_storage;

// Store hash table entries and free stack in RAM D1 (doesn't fit in DCTMRAM)
__attribute__((section(".ram_d1")))
HashEntry entries[HASH_TABLE_SIZE] = {0};

__attribute__((section(".ram_d1")))
uint16_t hash_fls_mem[HASH_TABLE_SIZE];

#define DATABASE_TASK_STACK_SIZE 2048
#define HASH_TABLE_START_SECTOR 0
#define DATABASE_TASK_PRIORITY  osPriorityNormal

void dbTask(void* arg)
{
    // Zero Entries
    memset(entries, 0, sizeof(entries));

    HashTable table;
    FreeList hash_fls;
    free_list_init(&hash_fls, hash_fls_mem, HASH_TABLE_SIZE);

    // Storage Context
    SDStorageContext sd_ctx;
    SDStorage_Init(&sd_ctx, HASH_TABLE_START_SECTOR, sizeof(Contact));

    // Adding context to struct 
    sd_storage.context = (void*) &sd_ctx;


    // Create Hash Table
    hash_init(&table, &hash_fls, entries, HASH_TABLE_SIZE, &sd_storage);

    if (!test_db_run(&table))
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

void DB_Init(void)
{
    // Create and start the task
    osThreadAttr_t task_attr = {
        .name = "Database Task",
        .stack_size = DATABASE_TASK_STACK_SIZE, // Increased stack size for display operations
        .priority = DATABASE_TASK_PRIORITY};
    osThreadId_t thread_id = osThreadNew(dbTask, NULL, &task_attr);

}
