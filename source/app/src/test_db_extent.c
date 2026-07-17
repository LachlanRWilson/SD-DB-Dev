#include "test_db_extent.h"

#include <stdio.h>
#include <string.h>

#include "message_extent.h"
#include "free_list_stack.h"


/**
 * @brief Verify a message is equal.
 */
static bool message_equal(
    const Message *a,
    const Message *b)
{
    return
        a->timestamp == b->timestamp &&
        a->direction == b->direction &&
        strcmp(a->str, b->str) == 0;
}



/**
 * @brief Test allocating a new message extent.
 */
bool test_extent_allocate(MessageExtent *extent)
{

    // Allocate message extent (will init a block on the sd card as well)
    uint16_t idx = message_extent_get( extent, UINT16_MAX);


    if (idx == UINT16_MAX)
    {
        return false;
    }


    if (extent->num_extents != 1)
    {
        return false;
    }


    return true;
}



/**
 * @brief Test writing and reading a single message.
 */
bool test_single_message(MessageExtent *extent)
{

    // Get a message extent sector index
    uint16_t idx = message_extent_get( extent, UINT16_MAX);


    // Create a message
    Message tx = {0};

    tx.timestamp = 123;
    tx.direction = true;

    strcpy( tx.str, "Hello World");


    // append a message to the extent 
    if (!message_extent_append( extent, &idx, &tx))
    {
        return false;
    }



    MessageBlockBuffer block;


    //  read message block
    if (!extent->storage->read_block( extent->storage->context, idx, block.buffer))
    {
        return false;
    }



    // check the message block is equal to the appended one
    if (!message_equal( &tx, &block.var.messages[0]))
    {
        return false;
    }

    return true;
}




/**
 * @brief Test multiple messages in one extent.
 */
bool test_multiple_messages(MessageExtent *extent)
{

    // Get a message extent sector from allocator
    uint16_t idx = message_extent_get( extent, UINT16_MAX);


    Message msg = {0};


    // Append 20 messages to given message extent
    for(uint16_t i = 0; i < 20; i++)
    {
        msg.timestamp = i;

        sprintf( msg.str, "Message %u", i);


        // append message to message block
        if(!message_extent_append( extent, &idx, &msg))
        {
            return false;
        }
    }

    // get the count extent (should be for only one extent)
    uint16_t count = message_extent_count( extent, idx);

    // check the message block count
    if(count != 20)
    {
        return false;
    }

    return true;
}




/**
 * @brief Test allocation of a new extent when full.
 */
bool test_extent_chaining(MessageExtent *extent)
{

    uint16_t idx = message_extent_get( extent, UINT16_MAX);

    Message msg = {0};


    for(uint32_t i = 0; i < MESSAGE_BLOCK_CAPACITY + 10; i++)
    {
        msg.timestamp = i;

        if(!message_extent_append( extent, &idx, &msg))
        {
            return false;
        }
    }



    MessageBlockBuffer block;


    extent->storage->read_block( extent->storage->context, idx, block.buffer);

    if(block.var.header.prev == UINT16_MAX)
    {
        return false;
    }


    return true;
}




/**
 * @brief Test deleting a conversation.
 */
bool test_delete_conversation(MessageExtent *extent)
{

    uint16_t idx = message_extent_get( extent, UINT16_MAX);

    Message msg = {0};


    // create 10 messages
    for(int i = 0; i < 10; i++)
    {
        msg.timestamp = i;

        message_extent_append( extent, &idx, &msg);
    }

    // Need to check the message has been tombstoned
    if(!message_extent_delete( extent, idx))
    {
        return false;
    }

    // NOTE: Tombstone does not need to be checked as if it is of the FLS then it is free to be
    // overwritten (same affect as tombstoning)
    
    uint16_t recycled_idx = message_extent_get(extent, UINT16_MAX); 

    // check the extent sector has been pushed back onto the FLS correctly
    if (recycled_idx != idx) 
    {
        return false;
    }


    return true;
}




/**
 * @brief Stress test large conversation.
 */
bool test_large_conversation(MessageExtent *extent)
{
    printf("Running Large Conversation Test...\r\n");


    uint16_t idx =
        message_extent_get(
            extent,
            UINT16_MAX);


    Message msg = {0};



    for(uint32_t i = 0; i < 1000; i++)
    {
        msg.timestamp = i;

        if(!message_extent_append(
                extent,
                &idx,
                &msg))
        {
            printf("FAILED\r\n");
            return false;
        }
    }



    uint16_t count =
        message_extent_count(
            extent,
            idx);



    if(count != 1000)
    {
        printf(
            "FAILED COUNT %u\r\n",
            count);

        return false;
    }


    printf("PASSED\r\n");

    return true;
}




/**
 * @brief Execute all message extent tests.
 */
bool test_db_extents(MessageExtent *extent)
{
    bool pass = true;


    pass &= test_extent_allocate(extent);

    pass &= test_single_message(extent);

    pass &= test_multiple_messages(extent);

    pass &= test_extent_chaining(extent);

    pass &= test_delete_conversation(extent);

    pass &= test_large_conversation(extent);


    return pass;
}
