/*
 * htest_ui.c - LCD view of the hardware unit tests (db_main.c).
 *
 *   +--------------------------+
 *   | HW UNIT TESTS            |  header, full-size font (16x24)
 *   |--------------------------|
 *   | 1 PhoneHashIgnore... PASS|  one row per test, half-size font (8x12),
 *   | 2 InsertContact      PASS|  newest at the bottom; scrolls once the
 *   | 3 FindContact        ... |  list is longer than the screen
 *   |--------------------------|
 *   | 41/41 PASSED             |  footer: progress, then the summary
 *   +--------------------------+
 *
 * The rows use lcd_font.c scaled down by 2: each 8x12 pixel is set when at
 * least 2 of the 2x2 font pixels under it are, which keeps the bold strokes
 * without filling in the counters.
 */
#include <stdio.h>
#include <string.h>

#include "fmc_func.h"
#include "lcd_font.h"
#include "htest_ui.h"

#define SMALL_W 8
#define SMALL_H 12

#define HEADER_H  32
#define FOOTER_H  32
#define ROW_H     (SMALL_H + 2)
#define LIST_Y    HEADER_H
#define LIST_ROWS ((LCD_HEIGHT - HEADER_H - FOOTER_H) / ROW_H)
#define ROW_CHARS (LCD_WIDTH / SMALL_W)
#define STATUS_CHARS 4 /* "PASS" / "FAIL" / "...." */

#define MAX_TESTS 64

#define COLOR_BG      LCD_RGB565(16, 24, 48)
#define COLOR_BAR     LCD_RGB565(32, 44, 80)
#define COLOR_TEXT    LCD_COLOR_WHITE
#define COLOR_DIM     LCD_RGB565(150, 160, 190)
#define COLOR_RUN     LCD_COLOR_YELLOW
#define COLOR_PASS    LCD_RGB565(60, 220, 90)
#define COLOR_FAIL    LCD_RGB565(255, 70, 70)
#define COLOR_ROW_RUN LCD_RGB565(60, 60, 20)
#define COLOR_ROW_FAIL LCD_RGB565(80, 20, 20)

typedef enum { ST_EMPTY = 0, ST_RUN, ST_PASS, ST_FAIL } RowState;

typedef struct
{
    const char *name;
    RowState    state;
    uint16_t    fail_line;
} Row;

static Row      g_rows[MAX_TESTS];
static uint32_t g_total;
static uint32_t g_shown; /* rows started so far */

/* One full-width band, rendered then blitted with lcd_draw_image(). */
static uint16_t g_band[LCD_WIDTH * FOOTER_H];

/* ------------------------------------------------------------------ *
 *  Text into g_band
 * ------------------------------------------------------------------ */
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

/* Half-size (8x12) text at (x, y) inside the band. */
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

/* ------------------------------------------------------------------ *
 *  Screen parts
 * ------------------------------------------------------------------ */
static void draw_header(void)
{
    band_fill(HEADER_H, COLOR_BAR);
    band_text(8, (HEADER_H - LCD_FONT_HEIGHT) / 2, "HW UNIT TESTS", COLOR_TEXT);
    lcd_draw_image(0, 0, LCD_WIDTH, HEADER_H, g_band);
}

static void draw_footer(const char *text, uint16_t color)
{
    band_fill(FOOTER_H, COLOR_BAR);
    band_text(8, (FOOTER_H - LCD_FONT_HEIGHT) / 2, text, color);
    lcd_draw_image(0, LCD_HEIGHT - FOOTER_H, LCD_WIDTH, FOOTER_H, g_band);
}

/* Draw list slot `slot` (0 = top) showing test `index`, or blank. */
static void draw_row(uint32_t slot, uint32_t index)
{
    const Row *r = (index < g_shown) ? &g_rows[index] : NULL;
    uint16_t bg = COLOR_BG;
    const char *status = "";
    uint16_t status_color = COLOR_TEXT;
    char line[ROW_CHARS + 1];

    if (r != NULL)
    {
        switch (r->state)
        {
        case ST_RUN:  bg = COLOR_ROW_RUN;  status = "...."; status_color = COLOR_RUN;  break;
        case ST_PASS:                      status = "PASS"; status_color = COLOR_PASS; break;
        case ST_FAIL: bg = COLOR_ROW_FAIL; status = "FAIL"; status_color = COLOR_FAIL; break;
        default: break;
        }
    }

    band_fill(ROW_H, bg);

    if (r != NULL)
    {
        /* "NN name" truncated to leave room for the status column. */
        int name_chars = ROW_CHARS - STATUS_CHARS - 1;
        snprintf(line, sizeof(line), "%2lu %-*.*s", (unsigned long)(index + 1),
                 name_chars - 3, name_chars - 3, r->name);
        band_text_small(0, 1, line, (r->state == ST_PASS) ? COLOR_DIM : COLOR_TEXT);
        band_text_small((ROW_CHARS - STATUS_CHARS) * SMALL_W, 1, status, status_color);
    }

    lcd_draw_image(0, (uint16_t)(LIST_Y + slot * ROW_H), LCD_WIDTH, ROW_H, g_band);
}

/* Redraw the list so the newest started test is on the bottom slot once the
 * list overflows the screen. */
static void draw_list(void)
{
    uint32_t first = (g_shown > LIST_ROWS) ? g_shown - LIST_ROWS : 0;

    for (uint32_t slot = 0; slot < LIST_ROWS; slot++)
    {
        draw_row(slot, first + slot);
    }
}

/* ------------------------------------------------------------------ *
 *  API
 * ------------------------------------------------------------------ */
void htest_ui_init(uint32_t total)
{
    char text[40];

    memset(g_rows, 0, sizeof(g_rows));
    g_total = total;
    g_shown = 0;

    lcd_init();
    lcd_fill_color(COLOR_BG);
    draw_header();
    snprintf(text, sizeof(text), "0/%lu", (unsigned long)total);
    draw_footer(text, COLOR_TEXT);
}

void htest_ui_start(uint32_t index, const char *name)
{
    char text[40];

    if (index >= MAX_TESTS)
    {
        return;
    }

    g_rows[index].name = name;
    g_rows[index].state = ST_RUN;
    if (index + 1 > g_shown)
    {
        g_shown = index + 1;
    }
    draw_list();

    snprintf(text, sizeof(text), "%lu/%lu", (unsigned long)(index + 1), (unsigned long)g_total);
    draw_footer(text, COLOR_RUN);
}

void htest_ui_result(uint32_t index, bool pass, uint32_t fail_line)
{
    if (index >= MAX_TESTS)
    {
        return;
    }

    g_rows[index].state = pass ? ST_PASS : ST_FAIL;
    g_rows[index].fail_line = (uint16_t)fail_line;
    draw_list();
}

void htest_ui_done(uint32_t passed, uint32_t failed)
{
    char text[40];

    if (failed == 0)
    {
        snprintf(text, sizeof(text), "%lu/%lu PASSED", (unsigned long)passed, (unsigned long)g_total);
        draw_footer(text, COLOR_PASS);
        return;
    }

    /* Point at the first failure: its number and the db_main.c line. */
    snprintf(text, sizeof(text), "%lu FAILED", (unsigned long)failed);
    for (uint32_t i = 0; i < g_shown; i++)
    {
        if (g_rows[i].state == ST_FAIL)
        {
            snprintf(text, sizeof(text), "%lu FAIL #%lu L%u", (unsigned long)failed,
                     (unsigned long)(i + 1), g_rows[i].fail_line);
            break;
        }
    }
    draw_footer(text, COLOR_FAIL);
}
