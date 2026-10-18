#ifndef PHONE_UI_H
#define PHONE_UI_H

#include <stdint.h>

#include "db_task.h"

/**
 * @brief Full screen terminal rendering of the UniQOS phone menus (Phone, SMS,
 * Contacts) backed by the database task. Arrow keys move, Enter selects,
 * Backspace goes back and q returns to the db> prompt. Needs an interactive
 * terminal.
 *
 * @param task running database task (db_init)
 */
void phone_ui_run(DatabaseTaskContext *task);

// Provided by db_cli.c
const char *strg_name(STRG_RET ret);
uint32_t next_ts(void);

#endif /* PHONE_UI_H */
