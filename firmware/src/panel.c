/*
 * Panel back end: the ILI9341 behind the FMC, and the path from a GUD bulk
 * transfer to pixels on glass.
 *
 * Everything about the FMC and the ILI9341 here was derived and measured in
 * phase 1. The load-bearing facts, so this file can be read on its own:
 *
 *   - LCD_RS is PG5 = FMC_A15, and NE1 maps NOR/PSRAM bank 1 at 0x60000000.
 *     On a 16-bit bus the FMC drives FMC_A[n] from HADDR[n+1] -- each FMC
 *     address step is one 16-bit half-word while the CPU counts bytes -- so
 *     A15 is HADDR[16] = 0x10000. Command address 0x60000000, data address
 *     0x60010000.
 *   - LCD_RST is PD3, not PB5. The pin-mapping spreadsheet has three labels
 *     rotated between PB5/PD3/PE0; PD3 is proven functionally.
 *   - A full-frame blit costs 3.162 ms, 48.56 MB/s, 7 HCLK per 16-bit write.
 *   - A __DSB() after every write, which the stock driver does, costs 22%.
 *     There is none in blit_cpu().
 *   - FMC timing tuning is pointless: all eight ADDSET/DATAST/BUSTURN
 *     combinations from (1,1,1) down to (0,0,0) measured the same.
 */

#include <stdint.h>

#include "stm32g4xx.h"
#include "gud_device.h"
#include "panel.h"

#define SYSCLK_HZ 170000000u

/* The two magic addresses -- see the header comment for the derivation. */
#define LCD_CMD  (*(volatile uint16_t *) 0x60000000u)
#define LCD_DATA (*(volatile uint16_t *) 0x60010000u)
#define LCD_DATA_ADDR 0x60010000u

/* ILI9341 commands, from the stock firmware's lcd_ili9341_registers.h. */
#define ILI_SLPOUT  0x11u
#define ILI_NORON   0x13u
#define ILI_DISPON  0x29u
#define ILI_CASET   0x2Au
#define ILI_RASET   0x2Bu
#define ILI_RAMWR   0x2Cu
#define ILI_MADCTL  0x36u
#define ILI_PIXFMT  0x3Au
#define ILI_IFCTL   0xF6u

/*
 * Memory Access Control. 0x60 sets MV (row/column exchange) and MX (column
 * order reversed), which rotates the panel into landscape and makes the
 * addressable area 320 wide by 240 high -- matching the mode we advertise over
 * GUD. This is the stock driver's ILI9341_MISKO_ROTATE_0 orientation.
 */
#define MADCTL_LANDSCAPE 0x60u

#define AF12_FMC 12u

/* ------------------------------------------------------------------ */
/* Statistics, read by main() for the console                          */
/* ------------------------------------------------------------------ */

uint32_t panel_buffers;        /* rectangles written                    */
uint64_t panel_pixel_bytes;    /* bytes of pixel data written to the FMC */
uint64_t panel_blit_cycles;    /* DWT cycles spent inside blit_cpu()     */
uint32_t panel_controller_on;
uint32_t panel_display_on;
uint32_t panel_last_x, panel_last_y, panel_last_w, panel_last_h;

static inline uint32_t dwt_now(void) { return DWT->CYCCNT; }

/* ------------------------------------------------------------------ */
/* GPIO and FMC bring-up                                               */
/* ------------------------------------------------------------------ */

static void gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2))) | (2u << (pin * 2));  /* AF   */
    port->OTYPER &= ~(1u << pin);                                          /* push-pull */
    port->OSPEEDR |= (3u << (pin * 2));                                    /* very high */
    port->PUPDR &= ~(3u << (pin * 2));                                     /* none */
    if (pin < 8)
        port->AFR[0] = (port->AFR[0] & ~(0xFu << (pin * 4))) | (af << (pin * 4));
    else
        port->AFR[1] = (port->AFR[1] & ~(0xFu << ((pin - 8) * 4)))
                     | (af << ((pin - 8) * 4));
}

static void gpio_set_output(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2))) | (1u << (pin * 2));
    port->OTYPER &= ~(1u << pin);
    port->OSPEEDR |= (3u << (pin * 2));
    port->PUPDR &= ~(3u << (pin * 2));
}

