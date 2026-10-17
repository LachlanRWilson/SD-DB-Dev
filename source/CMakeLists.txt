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
    app/src/message_history.c
    app/src/call_history.c
    app/src/sorted_array.c
    app/src/superheader.c
    app/src/database.c
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
        app/hw_tests/src/db_main.c
        # LCD view of the db_main.c hardware unit tests
        app/hw_tests/src/htest_ui.c
        # SD bus read-integrity diagnostic (-DSD_BUS_TEST=ON runs it instead of db_main.c)
        app/hw_tests/src/sd_bus_test.c
        # SD persistence demo on the LCD (-DPERSIST_DEMO=ON runs it instead of db_main.c)
        app/demos/src/persist_demo.c
        # Message history hardware unit tests (-DMH_TEST=ON runs them instead of db_main.c)
        app/hw_tests/src/mh_main.c
        # Message history ring buffer demo on the LCD (-DMH_DEMO=ON runs it instead of db_main.c)
        app/demos/src/mh_demo.c
        # Whole database walk-through on the LCD, stepped with the USER button (-DDB_DEMO=ON runs it instead of db_main.c)
        app/demos/src/db_demo.c
        # Sorted array hardware unit tests (-DSA_TEST=ON runs them instead of db_main.c)
        app/hw_tests/src/sa_main.c
        # Database API hardware unit tests (-DDATABASE_TEST=ON runs them instead of db_main.c)
        app/hw_tests/src/database_main.c
        # Scrolling text log on the LCD, shared by the demos
        app/src/lcd_log.c
        # AFR240320A0-2.0INTM LCD driver (over the FMC bus) + its on-hardware smoke tests
        app/src/fmc_func.c
        app/hw_tests/src/test_fmc.c
        app/src/lcd_font.c
        app/hw_tests/src/fmc_ui_test.c
    )

    # On-hardware unit tests and LCD demos live in their own folders; PUBLIC so
    # firmware/Core/Src/main.c and freertos.c can still #include their headers
    target_include_directories(app_logic PUBLIC
        app/hw_tests/include
        app/demos/include
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

    # -DPERSIST_DEMO=ON : main.c starts the persistence demo (persist_demo.c) instead of DB_Init()
    #   -DPERSIST_DEMO_WRITE=ON  (default) write the demo data set to the card, showing each record
    #   -DPERSIST_DEMO_WRITE=OFF           read it back from the card and show it
    option(PERSIST_DEMO "Run the SD persistence demo instead of the DB tests" OFF)
    option(PERSIST_DEMO_WRITE "persist_demo.c writes (ON) or reads (OFF) the card" ON)
    if (PERSIST_DEMO)
        target_compile_definitions(app_logic PUBLIC PERSIST_DEMO=1)
    endif()
    if (PERSIST_DEMO_WRITE)
        target_compile_definitions(app_logic PUBLIC PERSIST_DEMO_WRITE=1)
    else()
        target_compile_definitions(app_logic PUBLIC PERSIST_DEMO_WRITE=0)
    endif()

    # -DMH_TEST=ON : main.c starts the message history hardware unit tests (mh_main.c) instead of DB_Init()
    option(MH_TEST "Run the message history hardware unit tests instead of the DB tests" OFF)
    if (MH_TEST)
        target_compile_definitions(app_logic PUBLIC MH_TEST=1)
    endif()

    # -DSA_TEST=ON : main.c starts the sorted array hardware unit tests (sa_main.c) instead of DB_Init()
    option(SA_TEST "Run the sorted array hardware unit tests instead of the DB tests" OFF)
    if (SA_TEST)
        target_compile_definitions(app_logic PUBLIC SA_TEST=1)
    endif()

    # -DDATABASE_TEST=ON : main.c starts the database API hardware unit tests (database_main.c) instead of DB_Init()
    option(DATABASE_TEST "Run the database API hardware unit tests instead of the DB tests" OFF)
    if (DATABASE_TEST)
        target_compile_definitions(app_logic PUBLIC DATABASE_TEST=1)
    endif()

    # -DDB_DEMO=ON : main.c starts the database demo (db_demo.c) instead of DB_Init()
    option(DB_DEMO "Run the whole database demo instead of the DB tests" OFF)
    if (DB_DEMO)
        target_compile_definitions(app_logic PUBLIC DB_DEMO=1)
    endif()

    # -DMH_DEMO=ON : main.c starts the message history demo (mh_demo.c) instead of DB_Init()
    #   -DMH_DEMO_WRITE=ON  (default) fill (and wrap) a small history ring buffer on the card
    #   -DMH_DEMO_WRITE=OFF           rebuild it from the card and page through it with get_range
    option(MH_DEMO "Run the message history demo instead of the DB tests" OFF)
    option(MH_DEMO_WRITE "mh_demo.c writes (ON) or reads (OFF) the card" ON)
    if (MH_DEMO)
        target_compile_definitions(app_logic PUBLIC MH_DEMO=1)
    endif()
    if (MH_DEMO_WRITE)
        target_compile_definitions(app_logic PUBLIC MH_DEMO_WRITE=1)
    else()
        target_compile_definitions(app_logic PUBLIC MH_DEMO_WRITE=0)
    endif()
else ()
    target_sources(app_logic PRIVATE
        app/src/heap_storage.c
    )

    target_link_libraries(app_logic PRIVATE platform_host)
    target_compile_definitions(app_logic PUBLIC HOST_BUILD=1)
endif()

# Benchmarks (own targets, never part of app_logic): see source/benchmark/
add_subdirectory(benchmark)

# Host demos (own targets, never part of app_logic): see source/demo/
add_subdirectory(demo)
