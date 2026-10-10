#[[
Author: Lachlan Wilson
File: source/CMakeLists.txt
Title: Source Directory CMake file

Brief: Source Directory CMake file is responsible for creating the app_logic library target with the appropriate logic source files.
This files will be linked to test/CMakeLists.txt and firmware/CMakeLists.txt such that test_my_code.cpp and firmware/Core/main.c can
include "my_logic.h" header
]]

# Create a library target for application logic (sources shared by Host and STM32 builds)
# hash_table_phone.c is legacy (contact-only) and defines the same symbols as hash_table.c -- never link it.
add_library(app_logic
    app/src/free_list_stack.c
    app/src/hash_table.c
    app/src/storage.c
    app/src/contact.c
    app/src/message.c
    app/src/journal.c
    app/src/usage_bitmap.c
    app/src/iterator.c
    app/src/ring_buffer.c
    app/src/crc.c
)

# Make the include directory PUBLIC so other files such as tests/test_my_logic.cpp and firmware/Core/Src/main.c
# have access to my_logic.h header  (can do #include "my_header.h")
target_include_directories(app_logic PUBLIC
    app/include
)

# Print compiler include hierarchy
#target_compile_options(app_logic PRIVATE -H)

if (BUILD_STM32)
    target_sources(app_logic PRIVATE
        app/src/crc_stm32.c
        app/src/sd_storage.c
        # db_main.c runs the contact + message DB hardware tests (journalling + hash-table reconstruction)
        app/src/db_main.c
        # LCD view of the db_main.c hardware unit tests
        app/src/htest_ui.c
        # SD bus read-integrity diagnostic (-DSD_BUS_TEST=ON runs it instead of db_main.c)
        app/src/sd_bus_test.c
        # AFR240320A0-2.0INTM LCD driver (over the FMC bus) + its on-hardware smoke tests
        app/src/fmc_func.c
        app/src/test_fmc.c
        app/src/lcd_font.c
        app/src/fmc_ui_test.c
    )

    # PRIVATE as only app sources #include "platform_*.h"
    target_link_libraries(app_logic PRIVATE platform_stm32)

    # Use the portable software CRC-32 on-device too: the STM32 CRC peripheral
    # path (crc_stm32.c) needs an MX_CRC_Init/hcrc that main.c does not set up,
    # and the journal CRCs must match the host implementation byte-for-byte.
    target_compile_definitions(app_logic PUBLIC USE_SW_CRC=1)

    # db_main.c persistence toggle:
    #   -DDB_TEST_WRITE=ON  (default) wipe + journalled writes + reconstruct/verify
    #   -DDB_TEST_WRITE=OFF            reconstruct/verify only, no writes to the card
    option(DB_TEST_WRITE "db_main.c runs the writing test pass" ON)
    if (DB_TEST_WRITE)
        target_compile_definitions(app_logic PUBLIC DB_TEST_WRITE=1)
    else()
        target_compile_definitions(app_logic PUBLIC DB_TEST_WRITE=0)
    endif()

    # -DSD_BUS_TEST=ON : main.c starts the SD bus diagnostic (sd_bus_test.c) instead of DB_Init()
    option(SD_BUS_TEST "Run the SD bus diagnostic instead of the DB tests" OFF)
    if (SD_BUS_TEST)
        target_compile_definitions(app_logic PUBLIC SD_BUS_TEST=1)
    endif()
else ()
    target_sources(app_logic PRIVATE
        app/src/heap_storage.c
        app/src/superheader.c
    )

    target_link_libraries(app_logic PRIVATE platform_host)
    target_compile_definitions(app_logic PUBLIC HOST_BUILD=1)
endif()
