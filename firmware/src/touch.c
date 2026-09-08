/*
 * XPT2046 resistive touch panel, presented to Linux as a HID touch screen.
 *
 * Where the hardware facts come from
 * ----------------------------------
 * The board's own firmware, MiSKo3/Firmware/Test/, carries a working driver
 * and the .ioc that generated its pin setup. Everything below is taken from
 * there rather than guessed at:
 *
 *   Drivers/external/XPT2046_touch.h   IRQ = PD6, CS = PE1, SPI1,
 *                                      calibration constants, orientation
 *   Core/Src/spi.c                     SPI1 on PG2/PG3/PG4, AF5
 *   Test.ioc                           PD6 = TOUCH_PENIRQ, PE1 = TOUCH_CS,
 *                                      PG2 = SCK, PG3 = MISO, PG4 = MOSI
 *
 * None of those pins collides with anything this project already uses. The FMC
 * takes PD0/1/4/5/7/8/9/10/14/15, PE7..PE15 and PG5; the buttons take PG0,
 * PG1, PG6, PG8; USB takes PA11/PA12. PD6, PE1, PG2, PG3 and PG4 are free.
 *
 * Why this is a third USB interface
 * ---------------------------------
 * GUD is a display protocol and carries no input at all, so touch cannot ride
 * on the interface that carries pixels. It gets its own HID interface, which
 * also keeps it away from the gamepad: merging both behind report IDs on one
 * interface would change the gamepad's report layout and break a mapping that
 * already works.
 *
 * How the controller is read
 * --------------------------
 * The XPT2046 is a 12-bit SAR converter with a multiplexer across the touch
 * sheet. A read is one command byte followed by two bytes clocked out while it
 * converts; the result is left-aligned in those 16 bits. The command selects
 * which axis is being measured, and which command means which axis depends on
 * how the sheet is turned -- 0x90 and 0xD0 swap roles between portrait and
 * landscape. The stock driver's landscape pair is used here.
 *
 * PENIRQ is pulled low by the controller whenever the sheet is pressed, before
 * any conversion is asked for. Polling it costs one GPIO read, so the SPI
 * traffic only happens while a finger is actually down.
 */

#include "stm32g4xx.h"
#include "stm32g4xx_hal.h"
#include "tusb.h"

#include "touch.h"
#include "rtt.h"

#define SYSCLK_HZ 170000000u

/* Landscape command pair, from the stock driver. */
#define XPT_READ_X 0x90u
#define XPT_READ_Y 0xD0u

/*
 * Calibration, measured by the board's author on this hardware and copied from
 * XPT2046_touch.h. These are 16-bit left-aligned values, so the usable span is
 * a fraction of 0..32767: the sheet's active area does not reach the ends of
 * the resistive divider.
 */
#define XPT_MIN_RAW_X 1756u
#define XPT_MAX_RAW_X 28899u
#define XPT_MIN_RAW_Y 3327u
#define XPT_MAX_RAW_Y 30198u

/*
 * Samples averaged per axis per read.
 *
 * The stock driver averages 64. That is fine in a program whose only job is
 * the touch panel, but here every SPI byte is time tud_task() is not running.
 * At 664 kHz one sample pair costs 6 bytes, so 8 samples is about 600 us
 * against the 64-sample version's 4.6 ms. A resistive panel read by a finger
 * is not precise enough for the extra 56 samples to buy anything.
 */
#define XPT_SAMPLES 8u

/* Reports are rate-limited to the endpoint's 10 ms interval. */
#define TOUCH_PERIOD_MS 10u

static SPI_HandleTypeDef hspi1;
static int touch_ok;

uint32_t touch_reports;
uint32_t touch_presses;
uint32_t touch_down;
uint16_t touch_raw_x, touch_raw_y;
uint16_t touch_x, touch_y;
uint64_t touch_read_cycles;

static inline uint32_t dwt_now(void) { return DWT->CYCCNT; }

static inline void cs_low(void)  { GPIOE->BRR  = (1u << 1); }
static inline void cs_high(void) { GPIOE->BSRR = (1u << 1); }

/* PENIRQ is active low: 0 means the sheet is being pressed. */
static inline int pen_down(void)
{
    return (GPIOD->IDR & (1u << 6)) ? 0 : 1;
}

/*
 * One 12-bit conversion, left-aligned into 16 bits.
 *
 * The command byte and the two result bytes are one continuous transfer with
 * CS held low, because the controller starts converting on the last command
 * bit and streams the result out of the following clocks. Releasing CS in
 * between would abort it.
 */
static uint16_t xpt_read(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };

    if (HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3, 10u) != HAL_OK)
        return 0;

    return (uint16_t) (((uint16_t) rx[1] << 8) | rx[2]);
}

/*
 * Read both axes, averaged.
 *
 * Y is read before X on every iteration, matching the stock driver: the
 * multiplexer needs a settling time after switching axes, and interleaving the
 * two gives each the same settling history as the calibration constants were
 * taken with.
 */
static void xpt_read_axes(uint16_t *out_x, uint16_t *out_y)
{
    uint32_t sum_x = 0, sum_y = 0;

    cs_low();
    for (uint32_t i = 0; i < XPT_SAMPLES; i++) {
        sum_y += xpt_read(XPT_READ_Y);
        sum_x += xpt_read(XPT_READ_X);
    }
    cs_high();

    *out_x = (uint16_t) (sum_x / XPT_SAMPLES);
    *out_y = (uint16_t) (sum_y / XPT_SAMPLES);
}

