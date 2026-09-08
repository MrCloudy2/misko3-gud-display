#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>

/* Configure the seven button pins as inputs with pull-ups. */
void buttons_init(void);

/* Poll the buttons and send a HID report if anything changed.
 * Call from the main loop; ms_ticks is a free-running millisecond counter. */
void buttons_task(uint32_t ms_ticks);

/* Live button state, in the bit order the HID report uses. */
extern uint32_t buttons_state;

/*
 * Diagnostic chords, watched by main(). Both buttons still report to the host
 * as themselves; the pairing is only noticed here.
 *
 * Bit numbers are TinyUSB's GAMEPAD_BUTTON_* order, which is also Linux's
 * evdev BTN_* order:
 *
 *      bit 0 A   1 B   2 C   3 X   4 Y   5 Z   6 TL   7 TR
 *      bit 8 TL2   9 TR2   10 SELECT   11 START   12 MODE   13 THUMBL
 *
 * Two earlier attempts are worth recording, because both looked correct.
 *
 * The first hard-coded 0x6 for "B plus the stick click". That was right under
 * a positional A/B/X/Y mapping, but once the mapping moved to the
 * GAMEPAD_BUTTON_* aliases bit 2 became BTN_C, which this board never sets, so
 * the chord could not be satisfied at all.
 *
 * The second used the stick click properly, as bit 13. It was measured not to
 * work either: over a four minute capture PC13 never once read low while
 * BTN_OK on PC15 registered normally, and both pins are configured by the same
 * loop in buttons_init(). An input with a pull-up that never goes low means
 * nothing is pulling it down, so the stick's push switch is not connected on
 * this board.
 *
 * So neither chord uses it. Each is now both triggers plus one direction,
 * which is three switches that all measure good and a combination that does
 * not occur while playing.
 */
#define BTN_BIT_B       (1u << 1)    /* right switch */
#define BTN_BIT_X       (1u << 3)    /* left switch  */
#define BTN_BIT_TL2     (1u << 8)    /* BTN_ESC      */
#define BTN_BIT_TR2     (1u << 9)    /* BTN_OK       */

/* ESC + OK + left  */
#define CHORD_TEARFREE  (BTN_BIT_TL2 | BTN_BIT_TR2 | BTN_BIT_X)
/* ESC + OK + right */
#define CHORD_FPS       (BTN_BIT_TL2 | BTN_BIT_TR2 | BTN_BIT_B)

#endif /* BUTTONS_H */
