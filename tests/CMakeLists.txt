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

add_executable(test_hash_table
    test_hash_table.cpp
    #test_hash_table_edge.cpp
)

add_executable(test_free_list
    test_free_list_stack.cpp
)

add_executable(test_message_extent
    test_message_extent.cpp
)

add_executable(test_usage_bitmap
    test_usage_bitmap.cpp
)

add_executable(test_journal
    test_journal.cpp
    #    test_journal_edge.cpp
)

foreach(test
    test_hash_table
    test_free_list
    test_message_extent
    test_usage_bitmap
    test_journal
)
    target_link_libraries(${test}
        gtest
        gtest_main
        app_logic
        platform_host
    )

    gtest_discover_tests(${test})
endforeach()
