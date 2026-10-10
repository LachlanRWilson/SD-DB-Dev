/*
 * sd_bus_test.c - SD bus read-integrity diagnostic.
 *
 * Some written sectors read back with HAL_SD_ERROR_DATA_CRC_FAIL and the
 * data slipped by one nibble (one 4-bit bus clock) part-way through the
 * block. This task narrows down why by changing one bus setting at a time:
 *
 *   1. Write known patterns to a scratch range far past the database
 *      region, read each sector back many times and record whether the read
 *      failed, where the first wrong byte is, and which way the data slipped.
 *   2. Repeat the reads under each bus mode (see BusMode).
 *   3. Read (never write) the real database sectors that failed, under each
 *      mode, keeping the first 64 bytes of the first clean read.
 *
 * Slip direction:
 * (In MODE_BUS_1BIT a slip is one bit, so the nibble classification doesn't
 * apply there; use the ok/crc_fail counts.)
 *
 *   NIBBLE_DROP   - got[j] == exp[j] << 4 | exp[j+1] >> 4 : the host lost a
 *                   nibble, i.e. the card saw an extra CK edge.
 *   NIBBLE_INSERT - got[j] == exp[j-1] << 4 | exp[j] >> 4 : the host gained a
 *                   nibble, i.e. the card missed a CK edge.
 *
 * Results go in g_sd_bus_report, read over SWD while the board runs:
 *   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 <&g_sd_bus_report> <size>
 *
 * Enable with -DSD_BUS_TEST=ON (main.c then starts this instead of DB_Init()).
 */

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdmmc.h"
#include "sd_bus_test.h"

extern SD_HandleTypeDef hsd1;

/* Scratch range: far past the database region (which ends ~32k sectors). */
#define SCRATCH_START_SECTOR 1000000u
#define SECTORS_PER_PATTERN  8u
#define READS_PER_SECTOR     50u

#define BLOCK_BYTES 512u
#define MAX_OFFSETS 16u
#define REPORT_MAGIC 0x5DB05701u

#define BUS_TEST_STACK_SIZE (4 * 1024)

/* CK is PC12; D1-D3 are PC9-PC11 (see sdmmc.c). */
#define CK_PIN 12u

typedef enum
{
    MODE_NORMAL = 0,  /* CubeMX config: HWFC on, ClockDiv 3 (16MHz), very-high slew, no D1-D3 pull-ups */
    MODE_IRQ_OFF,     /* NORMAL, but interrupts disabled for the whole read                            */
    MODE_BUS_1BIT,    /* 1-bit bus: only D0 switches, so no simultaneous DAT edges                     */
    MODE_SLOW_CLK,    /* ClockDiv 8 (6MHz)                                                             */
    MODE_VSLOW_CLK,   /* ClockDiv 48 (1MHz)                                                            */
    MODE_CK_MEDIUM,   /* CK pin slew rate dropped to medium                                            */
    MODE_PULLUPS,     /* pull-ups on D1-D3                                                             */
    N_MODES
} BusMode;

typedef enum
{
    PAT_ZERO = 0,     /* 0x00 - no data-line transitions          */
    PAT_ONES,         /* 0xFF - no data-line transitions          */
    PAT_ALT,          /* 0x5A - every line toggles every clock    */
    PAT_INCR,         /* (i + sector) & 0xFF                      */
    PAT_RANDOM,       /* xorshift32 seeded per sector             */
    N_PATTERNS
} Pattern;

typedef struct
{
    uint32_t reads;
    uint32_t ok;              /* HAL_OK and data matched                         */
    uint32_t crc_fail;        /* HAL_SD_ERROR_DATA_CRC_FAIL                       */
    uint32_t other_err;       /* any other HAL error                              */
    uint32_t silent_mismatch; /* HAL_OK but data wrong                            */
    uint32_t crc_data_ok;     /* CRC failure but every data byte matched          */
    uint32_t slip_drop;       /* failing read classified NIBBLE_DROP              */
    uint32_t slip_insert;     /* failing read classified NIBBLE_INSERT            */
    uint32_t slip_other;      /* failing read with some other corruption          */
    uint32_t last_err;        /* last non-zero hsd1.ErrorCode                     */
    uint32_t n_offsets;       /* failing reads whose first bad byte was recorded  */
    uint16_t offsets[MAX_OFFSETS];
} BusStats;

