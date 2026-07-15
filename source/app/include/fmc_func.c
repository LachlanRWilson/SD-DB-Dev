#ifndef FMC_FUNC_H
#define FMC_FUNC_H

#include <stdint.h>

// --- LCD dimensions ---
#define LCD_WIDTH   240
#define LCD_HEIGHT  320

// --- RGB565 color definitions ---
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_BLACK   0x0000

// --- Public API ---

/**
 * @brief Initialize ST7789V display via FMC
 */
void lcd_init(void);

/**
 * @brief Fill entire screen with a single color
 * @param color RGB565 color
 */
void lcd_fill_color(uint16_t color);


#endif // FMC_FUNC_H
