/*
 * fmc_ui_test.c - scrolling list UI test for the AFR240320A0-2.0INTM LCD
 * (fmc_func.c/.h).
 *
 * Holds a list of FMC_UI_NAME_COUNT names and shows FMC_UI_VISIBLE_ROWS of
 * them at a time, one per horizontal band of the panel. Every
 * FMC_UI_SCROLL_MS the list scrolls up by one entry, wrapping from the last
 * name back to the first. Each scroll is animated pixel by pixel over
 * FMC_UI_ANIM_MS with an ease-in-out curve rather than jumping a whole row.
 *
 * The list is treated as one tall virtual strip of
 * FMC_UI_NAME_COUNT * FMC_UI_ROW_HEIGHT pixel rows that wraps around; a
 * frame is the LCD_HEIGHT rows of it starting at the current scroll
 * offset. Frames are rendered FMC_UI_STRIP_HEIGHT rows at a time into an
 * off-screen RGB565 buffer and blitted with lcd_draw_image().
 *
 * There is no tearing-effect (TE) sync with the panel, so some tearing
 * may be visible while a scroll is moving.
 */
#include "FreeRTOS.h"
#include "cmsis_os2.h"

#include <stdint.h>

#include "fmc_func.h"
#include "fmc_ui_test.h"
#include "lcd_font.h"

#define FMC_UI_NAME_COUNT   10
#define FMC_UI_VISIBLE_ROWS 5
#define FMC_UI_SCROLL_MS    1000
#define FMC_UI_ANIM_MS      300

#define FMC_UI_ROW_HEIGHT   (LCD_HEIGHT / FMC_UI_VISIBLE_ROWS)
#define FMC_UI_TEXT_X       16
#define FMC_UI_NAME_X       (FMC_UI_TEXT_X + 3 * LCD_FONT_WIDTH)
#define FMC_UI_TEXT_Y       ((FMC_UI_ROW_HEIGHT - LCD_FONT_HEIGHT) / 2)
#define FMC_UI_LIST_HEIGHT  ((uint32_t)FMC_UI_NAME_COUNT * FMC_UI_ROW_HEIGHT)
#define FMC_UI_STRIP_HEIGHT 64

#define FMC_UI_COLOR_BG_EVEN  LCD_RGB565(16, 24, 48)
#define FMC_UI_COLOR_BG_ODD   LCD_RGB565(32, 44, 80)
#define FMC_UI_COLOR_DIVIDER  LCD_RGB565(90, 100, 130)
#define FMC_UI_COLOR_INDEX    LCD_COLOR_YELLOW
#define FMC_UI_COLOR_NAME     LCD_COLOR_WHITE

static const char *const g_names[FMC_UI_NAME_COUNT] = {
    "Alice", "Ben", "Charlotte", "Daniel", "Emily",
    "Finn", "Grace", "Harry", "Isla", "Jack"};

/* FMC_UI_STRIP_HEIGHT rows of the screen, rendered then blitted at once. */
static uint16_t g_strip_buf[LCD_WIDTH * FMC_UI_STRIP_HEIGHT];

/* Writes row `gy` of each character of `s` into `line` (one LCD_WIDTH
 * pixel row), starting at column x. Characters outside the font's range
 * are drawn as '?'; pixels past the right edge are clipped. */
static void line_draw_text(uint16_t *line, uint16_t x, uint16_t gy, const char *s, uint16_t color)
{
    for (; *s != '\0'; s++, x += LCD_FONT_WIDTH)
    {
        char c = *s;
        if (c < LCD_FONT_FIRST || c > LCD_FONT_LAST)
        {
            c = '?';
        }

        uint16_t bits = lcd_font[c - LCD_FONT_FIRST][gy];
        for (uint16_t gx = 0; bits != 0 && gx < LCD_FONT_WIDTH; gx++, bits <<= 1)
        {
            uint16_t px = x + gx;
            if (px >= LCD_WIDTH)
            {
                return;
            }

            if (bits & 0x8000U)
            {
                line[px] = color;
            }
        }
    }
}

