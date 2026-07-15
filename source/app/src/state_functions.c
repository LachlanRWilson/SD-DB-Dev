/**
* source/app/include/state_functions.h
* Brief: Header files stores state functions, state transition functions, state
* check functions and change state funcitons for the GYR state machine
*/
#include "state_functions.h"

// State Timeout Duration 500 Ticks (500 ms)
#define TIMEOUT_DURATION 500

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
bool press_ev(uint32_t na) {
    // Usr Button Pin and Port
    gpio_t usrBtnPin;
    usrBtnPin.port = GPIOC;
    usrBtnPin.pin = GPIO_PIN_13;
    GPIO_STATE pinState;
    pinState = platform_gpio_read(usrBtnPin);    

    return (pinState == GPIO_HIGH);
}

/**
* timeout_ev()
* ---------------
* Event check. Check that if the state duration has been exceeded
*
* return: true if button is pressed, else false
*/
bool timeout_ev(uint32_t prevTimeout) {
    uint32_t now;
    // Get current time
     now = platform_time_GetTick();

    // Check if timeout duration has been long enough
    if ((now - prevTimeout) >= TIMEOUT_DURATION) {
        return true;
    }

    return false;
}


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
bool idle_green_check(void) {
    return true; // return true for the moment
}

/**
* idle_to_green()
* ---------------
* Idle State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void idle_to_green(state_t* currentState) {
    *currentState = GREEN;
}

void idle_handler(void) {
    return;
}

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
bool green_yellow_check(void) {
    gpio_t greenPin;
    greenPin.port = GPIOB;
    greenPin.pin = GPIO_PIN_0;
    platform_gpio_write_pin(greenPin, GPIO_LOW);
    return true;
}

/**
* green_to_yellow()
* ---------------
* Green State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void green_to_yellow(state_t* currentState) {
    *currentState = YELLOW;
}

// Turn green LED on
void green_handler(void) {
    gpio_t greenPin;
    greenPin.port = GPIOB;
    greenPin.pin = GPIO_PIN_0;
    platform_gpio_write_pin(greenPin, GPIO_HIGH);

}

// turn on yellow and red LEDs
bool green_error_check(void) {
    gpio_t yellowPin;
    yellowPin.port = GPIOE;
    yellowPin.pin = GPIO_PIN_1;
    platform_gpio_write_pin(yellowPin, GPIO_HIGH);

    gpio_t redPin;
    redPin.port = GPIOB;
    redPin.pin = GPIO_PIN_14;
    platform_gpio_write_pin(redPin, GPIO_HIGH);
    
    return true;

}
void green_to_error(state_t* currentState) {
    *currentState = ERR;
}

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
bool yellow_red_check(void) {
    gpio_t yellowPin;
    yellowPin.port = GPIOE;
    yellowPin.pin = GPIO_PIN_1;
    platform_gpio_write_pin(yellowPin, GPIO_LOW);
    return true;
}

/**
* yellow_to_red()
* ---------------
* Yellow State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void yellow_to_red(state_t* currentState) {
    *currentState = RED;
}

void yellow_handler(void) {
    gpio_t yellowPin;
    yellowPin.port = GPIOE;
    yellowPin.pin = GPIO_PIN_1;
    platform_gpio_write_pin(yellowPin, GPIO_HIGH);

}

// Turn on green and red LEDs
bool yellow_error_check(void) {
    gpio_t redPin;
    redPin.port = GPIOB;
    redPin.pin = GPIO_PIN_14;
    platform_gpio_write_pin(redPin, GPIO_HIGH);

    gpio_t greenPin;
    greenPin.port = GPIOB;
    greenPin.pin = GPIO_PIN_0;
    platform_gpio_write_pin(greenPin, GPIO_HIGH);

    return true;
}

// yellow -> err
void yellow_to_error(state_t* currentState) {
    *currentState = ERR;
}

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
bool red_idle_check(void) {
    gpio_t redPin;
    redPin.port = GPIOB;
    redPin.pin = GPIO_PIN_14;
    platform_gpio_write_pin(redPin, GPIO_LOW);
    return true;
}

/**
* red_to_idle()
* ---------------
* Red State transition 
* currState: pointer to the current state value to be changed to the next state
* return: void
*/
void red_to_idle(state_t* currentState) {
    *currentState = IDLE;
}

void red_handler(void) {
    gpio_t redPin;
    redPin.port = GPIOB;
    redPin.pin = GPIO_PIN_14;
    platform_gpio_write_pin(redPin, GPIO_HIGH);
}

// turn on green and yellow for error
bool red_error_check(void) {

    gpio_t yellowPin;
    yellowPin.port = GPIOE;
    yellowPin.pin = GPIO_PIN_1;
    platform_gpio_write_pin(yellowPin, GPIO_HIGH);

    gpio_t greenPin;
    greenPin.port = GPIOB;
    greenPin.pin = GPIO_PIN_0;
    platform_gpio_write_pin(greenPin, GPIO_HIGH);
    return true;
}

// red -> err
void red_to_error(state_t* currentState) {
    *currentState = ERR;
}

// Turn off error LEDs
bool error_idle_check(void) {
    gpio_t redPin;
    redPin.port = GPIOB;
    redPin.pin = GPIO_PIN_14;
    platform_gpio_write_pin(redPin, GPIO_LOW);

    gpio_t yellowPin;
    yellowPin.port = GPIOE;
    yellowPin.pin = GPIO_PIN_1;
    platform_gpio_write_pin(yellowPin, GPIO_LOW);

    gpio_t greenPin;
    greenPin.port = GPIOB;
    greenPin.pin = GPIO_PIN_0;
    platform_gpio_write_pin(greenPin, GPIO_LOW);

    return true;
}

// err -> idle
void error_to_idle(state_t *currentState) {
    *currentState = IDLE;
}

void error_handler(void) {
    // need to add platform_freertos before adding delays in here    
}