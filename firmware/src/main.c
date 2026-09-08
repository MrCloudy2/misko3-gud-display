/*
 * MiSKo3 -- a USB display and a gamepad on one cable.
 *
 * The board appears to Linux as two independent functions on one USB device:
 * a GUD display that the kernel's in-tree `gud` driver binds as a DRM card,
 * and a HID gamepad that usbhid binds as js0. Neither needs a custom host
 * driver.
 *
 * The pipeline, end to end:
 *
 *   host compositor -> XRGB8888 to RGB565 in the gud driver -> LZ4 -> split
 *   into 120-line bands -> GUD_REQ_SET_BUFFER on endpoint 0 -> bulk OUT ->
 *   decompressed in place in gud_band_buf -> column-major blit in scan order
 *   -> FMC -> ILI9341
 *
 * The two facts that shape the whole design:
 *
 *   A full RGB565 frame is 153,600 bytes and this part has 131,072, so the
 *   display's own framebuffer cannot exist here. Pixels are streamed as bands
 *   and the host is told so via max_buffer_size.
 *
 *   USB full speed carries ~0.77 MB/s, while the FMC blit runs at 48.6 MB/s.
 *   The wire is the bottleneck by a factor of 60, which is why compression
 *   buys everything and why there is CPU to spare for it.
 *
 * Reception, decompression and blitting are strictly serial: both happen
 * inside the bulk completion callback, and no USB transfer is serviced while
 * they run. Overlapping them would need a second band buffer, and the one we
 * have is 77 KB of the 131 KB available.
 *
 * Everything about the clock tree here is inherited from phase 1 and phase 2,
 * which measured it on this board. The USB half is the interesting part and is
 * commented where it is decided rather than where it is used.
 *
 * Bare metal + TinyUSB. No HAL. Console over RTT.
 */

#include <stdint.h>
#include "stm32g4xx.h"
#include "tusb.h"
#include "rtt.h"
#include "gud_device.h"
#include "buttons.h"
#include "panel.h"
#include "joystick.h"
#include "touch.h"

#define SYSCLK_HZ 170000000u

/* Counters kept by the other modules; printed from here so that no USB
 * callback ever has to wait on an SWD read. */
extern uint32_t gud_stat_buffers;
extern uint64_t gud_stat_bulk_bytes;
extern uint64_t gud_stat_raw_bytes;
extern uint64_t gud_stat_decomp_cycles;
extern uint32_t gud_stat_decomp_errors;
extern uint32_t gud_stat_ep_recoveries;
extern uint32_t gud_stat_errors;
extern uint32_t panel_buffers;
extern uint64_t panel_pixel_bytes;
extern uint64_t panel_blit_cycles;
extern uint64_t panel_sync_cycles;
extern uint32_t panel_resyncs;
extern uint32_t panel_tearfree;
extern uint32_t panel_last_x, panel_last_y, panel_last_w, panel_last_h;
extern uint32_t panel_controller_on;
extern uint32_t panel_display_on;
extern uint32_t buttons_reports;
extern uint32_t buttons_state;
extern uint8_t  buttons_hat;
extern int8_t   buttons_axis_x;
extern int8_t   buttons_axis_y;
extern int8_t   buttons_trigger_r;
extern uint32_t buttons_raw;

/* ------------------------------------------------------------------ */
/* Clock tree: 170 MHz from the 8 MHz external clock                    */
/* ------------------------------------------------------------------ */

/*
 *   PLL input = HSE / PLLM = 8 / 2  = 4 MHz    (must be 2.66..16 MHz)
 *   VCO       = 4 * PLLN   = 4 * 85 = 340 MHz  (must be 96..344 MHz)
 *   SYSCLK    = VCO / PLLR = 340 / 2 = 170 MHz (the part's maximum)
 *
 * PLLM = 2 rather than 1 because with PLLM = 1 the VCO would need N = 42.5,
 * which is not an integer -- 170 MHz is simply unreachable from an 8 MHz input
 * with PLLM = 1. That is why the stock MiSKo3 firmware runs at 168 MHz.
 */
