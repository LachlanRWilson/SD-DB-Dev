#[[
Author: Lachlan Wilson
File: cmake/Stm32Tools.cmake
Title: STM32 flash / debug targets

Brief: Included from the root CMakeLists.txt for STM32 builds only.
- flash  : write the ELF over SWD with STM32_Programmer_CLI, verify and reset
- server : start an OpenOCD (ST-Link) server in a new gnome-terminal
- debug  : start the server, then attach gdb-multiarch to localhost:3333
Each target is skipped with a status message if its tool is not installed.
]]

find_program(STM32_CLI STM32_Programmer_CLI
    HINTS $ENV{HOME}/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin
)
find_program(GDB_MULTIARCH gdb-multiarch)
find_program(OPENOCD openocd)
find_program(GNOME_TERMINAL gnome-terminal)

# Flash Target (--target flash)
if (STM32_CLI)
    add_custom_target(flash
        # -c port=SWD: Serial Wire Debug, -w: file to write, -v: verify, -rst: reset after flash
        COMMAND sudo ${STM32_CLI} -c port=SWD -w $<TARGET_FILE:${CMAKE_PROJECT_NAME}> -v -rst
        DEPENDS ${CMAKE_PROJECT_NAME}
        COMMENT "Flashing STM32 with: $<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    )
else ()
    message(STATUS "flash target disabled (STM32_Programmer_CLI NOT FOUND)")
endif()

# OpenOCD Server Target (--target server)
if (OPENOCD AND GNOME_TERMINAL)
    add_custom_target(server
        COMMAND ${GNOME_TERMINAL} -- bash -c
                "${OPENOCD} -f interface/stlink.cfg -f target/stm32h7x.cfg;exec bash"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Starting OpenOCD server..."
    )
else ()
    message(STATUS "server target disabled (openocd or gnome-terminal NOT FOUND)")
endif()

# GDB Debug Target (--target debug)
if (GDB_MULTIARCH AND TARGET server)
    add_custom_target(debug
        DEPENDS ${CMAKE_PROJECT_NAME} server
        COMMAND ${CMAKE_COMMAND} -E sleep 2
        COMMAND ${GDB_MULTIARCH}
                $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
                -ex "target extended-remote localhost:3333"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Starting GDB..."
    )
else ()
    message(STATUS "debug target disabled (gdb-multiarch or server target NOT FOUND)")
endif()
