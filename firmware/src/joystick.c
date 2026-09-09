/*
 * The analogue thumbstick, read through ADC4.
 *
 * WHERE THIS CAME FROM
 *
 * The board carries a PSP-style two-axis thumbstick (component "PSP_joystick"
 * on the "JOYSTICK + LED" schematic sheet). CLAUDE.md's pin table lists only
 * JOY_BTN, so the two analogue axes were missed until now. Traced from the
 * schematic PDF and confirmed against the stock firmware's CubeMX
 * configuration (MiSKo3/Firmware/Test/Test.ioc):
 *
 *      JOY_X  = PB14 = ADC4_IN4   single-ended, GPIO_MODE_ANALOG
 *      JOY_Y  = PB15 = ADC4_IN5   single-ended, GPIO_MODE_ANALOG
 *      JOY_BTN = PC13             plain GPIO, handled in buttons.c
 *
 * The stock firmware sets ADC4 up for exactly two conversions on channels 4
 * and 5 with a 640.5-cycle sampling time, which is the same pairing this file
 * uses.
 *
 * WHY 640.5 CYCLES
 *
 * A thumbstick is a pair of potentiometers, and a potentiometer is a high
 * impedance source: the ADC's internal sampling capacitor has to charge
 * through it before the conversion starts. Too short a sampling window and the
 * reading is dragged towards whatever the previous conversion left on the
 * capacitor. 640.5 cycles at 42.5 MHz is 15 us per axis -- enormously
 * conservative, costing 31 us of the 10 ms between polls, which is 0.3% of one
 * core. There is no reason to be clever here.
 */

#include "stm32g4xx.h"
#include "stm32g4xx_hal.h"
#include "joystick.h"

#define SYSCLK_HZ 170000000u

/* Channel numbers on ADC4, from the pin mapping above. */
#define JOY_CH_X 4u
#define JOY_CH_Y 5u

/*
 * Resting position of each axis -- measured at boot and then tracked.
 *
 * A cheap stick does not sit at exactly half scale and the two axes do not
 * agree, so the centre has to be measured rather than assumed to be 2048. That
 * much was obvious. What was not obvious until it was logged is that the rest
 * position also *moves*: on this board the Y axis calibrated at 1967 on one
 * boot and 1931 on the next, and wandered between 1699 and 1999 within a single
 * session -- about 300 counts, against a deadzone of 120.
 *
 * The symptom of a fixed centre against a drifting stick is not subtle. The
 * axis reports a small permanent deflection, so a character walks on its own,
 * and pushing the *other* way has to cross 300 counts of accumulated offset
 * before anything happens at all -- which is what "up and down sometimes just
 * doesn't work" actually was.
 *
 * So the centre follows the stick. Held in Q4 fixed point because the per-
 * sample correction is a fraction of an ADC count and would otherwise round to
 * zero and never move.
 */
static int32_t joy_centre_x_q4 = 2048 << 4;
static int32_t joy_centre_y_q4 = 2048 << 4;

/*
 * How far from the current centre a reading can be and still be treated as
 * "the stick is resting here". Wider than the deadzone, so drift that has
 * already pushed the reading out of the deadzone can still be pulled back;
 * far short of a deliberate push, so holding a direction cannot drag the zero
 * point with it.
 */
#define JOY_RECENTRE_WINDOW 400

/*
 * Time constant of that tracking, as a right shift in Q4. 8 gives 256 samples,
 * and buttons_task() polls at 100 Hz, so about 2.5 seconds -- slow enough that
 * a light sustained push is not silently cancelled within a game, fast enough
 * that the drift never accumulates into a visible offset.
 */
#define JOY_RECENTRE_SHIFT 8

/*
 * How long a reading may sit outside the re-centring window before the centre
 * is assumed wrong and snapped to it. 500 samples at the 100 Hz poll rate is
 * five seconds.
 *
 * This exists because the window tracking alone cannot recover from a bad boot
 * calibration. If the stick is disturbed, or its reading is unreliable, during
 * the 16 samples taken in joystick_init(), the centre can land hundreds of
 * counts away from the true rest position -- one boot on this board measured
 * Y at 1371 against a true rest near 1910. The reading then sits permanently
 * outside the window, tracking never engages, and the axis is stuck reporting
 * a large constant deflection: exactly "up and down do not work".
 *
 * Five seconds is long enough that holding a direction in a game is not
 * mistaken for a bad centre, and if it ever does snap wrongly the ordinary
 * window tracking pulls it back within a couple of seconds of release.
 */
