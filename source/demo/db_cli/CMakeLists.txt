#[[
Author: Lachlan Wilson
File: source/demo/db_cli/CMakeLists.txt
Title: Interactive database demo (host only)

== Host Build ==
    cmake -S . -B build-host
    cmake --build build-host --target db_cli
Run : build-host/source/demo/db_cli/db_cli [--image card.img] [--script file] [--interactive] [--echo]
See README.md for the command list.
]]

# The demo runs database.c over heap storage, so it only exists in the host build
if (BUILD_STM32)
    return()
endif()

add_executable(db_cli
    db_cli.c
    card_image.c
)

target_link_libraries(db_cli
    app_logic
    platform_host
)