static void fmc_gpio_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIODEN
                  | RCC_AHB2ENR_GPIOEEN | RCC_AHB2ENR_GPIOGEN;
    (void) RCC->AHB2ENR;

    /* Data bus and control, exactly as the stock fmc.c lists them:
     *   PD14 D0,  PD15 D1,  PD0 D2,   PD1 D3,   PE7..PE15 D4..D12,
     *   PD8  D13, PD9  D14, PD10 D15,
     *   PD4 NOE (LCD_RD), PD5 NWE (LCD_WR), PD7 NE1 (LCD_CS), PG5 A15 (LCD_RS) */
    const uint32_t portd[] = { 0, 1, 4, 5, 7, 8, 9, 10, 14, 15 };
    for (unsigned i = 0; i < sizeof(portd) / sizeof(portd[0]); i++)
        gpio_set_af(GPIOD, portd[i], AF12_FMC);

    for (uint32_t p = 7; p <= 15; p++)
        gpio_set_af(GPIOE, p, AF12_FMC);

    gpio_set_af(GPIOG, 5, AF12_FMC);

    /* LCD_RST = PD3 and LCD_BKLT = PB6 are plain GPIO outputs. */
    gpio_set_output(GPIOD, 3);
    gpio_set_output(GPIOB, 6);
}

#define LCD_RST_LOW()   (GPIOD->BRR  = (1u << 3))
#define LCD_RST_HIGH()  (GPIOD->BSRR = (1u << 3))
#define LCD_BKLT_ON()   (GPIOB->BSRR = (1u << 6))
#define LCD_BKLT_OFF()  (GPIOB->BRR  = (1u << 6))

/*
 * FMC NORSRAM bank 1: asynchronous, 16-bit, access mode A.
 *
 * BCR1: MBKEN enables the bank, MWID = 01 selects the 16-bit data bus, WREN
 * allows writes, FACCEN is a NOR-flash bit that is harmless for SRAM (the HAL
 * sets it too), EXTMOD stays 0 so one timing register covers reads and writes.
 *
 * BTR1, all in HCLK ticks of 5.882 ns:
 *   ADDSET  address setup -- NEx and A15 stable before NWE falls
 *   ADDHLD  address hold  -- only used in multiplexed/mode D, inert here
 *   DATAST  data phase    -- how long NWE stays low, the write strobe width
 *   BUSTURN dead time after the access, which gives the panel its control
 *           pulse high time and stops the next access colliding on the bus
 *   ACCMOD  00 = mode A
 */
static void fmc_init(void)
{
    RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN;
    (void) RCC->AHB3ENR;

    FMC_Bank1_R->BTCR[0] =
          FMC_BCRx_MBKEN
        | (0x1u << FMC_BCRx_MWID_Pos)
        | FMC_BCRx_FACCEN
        | FMC_BCRx_WREN;

    FMC_Bank1_R->BTCR[1] =
          (1u << FMC_BTRx_ADDSET_Pos)
        | (0xFu << FMC_BTRx_ADDHLD_Pos)
        | (1u << FMC_BTRx_DATAST_Pos)
        | (1u << FMC_BTRx_BUSTURN_Pos)
        | (0x0u << FMC_BTRx_ACCMOD_Pos);
}

/* ------------------------------------------------------------------ */
/* ILI9341                                                             */
/* ------------------------------------------------------------------ */

static void delay_ms(uint32_t ms)
{
    uint32_t start = dwt_now();
    uint32_t target = ms * (SYSCLK_HZ / 1000u);
    while ((dwt_now() - start) < target) { }
}

static inline void lcd_cmd(uint16_t c) { LCD_CMD = c; }
static inline void lcd_dat(uint16_t d) { LCD_DATA = d; }

/*
 * Set the drawing window and leave the controller expecting pixel data.
 *
 * CASET and RASET each take a 16-bit start and a 16-bit end, sent as four
 * bytes. After RAMWR every half-word written to the data address lands in the
 * next position inside that window, wrapping at the right edge and stopping at
 * the bottom -- which is exactly what a damage rectangle needs.
 */
