/*
 * The board's controls, presented to Linux as a HID gamepad.
 *
 * Pin map. The switches are from the board's own main.h; the two analogue axes
 * were traced from the schematic and are handled in joystick.c:
 *
 *   PG0  top switch     PG1  bottom switch
 *   PG6  left switch    PG8  right switch     -> face buttons Y/A/X/B
 *   PC15 BTN_OK         -> right trigger (BTN_TR2 + ABS_RZ)
 *   PC14 BTN_ESC        -> left trigger  (BTN_TL2 + ABS_Z)
 *   PC13 JOY_BTN        -> left stick click (BTN_THUMBL)
 *   PB14 JOY_X, PB15 JOY_Y (ADC4) -> left stick, see joystick.c
 *
 * The four direction switches were the D-pad up to this point, standing in for
 * a thumbstick nobody had noticed was on the board. Now that the stick is read,
 * they are free to be the face buttons a gamepad actually needs.
 *
 * Every switch connects its pin to ground and relies on the MCU's internal
 * pull-up, so an idle button reads 1 and a pressed button reads 0. The stock
 * firmware confirms the polarity: it drives the LEDs with the logical inverse
 * of each button read.
 *
 * PC14 and PC15 double as the LSE oscillator pins. They are usable as plain
 * GPIO here only because we never enable the LSE -- the 48 MHz USB reference
 * comes from HSI48 disciplined by CRS, and nothing in this project needs a
 * 32.768 kHz clock.
 *
 * None of these pins collides with the FMC bus (ports D, E and G5) or with
 * USB (PA11/PA12), so the gamepad and the display genuinely are independent.
 */

#include "stm32g4xx.h"
#include "tusb.h"
#include "buttons.h"
#include "joystick.h"

void buttons_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIOGEN;
    (void) RCC->AHB2ENR;

    const uint32_t pc[] = { 13, 14, 15 };
    for (unsigned i = 0; i < sizeof(pc) / sizeof(pc[0]); i++) {
        GPIOC->MODER &= ~(3u << (pc[i] * 2));                  /* 00 = input   */
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3u << (pc[i] * 2)))
                     | (1u << (pc[i] * 2));                    /* 01 = pull-up */
    }

    const uint32_t pg[] = { 0, 1, 6, 8 };
    for (unsigned i = 0; i < sizeof(pg) / sizeof(pg[0]); i++) {
        GPIOG->MODER &= ~(3u << (pg[i] * 2));
        GPIOG->PUPDR = (GPIOG->PUPDR & ~(3u << (pg[i] * 2)))
                     | (1u << (pg[i] * 2));
    }
}

/* Active low: return 1 when the button is pressed. */
static inline int btn(GPIO_TypeDef *port, uint32_t pin)
{
    return (port->IDR & (1u << pin)) ? 0 : 1;
}

/* Visible to main() for the console summary. */
uint32_t buttons_reports;
uint32_t buttons_state;
uint8_t  buttons_hat;
int8_t   buttons_axis_x;
int8_t   buttons_axis_y;
int8_t   buttons_trigger_r;

/*
 * Raw switch states, before any mapping is applied: one bit per pin, in the
 * order below. This exists because the net names in the board documentation
 * (BTN_UP, BTN_OK, BTN_ESC...) do not reliably say which physical switch is
 * which -- the same documentation already had three LCD pins rotated. Pressing
 * a control and reading this tells us the truth.
 */
uint32_t buttons_raw;
uint32_t buttons_raw_changed;

/*
 * Full deflection, in the units the report descriptor declares:
 * TUD_HID_REPORT_DESC_GAMEPAD() sets logical minimum -127 and maximum +127 for
 * each 8-bit axis. -128 is deliberately not used, so that the two directions
 * are symmetric about zero and a game that negates an axis cannot overflow.
 */
#define AXIS_MAX 127

