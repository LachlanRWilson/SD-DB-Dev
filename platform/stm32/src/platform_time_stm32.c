/**
 * platform/stm32/src/platform_time_stm32.c
 * Brief: All file functions perform STM32 delay operations. This will be linked with
 * main application logic when performing on STM32 binary file compilation.
 *
 */

#include "platform_time.h"


/**
* platform_time_GetTick()
* ---------------------
* Get the current system tick count
*
* return: current system tick count
*/
uint32_t platform_time_GetTick(void) {
    return HAL_GetTick();

}