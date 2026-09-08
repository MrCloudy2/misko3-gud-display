#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>

/* Configure the seven button pins as inputs with pull-ups. */
void buttons_init(void);

/* Poll the buttons and send a HID report if anything changed.
 * Call from the main loop; ms_ticks is a free-running millisecond counter. */
void buttons_task(uint32_t ms_ticks);

#endif /* BUTTONS_H */
