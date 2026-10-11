/*
 * lcd_log.c - scrolling text log on the LCD (see lcd_log.h).
 *
 * Rows use lcd_font.c scaled down by 2 (same scaling as htest_ui.c): each
 * 8x12 pixel is set when at least 2 of the 2x2 font pixels under it are.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fmc_func.h"
#include "lcd_font.h"
#include "lcd_log.h"

#define SMALL_W 8
#define SMALL_H 12

#define BAR_H     28
#define ROW_H     (SMALL_H + 1)
#define LIST_Y    BAR_H
#define LIST_ROWS ((LCD_HEIGHT - 2 * BAR_H) / ROW_H)
#define MAX_LINES 48

typedef struct
{
    char     text[LCD_LOG_ROW_CHARS + 1];
    uint16_t color;
} Line;

static Line     g_lines[MAX_LINES];
static uint32_t g_n_lines;

/* One full-width band, rendered then blitted with lcd_draw_image(). */
static uint16_t g_band[LCD_WIDTH * BAR_H];

static void band_fill(uint16_t h, uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * h; i++)
    {
        g_band[i] = color;
    }
}

static uint16_t glyph_index(char c)
{
    if (c < LCD_FONT_FIRST || c > LCD_FONT_LAST)
    {
        c = '?';
    }
    return (uint16_t)(c - LCD_FONT_FIRST);
}

/* Full-size (16x24) text at (x, y) inside the band. */
static void band_text(uint16_t x, uint16_t y, const char *s, uint16_t color)
{
    for (; *s != '\0' && x + LCD_FONT_WIDTH <= LCD_WIDTH; s++, x += LCD_FONT_WIDTH)
    {
        const uint16_t *g = lcd_font[glyph_index(*s)];
        for (uint16_t gy = 0; gy < LCD_FONT_HEIGHT; gy++)
        {
            for (uint16_t gx = 0; gx < LCD_FONT_WIDTH; gx++)
            {
                if (g[gy] & (0x8000U >> gx))
                {
                    g_band[(uint32_t)(y + gy) * LCD_WIDTH + x + gx] = color;
                }
            }
        }
    }
}

/* Half-size (8x12) text: a pixel is set when 2+ of the 2x2 font pixels under
 * it are. */
static void band_text_small(uint16_t x, uint16_t y, const char *s, uint16_t color)
{
    for (; *s != '\0' && x + SMALL_W <= LCD_WIDTH; s++, x += SMALL_W)
    {
        const uint16_t *g = lcd_font[glyph_index(*s)];
        for (uint16_t sy = 0; sy < SMALL_H; sy++)
        {
            uint16_t r0 = g[2 * sy];
            uint16_t r1 = g[2 * sy + 1];
            for (uint16_t sx = 0; sx < SMALL_W; sx++)
            {
                uint16_t m = (uint16_t)(0xC000U >> (2 * sx));
                int n = __builtin_popcount(r0 & m) + __builtin_popcount(r1 & m);
                if (n >= 2)
                {
                    g_band[(uint32_t)(y + sy) * LCD_WIDTH + x + sx] = color;
                }
            }
        }
    }
}

static void draw_bar(uint16_t y, const char *text, uint16_t color)
{
    band_fill(BAR_H, LOG_COLOR_BAR);
    band_text(8, (BAR_H - LCD_FONT_HEIGHT) / 2, text, color);
    lcd_draw_image(0, y, LCD_WIDTH, BAR_H, g_band);
}

/* Draw list slot `slot` (0 = top) showing line `index`, or blank. */
static void draw_row(uint32_t slot, uint32_t index)
{
    band_fill(ROW_H, LOG_COLOR_BG);
    if (index < g_n_lines)
    {
        band_text_small(0, 0, g_lines[index].text, g_lines[index].color);
    }
    lcd_draw_image(0, (uint16_t)(LIST_Y + slot * ROW_H), LCD_WIDTH, ROW_H, g_band);
}

void lcd_log_init(const char *title)
{
    g_n_lines = 0;
    lcd_init();
    lcd_fill_color(LOG_COLOR_BG);
    draw_bar(0, title, LOG_COLOR_TEXT);
}

void lcd_log_footer(const char *text, uint16_t color)
{
    draw_bar(LCD_HEIGHT - BAR_H, text, color);
}

/* Once the list is longer than the screen it scrolls so the newest row is at
 * the bottom. */
void lcd_log_line(uint16_t color, const char *fmt, ...)
{
    va_list args;

    if (g_n_lines >= MAX_LINES)
    {
        memmove(&g_lines[0], &g_lines[1], sizeof(Line) * (MAX_LINES - 1));
        g_n_lines--;
    }

    va_start(args, fmt);
    vsnprintf(g_lines[g_n_lines].text, sizeof(g_lines[0].text), fmt, args);
    va_end(args);
    g_lines[g_n_lines].color = color;
    g_n_lines++;

    if (g_n_lines <= LIST_ROWS)
    {
        draw_row(g_n_lines - 1, g_n_lines - 1);
        return;
    }

    for (uint32_t slot = 0; slot < LIST_ROWS; slot++)
    {
        draw_row(slot, g_n_lines - LIST_ROWS + slot);
    }
}