static void lcd_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t x1 = x + w - 1u, y1 = y + h - 1u;

    lcd_cmd(ILI_CASET);
    lcd_dat(x >> 8);  lcd_dat(x & 0xFFu);
    lcd_dat(x1 >> 8); lcd_dat(x1 & 0xFFu);

    lcd_cmd(ILI_RASET);
    lcd_dat(y >> 8);  lcd_dat(y & 0xFFu);
    lcd_dat(y1 >> 8); lcd_dat(y1 & 0xFFu);

    lcd_cmd(ILI_RAMWR);
}

/*
 * The blit. A plain CPU store loop with no memory barrier.
 *
 * Stores into the FMC window are posted: the CPU retires them into a write
 * FIFO and carries on while the FMC is still clocking bits out. Phase 1
 * measured this at 7 HCLK per half-word (48.56 MB/s) and measured the stock
 * driver's per-write __DSB() at 9 HCLK -- 22% slower for no benefit, because
 * ordering between consecutive writes to the same device is already
 * guaranteed.
 *
 * The source is uint8_t because that is how the pixels arrive over USB, but it
 * is read as uint16_t. That is safe here: the band buffer is a static array,
 * so it is at least 4-byte aligned, and RGB565 pixels are 2 bytes each, so
 * every pixel in it is 2-byte aligned.
 */
static void blit_cpu(const uint16_t *src, uint32_t count)
{
    volatile uint16_t *dst = (volatile uint16_t *) LCD_DATA_ADDR;
    while (count--)
        *dst = *src++;
}

static void fill_cpu(uint16_t colour, uint32_t count)
{
    volatile uint16_t *dst = (volatile uint16_t *) LCD_DATA_ADDR;
    while (count--)
        *dst = colour;
}

/* ------------------------------------------------------------------ */
/* Scan-out tracking: where is the panel refreshing right now?         */
/* ------------------------------------------------------------------ */

/*
 * The ILI9341's TE (tearing effect) output is not routed on this board -- it
 * is absent from the LCD sheet's port list, and `lcd.h` in the stock firmware
 * maps LCD_TE to PC14, which is BTN_ESC here. So there is no interrupt at the
 * start of each refresh, which is the normal way to avoid tearing.
 *
 * Command 0x45, GET_SCANLINE, is the way round it: the controller reports the
 * line it is currently scanning out, readable over the same parallel bus we
 * write pixels on. Phase 1 measured it working: values 0..323, wrapping 81
 * times per second, 1.223 us per read.
 *
 * WHICH AXIS DOES IT COUNT?
 *
 * This is the question the whole milestone turns on, and phase 1 answered it
 * without noticing. The counter reaches 323. Our image is 320 wide and 240
 * tall, so a counter that exceeds 240 cannot be counting our rows -- it must
 * be counting the 320-direction, which is our image *x*.
 *
 * That matches the hardware. The ILI9341 drives a 240x320 portrait panel: 320
 * gate lines, each showing 240 RGB sources. MADCTL = 0x60 sets MV (row/column
 * exchange), which rotates the image but not the panel: each of those 320 gate
 * lines still refreshes in the same physical order, and in the rotated image it
 * appears as a vertical line 240 pixels tall. So the refresh sweeps left to
 * right through 320 columns -- the panel refreshes column by column, not top to
 * bottom. 320 gate lines is also why the counter reaches 320-odd and not 240.
 *
 * The consequence is severe for the way M4 and M5 write pixels. With MV set, a
 * write sweeps the CASET direction first, and CASET is now the 320-axis: so
 * writing a single image *row* touches every one of the 320 gate lines. Every
 * band write crosses the entire scan range, and no choice of starting moment
 * can keep the write front ahead of the scan front. That is why tearing is
 * visible no matter when a band is blitted.
 */

#define SCAN_RESYNC_MS 100u

/* Filled in by scan_calibrate(). */
static uint32_t scan_total_lines = 324u;    /* 320 active + blanking     */
static uint32_t scan_cycles_per_line = 6481u;
static uint32_t scan_period_cycles = 2100010u;  /* 12.353 ms at 170 MHz  */

/* Phase reference: at DWT time scan_t0, the panel was at line 0. */
static uint32_t scan_t0;
static uint32_t scan_last_resync;

/* Cost accounting, reported by main(). */
uint64_t panel_sync_cycles;      /* time spent reading/ordering for sync */
uint32_t panel_resyncs;

