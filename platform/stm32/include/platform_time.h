/**
* platform/stm32/include/platform_time.h
* Brief: All file functions for STM32 HAL timing operations. This will be linked with
* main application logic when creating binary files 
*
*/

#pragma once

#include <stdint.h>
#include "stm32h7xx_hal.h"
#include "stm32h723xx.h"
/**
* platform_time_GetTick()
* ---------------------
* Get the current system tick count
*
* return: current system tick count
*/
uint32_t platform_time_GetTick(void);