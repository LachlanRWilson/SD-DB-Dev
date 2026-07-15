/**
* platform/host/include/platform_time.h
* Brief: All file functions emulate (mock) STM32 HAL timing operations. This will be linked with
* main application logic when performing on PC unit testing. 
*
*/

#include <stdint.h>

#pragma once

// C Wrapper of C++ test file
#ifdef __cplusplus
extern "C" {
#endif

/**
* platform_time_GetTick()
* ---------------------
* Get the current system tick count
*
* return: current system tick count
*/
uint32_t platform_time_GetTick(void);

/**
* platform_time_IncTick()
* --------------------------
* Increment the system tick count
*
* return: void
*/
void platform_time_IncTick(int tks);

/**
* platform_time_delay_count()
* --------------------------
* Reset the system tick count
*
* return: void
*/
void platform_time_ResetTick(void);


#ifdef __cplusplus
}
#endif