/*
 * Reads on this bus are far slower than writes. The ILI9341's register read
 * cycle is around 160 ns against a 66 ns write cycle, and we run writes at
 * 41 ns -- already outside the datasheet. So the bank is retimed to something
 * deliberately conservative for the read and put straight back afterwards.
 *
 * Retiming requires taking the bank offline: a live bank may latch the old
 * values. That is what the MBKEN clear and set are for.
 */
static void fmc_set_timing(uint32_t addset, uint32_t datast, uint32_t busturn)
{
    FMC_Bank1_R->BTCR[0] &= ~FMC_BCRx_MBKEN;
    __DSB();

    FMC_Bank1_R->BTCR[1] =
          ((addset  & 0xFu)  << FMC_BTRx_ADDSET_Pos)
        | ((0xFu)            << FMC_BTRx_ADDHLD_Pos)
        | ((datast  & 0xFFu) << FMC_BTRx_DATAST_Pos)
        | ((busturn & 0xFu)  << FMC_BTRx_BUSTURN_Pos)
        | (0x0u              << FMC_BTRx_ACCMOD_Pos);

    __DSB();
    FMC_Bank1_R->BTCR[0] |= FMC_BCRx_MBKEN;
    __DSB();
}

/*
 * Read GET_SCANLINE. Returns the gate line currently being scanned out, which
 * is our image x coordinate.
 *
 * The first read after a command is always a dummy -- the controller needs one
 * cycle to put real data on the bus. The result is GTS[9:0], split across two
 * further reads.
 */
static uint32_t lcd_read_scanline(void)
{
    uint32_t hi, lo;

    fmc_set_timing(4, 32, 15);      /* slow, for reads */

    LCD_CMD = 0x45u;
    __DSB();
    (void) LCD_DATA;                /* dummy */
    hi = (uint32_t) (LCD_DATA & 0x03u);
    lo = (uint32_t) (LCD_DATA & 0xFFu);

    fmc_set_timing(1, 1, 1);        /* back to the fast write timing */

    return (hi << 8) | lo;
}

/*
 * Measure the panel's refresh period and line count, once, at boot.
 *
 * Nothing here is assumed from the datasheet: the number of lines is whatever
 * the counter is seen to reach, and the period is measured between wraps with
 * the DWT cycle counter. Phase 1 got 0..323 and 80.95 Hz this way; if this
 * board's panel differs, the numbers below follow it rather than a constant.
 */
static void scan_calibrate(void)
{
    uint32_t mx = 0, wraps = 0, prev = 0xFFFFFFFFu;
    uint32_t first_wrap = 0, last_wrap = 0;
    uint32_t t_start = dwt_now();

    while ((dwt_now() - t_start) < (SYSCLK_HZ / 2u)) {   /* half a second */
        uint32_t v = lcd_read_scanline();

        if (v > mx)
            mx = v;

        /* A sharp drop means the counter restarted at the top of a frame. */
        if (prev != 0xFFFFFFFFu && v + 16u < prev) {
            uint32_t now = dwt_now();
            if (wraps == 0u)
                first_wrap = now;
            last_wrap = now;
            wraps++;
        }
        prev = v;
    }

    if (wraps >= 2u) {
        scan_total_lines = mx + 1u;
        scan_period_cycles = (last_wrap - first_wrap) / (wraps - 1u);
        scan_cycles_per_line = scan_period_cycles / scan_total_lines;
        scan_t0 = last_wrap;
    }

    scan_last_resync = dwt_now();
}

uint32_t panel_scan_total_lines(void) { return scan_total_lines; }
uint32_t panel_scan_period_cycles(void) { return scan_period_cycles; }

/*
 * Where is the scan now?
 *
 * Deliberately NOT a GET_SCANLINE read. Every read costs a bus retiming either
 * side of it and stalls the FMC, and we would need one before every band. The
 * panel's refresh is a free-running oscillator, so once its period and phase
 * are known the position can be computed from the DWT cycle counter for the
 * price of a subtraction and a divide.
 *
 * The phase is re-anchored with a real read every SCAN_RESYNC_MS, which bounds
 * the drift: the ILI9341's internal oscillator is not disciplined to anything
 * of ours, so the two clocks wander apart slowly.
 */