/* Renders one pixel row of the virtual list strip into `line`. `v` is the
 * row's position in the strip, 0 .. FMC_UI_LIST_HEIGHT - 1. */
static void render_list_line(uint16_t *line, uint32_t v)
{
    uint16_t name_index = (uint16_t)(v / FMC_UI_ROW_HEIGHT);
    uint16_t ry = (uint16_t)(v % FMC_UI_ROW_HEIGHT);

    uint16_t bg = (ry == FMC_UI_ROW_HEIGHT - 1)
                      ? FMC_UI_COLOR_DIVIDER /* 1px divider under each band */
                      : ((name_index & 1) ? FMC_UI_COLOR_BG_ODD : FMC_UI_COLOR_BG_EVEN);
    for (uint16_t x = 0; x < LCD_WIDTH; x++)
    {
        line[x] = bg;
    }

    if (ry < FMC_UI_TEXT_Y || ry >= FMC_UI_TEXT_Y + LCD_FONT_HEIGHT)
    {
        return;
    }
    uint16_t gy = ry - FMC_UI_TEXT_Y;

    /* 1-based list position, so the scroll is easy to follow on screen. */
    uint16_t number = name_index + 1;
    char index_str[3] = {
        (number >= 10) ? (char)('0' + number / 10) : ' ',
        (char)('0' + number % 10),
        '\0'};

    line_draw_text(line, FMC_UI_TEXT_X, gy, index_str, FMC_UI_COLOR_INDEX);
    line_draw_text(line, FMC_UI_NAME_X, gy, g_names[name_index], FMC_UI_COLOR_NAME);
}

/* Draws the whole screen with the list scrolled up by `scroll_px` rows. */
static void draw_frame(uint32_t scroll_px)
{
    for (uint16_t y0 = 0; y0 < LCD_HEIGHT; y0 += FMC_UI_STRIP_HEIGHT)
    {
        for (uint16_t y = 0; y < FMC_UI_STRIP_HEIGHT; y++)
        {
            render_list_line(&g_strip_buf[(uint32_t)y * LCD_WIDTH],
                             (scroll_px + y0 + y) % FMC_UI_LIST_HEIGHT);
        }

        lcd_draw_image(0, y0, LCD_WIDTH, FMC_UI_STRIP_HEIGHT, g_strip_buf);
    }
}

/* Cubic ease-in-out: maps t in [0, 1] to [0, 1], slow at both ends. */
static float ease_in_out(float t)
{
    if (t < 0.5f)
    {
        return 4.0f * t * t * t;
    }
    float u = 2.0f - 2.0f * t;
    return 1.0f - 0.5f * u * u * u;
}

void fmc_ui_test_run(void)
{
    _Static_assert(LCD_HEIGHT % FMC_UI_STRIP_HEIGHT == 0,
                   "strips must tile the screen exactly");

    uint32_t base_px = 0;
    uint32_t next_scroll = osKernelGetTickCount();

    draw_frame(base_px);

    for (;;)
    {
        next_scroll += FMC_UI_SCROLL_MS;
        osDelayUntil(next_scroll);

        /* Animate base_px -> base_px + one row, one frame per pass, timed
         * off the tick count so the speed doesn't depend on frame rate. */
        uint32_t anim_start = osKernelGetTickCount();
        uint32_t elapsed;
        do
        {
            elapsed = osKernelGetTickCount() - anim_start;
            if (elapsed > FMC_UI_ANIM_MS)
            {
                elapsed = FMC_UI_ANIM_MS;
            }

            float t = (float)elapsed / (float)FMC_UI_ANIM_MS;
            uint32_t offset = (uint32_t)(ease_in_out(t) * FMC_UI_ROW_HEIGHT + 0.5f);
            draw_frame((base_px + offset) % FMC_UI_LIST_HEIGHT);
        } while (elapsed < FMC_UI_ANIM_MS);

        base_px = (base_px + FMC_UI_ROW_HEIGHT) % FMC_UI_LIST_HEIGHT;
    }
}
