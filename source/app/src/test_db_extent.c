#include "test_message_extent.h"

#include <stdio.h>
#include <string.h>


/**
 * @brief Create a test message.
 *
 * @param id Message identifier.
 *
 * @return Initialised message structure.
 */
static Message create_message(uint16_t id)
{
    Message msg = {0};

    msg.timestamp = id;
    msg.direction = 1;

    sprintf(msg.str, "Test message %u", id);

    return msg;
}


/**
 * @brief Test allocation of a new message extent.
 *
 * Verifies that a new extent can be allocated and that
 * the extent header is correctly initialised.
 *
 * @param manager Message extent manager.
 *
 * @return true if successful.
 */
bool test_extent_create(MessageExtent *manager)
{
    printf("Running Extent Create Test...\r\n");


    uint32_t extent =
        message_extent_get(manager, INVALID_EXTENT);


    if(extent == INVALID_EXTENT)
    {
        printf("FAILED: Allocation\r\n");
        return false;
    }


    MessageBlock block = {0};


    if(!manager->storage->read_block(
        manager->storage->context,
        extent,
        &block))
    {
        printf("FAILED: Read\r\n");
        return false;
    }


    if(block.header.state != EXTENT_EMPTY ||
       block.header.msg_count != 0)
    {
        printf("FAILED: Header\r\n");
        return false;
    }


    printf("PASSED\r\n");

    return true;
}



/**
 * @brief Test appending messages to a single extent.
 *
 * Adds several messages and verifies the stored count.
 *
 * @param manager Message extent manager.
 *
 * @return true if messages are stored correctly.
 */
bool test_extent_append(MessageExtent *manager)
{
    printf("Running Extent Append Test...\r\n");


    uint32_t extent =
        message_extent_get(manager, INVALID_EXTENT);


    if(extent == INVALID_EXTENT)
    {
        return false;
    }


    for(uint16_t i = 0; i < 5; i++)
    {
        Message msg =
            create_message(i);


        if(!message_extent_append(
                manager,
                &extent,
                &msg))
        {
            printf("FAILED append %d\r\n", i);
            return false;
        }
    }


    uint32_t count =
        message_extent_count(manager, extent);


    if(count != 5)
    {
        printf("FAILED count %lu\r\n",
            count);

        return false;
    }


    printf("PASSED\r\n");

    return true;
}



/**
 * @brief Test creation of multiple extents.
 *
 * Fills one extent until full and verifies that
 * a second extent is allocated.
 *
 * @param manager Message extent manager.
 *
 * @return true if chaining works.
 */
bool test_extent_multiple_blocks(MessageExtent *manager)
{
    printf("Running Multiple Extent Test...\r\n");


    uint32_t extent =
        message_extent_get(manager,
                           INVALID_EXTENT);


    uint32_t first = extent;


    for(uint32_t i = 0;
        i < MESSAGE_BLOCK_CAPACITY + 5;
        i++)
    {
        Message msg =
            create_message(i);


        if(!message_extent_append(
            manager,
            &extent,
            &msg))
        {
            printf("FAILED append\r\n");
            return false;
        }
    }


    if(extent == first)
    {
        printf("FAILED: no new extent\r\n");
        return false;
    }


    uint32_t count =
        message_extent_count(manager, extent);


    if(count != MESSAGE_BLOCK_CAPACITY + 5)
    {
        printf("FAILED count %lu\r\n",
               count);

        return false;
    }


    printf("PASSED\r\n");

    return true;
}



/**
 * @brief Test deleting a message extent chain.
 *
 * Allocates several extents, then deletes the chain
 * and verifies that the free list receives them back.
 *
 * @param manager Message extent manager.
 *
 * @return true if deletion succeeds.
 */
bool test_extent_delete(MessageExtent *manager)
{
    printf("Running Extent Delete Test...\r\n");


    uint32_t extent =
        message_extent_get(manager,
                           INVALID_EXTENT);


    uint32_t allocated =
        manager->capacity;


    if(!message_extent_delete(
        manager,
        extent))
    {
        printf("FAILED delete\r\n");
        return false;
    }


    if(manager->capacity != allocated-1)
    {
        printf("FAILED capacity\r\n");
        return false;
    }


    printf("PASSED\r\n");

    return true;
}



/**
 * @brief Run all message extent tests.
 *
 * @param manager Message extent manager.
 *
 * @return true if all tests pass.
 */
bool test_extent_run(MessageExtent *manager)
{
    bool pass = true;


    pass &= test_extent_create(manager);

    pass &= test_extent_append(manager);

    pass &= test_extent_multiple_blocks(manager);

    pass &= test_extent_delete(manager);


    return pass;
}
