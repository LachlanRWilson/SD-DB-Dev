#[[
Author: Lachlan Wilson
File: source/benchmark/name_index/CMakeLists.txt
Title: Ordered name index benchmark (sorted array vs AVL tree)

== Host Build ==
    cmake -S . -B build-bench-host -DCMAKE_BUILD_TYPE=Release
Targets : test_name_index (gtest), name_index_bench_host (see scripts/run_host.sh)

== STM32 Build ==
    cmake -S . -B build-bench -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON -DNAME_INDEX_BENCH=ON -DCMAKE_BUILD_TYPE=Release
main.c then starts NameIndexBench_Init() instead of DB_Init() (see RESULTS.md for running it)
]]

# The benchmark compiles the indexes with one spare slot so "insert into a
# structure already holding N" can be timed at N = 10,000
set(NI_BENCH_CAPACITY 10001)

if (CMAKE_BUILD_TYPE)
    string(TOUPPER ${CMAKE_BUILD_TYPE} NI_BUILD_TYPE)
endif()
set(NI_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${NI_BUILD_TYPE}}")
string(STRIP "${NI_C_FLAGS}" NI_C_FLAGS)
string(REGEX MATCH "-O[0-3sgz]" NI_OPT_LEVEL "${CMAKE_C_FLAGS_${NI_BUILD_TYPE}}")
if (NOT NI_OPT_LEVEL)
    set(NI_OPT_LEVEL "-O0")
endif()

set(NI_INDEX_SOURCES sorted_array.c avl_tree.c)
set(NI_BENCH_SOURCES name_index_bench.c bench_names.c ${NI_INDEX_SOURCES})

if (NOT BUILD_STM32)
    # Index library at the default capacity (NI_MAX_CONTACTS = 10000) for the unit tests
    add_library(name_index ${NI_INDEX_SOURCES})
    target_include_directories(name_index PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    include(GoogleTest)
    add_executable(test_name_index test_name_index.cpp)
    target_include_directories(test_name_index PRIVATE ${gtest_SOURCE_DIR}/include)
    target_link_libraries(test_name_index gtest gtest_main name_index)
    gtest_discover_tests(test_name_index)

    # Host benchmark: same harness, steady clock instead of DWT (non-authoritative)
    add_executable(name_index_bench_host ${NI_BENCH_SOURCES})
    target_include_directories(name_index_bench_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(name_index_bench_host PRIVATE
        NI_MAX_CONTACTS=${NI_BENCH_CAPACITY} NI_C_FLAGS="${NI_C_FLAGS}" NI_OPT_LEVEL="${NI_OPT_LEVEL}")
    return()
endif()

# -DNAME_INDEX_BENCH=ON : main.c starts the name index benchmark instead of DB_Init()
option(NAME_INDEX_BENCH "Run the name index benchmark instead of the DB tests" OFF)
if (NOT NAME_INDEX_BENCH)
    return()
endif()

add_library(name_index_bench ${NI_BENCH_SOURCES} sd_baseline.c)
target_include_directories(name_index_bench PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(name_index_bench PRIVATE
    NI_TARGET=1 NI_MAX_CONTACTS=${NI_BENCH_CAPACITY} NI_C_FLAGS="${NI_C_FLAGS}" NI_OPT_LEVEL="${NI_OPT_LEVEL}")
target_compile_definitions(name_index_bench PUBLIC NAME_INDEX_BENCH=1)
target_link_libraries(name_index_bench PRIVATE app_logic platform_stm32 stm32cubemx)

# Link into the firmware executable (created in firmware/CMakeLists.txt)
target_link_libraries(${CMAKE_PROJECT_NAME} name_index_bench)
