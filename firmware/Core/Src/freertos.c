/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdint.h>

#include "fatfs.h"
#include "sdmmc.h"
#include "raw_sd.h"
extern SD_HandleTypeDef hsd1;
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osThreadId_t FMC_TaskHandle;

const osThreadAttr_t fmc_attributes = {
  .name = "FMC Task",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t heartbeatTaskHandle;

const osThreadAttr_t heartbeatTask_attributes = {
  .name = "Heartbeat Task",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 4096,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void HeartbeatTask(void *argument);
void fmcTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

static void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0;

    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* USER CODE BEGIN Header_StartDefaultTask */
// /**
//   * @brief  Function implementing the defaultTask thread.
//   * @param  argument: Not used
//   * @retval None
//   */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
    uint8_t tx_buffer[512];
    uint8_t rx_buffer[512];

    HAL_StatusTypeDef status;

    DWT_Init();
    /* Fill write buffer with known pattern */
    for (uint32_t i = 0; i < sizeof(tx_buffer); i++)
    {
        tx_buffer[i] = (uint8_t)i;
    }

    memset(rx_buffer, 0, sizeof(rx_buffer));


    /*
     * Check card state before operation
     */
    if (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
    {
        Error_Handler();
    }


    /*
     * Write sector 0
     */
    status = HAL_SD_WriteBlocks(
        &hsd1,
        tx_buffer,
        0,              // sector address
        1,              // number of blocks
        5000
    );

    if (status != HAL_OK)
    {
        Error_Handler();
    }


    /*
     * Wait until write completes
     */
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
    {
        osDelay(1);
    }


uint32_t start_cycles;
uint32_t end_cycles;
uint32_t elapsed_cycles;

start_cycles = DWT->CYCCNT;

status = HAL_SD_ReadBlocks(
    &hsd1,
    rx_buffer,
    0,
    1,
    5000
);

while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
{
    osDelay(1);
}

end_cycles = DWT->CYCCNT;

elapsed_cycles = end_cycles - start_cycles;


    /*
     * Verify data
     */
    if (memcmp(tx_buffer, rx_buffer, sizeof(tx_buffer)) != 0)
    {
        Error_Handler();
    }


    /*
     * Success indication
     */
    for (;;)
    {
        GPIOB->ODR ^= (1 << 0);
        osDelay(1000);
    }
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

void HeartbeatTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
      GPIOB->ODR ^= (1 << 0);
    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

void fmcTask(void *argument)
{
    // FMC Init
    for (;;) {
        // Write RGB to screen in between delays
        osDelay(500);
    }
}

/* USER CODE END Application */

