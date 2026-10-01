/*
 * test_fmc.c - on-hardware smoke test for the AFR240320A0-2.0INTM LCD
 * driver (fmc_func.c/.h).
 *
 * This is the FMC/display counterpart to db_main.c: a single FreeRTOS task
 * that brings the panel up over the FMC bus, fills the background, blits a
 * small generated test image into the centre of the screen, then reports
 * on the PB0 LED the same way db_main.c does:
 *   - fast continuous blink -> the draw sequence completed, check the panel
 *   - the LCD bus is write-only over FMC (no readback is attempted), so
 *     the real pass/fail signal is what actually shows up on the glass.
 */
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdint.h>

#include "fmc_func.h"
#include "test_fmc.h"

#define FMC_TEST_TASK_STACK_SIZE 2048
#define FMC_TEST_TASK_PRIORITY   osPriorityNormal

/* Small checkerboard test image, generated at run time rather than stored
 * as a large literal array - enough to prove lcd_draw_image() can blit an
 * arbitrary pixel buffer into a sub-window of the panel. */
#define TEST_IMAGE_SIZE  64
#define TEST_SQUARE_SIZE 8

static uint16_t g_test_image[TEST_IMAGE_SIZE * TEST_IMAGE_SIZE];

static void build_test_image(void)
{
    for (uint16_t y = 0; y < TEST_IMAGE_SIZE; y++)
    {
        for (uint16_t x = 0; x < TEST_IMAGE_SIZE; x++)
        {
            uint16_t square_x = x / TEST_SQUARE_SIZE;
            uint16_t square_y = y / TEST_SQUARE_SIZE;
            uint16_t color = ((square_x + square_y) & 1) ? LCD_COLOR_WHITE : LCD_COLOR_BLUE;

            g_test_image[(uint32_t)y * TEST_IMAGE_SIZE + x] = color;
        }
    }
}

static void blink_forever(void)
{
    for (;;)
    {
        GPIOB->ODR ^= (1U << 0);
        osDelay(150);
    }
}

static void fmcTestTask(void *arg)
{
    (void)arg;

    lcd_init();
    lcd_fill_color(LCD_COLOR_BLACK);

    build_test_image();

    uint16_t x0 = (LCD_WIDTH  - TEST_IMAGE_SIZE) / 2;
    uint16_t y0 = (LCD_HEIGHT - TEST_IMAGE_SIZE) / 2;
    lcd_draw_image(x0, y0, TEST_IMAGE_SIZE, TEST_IMAGE_SIZE, g_test_image);

    blink_forever();
}

void FMC_Test_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "FMC LCD Test",
        .stack_size = FMC_TEST_TASK_STACK_SIZE,
        .priority = FMC_TEST_TASK_PRIORITY};

    osThreadNew(fmcTestTask, NULL, &task_attr);
}