static uint32_t scan_line_now(void)
{
    uint32_t dt = dwt_now() - scan_t0;

    /* Fold whole frames out of the elapsed time. Unsigned arithmetic makes
     * this correct across the DWT's 25.3 s wrap. */
    while (dt >= scan_period_cycles) {
        scan_t0 += scan_period_cycles;
        dt -= scan_period_cycles;
    }

    return dt / scan_cycles_per_line;
}

/* Re-anchor the phase against the panel itself. Cheap enough at 10 Hz that it
 * does not show up in the frame budget, and it is what stops the estimate
 * drifting away from the panel over minutes. */
static void scan_resync_if_due(void)
{
    uint32_t now = dwt_now();

    if ((now - scan_last_resync) < (SYSCLK_HZ / 1000u) * SCAN_RESYNC_MS)
        return;

    scan_last_resync = now;

    uint32_t line = lcd_read_scanline();
    if (line < scan_total_lines) {
        /* Set the phase so that scan_line_now() would return `line` right now. */
        scan_t0 = dwt_now() - line * scan_cycles_per_line;
        panel_resyncs++;
    }
}

#define RGB565(r, g, b) \
    ((uint16_t) ((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/*
 * Something recognisable on screen before the host says anything, so that a
 * blank panel means "the display path is broken" rather than "the host has not
 * connected yet". Colour bars also make a wrong MADCTL or a swapped data line
 * obvious at a glance.
 */
static void draw_splash(void)
{
    static const uint16_t bars[8] = {
        RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 255, 255),
        RGB565(0, 255, 0),     RGB565(255, 0, 255), RGB565(255, 0, 0),
        RGB565(0, 0, 255),     RGB565(0, 0, 0),
    };
    volatile uint16_t *dst = (volatile uint16_t *) LCD_DATA_ADDR;

    lcd_window(0, 0, GUD_WIDTH, GUD_HEIGHT);
    for (uint32_t y = 0; y < GUD_HEIGHT; y++)
        for (uint32_t x = 0; x < GUD_WIDTH; x++)
            *dst = bars[(x * 8u) / GUD_WIDTH];
}

/* Defined further down, with the overlay. panel_init() calls it so the text
 * is on screen from boot. */
static void ovl_task(void);

void panel_init(void)
{
    fmc_gpio_init();
    fmc_init();

    /* Hardware reset on PD3, with the stock driver's timings. */
    LCD_RST_LOW();
    delay_ms(120);
    LCD_RST_HIGH();
    delay_ms(120);

    lcd_cmd(ILI_MADCTL);
    lcd_dat(MADCTL_LANDSCAPE);

    lcd_window(0, 0, GUD_WIDTH, GUD_HEIGHT);

    lcd_cmd(ILI_SLPOUT);          /* leave sleep mode */
    delay_ms(200);

    lcd_cmd(ILI_NORON);           /* normal display mode */
    delay_ms(100);

    lcd_cmd(ILI_PIXFMT);
    lcd_dat(0x55);                /* 0x55 = 16 bits/pixel on both interfaces */
    delay_ms(100);

    lcd_cmd(ILI_IFCTL);
    lcd_dat(0x49); lcd_dat(0x00); lcd_dat(0x20);

    lcd_cmd(ILI_DISPON);
    delay_ms(50);

    /* Learn the panel's own refresh period and line count before anything
     * depends on them. Half a second of polling, done once, with the bus
     * otherwise idle. */
    scan_calibrate();

    /* Start black, not with the previous contents of the panel's RAM. */
    lcd_window(0, 0, GUD_WIDTH, GUD_HEIGHT);
    fill_cpu(0x0000, GUD_WIDTH * GUD_HEIGHT);

    draw_splash();

    /* Draw the overlay now, even though no frame has arrived. That way the
     * text rendering is shown to work before any host connects: if the colour
     * bars come up with 0.0 fps over them, the font and the blit are fine and
     * any later problem is in the counting, not the drawing. */
    if (panel_fps_overlay)
        ovl_task();

    LCD_BKLT_ON();
}

/* ------------------------------------------------------------------ */
/* The GUD callbacks                                                   */
/* ------------------------------------------------------------------ */

void gud_panel_controller_enable(uint8_t enable)
{
    panel_controller_on = enable;

    /* DRM is enabling the CRTC. Clear whatever was on screen -- the splash, or
     * the last desktop image from before a disable -- so the first real frame
     * does not appear over stale pixels. */
    if (enable) {
        lcd_window(0, 0, GUD_WIDTH, GUD_HEIGHT);
        fill_cpu(0x0000, GUD_WIDTH * GUD_HEIGHT);
    }
}

/*
 * DPMS. This has to darken the panel, not merely stop updating it, so it
 * drives the backlight on PB6.
 *
 * The backlight is the honest place to implement DPMS on this board: the
 * ILI9341 also has a DISPOFF command, but that blanks the controller while
 * leaving the backlight burning, which looks like a white rectangle rather
 * than an off display.
 */
void gud_panel_display_enable(uint8_t enable)
{
    panel_display_on = enable;

    if (enable)
        LCD_BKLT_ON();
    else
        LCD_BKLT_OFF();
}

int gud_panel_state_commit(const struct gud_state_req *state, uint8_t format)
{
    (void) state;

    /* RGB565 is the only format advertised and SET_STATE_CHECK has already
     * rejected anything else; this is belt and braces, because the blit below
     * assumes 2 bytes per pixel. */
    if (format != GUD_PIXEL_FORMAT_RGB565)
        return -GUD_STATUS_INVALID_PARAMETER;

    return 0;
}

/*
 * One rectangle of pixels has arrived. Put it on the panel.
 *
 * This is called from the bulk endpoint's completion callback, which runs
 * inside tud_task(). The blit takes milliseconds, and during it no USB
 * transfer is serviced -- so reception and display are strictly serial. That
 * is a real cost and it is measured: see RESULTS.md. Overlapping them would
 * need a second band buffer to receive into while the first is being blitted,
 * and at 75 KB a band there is no RAM for one.
 *
 * The rectangle is honoured rather than assumed to be full width. The host
 * already sends damage rectangles -- GUD only sends whole frames if the device
 * asks for it with GUD_DISPLAY_FLAG_FULL_UPDATE, which we deliberately do not
 * -- so partial-width updates arrive from the very first frame.
 */
/*
 * Tear-free blit: write the rectangle one gate line at a time, in scan order,
 * starting just ahead of where the panel is currently refreshing.
 *
 * WHY THIS IS THE SHAPE OF THE FIX
 *
 * The scan advances one gate line every scan_cycles_per_line -- about 38 us,
 * or 12.353 ms for a whole sweep. Writing one column of a 120-line band is 120
 * pixels plus an 11-write window command, 131 bus writes at 7 HCLK each =
 * 5.4 us. So we move through the columns about seven times faster than the scan
 * moves through them.
 *
 * Start at the column the scan is about to reach and walk forwards, wrapping
 * at the end of the rectangle:
 *
 *   - Columns ahead of the scan are rewritten long before it arrives. It sees
 *     new content.
 *   - Columns behind the scan were read moments ago; the scan will not return
 *     to them for another full frame, 12.353 ms away, and we finish the whole
 *     rectangle in under 1 ms.
 *
 * So no column is ever read while half-written, and -- crucially -- there is
 * nothing to wait for. The cost is not idle time, it is the extra window
 * commands: one CASET/RASET/RAMWR sequence per column instead of one per
 * rectangle. That is 11 extra bus writes per column against 120 pixel writes,
 * about 9%.
 *
 * WHY NOT THE PLAN IN CLAUDE.md
 *
 * The plan was to sync once, run a timer at 80.95 Hz, and start each blit at
 * the top of vertical blanking. That works when a whole frame is blitted in
 * one go, which is what phase 1 measured -- but this device cannot hold a
 * whole frame (153,600 bytes against 131,072 of RAM), so pixels arrive as
 * bands at times USB chooses. A band written row-major touches every gate line
 * regardless of when it starts, so waiting for vertical blanking would buy
 * nothing. The timer half of the plan is kept and is doing real work: it is
 * scan_line_now(), which is what makes "just ahead of the scan" answerable
 * without a bus read per band.
 */
static void blit_columnwise(const struct gud_set_buffer_req *req,
                            const uint8_t *pixels)
{
    const uint16_t *src = (const uint16_t *) (const void *) pixels;
    uint32_t w = req->width;
    uint32_t h = req->height;
    uint32_t line = scan_line_now();

    /*
     * Which column of the rectangle is the scan about to reach?
     *
     * `line` is in image-x units. Two lines of margin covers the error in the
     * phase estimate between re-syncs and the time spent getting here.
     */
    int32_t rel = (int32_t) line + 2 - (int32_t) req->x;
    int32_t start = rel % (int32_t) w;
    if (start < 0)
        start += (int32_t) w;

    uint32_t c = (uint32_t) start;

    for (uint32_t i = 0; i < w; i++) {
        volatile uint16_t *dst = (volatile uint16_t *) LCD_DATA_ADDR;
        const uint16_t *s = src + c;

        lcd_window(req->x + c, req->y, 1u, h);

        /* One column of the source rectangle. The source is row-major, so
         * this walks it with a stride of one row. */
        for (uint32_t k = 0; k < h; k++) {
            *dst = *s;
            s += w;
        }

        if (++c >= w)
            c = 0;
    }
}

/* ------------------------------------------------------------------ */
/* On-screen frame rate overlay, top left corner                        */
/* ------------------------------------------------------------------ */

/*
 * Why on the panel and not only on the console.
 *
 * The RTT console needs a debug probe attached. This overlay makes the rate
 * readable when the board is connected by USB alone -- during a game, or on
 * somebody else's machine.
 *
 * It is redrawn after every band. Band 0 covers the top half of the screen
 * and would overwrite it, so it has to be restored each time; band 1 does not
 * reach it, but redrawing is cheap enough that telling the two apart is not
 * worth the complication.
 */

#define OVL_SCALE  2u
#define OVL_CHARS  9u
#define OVL_PAD    2u
#define OVL_W      (OVL_CHARS * 6u * OVL_SCALE + 2u * OVL_PAD)   /* 112 */
#define OVL_H      (7u * OVL_SCALE + 2u * OVL_PAD)               /*  18 */

/*
 * A 5 x 7 font holding only the characters this string needs: the digits, a
 * full stop, a blank, and the letters f, p and s. Each row is 5 bits, with the
 * most significant bit leftmost.
 */
static const uint8_t ovl_font[14][7] = {
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },   /* 0 */
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },   /* 1 */
    { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },   /* 2 */
    { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },   /* 3 */
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },   /* 4 */
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },   /* 5 */
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },   /* 6 */
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },   /* 7 */
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },   /* 8 */
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },   /* 9 */
    { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C },   /* . */
    { 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 },   /* f */
    { 0x00,0x00,0x1E,0x11,0x1E,0x10,0x10 },   /* p */
    { 0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E },   /* s */
};

