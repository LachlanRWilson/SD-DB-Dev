/**
* source/app/src/my_logic.h
* Brief: Header file for embedded code logic which can be unit tested as well. Utilises platform_*.h
* to abstract all HAL calls. DO NOT CAN ANY STM32 SPECIFIC FUNCTIONS FROM THIS FILE
*/
#include <stdint.h>
#include "state_functions.h"
#include "platform_gpio.h"
#include "platform_time.h"


// C Wrapper of C++ test file
#ifdef __cplusplus
extern "C" {
#endif


// Controller State Machine Struct
typedef struct {
    gpio_t gpio; // conroller gpio pins
    uint32_t stateTickDur; // number of ticks per state 
    GPIO_STATE state; // current state
    uint32_t prevStateTick; // previous state tick
} Controller;


/**
* controller_init()
* -----------------
* initialise state machine controller with state duration, gpio, last state change tick and state
*
* controller: controller pointer to be initialised (pointer used as to change struct members out of scope)
*
* return: void
*/
void controller_init(Controller* controller);

/**
* controller_tick()
* -----------------
* Blinky logic code which utilises controller state machine to change led on and off depending on state
* of the blinky controller
*
* return: void
*/
void controller_tick(StateData_t* blinkySM);

#ifdef __cplusplus
}
#endif