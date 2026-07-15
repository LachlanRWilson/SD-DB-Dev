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
bool test_db_run(HashTable *table);

/**
 * @brief Compare two contact buffers for equality.
 *
 * Compares all fields of two ContactBuffer structures, including
 * the contact ID, string lengths, name and phone number.
 *
 * @param a Pointer to the first contact buffer.
 * @param b Pointer to the second contact buffer.
 *
 * @return true if both contacts are identical.
 * @return false if any field differs.
 */
static bool contact_equal(const ContactBuffer *a, const ContactBuffer *b);

#endif /* TEST_DB_H */