#define GLYPH_DOT   10
#define GLYPH_F     11
#define GLYPH_P     12
#define GLYPH_S     13
#define GLYPH_BLANK 255

static uint16_t ovl_buf[OVL_W * OVL_H];

uint32_t panel_fps_overlay = 1;     /* overlay on */
uint32_t panel_fps_x10;             /* most recent rate, times ten */
uint64_t panel_overlay_cycles;      /* overlay cost, kept apart from the blit */

static uint64_t ovl_prev_bytes;
static uint32_t ovl_last_t;
static uint32_t ovl_drawn_x10 = 0xFFFFFFFFu;

/* Draw one glyph into ovl_buf at character position `slot`. */
static void ovl_glyph(uint32_t slot, uint8_t idx, uint16_t fg)
{
    if (idx == GLYPH_BLANK)
        return;

    uint32_t x0 = OVL_PAD + slot * 6u * OVL_SCALE;
    uint32_t y0 = OVL_PAD;

    for (uint32_t row = 0; row < 7u; row++) {
        uint8_t bits = ovl_font[idx][row];
        for (uint32_t col = 0; col < 5u; col++) {
            if (!((bits >> (4u - col)) & 1u))
                continue;
            /* One font pixel is an OVL_SCALE x OVL_SCALE square. */
            for (uint32_t dy = 0; dy < OVL_SCALE; dy++)
                for (uint32_t dx = 0; dx < OVL_SCALE; dx++)
                    ovl_buf[(y0 + row * OVL_SCALE + dy) * OVL_W
                          + (x0 + col * OVL_SCALE + dx)] = fg;
        }
    }
}

