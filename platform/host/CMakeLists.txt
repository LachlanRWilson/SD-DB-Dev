#[[
Author: Lachlan Wilson
File: platform/host/CMakeLists.txt
Title: Host Directory CMake file

Brief: Host Platform Abstractin Layer (PAL) mocks MCU
]]

# Create a library for the host platform code 
add_library(platform_host
    # User mock source files

    # GPIO Mock
    src/platform_gpio_host.c 

    # DELAY Mock
    src/platform_time_host.c  

    # FreeRTOS queue/task Mock (pthreads)
    src/platform_os_host.c
)

find_package(Threads REQUIRED)
target_link_libraries(platform_host PUBLIC
    Threads::Threads
)

# Make the include directory PUBLIC so other files such as tests/test_my_logic.cpp and source/app/my_logic.c have access to header
# (can do: #include "platform_*.h")
target_include_directories(platform_host PUBLIC 
    include
)