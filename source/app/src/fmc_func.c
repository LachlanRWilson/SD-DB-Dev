/*
 * fmc_func.c - LCD driver for the AFR240320A0-2.0INTM over the STM32H723
 * FMC bus. See fmc_func.h for the public API.
 *
 * The panel's 8/9/16/18-bit MCU interface is wired to FMC bank 1 as a
 * 16-bit NOR/SRAM device (hsram1 in fmc.c): FMC_A16 (PD11) drives the
 * panel's DCX/D-C pin, so address bit 16 selects command vs. data
 * registers, and NOE/NWE give the ST7789V 8080-system write cycle from
 * AFR240320A0-2.0INTM-spec.pdf section 7.1 for free. The panel's CSX
 * (PD7) and RESX (PD3) are plain GPIOs. MX_GPIO_Init() and MX_FMC_Init()
 * must have already run, and the MPU must allow access to 0x60000000
 * (see main.c), before any function here is called.
 */
#include "FreeRTOS.h"
#include "cmsis_os2.h"

#include "main.h"
#include "fmc_func.h"

/* FMC bank 1 base address, split into the command (A16 = 0) and data
 * (A16 = 1) halves by the panel's DCX pin. With a 16-bit bus the FMC
 * drives HADDR[25:1] onto A[24:0], so A16 is byte-address bit 17. */
#define LCD_CMD   (*((volatile uint16_t *)0x60000000))
#define LCD_DATA  (*((volatile uint16_t *)(0x60000000 | (1UL << 17))))

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
    /* CSX is a GPIO and the panel is the only device on the bus, so
     * just hold it selected. Then pulse RESX (>=10us low, datasheet
     * section 7.5) followed by a software reset for good measure.
     * Datasheet section 7.5 note 7 requires Sleep Out to wait >=120ms
     * after a reset; osDelay() needs to run from task context (i.e.
     * after osKernelStart()), which is why lcd_init() is called from a
     * task rather than directly in main(). */
    HAL_GPIO_WritePin(FMC_CS_GPIO_Port, FMC_CS_Pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(FMC_RES_GPIO_Port, FMC_RES_Pin, GPIO_PIN_RESET);
    osDelay(10);
    HAL_GPIO_WritePin(FMC_RES_GPIO_Port, FMC_RES_Pin, GPIO_PIN_SET);
    osDelay(120);

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
