/*
 * lcd_log.h - scrolling text log on the LCD, used by the on-hardware demos
 * (persist_demo.c, mh_demo.c).
 *
 *   +--------------------------+
 *   | TITLE                    |  header, full-size font (16x24)
 *   |--------------------------|
 *   | line                     |  one row per line, half-size font (8x12),
 *   | ...                      |  scrolls once longer than the screen
 *   |--------------------------|
 *   | footer                   |  status / result
 *   +--------------------------+
 *
 * All calls must come from one task.
 */
#ifndef LCD_LOG_H
#define LCD_LOG_H

#include <stdint.h>

#include "fmc_func.h"

/* Characters that fit on one row. */
#define LCD_LOG_ROW_CHARS (LCD_WIDTH / 8)

#define LOG_COLOR_BG      LCD_RGB565(16, 24, 48)
#define LOG_COLOR_BAR     LCD_RGB565(32, 44, 80)
#define LOG_COLOR_TEXT    LCD_COLOR_WHITE
#define LOG_COLOR_DIM     LCD_RGB565(150, 160, 190)
#define LOG_COLOR_SECTION LCD_COLOR_CYAN
#define LOG_COLOR_RUN     LCD_COLOR_YELLOW
#define LOG_COLOR_PASS    LCD_RGB565(60, 220, 90)
#define LOG_COLOR_FAIL    LCD_RGB565(255, 70, 70)

/* Bring up the LCD, clear it and draw the header. */
void lcd_log_init(const char *title);

/* Append a printf-style row; the newest row stays at the bottom. */
void lcd_log_line(uint16_t color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Redraw the footer bar. */
void lcd_log_footer(const char *text, uint16_t color);

#endif /* LCD_LOG_H */
