#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

/*
 * Orientation.
 *
 * The panel is rotated into landscape by MADCTL = 0x60, but the touch layer is
 * a separate sheet of film with its own idea of which corner is the origin,
 * and nothing in the schematic says how the two line up. These three flags are
 * the only part of this driver that has to be settled by touching the screen
 * and watching the numbers.
 *
 * The starting values follow the stock firmware's
 * XPT2046_ORIENTATION_LANDSCAPE_ROT180, which is the orientation its own
 * calibration constants were taken in.
 *
 * How to check: touch the top-left corner of the image. touch_task() logs the
 * raw and the mapped coordinates once a second, and the mapped pair should
 * read close to 0,0. If X counts backwards, flip TOUCH_INVERT_X; likewise for
 * Y. If moving a finger horizontally changes the Y number, set TOUCH_SWAP_XY.
 */
#define TOUCH_SWAP_XY   0
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  1   /* stock driver does raw_y = 32768 - raw_y */

/*
 * Bring up SPI1 and the two GPIOs, and check that the controller answers.
 * Call after the 170 MHz clock and the DWT counter are running. Safe to call
 * even if nothing is attached: a controller that does not answer is recorded
 * as absent and everything else on the board carries on.
 */
void touch_init(void);

/*
 * Poll the panel and send a HID report when the state changes. Call from the
 * main loop, never from a USB callback: a read takes about 600 us and
 * tud_task() is not serviced while it runs.
 */
void touch_task(uint32_t ms_ticks);

/* 0 if the controller never answered at boot. */
int touch_present(void);

/* Counters and last values, for the console. */
extern uint32_t touch_reports;      /* HID reports sent                     */
extern uint32_t touch_presses;      /* pen-down transitions seen            */
extern uint32_t touch_down;         /* 1 while a finger is on the panel     */
extern uint16_t touch_raw_x, touch_raw_y;   /* averaged 16-bit ADC readings */
extern uint16_t touch_x, touch_y;           /* mapped, 0..4095              */
extern uint64_t touch_read_cycles;  /* time spent inside SPI reads          */

#endif /* TOUCH_H */
