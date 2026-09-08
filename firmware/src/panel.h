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

#endif /* PANEL_H */
