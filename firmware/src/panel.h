#ifndef PANEL_H
#define PANEL_H

#include <stdint.h>

/* Bring up the FMC bus and the ILI9341, calibrate the scan-out timing, then
 * show colour bars. Call once, after the 170 MHz clock and the DWT counter are
 * running. */
void panel_init(void);

/* Measured at boot by scan_calibrate(), not taken from the datasheet. */
uint32_t panel_scan_total_lines(void);
uint32_t panel_scan_period_cycles(void);


/* On-screen frame rate overlay, top left corner. */
extern uint32_t panel_fps_overlay;      /* 1 = on */
extern uint32_t panel_fps_x10;          /* most recent rate, times ten */
extern uint64_t panel_overlay_cycles;   /* cost of drawing it, in cycles */

/* Toggle the overlay. Switching it off restores the pixels underneath. */
void panel_fps_overlay_set(uint32_t on);

#endif /* PANEL_H */
