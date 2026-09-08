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
 * Each chord is a trigger plus the stick click, which is a deliberate two-hand
 * press that does not happen during play. Bit numbers are TinyUSB's
 * GAMEPAD_BUTTON_* order, which is also Linux's evdev BTN_* order:
 *
 *      bit 0 A   1 B   2 C   3 X   4 Y   5 Z   6 TL   7 TR
 *      bit 8 TL2   9 TR2   10 SELECT   11 START   12 MODE   13 THUMBL
 *
 * The gap at bit 2, BTN_C, is why these are written as named masks. An earlier
 * version hard-coded 0x6 for "B plus the stick click", which was right under a
 * positional A/B/X/Y mapping but became unreachable once the mapping moved to
 * the GAMEPAD_BUTTON_* aliases: bit 2 is BTN_C, which this board never sets,
 * so the chord could not fire at all.
 */
#define BTN_BIT_TL2     (1u << 8)    /* BTN_ESC  */
#define BTN_BIT_TR2     (1u << 9)    /* BTN_OK   */
#define BTN_BIT_THUMBL  (1u << 13)   /* JOY_BTN  */

#define CHORD_TEARFREE  (BTN_BIT_TL2 | BTN_BIT_THUMBL)   /* ESC + stick click */
#define CHORD_FPS       (BTN_BIT_TR2 | BTN_BIT_THUMBL)   /* OK  + stick click */

#endif /* BUTTONS_H */