static void clock_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void) RCC->APB1ENR1;

    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_VOS_Msk) | (0x1u << PWR_CR1_VOS_Pos);
    while (PWR->SR2 & PWR_SR2_VOSF) { }

    /* Boost mode is required above 150 MHz, and RM0440 wants the system clock
     * divided down while it engages. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE_Msk) | (0x8u << RCC_CFGR_HPRE_Pos);
    PWR->CR5 &= ~PWR_CR5_R1MODE;

    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | FLASH_ACR_LATENCY_4WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY_Msk) != FLASH_ACR_LATENCY_4WS) { }

    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    /* External clock, not a crystal: HSEBYP before HSEON. */
    RCC->CR |= RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) { }

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    RCC->PLLCFGR =
          (0x3u << RCC_PLLCFGR_PLLSRC_Pos)
        | ((2u - 1u) << RCC_PLLCFGR_PLLM_Pos)
        | (85u << RCC_PLLCFGR_PLLN_Pos)
        | (0x0u << RCC_PLLCFGR_PLLR_Pos)
        | RCC_PLLCFGR_PLLREN;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    RCC->CFGR &= ~(RCC_CFGR_PPRE1_Msk | RCC_CFGR_PPRE2_Msk);
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) { }

    for (volatile int i = 0; i < 1000; i++) { }
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;
}

/* ------------------------------------------------------------------ */
/* USB 48 MHz clock: HSI48 disciplined by CRS                           */
/* ------------------------------------------------------------------ */

/*
 * The USB peripheral needs a 48 MHz reference. RCC_CCIPR.CLK48SEL picks the
 * source: 00 = HSI48, 10 = the PLL's Q output.
 *
 * HSI48 is forced here, not preferred. Reaching 170 MHz pins the VCO at
 * 340 MHz (SYSCLK = VCO/PLLR with PLLR in {2,4,6,8}, and the G4's VCO range is
 * 96..344 MHz, so 340 is the only legal value). PLLQ divides that same
 * 340 MHz by 2, 4, 6 or 8, giving 170 / 85 / 56.67 / 42.5 MHz. None is 48. So
 * 170 MHz SYSCLK and a 48 MHz PLLQ are mutually exclusive on an 8 MHz input.
 *
 * HSI48 on its own is not good enough. It is an RC oscillator: about +/-1%
 * trimmed at 25 C, drifting to roughly +/-3% over voltage and temperature,
 * while USB full speed demands +/-0.25%. CRS closes that gap. The host sends a
 * Start-Of-Frame packet every 1 ms; CRS counts HSI48 cycles between SOFs,
 * compares against the expected 48000, and nudges the HSI48 trim register to
 * null the error. That is what makes crystal-less USB legal rather than lucky.
 *
 * The stock MiSKo3 firmware enables HSI48 and never configures CRS. It works
 * on a warm desk and is out of specification over temperature.
 */
static void usb_clock_init(void)
{
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) { }

    RCC->CCIPR &= ~RCC_CCIPR_CLK48SEL_Msk;   /* CLK48SEL = 00 = HSI48 */

    RCC->APB1ENR1 |= RCC_APB1ENR1_CRSEN;
    (void) RCC->APB1ENR1;

    CRS->CFGR =
          (47999u << CRS_CFGR_RELOAD_Pos)   /* 48 MHz / 1 kHz SOF, minus 1  */
        | (34u << CRS_CFGR_FELIM_Pos)       /* error limit, ST's default    */
        | CRS_CFGR_SYNCSRC_1;               /* 10 = USB SOF as sync source  */

    /* AUTOTRIMEN lets the hardware adjust the HSI48 trim with no CPU
     * involvement; CEN starts the frequency error counter. */
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    RCC->APB1ENR1 |= RCC_APB1ENR1_USBEN;
    (void) RCC->APB1ENR1;
}

/*
 * PA11 = USB_DM, PA12 = USB_DP need no GPIO configuration on the G4: once
 * USBEN is set the transceiver takes the pads over directly, and they are left
 * in their reset (analog) state deliberately. There is also no VDDUSB supply
 * gate on this family -- PWR_CR2.USV is an L4/L5 feature, which is why the G4
 * HAL has no HAL_PWREx_EnableVddUSB() at all.
 */

/* ------------------------------------------------------------------ */
/* DWT, used only for the millisecond tick                              */
/* ------------------------------------------------------------------ */

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t dwt_now(void) { return DWT->CYCCNT; }

