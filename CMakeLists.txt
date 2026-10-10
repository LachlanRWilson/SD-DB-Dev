#[[
Author: Lachlan Wilson
File: ./CMakeLists.txt
Title: Root Directory CMake file

Brief: Selects the Host or STM32 build and adds the matching subdirectories.

== Host Build ==
    cmake -S . -B build-host
Subdirectories : platform/host, tests, source
Targets        : run_tests, test_help, test_debug (see tests/CMakeLists.txt)

== STM32 Build ==
    cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON
Subdirectories : firmware, platform/stm32, source
Targets        : flash, server, debug (see cmake/Stm32Tools.cmake)

== Both ==
Targets        : misra, tags, struct_layout (see cmake/DevTools.cmake)
]]

cmake_minimum_required(VERSION 3.22)

option(BUILD_STM32 "Build for STM32 target" OFF)

# STM32 configure and generate argument checks (Toolchain and Generator)
if (BUILD_STM32 AND (NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT CMAKE_GENERATOR STREQUAL "Ninja"))
    message(FATAL_ERROR
        "\nUSAGE ERROR: attempting to configure & generate STM32 build with incorrect arguments\n"
        "cmake -S <ROOT DIR> -B <BUILD DIR> -G Ninja -DCMAKE_TOOLCHAIN_FILE=\"firmware/cmake/gcc-arm-none-eabi.cmake\" -DBUILD_STM32=ON"
    )
endif()

project(MyProject C CXX)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if (BUILD_STM32)
    message(STATUS "Building for STM32 platform")
    enable_language(ASM)
    add_subdirectory(firmware)
    add_subdirectory(platform/stm32)
    include(Stm32Tools)
else ()
    message(STATUS "Building for Host platform")
    add_subdirectory(platform/host)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(source)

include(DevTools)
