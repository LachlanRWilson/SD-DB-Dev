/**
 * platform/stm/include/platform_gpio.h
 * Brief: All file functions emulate (mock) STM32 GPIO operations. This will be linked with
 * main application logic when performing on PC unit testing. STM32H723ZG6T has 11 x 16 Pin GPIO ports
 *
*/

#ifndef PLATFORM_GPIO_H
#define PLATFORM_GPIO_H

#include "stm32h723xx.h"
#include "stm32h7xx_hal.h"
// GPIO States
typedef enum {
    GPIO_LOW = 0,
    GPIO_HIGH = 1
} GPIO_STATE;

// Struct needed to generalise gpio between host and stm32 platform abstraction
typedef struct{
    GPIO_TypeDef *port; // STM32 specific
    uint16_t pin;
} gpio_t;

// Note: this doesn't have the C++ 'C' wrapper at it will not be compiles with the 
// gtest unit test file

/** 
 * platform_gpio_init()
 * --------------------
 * Initialises all pins on GPIO port (TODO: NEED TO BE CHANGED TO SELECTED PINS)
 *
 * gpio: GPIO struct containting port and pins to be operated on
 *
 * return: void
 */
void platform_gpio_init(gpio_t gpio);

/** 
 * platform_gpio_write_pin()
 * --------------------
 * Write state to specified GPIO port pin
 *
 * gpio: GPIO struct containting port and pins to be operated on
 * state: state being written to the pin (GPIO_LOW: 0, GPIO_HIGH: 1)
 *
 * Return: void
 */
void platform_gpio_write_pin(gpio_t gpio, GPIO_STATE state);

/** 
 * platform_gpio_write_port()
 * --------------------------
 * Write all pin states to the specified port
 *
 * gpio: GPIO struct containting port and pins to be operated on
 * state: state being written to the pin (GPIO_LOW: 0, GPIO_HIGH: 1)
 *
 * Return: void
 */
void platform_gpio_write_port(gpio_t gpio);

/** 
 * platform_gpio_read()
 * --------------------
 * Read state to specified GPIO port and pin
 *
 *
 * gpio: GPIO struct containting port and pins to be operated on
 *
 * Return: gpio state (GPIO_LOW: 0, GPIO_HIGH: 1)
 */
GPIO_STATE platform_gpio_read(gpio_t gpio);
/** 
 * platform_gpio_toggle()
 * --------------------
 * Toggle state of specified GPIO pin and port
 *
 * gpio: GPIO struct containting port and pins to be operated on
 *
 * Return: void
 */
void platform_gpio_toggle(gpio_t gpio);

#endif