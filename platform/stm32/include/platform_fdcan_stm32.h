/**
 * platform/stm32/src/platform_fdcan_stm32.c
 * Brief: All file functions perform STM32 operations. This will be linked with
 * main application logic when performing on STM32 binary file compilation. 
 * STM32H723ZG6T has 3 FDCAN ports
 *
 */
#ifndef PLATFORM_GPIO_H
#define PLATFORM_GPIO_H

#include "stm32h723xx.h"
#include "stm32h7xx_hal.h"
void can_enable();
void can_recv();
void can_send();

#endif
