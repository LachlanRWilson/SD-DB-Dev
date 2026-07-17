#ifndef TEST_DB_MAIN_H
#define TEST_DB_MAIN_H

#include <stdbool.h>

#include "hash_table.h"

/**
 * @brief Execute all database tests.
 *
 * Runs each database test in sequence and prints the
 * result of each test over the debug console.
 *
 * @param table Pointer to an initialized hash table.
 *
 * @return true if every test passes.
 * @return false if one or more tests fail.
 */
bool test_db_run(HashTable *table, Storage *storage);

#endif /* TEST_DB_H */