#define JOY_SNAP_TICKS 500

/* Live values, exported so the console can show what the hardware is doing.
 * These are what any re-tuning of JOY_SPAN should be based on. */
uint16_t joy_raw_x, joy_raw_y;
uint16_t joy_min_x = 0xFFFFu, joy_max_x;
uint16_t joy_min_y = 0xFFFFu, joy_max_y;

/*
 * Deadzone, in raw ADC counts either side of centre.
 *
 * Below this the axis reports exactly zero. Without it the stick never quite
 * returns to its centre reading and a character drifts across the screen on
 * its own -- the single most noticeable defect an analogue stick can have.
 *
 * 200 counts of 4096 is about 5% of full scale, which is unremarkable for a
 * gamepad, and it is set against measurement: the X axis rests within a few
 * counts, but Y wanders by far more than the 120 this started at.
 */
#define JOY_DEADZONE 200

/*
 * Raw counts from centre that correspond to full deflection.
 *
 * Measured on this board rather than assumed: sweeping the X axis to both
 * stops gave a raw range of 912..3149 about a centre of 2007, i.e. roughly
 * +/-1100 counts. 1000 is set slightly inside that so every direction can
 * actually reach full deflection and clamp, rather than topping out at about
 * 80% as the initial guess of 1400 did.
 */
#define JOY_SPAN 1000

/*
 * Whether the ADC came up.
 *
 * joystick_init() runs before the panel and before USB, so a hardware fault
 * here must not be able to wedge the board: every wait below is bounded, and
 * if any of them times out the stick is simply marked absent and the display
 * and buttons carry on working. A dead stick is a nuisance; a dead display is
 * the whole project.
 */
static int joy_present;

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t target = us * (SYSCLK_HZ / 1000000u);
    while ((DWT->CYCCNT - start) < target) { }
}

/*
 * Spin until (*reg & mask) == want, or give up after 10 ms.
 * Returns 1 on success, 0 on timeout.
 */
static int wait_until(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t limit = 10u * (SYSCLK_HZ / 1000u);

    while ((*reg & mask) != want) {
        if ((DWT->CYCCNT - start) > limit)
            return 0;
    }
    return 1;
}

/* ADC4 handle, which HAL needs on every call. */
static ADC_HandleTypeDef hadc4;

/*
 * One conversion on the requested channel, through HAL.
 *
 * Against the direct version in m7: there we wrote the channel number into
 * SQR1 and set ADSTART. HAL_ADC_ConfigChannel() does the same and additionally
 * sets the sampling time and checks that the channel exists on this ADC.
 *
 * HAL_ADC_PollForConversion() takes a timeout in milliseconds, which is the
 * equivalent of our own wait_until(). It gets those milliseconds from
 * HAL_GetTick(), derived from the DWT counter in hal_glue.c.
 */
static uint16_t adc_read(uint32_t channel)
{
    ADC_ChannelConfTypeDef ch = {0};

    ch.Channel      = (channel == JOY_CH_X) ? ADC_CHANNEL_4 : ADC_CHANNEL_5;
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset       = 0;

    if (HAL_ADC_ConfigChannel(&hadc4, &ch) != HAL_OK)
        return 2048;

    if (HAL_ADC_Start(&hadc4) != HAL_OK)
        return 2048;

    if (HAL_ADC_PollForConversion(&hadc4, 10u) != HAL_OK) {
        HAL_ADC_Stop(&hadc4);
        return 2048;            /* mid-scale: reads as centred, not deflected */
    }

    uint16_t v = (uint16_t) HAL_ADC_GetValue(&hadc4);
    HAL_ADC_Stop(&hadc4);
    return v;
}

/*
 * Is this pin actually connected to something, or is it floating?
 *
 * The internal pull-up and pull-down resistors are around 40 kohm. A
 * potentiometer wiper is a far lower impedance -- a few kohm for a 10 kohm pot
 * -- so it holds the pin at its own voltage no matter which internal resistor
 * is applied. A pin connected to nothing simply follows whichever one is
 * switched on.
 *
 * So: pull up, read; pull down, read. If the level changed, nothing is driving
 * the pin.
 *
 * This has to be done in digital input mode, not analogue: RM0440 disables the
 * pull-up and pull-down resistors when a pin is configured as analogue, which
 * is also why the ADC cannot be used for this test.
 *
 * Returns 1 if something is driving the pin, 0 if it is floating.
 */