/* Map one raw axis onto the 0..4095 range the report descriptor declares. */
static uint16_t map_axis(uint32_t raw, uint32_t lo, uint32_t hi, int invert)
{
    uint32_t v;

    if (raw < lo) raw = lo;
    if (raw > hi) raw = hi;

    v = (raw - lo) * 4095u / (hi - lo);
    return (uint16_t) (invert ? (4095u - v) : v);
}

void touch_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* PENIRQ, input. The controller drives it low; the pull-up holds it high
     * while nothing is touching, so no external resistor is needed. */
    g.Pin  = GPIO_PIN_6;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &g);

    /* CS, output, idle high. Set before the pin is driven so the controller
     * never sees a spurious select during bring-up. */
    cs_high();
    g.Pin   = GPIO_PIN_1;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOE, &g);

    /* SCK, MISO, MOSI on PG2/PG3/PG4, alternate function 5. */
    g.Pin       = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOG, &g);

    /*
     * SPI1 sits on APB2, which runs at the full 170 MHz here. The XPT2046's
     * header warns to stay under 2.5 Mbit and recommends around 650 kbit, so
     * the prescaler is 256: 170 MHz / 256 = 664 kHz.
     *
     * Mode 0 (CPOL = 0, CPHA = 1 edge) and MSB first, as the datasheet and the
     * stock driver both use. NSS is software because CS is a plain GPIO.
     */
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7;
    hspi1.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
        return;                     /* touch_ok stays 0, the board carries on */

    /*
     * Prove the controller is there before claiming it works.
     *
     * A conversion is asked for with nothing touching the sheet. The XPT2046
     * always answers something; what distinguishes a present controller from a
     * dead bus is that the answer is neither all zeros nor all ones. A missing
     * device leaves MISO floating, which with no pull reads as one or the
     * other consistently.
     */
    uint16_t probe_x, probe_y;
    xpt_read_axes(&probe_x, &probe_y);

    if ((probe_x == 0u && probe_y == 0u) ||
        (probe_x >= 0xFFF8u && probe_y >= 0xFFF8u))
        return;

    touch_ok = 1;
}

int touch_present(void) { return touch_ok; }

void touch_task(uint32_t ms_ticks)
{
    static uint32_t next_ms;
    static uint32_t prev_down;
    static uint16_t prev_x, prev_y;

    if (!touch_ok)
        return;

    /* HID instance 1 is the touch screen. Not ready means the previous report
     * has not been collected yet, so there is nothing useful to do. */
    if (!tud_hid_n_ready(1))
        return;

    if ((int32_t) (ms_ticks - next_ms) < 0)
        return;
    next_ms = ms_ticks + TOUCH_PERIOD_MS;

    uint32_t down = (uint32_t) pen_down();

    if (down) {
        uint32_t t0 = dwt_now();
        uint16_t rx, ry;

        xpt_read_axes(&rx, &ry);
        touch_read_cycles += (uint64_t) (dwt_now() - t0);

        /*
         * The stock driver mirrors Y for this orientation by subtracting from
         * 32768 before scaling. Doing it with the invert flag inside map_axis
         * is the same operation applied after scaling, which keeps the
         * calibration constants meaningful in the direction they were measured.
         */
        touch_raw_x = rx;
        touch_raw_y = ry;

#if TOUCH_SWAP_XY
        touch_x = map_axis(ry, XPT_MIN_RAW_Y, XPT_MAX_RAW_Y, TOUCH_INVERT_X);
        touch_y = map_axis(rx, XPT_MIN_RAW_X, XPT_MAX_RAW_X, TOUCH_INVERT_Y);
#else
        touch_x = map_axis(rx, XPT_MIN_RAW_X, XPT_MAX_RAW_X, TOUCH_INVERT_X);
        touch_y = map_axis(ry, XPT_MIN_RAW_Y, XPT_MAX_RAW_Y, TOUCH_INVERT_Y);
#endif

        if (!prev_down)
            touch_presses++;
    }

    touch_down = down;

    /*
     * Send only on change, with one exception: the pen-up report must always
     * go out, or the host is left believing a finger is still down.
     *
     * A small movement threshold would cut traffic further, but a resistive
     * panel already averages eight samples and the endpoint only fires every
     * 10 ms, so the report rate is bounded at 100/s regardless.
     */
    if (down == prev_down && touch_x == prev_x && touch_y == prev_y)
        return;

    prev_down = down;
    prev_x = touch_x;
    prev_y = touch_y;

    /*
     * Report layout, matching desc_touch_report in usb_descriptors.c:
     *   [0] tip switch in bit 0
     *   [1] contact identifier
     *   [2..3] X, little endian
     *   [4..5] Y, little endian
     *   [6] contact count
     */
    uint8_t report[7];

    report[0] = down ? 0x01u : 0x00u;
    report[1] = 0;
    report[2] = (uint8_t) (touch_x & 0xFFu);
    report[3] = (uint8_t) (touch_x >> 8);
    report[4] = (uint8_t) (touch_y & 0xFFu);
    report[5] = (uint8_t) (touch_y >> 8);
    report[6] = down ? 1u : 0u;

    if (tud_hid_n_report(1, 0, report, sizeof(report)))
        touch_reports++;
}
