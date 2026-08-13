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

# Test Files
add_executable(test_my_logic
    # hash table unit testing
    test_hash_table.cpp
    #test_hash_table_edge.cpp
    test_free_list_stack.cpp
    #test_message_extent.cpp
    #test_hash_table_messages.cpp
)

# Link libraries
target_link_libraries(test_my_logic
    gtest
    gtest_main

    # User Libraries
    # Application Logic
    app_logic   

    # Host PAL
    platform_host 
)

# Enable CTest
enable_testing()

# Unit Tests (NAME <name> COMMAND <executable>)
gtest_discover_tests(test_my_logic)
