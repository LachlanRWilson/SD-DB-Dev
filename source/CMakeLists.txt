#[[
Author: Lachlan Wilson
File: source/CMakeLists.txt
Title: Source Directory CMake file

Brief: Source Directory CMake file is responsible for creating the app_logic library target with the appropriate logic source files.
This files will be linked to test/CMakeLists.txt and firmware/CMakeLists.txt such that test_my_code.cpp and firmware/Core/main.c can
include "my_logic.h" header
]]

# Create a library target for appilcaion logic
if (BUILD_STM32)
add_library(app_logic
    # Add App Source Files
    #
    # db_main.c runs the phone-keyed DB hardware tests (journalling +
    # hash-table reconstruction), so the STM32 build now links the same
    # phone-keyed implementation the host tests use, plus the journal /
    # usage-bitmap / storage helpers it depends on.
    app/src/free_list_stack.c
    app/src/hash_table_phone.c
    app/src/storage.c
    app/src/contact.c
    app/src/journal.c
    app/src/usage_bitmap.c
    app/src/crc.c
    app/src/crc_stm32.c
    app/src/sd_storage.c
    app/src/db_main.c
)

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

else ()
add_library(app_logic
    # Add App Source Files
    app/src/free_list_stack.c
    # hash_table.c (ID-keyed) and hash_table_phone.c (phone-keyed) define
    # the same symbols (hash_init, hash_size, hash_phone, ...). The host
    # build / tests use the phone-keyed implementation only.
    app/src/hash_table_phone.c
    app/src/message_extent.c
    app/src/storage.c
    app/src/heap_storage.c
    app/src/journal.c
    app/src/crc.c
    app/src/superheader.c
    app/src/usage_bitmap.c
    app/src/contact.c
    app/src/message.c
    #app/src/database.c
)
endif()

# Print compiler include hierarchy
#target_compile_options(app_logic PRIVATE -H)

# Make the include directory PUBLIC so other files such as tests/test_my_logic.cpp and firmware/Core/Src/main.c
# have access to my_logic.h header  (can do #include "my_header.h")
target_include_directories(app_logic PUBLIC
    app/include
)

# If building for STM32 link STM32 Platform else link Host Platform (PRIVATE as only my_logic.c #include "platform_*h" header)
if (BUILD_STM32)

    # Link STM32 Platform Library
    target_link_libraries(app_logic PRIVATE
        platform_stm32
    )
else()

    # Link Host Platform Library
    target_link_libraries(app_logic PRIVATE
        platform_host
    )
    target_compile_definitions(app_logic PUBLIC HOST_BUILD=1)
    endif()
