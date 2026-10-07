#[[
Author: Lachlan Wilson
File: tests/cmake/test_help.cmake
Title: Usage printer for the test_help target

Brief: Prints how to call the run_tests and test_debug targets, and lists each test executable with its test suites.
== Inputs ==
-DTEST_EXES=<|-separated test executable paths>
]]

string(REPLACE "|" ";" TEST_EXES "${TEST_EXES}")

message([=[

== run_tests ==  Build and run unit tests with ctest
    cmake --build build-host --target run_tests
    N=<regex> cmake --build build-host --target run_tests

    N   optional, ctest -R regex on test names (e.g. N=RingBuffer, N=JournalTest.Replay)

== test_debug ==  Build and debug a single unit test in gdb
    N=<Suite.Name> [F=<test_file>] cmake --build build-host --target test_debug

    N   required, gtest name Suite.Name - breakpoint set on its TestBody and only it is run
    F   optional, test executable or source file (e.g. test_journal, test_journal_edge.cpp)
        auto-detected from N if omitted

== test_help ==  Show this message
    cmake --build build-host --target test_help

== Test executables and suites ==]=])

foreach(exe ${TEST_EXES})
    get_filename_component(exe_name "${exe}" NAME)
    execute_process(COMMAND ${exe} --gtest_list_tests OUTPUT_VARIABLE listing ERROR_QUIET)

    # Suite names are the unindented identifier lines ending in "." (skips the gtest_main banner)
    string(REGEX MATCHALL "\n[A-Za-z0-9_/]+\\.\n" suites "\n${listing}")
    string(REPLACE "\n" "" suites "${suites}")
    string(REPLACE "." "" suites "${suites}")
    list(JOIN suites ", " suites)

    message("    ${exe_name}: ${suites}")
endforeach()

message("\nList every test name with: ctest --test-dir build-host -N\n")
