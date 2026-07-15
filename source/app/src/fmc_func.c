#include "stdint.h"

// FMC base addresses
#define LCD_CMD   (*((volatile uint16_t *)0x60000000)) // A0 = 0
#define LCD_DATA  (*((volatile uint16_t *)0x60000002)) // A0 = 1

// Basic colors (RGB565)
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F

#define LCD_WIDTH   240
#define LCD_HEIGHT  320

// --- Low-level write ---
// STM32 HAL FMC with handle all the other pins, i.e, WRX, CRW, etc
static void lcd_write_cmd(uint16_t cmd)
{
    LCD_CMD = cmd;
}

static void lcd_write_data(uint16_t data)
{
    LCD_DATA = data;
}

// --- Set drawing window ---
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    // Column address set
    lcd_write_cmd(0x2A);
    lcd_write_data(x0 >> 8);
    lcd_write_data(x0 & 0xFF);
    lcd_write_data(x1 >> 8);
    lcd_write_data(x1 & 0xFF);

    // Row address set
    lcd_write_cmd(0x2B);
    lcd_write_data(y0 >> 8);
    lcd_write_data(y0 & 0xFF);
    lcd_write_data(y1 >> 8);
    lcd_write_data(y1 & 0xFF);

    // Memory write
    lcd_write_cmd(0x2C);
}

// --- Fill screen ---
void lcd_fill_color(uint16_t color)
{
    uint32_t i;
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    for (i = 0; i < (LCD_WIDTH * LCD_HEIGHT); i++)
    {
        lcd_write_data(color);
    }
}

// --- Basic init sequence ---
void lcd_init(void)
{
    // Software reset
    lcd_write_cmd(0x01);
    for (volatile int i = 0; i < 1000000; i++);

    // Sleep out
    lcd_write_cmd(0x11);
    for (volatile int i = 0; i < 1000000; i++);

    // Color mode: 16-bit
    lcd_write_cmd(0x3A);
    lcd_write_data(0x55);

    // Memory access control (orientation)
    lcd_write_cmd(0x36);
    lcd_write_data(0x00);

    // Display ON
    lcd_write_cmd(0x29);
}