typedef struct
{
    uint32_t reads;
    uint32_t ok;
    uint32_t crc_fail;
    uint32_t other_err;
    uint32_t last_err;
} DbSectorStats;

#define N_DB_SECTORS 4u
static const uint32_t g_db_sectors[N_DB_SECTORS] = { 1u, 11u, 30980u, 2394u };

typedef struct
{
    uint32_t magic;
    uint32_t done;
    uint32_t mode;               /* mode running now          */
    uint32_t bus_1bit_ok;        /* MODE_BUS_1BIT switch succeeded */
    uint32_t pattern;            /* pattern running now       */
    uint32_t write_fail;         /* scratch writes that failed */
    uint32_t write_last_err;
    BusStats stats[N_MODES][N_PATTERNS];
    DbSectorStats db[N_MODES][N_DB_SECTORS];
    uint8_t  db_copy[N_DB_SECTORS][64]; /* first clean read of each db sector */
    uint32_t db_copy_mode[N_DB_SECTORS]; /* mode that produced it, 0xFF none   */
} SdBusReport;

volatile SdBusReport g_sd_bus_report;

static uint8_t g_expected[BLOCK_BYTES] __attribute__((aligned(32)));
static uint8_t g_got[BLOCK_BYTES] __attribute__((aligned(32)));

/* ------------------------------------------------------------------ *
 *  Bus configuration
 * ------------------------------------------------------------------ */
static uint32_t g_clkcr_default;
static uint32_t g_ospeedr_default;
static uint32_t g_pupdr_default;

static void wait_transfer_state(void)
{
    for (int i = 0; i < 1000; i++)
    {
        if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
        {
            return;
        }
        osDelay(1);
    }
}

static bool g_bus_1bit;

static void bus_restore(void)
{
    if (g_bus_1bit)
    {
        HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B);
        g_bus_1bit = false;
    }
    hsd1.Instance->CLKCR = g_clkcr_default;
    GPIOC->OSPEEDR = g_ospeedr_default;
    GPIOC->PUPDR = g_pupdr_default;
    osDelay(1);
}

static void bus_apply(BusMode mode)
{
    bus_restore();

    switch (mode)
    {
    case MODE_BUS_1BIT:
        /* ACMD6 to the card + WIDBUS on the host; CLKCR is otherwise rebuilt
         * from hsd1.Init, i.e. unchanged. */
        g_bus_1bit = (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_1B) == HAL_OK);
        break;
    case MODE_SLOW_CLK:
        MODIFY_REG(hsd1.Instance->CLKCR, SDMMC_CLKCR_CLKDIV, 8u);
        break;
    case MODE_VSLOW_CLK:
        MODIFY_REG(hsd1.Instance->CLKCR, SDMMC_CLKCR_CLKDIV, 48u);
        break;
    case MODE_CK_MEDIUM:
        MODIFY_REG(GPIOC->OSPEEDR, 3u << (CK_PIN * 2u), 1u << (CK_PIN * 2u));
        break;
    case MODE_PULLUPS:
        for (uint32_t pin = 9u; pin <= 11u; pin++)
        {
            MODIFY_REG(GPIOC->PUPDR, 3u << (pin * 2u), 1u << (pin * 2u));
        }
        break;
    default:
        break;
    }

    osDelay(1);
}

