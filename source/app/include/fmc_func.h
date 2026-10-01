/*
 * fmc_func.h - LCD driver for the AFR240320A0-2.0INTM (ST7789V controller)
 * over the STM32H723 FMC NOR/SRAM bus (8080 parallel interface, bank 1,
 * NE1). See firmware/Core/Src/fmc.c for the bus timing/GPIO setup and
 * AFR240320A0-2.0INTM-spec.pdf section 7.1 for the 8080 write-cycle timing
 * this is driving.
 */
#ifndef FMC_FUNC_H
#define FMC_FUNC_H

#include <stdint.h>

/* Panel resolution (AFR240320A0-2.0INTM-spec.pdf section 1, 240(RGB)*320). */
#define LCD_WIDTH  240
#define LCD_HEIGHT 320

/* RGB565 helpers for the 16-bit colour mode (COLMOD 0x55). */
#define LCD_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_MAGENTA 0xF81F

/* Runs the ST7789V power-up/init command sequence over the FMC bus. Must
 * be called from a task context (it uses osDelay()), after MX_FMC_Init()
 * has brought up the SRAM1 bank. */
void lcd_init(void);

/* Sets the active drawing window (inclusive pixel bounds) and leaves the
 * controller ready to accept pixel data (RAMWR already issued). */
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Fills the whole panel with a single RGB565 colour. */
void lcd_fill_color(uint16_t color);

/* Writes a single RGB565 pixel at (x, y). */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/* Blits a width x height RGB565 pixel buffer (row-major, no stride padding)
 * into the panel with its top-left corner at (x0, y0). The caller owns
 * `pixels` and must size it to width * height entries. */
void lcd_draw_image(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *pixels);

#endif /* FMC_FUNC_H */
