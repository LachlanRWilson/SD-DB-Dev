#include "platform_time.h"
#include <stdint.h>

// Mock System Tick Count (MOCK ONLY)
static uint32_t sysTick = 0;

/**
* platform_time_GetTick()
* ---------------------
* Return the current system tick count
*
* return: current system tick count
*/
uint32_t platform_time_GetTick(void) {
    return sysTick;
}

/**
* platform_time_IncTick()
* --------------------------
* Increment the system tick count
*
* return: void 
*/
 void platform_time_IncTick(int tks) {
    sysTick += tks;
}

/**
* platform_time_ResetTick()
* --------------------------
* Reset the system tick count
*
* return: void
*/
void platform_time_ResetTick(void) {
    sysTick = 0;
}