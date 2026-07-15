#include <gtest/gtest.h>

extern "C" {
#include "my_logic.h"
#include "platform_gpio.h"
#include "platform_time.h"
}

static gpio_t testGpio;

TEST(StartState, BasicTest) {
    Controller controller;
    // Reset the timer tick
    platform_time_ResetTick();
    controller_init(&controller);
    controller_tick(&controller);
    testGpio.port = GPIOB;
    testGpio.pin = GPIO_PIN_0;
    EXPECT_EQ(platform_gpio_read_pin(testGpio), GPIO_LOW);

    // Increment tick count by 300
    platform_time_IncTick(300);
    controller_tick(&controller);

    // State duration is 250 therefore if tick time is 300 state should have been changed to high
    EXPECT_EQ(platform_gpio_read_pin(testGpio), GPIO_HIGH);
}

TEST(MultipleStateChange, BasicTest) {
    Controller controller;
    // Reset the timer tick
    platform_time_ResetTick();
    controller_init(&controller);
    controller_tick(&controller);
    testGpio.port = GPIOB;
    testGpio.pin = GPIO_PIN_0;
    EXPECT_EQ(platform_gpio_read_pin(testGpio), GPIO_LOW);

    // Increment tick count by 300
    platform_time_IncTick(800);
    controller_tick(&controller);

    // State duration is 250 therefore if tick time is 300 state should have been changed to high
    EXPECT_EQ(platform_gpio_read_pin(testGpio), GPIO_HIGH);

}