/* One single-block read straight into buf. Returns hsd1.ErrorCode (0 = OK). */
static uint32_t read_block(BusMode mode, uint32_t sector, uint8_t *buf)
{
    HAL_StatusTypeDef st;
    uint32_t err;

    memset(buf, 0xA5, BLOCK_BYTES);

    if (mode == MODE_IRQ_OFF)
    {
        __disable_irq();
        st = HAL_SD_ReadBlocks(&hsd1, buf, sector, 1, 1000);
        err = hsd1.ErrorCode;
        __enable_irq();
    }
    else
    {
        st = HAL_SD_ReadBlocks(&hsd1, buf, sector, 1, 1000);
        err = hsd1.ErrorCode;
    }

    wait_transfer_state();

    if (st != HAL_OK && err == 0)
    {
        err = 0x80000000u; /* HAL failure with no error code (e.g. busy) */
    }
    return err;
}

static bool write_block(uint32_t sector, uint8_t *buf)
{
    HAL_StatusTypeDef st = HAL_SD_WriteBlocks(&hsd1, buf, sector, 1, 1000);
    uint32_t err = hsd1.ErrorCode;

    wait_transfer_state();

    if (st != HAL_OK)
    {
        g_sd_bus_report.write_fail++;
        g_sd_bus_report.write_last_err = err;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 *  Patterns + classification
 * ------------------------------------------------------------------ */
static void fill_pattern(Pattern p, uint32_t sector, uint8_t *buf)
{
    uint32_t x = sector * 2654435761u + 1u;

    for (uint32_t i = 0; i < BLOCK_BYTES; i++)
    {
        switch (p)
        {
        case PAT_ZERO:  buf[i] = 0x00; break;
        case PAT_ONES:  buf[i] = 0xFF; break;
        case PAT_ALT:   buf[i] = 0x5A; break;
        case PAT_INCR:  buf[i] = (uint8_t)(i + sector); break;
        default:
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = (uint8_t)x;
            break;
        }
    }
}

typedef enum { SLIP_NONE, SLIP_DROP, SLIP_INSERT, SLIP_OTHER } Slip;

/* Check the bytes after the first mismatch k against a one-nibble shift. */
static Slip classify(const uint8_t *exp, const uint8_t *got, uint32_t *first_bad)
{
    uint32_t k = 0;

    while (k < BLOCK_BYTES && exp[k] == got[k]) { k++; }
    *first_bad = k;

    if (k == BLOCK_BYTES) { return SLIP_NONE; }

    /* Compare up to 64 bytes after k; the byte at k itself is half-shifted. */
    uint32_t end = (k + 65u < BLOCK_BYTES) ? k + 65u : BLOCK_BYTES - 1u;
    bool drop = true;
    bool insert = true;

    for (uint32_t j = k + 1u; j < end; j++)
    {
        uint8_t d = (uint8_t)((exp[j] << 4) | (exp[j + 1u] >> 4));
        uint8_t n = (uint8_t)((exp[j - 1u] << 4) | (exp[j] >> 4));
        if (got[j] != d) { drop = false; }
        if (got[j] != n) { insert = false; }
    }

    /* A slip in the last few bytes leaves nothing to compare. */
    if (end <= k + 1u) { return SLIP_OTHER; }

    if (drop)   { return SLIP_DROP; }
    if (insert) { return SLIP_INSERT; }
    return SLIP_OTHER;
}

/* ------------------------------------------------------------------ *
 *  Phases
 * ------------------------------------------------------------------ */
static void run_reads(BusMode mode, Pattern p)
{
    volatile BusStats *s = &g_sd_bus_report.stats[mode][p];

    for (uint32_t n = 0; n < SECTORS_PER_PATTERN; n++)
    {
        uint32_t sector = SCRATCH_START_SECTOR + p * SECTORS_PER_PATTERN + n;
        fill_pattern(p, sector, g_expected);

        for (uint32_t r = 0; r < READS_PER_SECTOR; r++)
        {
            uint32_t err = read_block(mode, sector, g_got);
            uint32_t first_bad;
            Slip slip = classify(g_expected, g_got, &first_bad);

            s->reads++;

            if (err != 0)
            {
                s->last_err = err;
                if (err & HAL_SD_ERROR_DATA_CRC_FAIL) { s->crc_fail++; }
                else                                  { s->other_err++; }
            }

            if (err == 0 && slip == SLIP_NONE)
            {
                s->ok++;
                continue;
            }

            if (err == 0) { s->silent_mismatch++; }

            switch (slip)
            {
            case SLIP_NONE:   s->crc_data_ok++; break;
            case SLIP_DROP:   s->slip_drop++;   break;
            case SLIP_INSERT: s->slip_insert++; break;
            default:          s->slip_other++;  break;
            }

            if (slip != SLIP_NONE && s->n_offsets < MAX_OFFSETS)
            {
                s->offsets[s->n_offsets++] = (uint16_t)first_bad;
            }
        }
    }
}

static void run_db_reads(BusMode mode)
{
    for (uint32_t i = 0; i < N_DB_SECTORS; i++)
    {
        volatile DbSectorStats *s = &g_sd_bus_report.db[mode][i];

        for (uint32_t r = 0; r < READS_PER_SECTOR; r++)
        {
            uint32_t err = read_block(mode, g_db_sectors[i], g_got);

            s->reads++;
            if (err == 0)
            {
                s->ok++;
                if (g_sd_bus_report.db_copy_mode[i] == 0xFFu)
                {
                    memcpy((void *)g_sd_bus_report.db_copy[i], g_got, 64);
                    g_sd_bus_report.db_copy_mode[i] = mode;
                }
            }
            else
            {
                s->last_err = err;
                if (err & HAL_SD_ERROR_DATA_CRC_FAIL) { s->crc_fail++; }
                else                                  { s->other_err++; }
            }
        }
    }
}

static void sd_bus_task(void *arg)
{
    (void)arg;

    memset((void *)&g_sd_bus_report, 0, sizeof(g_sd_bus_report));
    for (uint32_t i = 0; i < N_DB_SECTORS; i++)
    {
        g_sd_bus_report.db_copy_mode[i] = 0xFFu;
    }
    g_sd_bus_report.magic = REPORT_MAGIC;

    g_clkcr_default = hsd1.Instance->CLKCR;
    g_ospeedr_default = GPIOC->OSPEEDR;
    g_pupdr_default = GPIOC->PUPDR;

    /* Patterns are written once, with the default bus config. */
    for (Pattern p = 0; p < N_PATTERNS; p++)
    {
        for (uint32_t n = 0; n < SECTORS_PER_PATTERN; n++)
        {
            uint32_t sector = SCRATCH_START_SECTOR + p * SECTORS_PER_PATTERN + n;
            fill_pattern(p, sector, g_expected);
            write_block(sector, g_expected);
        }
    }

    for (BusMode m = 0; m < N_MODES; m++)
    {
        g_sd_bus_report.mode = m;
        bus_apply(m);
        if (m == MODE_BUS_1BIT) { g_sd_bus_report.bus_1bit_ok = g_bus_1bit; }

        for (Pattern p = 0; p < N_PATTERNS; p++)
        {
            g_sd_bus_report.pattern = p;
            run_reads(m, p);
        }

        run_db_reads(m);
    }

    bus_restore();
    g_sd_bus_report.done = 1;

    for (;;)
    {
        osDelay(1000);
    }
}

static StaticTask_t g_bus_task_cb;
static uint8_t g_bus_task_stack[BUS_TEST_STACK_SIZE] __attribute__((aligned(8)));

void SD_BusTest_Init(void)
{
    osThreadAttr_t task_attr = {
        .name = "SD Bus Test",
        .cb_mem = &g_bus_task_cb,
        .cb_size = sizeof(g_bus_task_cb),
        .stack_mem = g_bus_task_stack,
        .stack_size = sizeof(g_bus_task_stack),
        .priority = osPriorityNormal};

    osThreadNew(sd_bus_task, NULL, &task_attr);
}
