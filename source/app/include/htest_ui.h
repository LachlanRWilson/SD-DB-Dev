/*
 * htest_ui.h - LCD view of the hardware unit tests (db_main.c).
 *
 * Call order: htest_ui_init() once, then htest_ui_start()/htest_ui_result()
 * around each test, then htest_ui_done(). All calls must come from one task.
 */
#ifndef HTEST_UI_H
#define HTEST_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the LCD and draw the empty screen for a suite of `total` tests. */
void htest_ui_init(uint32_t total);

/* Show test `index` (0-based) as running. */
void htest_ui_start(uint32_t index, const char *name);

/* Show test `index` as passed, or failed at db_main.c line `fail_line`. */
void htest_ui_result(uint32_t index, bool pass, uint32_t fail_line);

/* Show the final summary. */
void htest_ui_done(uint32_t passed, uint32_t failed);

#endif /* HTEST_UI_H */