/* Build the bitmap for a string like "  40.0 fps" from rate-times-ten. */
static void ovl_render(uint32_t fps_x10)
{
    uint8_t slots[OVL_CHARS];
    uint32_t whole = fps_x10 / 10u;
    uint32_t frac  = fps_x10 % 10u;

    if (whole > 999u) { whole = 999u; frac = 9u; }

    for (uint32_t i = 0; i < OVL_CHARS; i++)
        slots[i] = GLYPH_BLANK;

    /* Whole part right-aligned in slots 0..2 so the text does not jitter. */
    slots[2] = (uint8_t) (whole % 10u);
    if (whole >= 10u)  slots[1] = (uint8_t) ((whole / 10u) % 10u);
    if (whole >= 100u) slots[0] = (uint8_t) (whole / 100u);

    slots[3] = GLYPH_DOT;
    slots[4] = (uint8_t) frac;
    /* slot 5 stays blank */
    slots[6] = GLYPH_F;
    slots[7] = GLYPH_P;
    slots[8] = GLYPH_S;

    for (uint32_t i = 0; i < OVL_W * OVL_H; i++)
        ovl_buf[i] = 0x0000;                    /* black ground, for contrast */

    for (uint32_t i = 0; i < OVL_CHARS; i++)
        ovl_glyph(i, slots[i], 0xFFFFu);        /* white text */
}