static int pin_is_driven(uint32_t pin)
{
    GPIO_InitTypeDef g = {0};
    uint16_t mask = (uint16_t) (1u << pin);
    int with_up, with_down;

    g.Pin  = mask;
    g.Mode = GPIO_MODE_INPUT;

    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &g);
    delay_us(200);                       /* 40k against any stray capacitance */
    with_up = (HAL_GPIO_ReadPin(GPIOB, mask) == GPIO_PIN_SET);

    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOB, &g);
    delay_us(200);
    with_down = (HAL_GPIO_ReadPin(GPIOB, mask) == GPIO_PIN_SET);

/*
     * Back to analogue mode. The m7 version saved and restored the whole MODER
     * and PUPDR registers; HAL works per pin, so the final state is stated
     * explicitly. It is also the state the ADC needs.
     */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &g);

    return (with_up == with_down);
}

int joy_x_driven, joy_y_driven;

/*
 * Continuous wiring check.
 *
 * The one-shot test at boot said both pins were driven, but a one-shot test
 * cannot see an intermittent joint -- and the evidence pointed at exactly
 * that: X's resting reading varied by 49 counts across six boots while Y read
 * 1941/1931/1967/1990/1933 and then, once, 1371. Repeating the test lets the
 * failure rate be counted rather than argued about.
 *
 * Costs 400 us once a second, so 0.04% of the time, and briefly puts the pin
 * into digital input mode -- which is why it is called from the main loop and
 * never from inside a USB callback.
 */
uint32_t joy_wiring_checks;
uint32_t joy_x_float_count, joy_y_float_count;

void joystick_wiring_poll(void)
{
    if (!joy_present)
        return;

    if (!pin_is_driven(14))
        joy_x_float_count++;
    if (!pin_is_driven(15))
        joy_y_float_count++;

    joy_wiring_checks++;

    /* pin_is_driven() already leaves both pins in analogue mode, so nothing
     * is needed here. The m7 version had another MODER write at this point,
     * because that implementation only restored whatever it found on entry. */
}

void joystick_init(void)
{
    /*
     * HAL_Init() enables the instruction and data caches and calls
     * HAL_InitTick(), which hal_glue.c overrides so that no SysTick interrupt
     * is started: this project deliberately runs without interrupts other
     * than USB.
     */
    HAL_Init();

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Check the wiring before trusting either reading. */
    joy_x_driven = pin_is_driven(14);
    joy_y_driven = pin_is_driven(15);

    /* pin_is_driven() leaves both pins analogue with no pull, which is
     * exactly what the ADC needs: the digital input buffer is disconnected
     * and cannot load the potentiometer. */

    /* ADC4 lives in the ADC345 group and has its own clock enable. */
    __HAL_RCC_ADC345_CLK_ENABLE();

    /*
     * ADC4 configuration.
     *
     * ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4 means HCLK/4 = 42.5 MHz, which
     * is inside the G4's 60 MHz ADC limit. It is the same value the m7 version
     * wrote by hand into ADC345_COMMON->CCR as CKMODE = 11.
     *
     * HAL_ADC_Init() handles leaving deep power-down (DEEPPWD), starts the
     * internal regulator and waits the specified 20 us.
     */
    hadc4.Instance                      = ADC4;
    hadc4.Init.ClockPrescaler           = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc4.Init.Resolution               = ADC_RESOLUTION_12B;
    hadc4.Init.DataAlign                = ADC_DATAALIGN_RIGHT;
    hadc4.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    hadc4.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    hadc4.Init.LowPowerAutoWait         = DISABLE;
    hadc4.Init.ContinuousConvMode       = DISABLE;
    hadc4.Init.NbrOfConversion          = 1;
    hadc4.Init.DiscontinuousConvMode    = DISABLE;
    hadc4.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    hadc4.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc4.Init.DMAContinuousRequests    = DISABLE;
    hadc4.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc4.Init.OversamplingMode         = DISABLE;
    hadc4.Init.GainCompensation         = 0;

    if (HAL_ADC_Init(&hadc4) != HAL_OK)
        return;                     /* joy_present stays 0, the board carries on */

    /*
     * Calibration.
     *
     * This is where HAL is genuinely better than the hand-written version.
     * RM0440 requires ADEN to be set only 4 ADC clock cycles after hardware
     * clears ADCAL. m7 missed that delay and the board hung waiting for ADRDY;
     * the fix there was a manual delay_us(2).
     *
     * HAL_ADCEx_Calibration_Start() knows the sequence and inserts the delay
     * itself.
     */
    if (HAL_ADCEx_Calibration_Start(&hadc4, ADC_SINGLE_ENDED) != HAL_OK)
        return;

    /*
     * Measure the resting position.
     *
     * Averaged over 16 samples to shake off noise. If the result is nowhere
     * near mid-scale the stick was almost certainly being held at boot, so
     * fall back to the nominal centre rather than baking a bad offset in for
     * the rest of the session -- a wrong centre is far worse than a slightly
     * imprecise one, because it makes the stick permanently deflected.
     */
    uint32_t sx = 0, sy = 0;

    for (int i = 0; i < 16; i++) {
        sx += adc_read(JOY_CH_X);
        sy += adc_read(JOY_CH_Y);
    }

    uint16_t cx = (uint16_t) (sx / 16u);
    uint16_t cy = (uint16_t) (sy / 16u);

    joy_centre_x_q4 = (int32_t) ((cx > 1024u && cx < 3072u) ? cx : 2048u) << 4;
    joy_centre_y_q4 = (int32_t) ((cy > 1024u && cy < 3072u) ? cy : 2048u) << 4;

    joy_present = 1;
}

