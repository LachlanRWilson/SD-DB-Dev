#[[
Author: Lachlan Wilson
File: tests/cmake/run_tests.cmake
Title: ctest wrapper for the run_tests target

Brief: Runs ctest on the build directory, filtered by the N environment variable (ctest -R regex) if set.
== Inputs ==
-DCTEST=<ctest path> -DBUILD_DIR=<build dir>, ENV{N} = optional test name regex
]]

set(args --test-dir ${BUILD_DIR} --output-on-failure)

if(NOT "$ENV{N}" STREQUAL "")
    list(APPEND args -R "$ENV{N}")
endif()

execute_process(COMMAND ${CTEST} ${args} RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "ctest failed")
endif()