/*
 * Button numbering: TinyUSB's aliases, which follow Linux.
 *
 * An earlier version of this file used raw bit indices 0,1,2,3 for A,B,X,Y on
 * the assumption that SDL's positional ordering was what mattered. Testing on
 * the board disproved it. Linux's hid-input.c maps a HID gamepad's buttons
 * straight onto the evdev BTN_* block, which has a gap that the SDL ordering
 * does not:
 *
 *      bit 0 BTN_A(SOUTH)   1 BTN_B(EAST)   2 BTN_C     3 BTN_X(NORTH)
 *      4 BTN_Y(WEST)        5 BTN_Z         6 BTN_TL    7 BTN_TR
 *      8 BTN_TL2            9 BTN_TR2      10 SELECT   11 START
 *     12 MODE              13 THUMBL       14 THUMBR
 *
 * BTN_C sits between B and X, so the naive 0,1,2,3 put X's press on BTN_C --
 * a button most software does not recognise, which showed up as "the one that
 * should be X does nothing" -- and shifted the intended Y onto BTN_X, which
 * showed up as "the one reading X should be Y". Both symptoms are explained by
 * that single missing gap.
 *
 * TinyUSB's GAMEPAD_BUTTON_* aliases already encode this table correctly, so
 * they are used directly rather than re-derived here.
 */
#define PAD_A     GAMEPAD_BUTTON_A        /* bit 0  */
#define PAD_B     GAMEPAD_BUTTON_B        /* bit 1  */
#define PAD_X     GAMEPAD_BUTTON_X        /* bit 3  */
#define PAD_Y     GAMEPAD_BUTTON_Y        /* bit 4  */
#define PAD_LT    GAMEPAD_BUTTON_TL2      /* bit 8  */
#define PAD_RT    GAMEPAD_BUTTON_TR2      /* bit 9  */
#define PAD_LS    GAMEPAD_BUTTON_THUMBL   /* bit 13 */


