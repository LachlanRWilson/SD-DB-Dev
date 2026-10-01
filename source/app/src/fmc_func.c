/*
 * fmc_func.c - LCD driver for the AFR240320A0-2.0INTM over the STM32H723
 * FMC bus. See fmc_func.h for the public API.
 *
 * The panel's 8/9/16/18-bit MCU interface is wired to FMC bank 1 (NE1) as
 * a 16-bit NOR/SRAM device (hsram1 in fmc.c): FMC_A0 drives the panel's
 * DCX/D-C pin, so bit 0 of the word address selects command vs. data
 * registers, and NOE/NWE/NE1 give the ST7789V 8080-system write cycle
 * from AFR240320A0-2.0INTM-spec.pdf section 7.1 for free. MX_FMC_Init()
 * must have already run before any function here is called.
 */
#include "FreeRTOS.h"
#include "cmsis_os2.h"

#include "fmc_func.h"

/* FMC bank 1 base address, split into the command (A0 = 0) and data
 * (A0 = 1) halves by the panel's DCX pin. */
#define LCD_CMD   (*((volatile uint16_t *)0x60000000))
#define LCD_DATA  (*((volatile uint16_t *)0x60000002))

static void lcd_write_cmd(uint16_t cmd)
{
    LCD_CMD = cmd;
}

static void lcd_write_data(uint16_t data)
{
    LCD_DATA = data;
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_write_cmd(0x2A); /* CASET - column address set */
    lcd_write_data(x0 >> 8);
    lcd_write_data(x0 & 0xFF);
    lcd_write_data(x1 >> 8);
    lcd_write_data(x1 & 0xFF);

    lcd_write_cmd(0x2B); /* RASET - row address set */
    lcd_write_data(y0 >> 8);
    lcd_write_data(y0 & 0xFF);
    lcd_write_data(y1 >> 8);
    lcd_write_data(y1 & 0xFF);

    lcd_write_cmd(0x2C); /* RAMWR - memory write: following data writes
                             fill the window in row-major order */
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_set_window(x, y, x, y);
    lcd_write_data(color);
}

void lcd_fill_color(uint16_t color)
{
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++)
    {
        lcd_write_data(color);
    }
}

void lcd_draw_image(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *pixels)
{
    lcd_set_window(x0, y0, (uint16_t)(x0 + width - 1), (uint16_t)(y0 + height - 1));

    for (uint32_t i = 0; i < (uint32_t)width * height; i++)
    {
        lcd_write_data(pixels[i]);
    }
}

void lcd_init(void)
{
    /* RESX isn't wired to a GPIO on this board (see fmc.c), so bring the
     * controller up with a software reset instead of a hardware pulse.
     * Datasheet section 7.5 note 7 requires Sleep Out to wait >=120ms
     * after a reset; osDelay() needs to run from task context (i.e.
     * after osKernelStart()), which is why lcd_init() is called from the
     * test task rather than directly in main(). */
    lcd_write_cmd(0x01); /* SWRESET */
    osDelay(150);

    lcd_write_cmd(0x11); /* SLPOUT */
    osDelay(120);

    lcd_write_cmd(0x3A); /* COLMOD - interface pixel format */
    lcd_write_data(0x55); /* 16bpp (RGB565) for both the MCU and RGB interfaces */

    lcd_write_cmd(0x36); /* MADCTL - memory access control (orientation) */
    lcd_write_data(0x00); /* If red/blue come out swapped on real hardware,
                              set bit 3 (0x08) to switch RGB<->BGR; bits
                              5-7 (0x20/0x40/0x80) mirror rows/columns. */

    lcd_write_cmd(0x21); /* INVON - display inversion on; most ST7789V IPS
                             panels need this for correct (non-negative)
                             colour. Drop it if colours look washed out. */
    osDelay(10);

    lcd_write_cmd(0x29); /* DISPON */
    osDelay(10);
}
