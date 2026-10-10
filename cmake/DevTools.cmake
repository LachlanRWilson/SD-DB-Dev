#[[
Author: Lachlan Wilson
File: cmake/DevTools.cmake
Title: Developer tool targets

Brief: Included from the root CMakeLists.txt for both Host and STM32 builds.
- misra         : cppcheck + MISRA C:2012 addon over source/
- tags          : ctags index of the project (excludes build dirs and external/)
- struct_layout : print struct layouts / static assert results (tools/struct_layout.py)
Each target is skipped with a status message if its tool is not installed.
]]

# MISRA Checking (--target misra)
find_program(CPPCHECK cppcheck)
# Addon name or path to misra.py (cppcheck resolves bundled addons by name)
set(MISRA_ADDON "misra" CACHE STRING "cppcheck MISRA addon name or path to misra.py")

if (CPPCHECK)
    # --enable          : warning, style, performance and portability checks
    # --addon           : MISRA C:2012 rules
    # --inline-suppr    : allow /*cppcheck-suppress misra-c2012-11.4*/ deviation comments
    # --force           : keep analysing all configurations even when cppcheck hits problems
    # --std             : parse as C99 (change to match the compiler)
    # --platform=embedded can be added to reduce false MISRA violations
    add_custom_target(misra
        COMMAND ${CPPCHECK}
            --enable=warning,style,performance,portability
            --addon=${MISRA_ADDON}
            --inline-suppr
            --force
            --std=c99
            -v
            -I ${CMAKE_SOURCE_DIR}/source
            ${CMAKE_SOURCE_DIR}/source
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running MISRA C static analysis (cppcheck)"
    )
else ()
    message(STATUS "misra target disabled (cppcheck NOT FOUND)")
endif()

# Tags Target (--target tags)
find_program(CTAGS ctags)

if (CTAGS)
    add_custom_target(tags
        COMMAND ${CTAGS}
            --languages=C,C++
            --exclude=build
            --exclude=build-host
            --exclude=external
            -R
            ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
else ()
    message(STATUS "tags target disabled (ctags NOT FOUND)")
endif()

# Struct layout / static assert report (--target struct_layout)
# Always compiles with the host gcc, so it works from both Host and STM32 builds
find_package(Python3 COMPONENTS Interpreter)

if (Python3_Interpreter_FOUND)
    add_custom_target(struct_layout
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/struct_layout.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Printing struct layouts and static assert results"
    )
else ()
    message(STATUS "struct_layout target disabled (python3 NOT FOUND)")
endif()