int joystick_present(void) { return joy_present; }

uint16_t joystick_centre_x(void) { return (uint16_t) (joy_centre_x_q4 >> 4); }
uint16_t joystick_centre_y(void) { return (uint16_t) (joy_centre_y_q4 >> 4); }

/* Pull the centre towards a reading that looks like the stick at rest. */
static void track_centre(uint16_t raw, int32_t *centre_q4, uint32_t *out_ticks)
{
    int32_t d = ((int32_t) raw << 4) - *centre_q4;

    if (d > (JOY_RECENTRE_WINDOW << 4) || d < -(JOY_RECENTRE_WINDOW << 4)) {
        /* A held direction looks like this and must not drag the zero with
         * it -- but so does a centre that was mis-measured at boot, and that
         * one never recovers on its own. See JOY_SNAP_TICKS. */
        if (++(*out_ticks) > JOY_SNAP_TICKS) {
            *centre_q4 = (int32_t) raw << 4;
            *out_ticks = 0;
        }
        return;
    }

    *out_ticks = 0;
    *centre_q4 += d >> JOY_RECENTRE_SHIFT;
}

/*
 * Convert one raw reading into the -127..127 the HID report descriptor
 * declares, applying the deadzone and scaling from it rather than from centre
 * so there is no step at the edge of the deadzone.
 */
static int8_t scale_axis(uint16_t raw, int32_t centre_q4, int invert)
{
    int32_t d = (int32_t) raw - (centre_q4 >> 4);
    int32_t sign = (d < 0) ? -1 : 1;
    int32_t mag = (d < 0) ? -d : d;

    mag -= JOY_DEADZONE;
    if (mag <= 0)
        return 0;

    int32_t v = (mag * 127) / (JOY_SPAN - JOY_DEADZONE);
    if (v > 127)
        v = 127;

    if (invert)
        sign = -sign;

    return (int8_t) (sign * v);
}

void joystick_read(int8_t *out_x, int8_t *out_y)
{
    if (!joy_present) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    joy_raw_x = adc_read(JOY_CH_X);
    joy_raw_y = adc_read(JOY_CH_Y);

    if (joy_raw_x < joy_min_x) joy_min_x = joy_raw_x;
    if (joy_raw_x > joy_max_x) joy_max_x = joy_raw_x;
    if (joy_raw_y < joy_min_y) joy_min_y = joy_raw_y;
    if (joy_raw_y > joy_max_y) joy_max_y = joy_raw_y;

    /*
     * HID's Y axis points DOWN (HID Usage Tables, Generic Desktop), so pushing
     * the stick up must produce a negative value. Whether that means inverting
     * the ADC reading depends on which way round the potentiometer is wired,
     * which the schematic does not say -- so JOY_INVERT_Y is the one thing
     * here that has to be confirmed by pushing the stick and looking.
     */
    static uint32_t out_ticks_x, out_ticks_y;

    track_centre(joy_raw_x, &joy_centre_x_q4, &out_ticks_x);
    track_centre(joy_raw_y, &joy_centre_y_q4, &out_ticks_y);

    *out_x = scale_axis(joy_raw_x, joy_centre_x_q4, JOY_INVERT_X);
    *out_y = scale_axis(joy_raw_y, joy_centre_y_q4, JOY_INVERT_Y);
}
