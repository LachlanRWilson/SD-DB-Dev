/**
* source/app/src/my_logic.c
* Brief: Source file for embedded code logic which can be unit tested as well. Utilises platform_*.h
* to abstract all HAL calls. DO NOT CAN ANY STM32 SPECIFIC FUNCTIONS FROM THIS FILE
*/
#include "my_logic.h"


// State Transition Table
static StateTransition_t stateTransTable[] = {
    // Curr State || Event Check || Transition Check   ||   State Change
    {IDLE,      &press_ev,      &idle_green_check,      &idle_to_green}, // IDLE -> GREEN (Button Press) 
    {GREEN,     &timeout_ev,    &green_yellow_check,    &green_to_yellow}, // GREEN -> YELLOW (State Timemout)
    {YELLOW,    &timeout_ev,    &yellow_red_check,      &yellow_to_red}, // YELLOW -> RED (State Timemout)
    {RED,       &timeout_ev,    &red_idle_check,        &red_to_idle}, // RED -> IDLE (State Timemout)
    {ERR,       &press_ev,      &error_idle_check,      &error_to_idle}, // ERR -> IDLE (Button Press) 
    {GREEN,     &press_ev,      &green_error_check,     &green_to_error}, // GREEN -> ERR (Button Press)
    {YELLOW,    &press_ev,      &yellow_error_check,    &yellow_to_error}, // YELLOW -> ERR (Button Press)
    {RED,       &press_ev,      &red_error_check,       &red_to_error} // RED -> ERR (Button Press)

};


/**
* controller_init()
* -----------------
* initialise state machine controller with state duration, gpIo, last state change tick and state
*
* controller: controller pointer to be initialised (pointer used as to change struct members out of scope)
*
* return: void
*/
void controller_init(Controller* controller) {

    // Initialise blinky gpio struct GPIOB Pin 0 (LED 1)
    gpio_t blinkyGpio;
    blinkyGpio.port = GPIOE;
    blinkyGpio.pin = GPIO_PIN_1;

    // Set controller gpio
    controller->gpio = blinkyGpio;

    // Set the number of ticks between state change 250 ticks
    controller->stateTickDur = 1000;

    // Starting state is LED_OFF
    controller->state = GPIO_HIGH;

    // Set starting previous state tick
    controller->prevStateTick = 0;

    // Initialise blinky gpio
    platform_gpio_init(controller->gpio);
}

// Check for state change
void state_change_check(StateData_t* sm) {
    // Iterate over state transition table
    for (int i = 0; i < NUM_STATES_TRANS; i++) {
        // If state transition is for this state AND can transition AND next state able to transition to
        if ((sm->currState == stateTransTable[i].transState) && 
        (stateTransTable[i].canTransition)(sm->prevTimeout) && (stateTransTable[i].transition)()) {

            // change state
            (stateTransTable[i].changeState)(&sm->currState);

            // Update previous timeout on state transition
            sm->prevTimeout = platform_time_GetTick();
            return;
        }
    }

}

/**
* controller_init()
* -----------------
* Blinky logic code which utilises controller state machine to change led on and off depending on state
* of the blinky controller
*
* return: void
*/
void controller_tick(StateData_t* blinkySM) {
    state_change_check(blinkySM);
    switch(blinkySM->currState) {

        case IDLE:
            idle_handler();
            break;

        case GREEN:
            green_handler();
            break;

        case YELLOW:
            yellow_handler();
            break;

        case RED:
            red_handler();
            break;

        default:
            // Error state will go in here as well
            error_handler();
            break;
    }
}

