#[[
Author: Lachlan Wilson
File: ./CMakeLists.txt
Title: Root Directory CMake file

Brief: Root CMake file handles Host and STM32 Build selections and adds appropriate sources depending on selection.
== Host Build (-DSTM32_BUILD=OFF) ==
CMake file added -> Target:
- tests/CMakeLists.txt -> test_my_logic
- source/CMakeLists.txt -> app_logic
- platform/host/CMakeLists.txt -> platform_host

== STM32 BUILD (-G Ninja -DCMAKE-TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DSTM32_BUILD=ON) ==
Cmake file added -> Target:
- firmware/CMakeLists.txt
- platform/stm32/CMakeLists.txt
- source/CMakeLists/txt
]]

cmake_minimum_required(VERSION 3.22)

# STM32 configure and generate argument checks (Toolchain and Generator)
if (BUILD_STM32)
    if (NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
        message(FATAL_ERROR
        "\nUSAGE ERROR: attempting to configure & generate STM32 build with incorrect arguments\n"
        "cmake -S <ROOT DIR> -B <BUILD DIR> -G Ninja -DCMAKE_TOOLCHAIN_FILE=\"firmware/cmake/gcc-arm-none-eabi.cmake\" -DBUILD_STM32=ON"
        )
    endif()
endif()

project(MyProject C CXX)

set(STM32_CLI "/home/lachlan-wilson/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI")

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------
# Options (STM32 or HOST)
# -------------------------

# Default -DBUILD_STM32=OFF
option(BUILD_STM32 "Build for STM32 target" OFF)


# ----------------------------------
# Platform selection (STM32 or HOST)
# ----------------------------------

# -DBUILD_STM32=ON
if(BUILD_STM32)

    message(STATUS "Building for STM32 platform")

    enable_language(ASM)

    # Run firmware/CMakeLists.txt
    add_subdirectory(firmware)

    # Run platform/stm32/CMakeLists.txt
    add_subdirectory(platform/stm32)

    # Flash Target (--target flash)
    add_custom_target(flash
        # Run STM32_Programmer_CLI
        COMMAND sudo ${STM32_CLI}
            -c port=SWD # Connection Type (SMD = Serial Wire Debug)
            -w $<TARGET_FILE:${CMAKE_PROJECT_NAME}> # Target File Name 
            -v # Verify flash
            -rst # Reset Device after flash
    DEPENDS ${CMAKE_PROJECT_NAME}  
        COMMENT "Flashing STM32 with: $<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    )

find_program(GDB_MULTIARCH gdb-multiarch REQUIRED)
find_program(OPENOCD openocd REQUIRED)
find_program(GNOME_TERMINAL gnome-terminal REQUIRED)

add_custom_target(server
    COMMAND ${GNOME_TERMINAL} -- bash -c
            "${OPENOCD} -f interface/stlink.cfg -f target/stm32h7x.cfg;exec bash"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Starting OpenOCD server..."
)

add_custom_target(debug
    DEPENDS ${CMAKE_PROJECT_NAME} server

    COMMAND ${CMAKE_COMMAND} -E sleep 2

    COMMAND ${GDB_MULTIARCH}
            ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
            -ex "target extended-remote localhost:3333"

    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Starting GDB..."
)

else()
    # -DBUILD_STM32=OFF
    message(STATUS "Building for Host platform")

    # Run platform/host/CMakeLists.txt
    add_subdirectory(platform/host) 

    enable_testing()

    # Run tests/CMakeLists.txt
    add_subdirectory(tests) 
endif()

# App logic (source/my_logic.c)

# Run source/CMakeLists.txt
add_subdirectory(source) 

# MISRA Checking (cppcheck)
find_program(CPPCHECK cppcheck)

if (CPPCHECK) 
    # Add MISRA target that can be called when building code
    add_custom_target(misra
    COMMAND ${CPPCHECK}
        # Enable cppcheck categories: warnaing, stype, performance and portability
            --enable=warning,style,performance,portability
            # Load cppcheck MISRA C:2012 addon (Enable MISRA Checks) (USER DEFINED PATH) 
            --addon="C:/Program Files/Cppcheck/addons/misra.py"
            # Enable inline suppression comments "/*cppcheck-supress misra-c2012-11.4*/" for deviations from MISRA
            --inline-suppr
            # Force cppcheck to analyse files even when it runs into problems
            --force
            # Parse as C99 code (Change to appropriate C compiler version)
            --std=c99
            # Change some cppcheck assumptions (reduce false MISRA violations)
            #--platform=embedded
            -v
            # Check Source and Platform Code
            -I ${CMAKE_SOURCE_DIR}/source
            # Files to be analysed
            ${CMAKE_SOURCE_DIR}/source
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running MISRA C static analysis (cppcheck)"
    )
else ()
    # Message is cppcheck is not found
    message(STATUS "MISRA C static analysis error (cppcheck NOT FOUND)")
endif()

add_custom_target(tags
    COMMAND ctags
        --languages=C,C++
        --exclude=build
        --exclude=build-host
        --exclude=external
        -R
        ${CMAKE_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
