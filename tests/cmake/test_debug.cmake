#[[
Author: Lachlan Wilson
File: tests/cmake/test_debug.cmake
Title: gdb launcher for the test_debug target

Brief: Opens a unit test executable in gdb, breaks at the start of a single gtest test body and runs only that test.
== Inputs ==
-DGDB=<gdb path> -DTEST_EXES=<|-separated test executable paths>
ENV{N} = gtest test name (Suite.Name), required
ENV{F} = test executable or source file (e.g. test_journal or test_journal_edge.cpp), optional - auto-detected from N if not given
]]

string(REPLACE "|" ";" TEST_EXES "${TEST_EXES}")

set(test_name "$ENV{N}")
set(test_file "$ENV{F}")

if(test_name STREQUAL "")
    message(FATAL_ERROR "\nUSAGE: N=<Suite.Name> [F=<test_file>] cmake --build <build dir> --target test_debug\n"
                        "See: cmake --build <build dir> --target test_help")
endif()

# Returns TRUE in out_var if the executable contains a test matching test_name
function(exe_has_test exe out_var)
    execute_process(
        COMMAND ${exe} --gtest_list_tests --gtest_filter=${test_name}
        OUTPUT_VARIABLE listing
        ERROR_QUIET
    )
    # Test names are listed indented by two spaces beneath their suite
    if(listing MATCHES "\n  [^ \n]")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Use F directly if it names a test executable
set(exe "")
if(NOT test_file STREQUAL "")
    get_filename_component(test_file "${test_file}" NAME_WE)
    foreach(candidate ${TEST_EXES})
        get_filename_component(candidate_name "${candidate}" NAME_WE)
        if(candidate_name STREQUAL test_file)
            set(exe "${candidate}")
        endif()
    endforeach()
endif()

# Otherwise find the executable that contains the test (also handles F being a source file like test_journal_edge.cpp)
if(exe STREQUAL "")
    set(matches "")
    foreach(candidate ${TEST_EXES})
        exe_has_test(${candidate} found)
        if(found)
            list(APPEND matches "${candidate}")
        endif()
    endforeach()

    list(LENGTH matches match_count)
    if(match_count EQUAL 0)
        message(FATAL_ERROR "No test executable contains a test matching '${test_name}'")
    elseif(NOT match_count EQUAL 1)
        message(FATAL_ERROR "'${test_name}' matches tests in multiple executables, choose one with F=<test_file>:\n${matches}")
    endif()
    set(exe "${matches}")
endif()

# TEST(Suite, Name) generates Suite_Name_Test::TestBody (strip any Prefix/ and /index from parameterised tests)
set(gdb_break "")
if(test_name MATCHES "^([^./]*/)?([A-Za-z0-9_]+)\\.([A-Za-z0-9_]+)(/.*)?$")
    set(gdb_break -ex "break ${CMAKE_MATCH_2}_${CMAKE_MATCH_3}_Test::TestBody")
else()
    message(WARNING "'${test_name}' is not a single Suite.Name, no breakpoint set")
endif()

get_filename_component(exe_dir "${exe}" DIRECTORY)
message(STATUS "Debugging ${test_name} in ${exe}")

execute_process(
    COMMAND ${GDB} -q
        -ex "set breakpoint pending on"
        ${gdb_break}
        -ex "run"
        --args ${exe} --gtest_filter=${test_name}
    WORKING_DIRECTORY ${exe_dir}
)