/*
 * Recompute the rate and draw the overlay.
 *
 * The rate comes from panel_pixel_bytes, that is from bytes actually written
 * to the panel, not from a count of bands. A whole frame is
 * 320 * 240 * 2 = 153,600 B, so fps = bytes_per_second / 153,600.
 *
 * The interval is not exactly one second, because this runs only when a band
 * arrives. So the elapsed time is measured and divided out.
 */
static void ovl_task(void)
{
    uint32_t now = dwt_now();
    uint32_t dt = now - ovl_last_t;

    if (dt >= SYSCLK_HZ) {
        uint64_t d = panel_pixel_bytes - ovl_prev_bytes;

        ovl_prev_bytes = panel_pixel_bytes;
        ovl_last_t = now;

        /* fps * 10 = (bytes * 10 * f_cpu) / (dt * bytes_per_frame) */
        panel_fps_x10 = (uint32_t) ((d * 10ull * (uint64_t) SYSCLK_HZ)
                                    / ((uint64_t) dt
                                       * (uint64_t) (GUD_WIDTH * GUD_HEIGHT * 2u)));
    }

    if (panel_fps_x10 != ovl_drawn_x10) {
        ovl_render(panel_fps_x10);
        ovl_drawn_x10 = panel_fps_x10;
    }

    lcd_window(0, 0, OVL_W, OVL_H);
    blit_cpu(ovl_buf, OVL_W * OVL_H);
}

/*
 * One rectangle of pixels has arrived. Put it on the panel.
 *
 * Called from the bulk endpoint's completion callback, inside tud_task(), so
 * no USB transfer is serviced while this runs.
 *
 * panel_tearfree selects between the two strategies so they can be compared on
 * the same hardware with the same content: press BTN_OK to toggle. Everything
 * else is identical, which is what makes the comparison worth anything.
 */
uint32_t panel_tearfree = 1;

void gud_panel_write_buffer(const struct gud_set_buffer_req *req,
                            const uint8_t *pixels)
{
    uint32_t t0, t1, t2;

    panel_last_x = req->x;
    panel_last_y = req->y;
    panel_last_w = req->width;
    panel_last_h = req->height;

    t0 = dwt_now();
    scan_resync_if_due();
    t1 = dwt_now();

    if (panel_tearfree) {
        blit_columnwise(req, pixels);
    } else {
        /* M5's path: one window, one straight run of pixels. Faster, and it
         * tears. */
        lcd_window(req->x, req->y, req->width, req->height);
        blit_cpu((const uint16_t *) (const void *) pixels,
                 req->width * req->height);
    }

    t2 = dwt_now();

    panel_sync_cycles += (uint64_t) (t1 - t0);
    panel_blit_cycles += (uint64_t) (t2 - t1);
    panel_pixel_bytes += (uint64_t) req->width * req->height * 2u;
    panel_buffers++;

    /* Overlay last, once the counters are updated, and timed separately so
     * it does not pollute panel_blit_cycles. */
    if (panel_fps_overlay) {
        uint32_t t3 = dwt_now();
        ovl_task();
        panel_overlay_cycles += (uint64_t) (dwt_now() - t3);
    }
}
