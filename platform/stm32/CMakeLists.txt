#[[
Author: Lachlan Wilson
File: platform/stm32/CMakeLists.txt
Title: STM32 PAL Directory CMake file

Brief: Platform Abstraction Layer (PAL) for STM32H723xx MCU
]]

# Create a library for the host platform code 
add_library(platform_stm32
    # User source files

    # GPIO
    src/platform_gpio_stm32.c

    # SysTick (HAL_GetTick())
    src/platform_time_stm32.c
)

# Make the include directory PUBLIC so other files such as tests/test_my_logic.cpp and source/app/my_logic.c have access to it
# (can do #include "platform.h")
target_include_directories(platform_stm32 PUBLIC 
    include
)

# Linking all the driver libraries to platform_stm32
target_link_libraries(platform_stm32 PUBLIC 
    stm32cubemx
)