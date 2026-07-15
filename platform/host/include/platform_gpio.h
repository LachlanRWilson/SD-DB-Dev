/**
 * platform/host/include/platform_gpio.h
 * Brief: All file functions emulate (mock) STM32 GPIO operations. This will be linked with
 * main application logic when performing on PC unit testing. STM32H723ZG6T has 11 x 16 Pin GPIO ports
 *
*/

#ifndef PLATFORM_GPIO_H
#define PLATFORM_GPIO_H
#include <stdint.h>

// C Wrapper of C++ test file
#ifdef __cplusplus
extern "C" {
#endif

// GPIO States
typedef enum {
    GPIO_LOW = 0,
    GPIO_HIGH = 1
} GPIO_STATE;

// GPIO Pins
typedef enum {
    GPIO_PIN_0 = (1 << 0),
    GPIO_PIN_1 = (1 << 1),
    GPIO_PIN_2 = (1 << 2),
    GPIO_PIN_3 = (1 << 3),
    GPIO_PIN_4 = (1 << 4),
    GPIO_PIN_5 = (1 << 5),
    GPIO_PIN_6 = (1 << 6),
    GPIO_PIN_7 = (1 << 7),
    GPIO_PIN_8 = (1 << 8),
    GPIO_PIN_9 = (1 << 9),
    GPIO_PIN_10 = (1 << 10),
    GPIO_PIN_11 = (1 << 11),
    GPIO_PIN_12 = (1 << 12),
    GPIO_PIN_13 = (1 << 13),
    GPIO_PIN_14 = (1 << 14),
    GPIO_PIN_15 = (1 << 15)
} GPIO_PIN;


/* GPIO_TypeDef (as per STM32 HAL) */
typedef struct {
    uint32_t MODER;        /*!< GPIO port mode register,               Address offset: 0x00      */
    uint32_t OTYPER;       /*!< GPIO port output type register,        Address offset: 0x04      */
    uint32_t OSPEEDR;      /*!< GPIO port output speed register,       Address offset: 0x08      */
    uint32_t PUPDR;        /*!< GPIO port pull-up/pull-down register,  Address offset: 0x0C      */
    uint32_t IDR;          /*!< GPIO port input data register,         Address offset: 0x10      */
    uint32_t ODR;          /*!< GPIO port output data register,        Address offset: 0x14      */
    uint32_t BSRR;         /*!< GPIO port bit set/reset register,      Address offset: 0x1A */
    uint32_t LCKR;         /*!< GPIO port configuration lock register, Address offset: 0x1C      */
    uint32_t AFR[2];       /*!< GPIO alternate function registers,     Address offset: 0x20-0x24 */
    uint32_t BRR;          /*!< GPIO bit reset register,               Address offset: 0x28 */
} GPIO_TypeDef;

// GPIO struct to allow platform_gpio abraction between host and stm32 
typedef struct {
    int port;
    uint16_t pin; 
} gpio_t;

// Number of pins per port
#define NUM_GPIO_PINS 16
// Number of ports
#define NUM_GPIO_PORTS 11

/** 
 * platform_gpio_init()
 * --------------------
 * Initiales specified GPIO pins and ports
 * port: GPIO port to init 
 */
void platform_gpio_init(gpio_t gpio);

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
void platform_gpio_write_pin(gpio_t gpio, GPIO_STATE state);

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
void platform_gpio_write_port(gpio_t gpio);

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
GPIO_STATE platform_gpio_read_pin(gpio_t gpio);
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
void platform_gpio_toggle(gpio_t gpio);

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
uint16_t platform_gpio_read_port(gpio_t gpio);

#ifdef __cplusplus
}
#endif

#endif