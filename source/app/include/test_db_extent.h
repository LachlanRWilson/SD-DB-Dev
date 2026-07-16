#ifndef TEST_MESSAGE_EXTENT_H
#define TEST_MESSAGE_EXTENT_H

#include <stdbool.h>

#include "message_extent.h"


bool test_extent_create(MessageExtent *manager);

bool test_extent_append(MessageExtent *manager);

bool test_extent_multiple_blocks(MessageExtent *manager);

bool test_extent_delete(MessageExtent *manager);

bool test_extent_run(MessageExtent *manager);


#endif
