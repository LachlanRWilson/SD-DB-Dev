#[[
Author: Lachlan Wilson
File: tests/CMakeLists.txt
Title: Tests Directory CMake file

Brief: Tests Directory CMake file is responsible for linking and creating the unit test executable
== Commands ==
Quick Test:
    ctest -C Debug
Verbose Test:
    ctest -C Debug -V
Usage help for the test targets:
    cmake --build build-host --target test_help
Run tests (N = optional ctest -R regex):
    N=<test_name> cmake --build build-host --target run_tests
Debug a single test in gdb (F = optional test executable/source file, auto-detected if omitted):
    N=<Suite.Name> F=<test_file> cmake --build build-host --target test_debug
]]

# Make sure we are building host tests only (redundant check)
if(BUILD_STM32)
    message(STATUS "Skipping tests: not building for host")
    return()
endif()

# Find GoogleTest (GTest)
include(GoogleTest)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/googletest # actual source location of gtest
                 ${CMAKE_BINARY_DIR}/external/googletest_build # where build files of gtest are located
                 )

# Include GTest Directories
include_directories(
    ${gtest_SOURCE_DIR}/include
    ${gmock_SOURCE_DIR}/include
)

add_executable(test_hash_table
    test_hash_table.cpp
    test_hash_table_edge.cpp
    test_hash_table_iterator.cpp
)

add_executable(test_free_list
    test_free_list_stack.cpp
    test_free_list_stack_edge.cpp
)

add_executable(test_usage_bitmap
    test_usage_bitmap.cpp
    test_usage_bitmap_edge.cpp
)

add_executable(test_journal
    test_journal.cpp
    test_journal_edge.cpp
)

add_executable(test_ring_buffer
    test_ring_buffer.cpp
)

add_executable(test_sector_crc
    test_sector_crc.cpp
)

add_executable(test_db_recovery
    test_db_recovery.cpp
)

set(UNIT_TESTS
    test_hash_table
    test_free_list
    test_usage_bitmap
    test_journal
    test_ring_buffer
    test_sector_crc
    test_db_recovery
)

foreach(test ${UNIT_TESTS})
    target_link_libraries(${test}
        gtest
        gtest_main
        app_logic
        platform_host
    )

    gtest_discover_tests(${test})
endforeach()

# Run Tests Target (N=<test_name> cmake --build build-host --target run_tests)
add_custom_target(run_tests
    COMMAND ${CMAKE_COMMAND}
        -DCTEST=${CMAKE_CTEST_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_tests.cmake
    DEPENDS ${UNIT_TESTS}
    USES_TERMINAL
    COMMENT "Running unit tests"
)

# Executable paths are joined with | since ; would split the custom command argument
set(UNIT_TEST_EXES "")
foreach(test ${UNIT_TESTS})
    string(APPEND UNIT_TEST_EXES "$<TARGET_FILE:${test}>|")
endforeach()

# Test Help Target (cmake --build build-host --target test_help)
add_custom_target(test_help
    COMMAND ${CMAKE_COMMAND}
        -DTEST_EXES=${UNIT_TEST_EXES}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_help.cmake
    DEPENDS ${UNIT_TESTS}
    VERBATIM
)

# Debug Test Target (N=<Suite.Name> F=<test_file> cmake --build build-host --target test_debug)
find_program(GDB gdb)

if (GDB)

    add_custom_target(test_debug
        COMMAND ${CMAKE_COMMAND}
            -DGDB=${GDB}
            -DTEST_EXES=${UNIT_TEST_EXES}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_debug.cmake
        DEPENDS ${UNIT_TESTS}
        USES_TERMINAL
        VERBATIM
        COMMENT "Debugging unit test in gdb"
    )
else ()
    message(STATUS "test_debug target disabled (gdb NOT FOUND)")
endif()
