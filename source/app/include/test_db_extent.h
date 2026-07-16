#ifndef TEST_DB_EXTENTS_H
#define TEST_DB_EXTENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "message_extent.h"


/**
 * @brief Run all message extent hardware tests.
 *
 * @param extent Initialized message extent manager.
 *
 * @return true if all tests pass.
 */
bool test_db_extents(MessageExtent *extent);


#ifdef __cplusplus
}
#endif

#endif