static void delay_ms(uint32_t ms)
{
    uint32_t start = dwt_now();
    uint32_t target = ms * (SYSCLK_HZ / 1000u);
    while ((dwt_now() - start) < target) { }
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                           */
/* ------------------------------------------------------------------ */

/*
 * The G4 splits USB into high- and low-priority vectors; TinyUSB has one
 * handler, so both are routed to it. In practice only USB_LP fires for a
 * device like this, but wiring both costs nothing and avoids a silent hang if
 * an isochronous or double-buffered transfer ever raises USB_HP.
 */
void USB_LP_IRQHandler(void) { tud_int_handler(0); }
void USB_HP_IRQHandler(void) { tud_int_handler(0); }

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    clock_init();
    dwt_init();
    rtt_init();

    delay_ms(300);

    rtt_printf("\r\n=== MiSKo3 M9: HAL joystick, on-screen fps, XPT2046 touch ===\r\n");
    rtt_printf("SYSCLK 170 MHz | USB clock = HSI48 + CRS (SOF-disciplined)\r\n");
    /* Read the IDs back out of the descriptor we actually hand the host, rather
     * than repeating them here -- a hardcoded banner goes stale the moment the
     * descriptor changes, which is exactly what happened when these moved from
     * 1209:4FB3 to 1D50:614D. */
    {
        const tusb_desc_device_t *dd =
            (const tusb_desc_device_t *) tud_descriptor_device_cb();
        rtt_printf("VID:PID %04X:%04X, interface class 0xFF -- the gud driver\r\n"
                   "binds only this combination.\r\n",
                   (unsigned) dd->idVendor, (unsigned) dd->idProduct);
    }
    rtt_printf("mode %ux%u RGB565, max_buffer_size %lu B (%lu lines)\r\n",
               (unsigned) GUD_WIDTH, (unsigned) GUD_HEIGHT,
               (unsigned long) GUD_MAX_BUFFER_SIZE,
               (unsigned long) (GUD_MAX_BUFFER_SIZE / (GUD_WIDTH * 2u)));

    usb_clock_init();

    rtt_printf("CRS->CR=%08lX CRS->CFGR=%08lX CLK48SEL=%lu\r\n",
               (unsigned long) CRS->CR, (unsigned long) CRS->CFGR,
               (unsigned long) ((RCC->CCIPR & RCC_CCIPR_CLK48SEL_Msk)
                                >> RCC_CCIPR_CLK48SEL_Pos));

    /* USB interrupts must be enabled before tud_init() brings the pull-up up,
     * or the first host transaction can be missed. */
    NVIC_SetPriority(USB_LP_IRQn, 5);
    NVIC_SetPriority(USB_HP_IRQn, 5);
    NVIC_EnableIRQ(USB_LP_IRQn);
    NVIC_EnableIRQ(USB_HP_IRQn);

    buttons_init();
    joystick_init();
    touch_init();

    /* Panel first, so colour bars appear before USB is even up. A blank panel
     * from here on means the display path is broken, not that the host has yet
     * to connect. */
    panel_init();
    {
        uint32_t lines = panel_scan_total_lines();
        uint32_t per = panel_scan_period_cycles();
        uint32_t us = (uint32_t) ((uint64_t) per / (SYSCLK_HZ / 1000000u));
        uint32_t hz_x100 = us ? (uint32_t) (100000000u / us) : 0u;

        rtt_printf("panel up: ILI9341 landscape %ux%u RGB565, colour bars shown\r\n",
                   (unsigned) GUD_WIDTH, (unsigned) GUD_HEIGHT);
        rtt_printf("scan-out measured: %lu lines, %lu us/frame = %lu.%02lu Hz, "
                   "%lu cycles/line\r\n",
                   (unsigned long) lines, (unsigned long) us,
                   (unsigned long) (hz_x100 / 100), (unsigned long) (hz_x100 % 100),
                   (unsigned long) (per / lines));
        rtt_printf("GET_SCANLINE counts %lu lines but the image is only %u tall,\r\n"
                   "so it tracks image X: the panel refreshes column by column.\r\n",
                   (unsigned long) lines, (unsigned) GUD_HEIGHT);
        rtt_printf("press BTN_OK to toggle tear-free mode (now: ON)\r\n");
    }

    tusb_init();
    rtt_printf("tusb_init() done: interface 0 = GUD (vendor 0xFF), "
               "interface 1 = HID gamepad\r\n");
    rtt_printf("joystick: ADC4 %s, centre x=%u y=%u\r\n",
               joystick_present() ? "OK" : "*** NOT PRESENT ***",
               (unsigned) joystick_centre_x(), (unsigned) joystick_centre_y());
    rtt_printf("touch: XPT2046 on SPI1 %s (IRQ PD6, CS PE1, PG2/3/4)\r\n",
               touch_present() ? "OK" : "*** NOT RESPONDING ***");
    rtt_printf("joystick wiring: PB14 (X) %s, PB15 (Y) %s\r\n",
               joy_x_driven ? "DRIVEN" : "*** FLOATING - not connected ***",
               joy_y_driven ? "DRIVEN" : "*** FLOATING - not connected ***");
    rtt_printf("waiting for the host...\r\n");

    uint32_t ms_ticks = 0;
    uint32_t last_tick = dwt_now();
    uint32_t last_report = 0;
    uint32_t last_mounted = 0xFFu;

    uint32_t last_reports = 0xFFFFFFFFu;
    uint32_t prev_chord = 0;
    uint32_t last_hid_log = 0;
    uint32_t last_touch_log = 0, last_touch_reports = 0;
    uint32_t peak_wire_kbps = 0, peak_px_kbps = 0;
    uint64_t prev_bytes = 0, prev_cycles = 0;
    uint64_t prev_wire = 0, prev_decomp = 0, prev_sync = 0, prev_ovl = 0;
    uint32_t prev_bufs = 0;
    int was_active = 0;

    for (;;) {
        tud_task();
        buttons_task(ms_ticks);
        touch_task(ms_ticks);

        /* Roll a 1 ms tick from the cycle counter. */
        if ((dwt_now() - last_tick) >= (SYSCLK_HZ / 1000u)) {
            last_tick += (SYSCLK_HZ / 1000u);
            ms_ticks++;
        }

        /* Report mount transitions, so enumeration is visible as it happens. */
        uint32_t mounted = tud_mounted() ? 1u : 0u;
        if (mounted != last_mounted) {
            last_mounted = mounted;
            rtt_printf("[usb] %s (CRS ISR=%08lX)\r\n",
                       mounted ? "MOUNTED" : "not mounted",
                       (unsigned long) CRS->ISR);
        }

        /*
         * Once a second, report what actually moved.
         *
         * Printed here rather than from the USB callbacks: RTT is drained by
         * the host over SWD while the target runs, and a blocked write inside
         * a callback would stall the USB state machine. Deltas are taken
         * against the previous snapshot so the numbers are a rate, not a
         * lifetime total.
         */
        if ((ms_ticks - last_report) >= 1000u) {
            uint32_t elapsed_ms = ms_ticks - last_report;

            /* Re-test the joystick wiring once a second and report any pin
             * that looked disconnected. An intermittent joint shows up here
             * as a non-zero count that creeps up over minutes. */
            joystick_wiring_poll();
            if (joy_x_float_count || joy_y_float_count)
                rtt_printf("[joy] wiring: %lu checks, X floated %lu, "
                           "Y floated %lu\r\n",
                           (unsigned long) joy_wiring_checks,
                           (unsigned long) joy_x_float_count,
                           (unsigned long) joy_y_float_count);
            last_report = ms_ticks;

            uint64_t bytes = panel_pixel_bytes;
            uint64_t cycles = panel_blit_cycles;
            uint64_t wire = gud_stat_bulk_bytes;
            uint64_t decomp = gud_stat_decomp_cycles;
            uint64_t sync = panel_sync_cycles;
            uint32_t bufs = panel_buffers;

            uint32_t d_bytes = (uint32_t) (bytes - prev_bytes);
            uint32_t d_cycles = (uint32_t) (cycles - prev_cycles);
            uint32_t d_wire = (uint32_t) (wire - prev_wire);
            uint32_t d_decomp = (uint32_t) (decomp - prev_decomp);
            uint32_t d_sync = (uint32_t) (sync - prev_sync);
            uint32_t d_ovl = (uint32_t) (panel_overlay_cycles - prev_ovl);
            uint32_t d_bufs = bufs - prev_bufs;

            prev_bytes = bytes;
            prev_cycles = cycles;
            prev_wire = wire;
            prev_decomp = decomp;
            prev_sync = sync;
            prev_ovl = panel_overlay_cycles;
            prev_bufs = bufs;

            if (d_bufs == 0u) {
                /* GUD is damage-driven: a display whose contents are not
                 * changing receives nothing at all. Silence here is correct
                 * behaviour, not a stall, so say so once rather than
                 * repeating a row of zeroes. */
                if (was_active) {
                    was_active = 0;
                    rtt_printf("[rate] idle - nothing on screen is changing\r\n");
                }
            } else {
                was_active = 1;

                /* kB/s, and the equivalent rate in whole 320x240 frames.
                 * A full RGB565 frame is 153600 bytes; damage rectangles mean
                 * the real update rate is usually higher than this figure. */
                uint32_t kbps = (uint32_t) (((uint64_t) d_bytes * 1000u)
                                            / elapsed_ms / 1000u);

                /*
                 * Peak rates since boot, latched.
                 *
                 * These exist so throughput can be measured with no debugger
                 * running. RTT output is dropped while nothing is draining it,
                 * but the counters keep accumulating -- so the board can be
                 * driven hard with probe-rs detached, then attached afterwards
                 * to read what it reached. That is the only way to answer
                 * "does SWD traffic cost us bandwidth?" on a board whose
                 * ST-LINK cannot be unplugged separately.
                 */
                uint32_t wire_kbps = (uint32_t) (((uint64_t) d_wire * 1000u)
                                                 / elapsed_ms / 1000u);
                if (wire_kbps > peak_wire_kbps) peak_wire_kbps = wire_kbps;
                if (kbps > peak_px_kbps)        peak_px_kbps = kbps;
                uint32_t fps_centi = (uint32_t) (((uint64_t) d_bytes * 100000u)
                                                 / elapsed_ms / 153600u);

                /* Microseconds of blit per second, and what share of the
                 * wall clock that is. This is the number that says whether
                 * the FMC or USB is the limit. */
                uint32_t blit_us = (uint32_t) ((uint64_t) d_cycles
                                               / (SYSCLK_HZ / 1000000u));
                /*
                 * Compression ratio, times 100. This is the number the whole
                 * milestone turns on: pixels produced divided by bytes that
                 * had to cross the wire.
                 */
                uint32_t ratio_x100 = d_wire ? (uint32_t) (((uint64_t) d_bytes * 100u)
                                                           / d_wire) : 0u;

                /* Microseconds per second spent decompressing, and blitting. */
                uint32_t dec_us = (uint32_t) ((uint64_t) d_decomp
                                              / (SYSCLK_HZ / 1000000u));
                uint32_t cpu_pct = (uint32_t) (((uint64_t) (blit_us + dec_us
                                                 + (uint64_t) d_sync
                                                   / (SYSCLK_HZ / 1000000u)) * 100u)
                                               / ((uint64_t) elapsed_ms * 1000u));

                uint32_t sync_us = (uint32_t) ((uint64_t) d_sync
                                               / (SYSCLK_HZ / 1000000u));
                uint32_t ovl_us = (uint32_t) ((uint64_t) d_ovl
                                              / (SYSCLK_HZ / 1000000u));

                rtt_printf("[rate] %s %lu rect/s | wire %lu kB/s -> px %lu kB/s "
                           "(%lu.%02lux) = %lu.%02lu full fps | dec %lu + blit %lu "
                           "+ sync %lu us + ovl %lu us = %lu%% cpu | err %lu/%lu rec %lu | "
                           "PEAK wire %lu px %lu kB/s\r\n",
                           panel_tearfree ? "TF" : "--",
                           (unsigned long) d_bufs,
                           (unsigned long) ((uint64_t) d_wire * 1000u / elapsed_ms / 1000u),
                           (unsigned long) kbps,
                           (unsigned long) (ratio_x100 / 100),
                           (unsigned long) (ratio_x100 % 100),
                           (unsigned long) (fps_centi / 100),
                           (unsigned long) (fps_centi % 100),
                           (unsigned long) dec_us,
                           (unsigned long) blit_us,
                           (unsigned long) sync_us,
                           (unsigned long) ovl_us,
                           (unsigned long) cpu_pct,
                           (unsigned long) gud_stat_decomp_errors,
                           (unsigned long) gud_stat_errors,
                           (unsigned long) gud_stat_ep_recoveries,
                           (unsigned long) peak_wire_kbps,
                           (unsigned long) peak_px_kbps);
            }
        }

        /*
         * Button changes are reported as they happen rather than on the
         * one-second timer, because the point of this milestone is watching a
         * press register. buttons_task() only sends a HID report when
         * something actually changed, so this cannot flood the console.
         */
        /*
         * Log HID activity, but rate-limited to 4 lines a second.
         *
         * Without the limit the analogue stick alone produces a report -- and
         * a line -- at up to 100 Hz, which floods the console faster than SWD
         * can drain it. That is a debug feature costing real bandwidth in the
         * main loop, so it is capped.
         */
        /*
         * Touch, logged only while something is happening and at most four
         * times a second. This is the line to watch when settling the
         * orientation flags in touch.h: touch the top-left corner of the
         * image and the mapped pair should read close to 0,0.
         */
        if (touch_reports != last_touch_reports
            && (ms_ticks - last_touch_log) >= 250u) {
            last_touch_reports = touch_reports;
            last_touch_log = ms_ticks;
            rtt_printf("[touch] %s raw=%u,%u -> %u,%u (of 4095) "
                       "| reports %lu presses %lu\r\n",
                       touch_down ? "DOWN" : "up  ",
                       (unsigned) touch_raw_x, (unsigned) touch_raw_y,
                       (unsigned) touch_x, (unsigned) touch_y,
                       (unsigned long) touch_reports,
                       (unsigned long) touch_presses);
        }

        if (buttons_reports != last_reports && (ms_ticks - last_hid_log) >= 250u) {
            last_reports = buttons_reports;
            last_hid_log = ms_ticks;

            /*
             * Tear-free toggle: BTN_ESC and JOY_BTN held together.
             *
             * This was BTN_OK alone while M6 was being tested, which is wrong
             * now that the board is a gamepad: BTN_OK is button A, so every
             * jump in a game would have flipped the display's blit strategy.
             *
             * A chord of two buttons that are never pressed together in normal
             * play (B plus the stick click) keeps the diagnostic available
             * without it firing by accident. Both buttons still report to the
             * host as themselves; the chord is only watched here.
             *
             * Edge-triggered on the chord forming, so holding it is one toggle.
             */
            uint32_t chord = ((buttons_state & 0x6u) == 0x6u) ? 1u : 0u;

            if (chord && !prev_chord) {
                panel_tearfree = !panel_tearfree;
                rtt_printf("[sync] tear-free mode %s\r\n",
                           panel_tearfree ? "ON (column-major, scan-ordered)"
                                          : "OFF (row-major, M5 behaviour)");
            }
            prev_chord = chord;

            /* PINS shows which physical switch was actually pressed, before
             * any A/B/X/Y interpretation, so the mapping can be established
             * by pressing things one at a time. */
            rtt_printf("[hid] PINS G0=%lu G1=%lu G6=%lu G8=%lu C13=%lu C14=%lu "
                       "C15=%lu | btn=%08lX hat=%u stick=%d,%d rt=%d | "
                       "raw x=%u (%u..%u) y=%u (%u..%u) ctr=%u,%u\r\n",
                       (unsigned long) ((buttons_raw >> 0) & 1u),
                       (unsigned long) ((buttons_raw >> 1) & 1u),
                       (unsigned long) ((buttons_raw >> 2) & 1u),
                       (unsigned long) ((buttons_raw >> 3) & 1u),
                       (unsigned long) ((buttons_raw >> 4) & 1u),
                       (unsigned long) ((buttons_raw >> 5) & 1u),
                       (unsigned long) ((buttons_raw >> 6) & 1u),
                       (unsigned long) buttons_state,
                       (unsigned) buttons_hat,
                       (int) buttons_axis_x, (int) buttons_axis_y,
                       (int) buttons_trigger_r,
                       (unsigned) joy_raw_x, (unsigned) joy_min_x,
                       (unsigned) joy_max_x,
                       (unsigned) joy_raw_y, (unsigned) joy_min_y,
                       (unsigned) joy_max_y,
                       (unsigned) joystick_centre_x(),
                       (unsigned) joystick_centre_y());
        }
    }
}

/*
 * Called from the startup file before main().
 *
 * Enabling the FPU has to happen this early. The Cortex-M4F's single-precision
 * FPU sits in coprocessor slots CP10 and CP11, and out of reset both are set
 * to "access denied" in the Coprocessor Access Control Register. Writing 0b11
 * into each of their 2-bit fields (CPACR bits 20-21 and 22-23) grants full
 * access.
 *
 * This is not optional even though nothing here uses a float. We build with
 * -mfloat-abi=hard, so GCC may use the VFP register file for anything it likes
 * -- and at -O2 it does, emitting instructions like `vpush {d8}` to move 64
 * bits at a time. Executing one with the FPU disabled raises a UsageFault with
 * UFSR.NOCP set, which escalates straight to HardFault. That cost an hour in
 * M1 before the fault registers were read.
 *
 * DSB then ISB: the DSB makes sure the CPACR store has reached the register,
 * the ISB flushes the pipeline so nothing fetched under the old permissions is
 * still in flight.
 */
void SystemInit(void)
{
    SCB->CPACR |= (0x3u << 20) | (0x3u << 22);
    __DSB();
    __ISB();
}