void buttons_task(uint32_t ms_ticks)
{
    static uint32_t next_ms = 0;
    static uint32_t prev_buttons = 0xFFFFFFFFu;
    static uint8_t  prev_hat = 0xFFu;
    static int8_t   prev_x = 1, prev_y = 1;   /* impossible values: force a
                                               * first report after mount */
    static int8_t   prev_tr = 0, prev_tl = 0;   /* != resting value: forces a
                                                 * first report after mount */

    /*
     * The pins are read whether or not USB can take a report.
     *
     * This check used to sit here, above everything, which meant that when the
     * HID endpoint was busy the switches were not even sampled. That is fine
     * on an idle desktop and wrong under load: at 88 rectangles a second the
     * blit and the decompressor starve tud_task(), the endpoint stays busy,
     * and buttons_task() returned at its first line for minutes at a time.
     * Measured that way, only three reports got through in three minutes and
     * the diagnostic chords in main() could never form.
     *
     * Reading is cheap and has nothing to do with USB. Only the report send
     * below waits for the endpoint.
     */

    /* 100 Hz, matching the 10 ms bInterval in the endpoint descriptor. Polling
     * faster than the host asks for would just burn CPU. */
    if ((int32_t) (ms_ticks - next_ms) < 0)
        return;
    next_ms = ms_ticks + 10u;

    /*
     * The four direction switches are the face buttons, laid out to match
     * where they physically sit: an Xbox pad has Y at the top, A at the
     * bottom, X on the left and B on the right, and so does this board's
     * four-way cluster. Pressing the top switch giving Y is what a player
     * expects without being told.
     */
    int up    = btn(GPIOG, 0);
    int down  = btn(GPIOG, 1);
    int left  = btn(GPIOG, 6);
    int right = btn(GPIOG, 8);

    int sw_c13 = btn(GPIOC, 13);
    int sw_c14 = btn(GPIOC, 14);
    int sw_c15 = btn(GPIOC, 15);

    buttons_raw = (uint32_t) (up | (down << 1) | (left << 2) | (right << 3)
                            | (sw_c13 << 4) | (sw_c14 << 5) | (sw_c15 << 6));

    uint32_t buttons = 0;
    if (up)    buttons |= PAD_Y;                 /* PG0 top    */
    if (down)  buttons |= PAD_A;                 /* PG1 bottom */
    if (left)  buttons |= PAD_X;                 /* PG6 left   */
    if (right) buttons |= PAD_B;                 /* PG8 right  */

    /*
     * Triggers and the stick click. Pin identities confirmed by pressing each
     * control and reading the raw GPIO state, not taken from the pin table:
     * PC15 is OK, PC14 is ESC, PC13 is the stick's own push switch.
     */
    if (sw_c15) buttons |= PAD_RT;               /* OK      -> right trigger */
    if (sw_c14) buttons |= PAD_LT;               /* ESC     -> left trigger  */
    if (sw_c13) buttons |= PAD_LS;               /* JOY_BTN -> stick click   */

    /*
     * Movement comes from the analogue thumbstick on PB14/PB15, read through
     * ADC4. This is the hardware the board always had and that earlier
     * milestones never used -- the direction switches were standing in for it.
     */
    int8_t axis_x, axis_y;
    joystick_read(&axis_x, &axis_y);

    /*
     * The triggers, also as axes.
     *
     * On a real pad a trigger is analogue, and different software looks for it
     * in different places: some read the button (BTN_TL2 / BTN_TR2, set
     * above), some read an axis. TUD_HID_REPORT_DESC_GAMEPAD() declares Z and
     * Rz, which Linux exposes as ABS_Z and ABS_RZ, and those are the
     * conventional homes for the left and right trigger. Reporting both costs
     * nothing and means neither kind of game has to be configured by hand.
     *
     * A switch has no travel, so each reads either fully released or fully
     * pressed -- and "released" has to be the logical MINIMUM, -127, not zero.
     *
     * This was 0 at first, on the theory that a game reading Z or Rz as a
     * second stick would otherwise see that stick permanently pegged. Testing
     * in Steam disproved it. Steam treats these as triggers and maps the
     * declared -127..+127 onto 0..32767, so resting at 0 landed exactly
     * half way: the trigger read 16303 untouched and 32767 pressed, never
     * crossing "released", and so never registered as a press at all.
     *
     * Resting at the minimum is simply what a trigger axis is: at rest it
     * reads 0% travel, pressed it reads 100%.
     */
    int8_t trigger_l = sw_c14 ? AXIS_MAX : -AXIS_MAX;
    int8_t trigger_r = sw_c15 ? AXIS_MAX : -AXIS_MAX;

    /*
     * Derive the hat from the stick, so games that only read a D-pad still
     * work now that the direction switches have become face buttons.
     */
    /*
     * The hat stays centred. Nothing on this board drives it.
     *
     * It used to be derived from the analogue stick, so that games reading
     * only a D-pad would still respond. That turned out to be worse than the
     * problem it solved: a game that reads both the stick and the hat sees one
     * physical movement twice and moves at double speed, which is what happens
     * on a real pad only when two separate controls are pushed together.
     *
     * The four direction switches cannot take the hat over either. They are
     * the face buttons A, B, X and Y, and a switch cannot be in two places at
     * once.
     *
     * The field is still declared in the report descriptor and still sent, at
     * its centred value. Removing it would change the report layout and break
     * any mapping a host has already learned for this device.
     */
    uint8_t hat = GAMEPAD_HAT_CENTERED;

    /*
     * Only transmit on change.
     *
     * This matters more here than in phase 2. The gamepad and the display share
     * one 12 Mbit/s full-speed bus, and an interrupt endpoint with a 10 ms
     * interval reserves a slot in every frame whether or not we use it. Sending
     * nothing when nothing has changed leaves that bandwidth to the pixels.
     */
    /*
     * Publish the state now, before anything can return early. main() watches
     * it for the chords, and those must keep working while the display is
     * saturated, which is exactly when a diagnostic toggle is wanted.
     */
    buttons_state = buttons;
    buttons_hat = hat;
    buttons_axis_x = axis_x;
    buttons_axis_y = axis_y;
    buttons_trigger_r = trigger_r;

    if (buttons == prev_buttons && hat == prev_hat &&
        axis_x == prev_x && axis_y == prev_y &&
        trigger_r == prev_tr && trigger_l == prev_tl)
        return;

    buttons_raw_changed++;

    /* Something changed, so a report is due. If the endpoint is still busy,
     * leave prev_* alone and try again on the next pass rather than losing
     * the change. */
    if (!tud_hid_ready())
        return;

    prev_buttons = buttons;
    prev_hat = hat;
    prev_x = axis_x;
    prev_y = axis_y;
    prev_tr = trigger_r;
    prev_tl = trigger_l;

    /*
     * Report ID 0, then the six axes in descriptor order: X, Y, Z, Rz, Rx, Ry.
     * X and Y are the thumbstick, Z is the left trigger and Rz the right.
     * Rx and Ry would be a second stick, which this board does not have.
     */
    tud_hid_gamepad_report(0, axis_x, axis_y, trigger_l, trigger_r,
                           0, 0, hat, buttons);

    buttons_reports++;
}
