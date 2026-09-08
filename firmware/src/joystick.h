#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>

/*
 * Axis direction.
 *
 * Which way an axis reads depends on how the potentiometer is wired, and the
 * schematic does not say. These two flags are the only part of the joystick
 * driver that has to be settled by pushing the stick and watching -- flip a 0
 * to a 1 if a direction comes out backwards.
 *
 * HID's Y axis points down, so "up" on the stick must produce a negative Y.
 */
#define JOY_INVERT_X 0
#define JOY_INVERT_Y 1   /* measured: up read positive, HID needs negative */

/* Configure PB14/PB15 as analogue inputs, bring up ADC4, calibrate it, and
 * measure the stick's resting position. Call once, after the 170 MHz clock and
 * the DWT counter are running. */
void joystick_init(void);

/* Read both axes and scale them to the -127..127 that the HID report
 * descriptor declares, with the deadzone applied. */
void joystick_read(int8_t *out_x, int8_t *out_y);

/* 0 if the ADC failed to come up. The stick then reads as centred and
 * everything else on the board carries on regardless. */
int joystick_present(void);

/* Wiring check made at boot: 0 means the pin is floating, i.e. nothing is
 * driving it, so any reading from that axis is meaningless. */
extern int joy_x_driven, joy_y_driven;

/* Repeat the wiring check; call about once a second from the main loop, never
 * from a USB callback. Counts how often each pin looked disconnected. */
void joystick_wiring_poll(void);
extern uint32_t joy_wiring_checks;
extern uint32_t joy_x_float_count, joy_y_float_count;

/* Resting position measured at boot, for the console. */
uint16_t joystick_centre_x(void);
uint16_t joystick_centre_y(void);

/* Live and extreme raw ADC readings, exported so JOY_SPAN can be re-tuned from
 * hardware rather than guessed at. */
extern uint16_t joy_raw_x, joy_raw_y;
extern uint16_t joy_min_x, joy_max_x;
extern uint16_t joy_min_y, joy_max_y;

#endif /* JOYSTICK_H */
