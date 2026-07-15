/**
 * host/src/platform_gpio_host.c
 * Brief: All file functions emulate (mock) STM32 operations. This will be linked with
 * main application logic when performing on PC unit testing. STM32H723ZG6T has 11 x 16 Pin GPIO ports
 *
 */
#include <stdlib.h>
#include "platform_gpio.h" 

/* GPIO Memory Allocation */
GPIO_TypeDef sim_GPIOA;
GPIO_TypeDef sim_GPIOB;
GPIO_TypeDef sim_GPIOC;
GPIO_TypeDef sim_GPIOD;
GPIO_TypeDef sim_GPIOE;
GPIO_TypeDef sim_GPIOF;
GPIO_TypeDef sim_GPIOG;
GPIO_TypeDef sim_GPIOH;
GPIO_TypeDef sim_GPIOI;
GPIO_TypeDef sim_GPIOJ;
GPIO_TypeDef sim_GPIOK;

/* GPIO Macro Defines */
#define GPIOA (&sim_GPIOA)
#define GPIOB (&sim_GPIOB)
#define GPIOC (&sim_GPIOC)
#define GPIOD (&sim_GPIOE)
#define GPIOE (&sim_GPIOE)
#define GPIOF (&sim_GPIOF)
#define GPIOG (&sim_GPIOG)
#define GPIOH (&sim_GPIOH)
#define GPIOI (&sim_GPIOI)
#define GPIOJ (&sim_GPIOJ)
#define GPIOK (&sim_GPIOK)

// GPIO Ports and Pins
static uint16_t gpio_port[NUM_GPIO_PORTS];

/** 
 * platform_gpio_init()
 * --------------------
 * @brief Initiales specified GPIO port
 * @param port: GPIO port to init 
 */
void platform_gpio_init(gpio_t gpio) {
    // Initialise bit vector to mock port initialisation
    gpio_port[gpio.port] = (uint16_t) 0;
}

/** 
 * platform_gpio_write_pin()
 * --------------------
 * Write state to specified GPIO port pin
 *
 * port: GPIO port to write to
 * pin: GPIO pin to write to 
 * state: state being written to the pin (GPIO_LOW: 0, GPIO_HIGH: 1)
 *
 * Return: void
 */
void platform_gpio_write_pin(gpio_t gpio, GPIO_STATE state){
    // Port Check 
    if (gpio.port >= NUM_GPIO_PORTS) {
        // Need to add error statement
        return;
    }
    // Clea pin if GPIO_LOW else set high
    (state == GPIO_LOW) ? (gpio_port[gpio.port] &= ~gpio.pin) : (gpio_port[gpio.port] |= gpio.pin);
}
/** 
 * platform_gpio_write_port()
 * --------------------------
 * Write all pin states to the specified port
 *
 * port: GPIO port to write to
 * pin: GPIO pin to write to 
 * state: state being written to the pin (GPIO_LOW: 0, GPIO_HIGH: 1)
 *
 * Return: void
 */
void platform_gpio_write_port(gpio_t gpio){
    // Null Check and Port Size check
    if (gpio.port >= NUM_GPIO_PORTS) {
        // Need to add error statement
        return;
    }

    gpio_port[gpio.port] |= gpio.pin;


}

/** 
 * platform_gpio_read()
 * --------------------
 * Read state to specified GPIO port and pin
 *
 * port: GPIO port to write to
 * pin: GPIO pin to write to 
 *
 * Return: gpio state (GPIO_LOW: 0, GPIO_HIGH: 1)
 */
GPIO_STATE platform_gpio_read_pin(gpio_t gpio){
    // Null Check and Port Size check
    if (gpio.port >= NUM_GPIO_PORTS) {
        return GPIO_LOW;
    }

    // If number is not 0 after bitwise AND than bit is set
    return (gpio_port[gpio.port] & gpio.pin) ? GPIO_HIGH : GPIO_LOW;


}

/** 
 * platform_gpio_read_port()
 * --------------------
 * Read state of specified GPIO port
 *
 * port: GPIO port to write to
 * pin: GPIO pin to write to 
 *
 * Return: gpio bit vector
 */
uint16_t platform_gpio_read_port(gpio_t gpio){
    // Null Check and Port Size check
    if (gpio.port >= NUM_GPIO_PORTS) {
        return GPIO_LOW;
    }

    // Return GPIO bit vector
    return gpio_port[gpio.port];
}

/** 
 * platform_gpio_toggle()
 * --------------------
 * Toggle state of specified GPIO pin and port
 *
 * port: GPIO port
 * pin: GPIO pin
 *
 * Return: void
 */
void platform_gpio_toggle(gpio_t gpio){

    // Null Check and Port Size check
    if (gpio.port > NUM_GPIO_PORTS) {
        return;
    }

    // Toggle Pin
    gpio_port[gpio.port] ^= gpio.pin;
}