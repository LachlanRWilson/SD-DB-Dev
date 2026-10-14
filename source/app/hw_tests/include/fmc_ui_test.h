#ifndef FMC_UI_TEST_H
#define FMC_UI_TEST_H

/* Runs the scrolling name-list UI test on the LCD: shows 5 of a 10-name
 * list and scrolls by one entry every second. Must be called from a task
 * after lcd_init(); never returns. See fmc_ui_test.c. */
void fmc_ui_test_run(void);

#endif /* FMC_UI_TEST_H */
