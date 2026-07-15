/**
 * platform/stm32/src/platform_gpio_stm32.c
 * Brief: All file functions perform STM32 operations. This will be linked with
 * main application logic when performing on STM32 binary file compilation. STM32H723ZG6T has 11 x 16 Pin GPIO ports
 *
 */
#include "platform_gpio.h"

/** 
 * platform_gpio_init()
 * --------------------
 * Initialises all pins on GPIO port 
 * gpio: GPIO struct containting port and pins to be operated on
 */
void platform_gpio_init(gpio_t gpio) {
    // GPIO initialization structure
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // Enable the GPIO clock (for example GPIOB)
    __HAL_RCC_GPIOB_CLK_ENABLE();  // Replace with the correct GPIO port as needed

    // Configure GPIO pins (e.g., PA0 to PA15)
    GPIO_InitStruct.Pin = gpio.pin;  
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;  // Push-pull output mode
    GPIO_InitStruct.Pull = GPIO_NOPULL;  // No pull-up or pull-down resistors
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;  // Low speed
    HAL_GPIO_Init((GPIO_TypeDef*) gpio.port, &GPIO_InitStruct);  // Initialize GPIOB (LED 1 is PORTB PIN 0)
}

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
void platform_gpio_write_pin(gpio_t gpio, GPIO_STATE state) {
    GPIO_TypeDef *GPIOx = gpio.port;  
    
    if (state == GPIO_HIGH) {
        HAL_GPIO_WritePin(GPIOx, gpio.pin, GPIO_PIN_SET);  // Set the pin
    } else {
        HAL_GPIO_WritePin(GPIOx, gpio.pin, GPIO_PIN_RESET);  // Reset the pin
    }
}

// Read the state of a GPIO pin (HIGH/LOW)
GPIO_STATE platform_gpio_read(gpio_t gpio) {
    GPIO_TypeDef *GPIOx = gpio.port;  // Example: GPIOA (again, adjust based on your actual port)
    
    GPIO_PinState pinState = HAL_GPIO_ReadPin(GPIOx, gpio.pin);
    return (pinState == GPIO_PIN_SET) ? GPIO_HIGH : GPIO_LOW;
}

// Toggle the state of a GPIO pin
void platform_gpio_toggle(gpio_t gpio) {
    GPIO_TypeDef *GPIOx = gpio.port;  // Example: GPIOB
    
    HAL_GPIO_TogglePin(GPIOx, gpio.pin);  // Toggle the pin state
}
