#[[
Author: Lachlan Wilson
File: source/benchmark/database/CMakeLists.txt
Title: Database API benchmark (database_* calls through journal + storage)

== Host Build ==
    cmake -S . -B build-bench-host -DCMAKE_BUILD_TYPE=Release
Targets : database_bench_host, database_bench_host_quick (see scripts/run_host.sh)

== STM32 Build ==
    cmake -S . -B build-dbbench-Release -G Ninja -DCMAKE_TOOLCHAIN_FILE="firmware/cmake/gcc-arm-none-eabi.cmake" -DBUILD_STM32=ON -DDATABASE_BENCH=ON -DDATABASE_BENCH_NAME=contacts -DCMAKE_BUILD_TYPE=Release
The firmware is linked with -Wl,--wrap=DB_Init, so main.c's DB_Init() call starts
the benchmark task instead (no changes to main.c). See RESULTS.md for running it.
]]

if (CMAKE_BUILD_TYPE)
    string(TOUPPER ${CMAKE_BUILD_TYPE} DBB_BUILD_TYPE)
endif()
set(DBB_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${DBB_BUILD_TYPE}}")
string(STRIP "${DBB_C_FLAGS}" DBB_C_FLAGS)
string(REGEX MATCH "-O[0-3sgz]" DBB_OPT_LEVEL "${CMAKE_C_FLAGS_${DBB_BUILD_TYPE}}")
if (NOT DBB_OPT_LEVEL)
    set(DBB_OPT_LEVEL "-O0")
endif()

set(DBB_SOURCES database_bench.c bench_common.c bench_contacts.c bench_messages.c bench_calls.c bench_init.c)
set(DBB_DEFS DB_BENCH_C_FLAGS="${DBB_C_FLAGS}" DB_BENCH_OPT_LEVEL="${DBB_OPT_LEVEL}")

if (NOT BUILD_STM32)
    # Host benchmark: heap_storage (RAM) instead of the SD card (non-authoritative)
    add_executable(database_bench_host ${DBB_SOURCES})
    target_compile_definitions(database_bench_host PRIVATE ${DBB_DEFS})
    target_link_libraries(database_bench_host PRIVATE app_logic)

    # Small sweep for smoke testing the harness
    add_executable(database_bench_host_quick ${DBB_SOURCES})
    target_compile_definitions(database_bench_host_quick PRIVATE ${DBB_DEFS} DB_BENCH_QUICK=1)
    target_link_libraries(database_bench_host_quick PRIVATE app_logic)
    return()
endif()

# -DDATABASE_BENCH=ON : the firmware runs one database benchmark instead of DB_Init()
option(DATABASE_BENCH "Run a database API benchmark instead of the DB tests" OFF)
set(DATABASE_BENCH_NAME "contacts" CACHE STRING "Database benchmark to run on the target")
option(DATABASE_BENCH_QUICK "Small sweep for smoke testing the target harness" OFF)
if (NOT DATABASE_BENCH)
    return()
endif()
if (NAME_INDEX_BENCH)
    message(FATAL_ERROR "DATABASE_BENCH and NAME_INDEX_BENCH cannot both be ON")
endif()

add_library(database_bench OBJECT ${DBB_SOURCES})
target_compile_definitions(database_bench PRIVATE ${DBB_DEFS}
    DB_BENCH_TARGET=1 DB_BENCH_NAME="${DATABASE_BENCH_NAME}" DB_BENCH_QUICK=$<BOOL:${DATABASE_BENCH_QUICK}>)
target_link_libraries(database_bench PRIVATE app_logic platform_stm32 stm32cubemx)

# Link into the firmware executable (created in firmware/CMakeLists.txt) and
# route main.c's DB_Init() call to __wrap_DB_Init() in database_bench.c
target_link_libraries(${CMAKE_PROJECT_NAME} database_bench)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--wrap=DB_Init)
