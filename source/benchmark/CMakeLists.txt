#[[
Author: Lachlan Wilson
File: source/benchmark/CMakeLists.txt
Title: Benchmark Directory CMake file

Brief: Benchmarks are kept out of app_logic; each one has its own targets.
]]

add_subdirectory(name_index)
add_subdirectory(database)
