/**
* source/app/include/state_functions.h
* Brief: Header files stores state functions, state transition functions, state
* check functions and change state funcitons for the GYR state machine
*/

#include <stdbool.h>
#include <stdint.h>
#include "platform_gpio.h"
#include "platform_time.h"

#define NUM_STATES_TRANS 8


// State Types
typedef enum {
    IDLE = 0,
    GREEN,
    YELLOW,
    RED,
    ERR
} state_t;

// Event Types
typedef enum {
    EV_PRESS, // Button Press
    EV_TIMEOUT // State Timeout
} event_t;

// State Data
typedef struct {
    state_t currState; // Current state of the state machine
    state_t nextState; // Next state of the state machine (can check for state changes)
    uint32_t prevTimeout; // track timeout time

} StateData_t;

// State Transition Struct
typedef struct {
    state_t  transState; // Current State
    bool (*canTransition)(uint32_t); // Poll the repective event for transition
    bool (*transition)(void); // transition check (check state is good)
    void (*changeState)(state_t*); // change the current state to the next state 
} StateTransition_t;

//============================
// Event Check Functions
//============================

/**
* press_ev()
* ---------------
* Event check. Check that the USER push button has been pressed
*
* return: true if button is pressed, else false
*/
bool press_ev(uint32_t na);

/**
* timeout_ev()
* ---------------
* Event check. Check that if the state duration has been exceeded
*
* return: true if button is pressed, else false
*/
bool timeout_ev(uint32_t prevTimeout);

//============================
// IDLE STATE
//============================

/**
* idle_green_check()
* ---------------
* Idle State transition check. Checking if green state can be transitioned to 
*
* return: true if transistion is safe, else false
*/
bool idle_green_check(void);

/**
* idle_to_green()
* ---------------
* Idle State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void idle_to_green(state_t* currentState);

void idle_handler(void);

//============================
// GREEN STATE
//============================

/**
* green_yellow_check()
* ---------------
* Green State transition check. Checking if yellow state can be transitioned to 
*
* return: true if transistion is safe, else false
*/
bool green_yellow_check(void);

bool green_error_check(void);
void green_to_error(state_t* currentState);

/**
* green_to_yellow()
* ---------------
* Green State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void green_to_yellow(state_t* currentState);

void green_handler(void);

//============================
// YELLOW STATE
//============================

/**
* yellow_red_check()
* ---------------
* Yellow State transition check. Checking if red state can be transitioned to 
*
* return: true if transistion is safe, else false
*/
bool yellow_red_check(void);

bool yellow_error_check(void);
void yellow_to_error(state_t* currentState);

/**
* yellow_to_red()
* ---------------
* Yellow State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void yellow_to_red(state_t* currentState);

void yellow_handler(void);

//============================
// RED STATE
//============================

/**
* red_idle_check()
* ---------------
* Red State transition check. Checking if idle state can be transitioned to 
*
* return: true if transistion is safe, else false
*/
bool red_idle_check(void);

bool red_error_check(void);
void red_to_error(state_t* currentState);

/**
* red_to_idle()
* ---------------
* Red State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void red_to_idle(state_t* currentState);

void red_handler(void);



//============================
// ERROR STATE
//============================
bool error_idle_check(void);
void error_to_idle(state_t* currentState);
void error_handler(void);
