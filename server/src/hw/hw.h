/* hw.h — glue for Settings' hardware modules in lord-server's main loop: displays, uevents,
 * the system bus (Bluetooth, power profiles, timedated) and batteries. See docs/SETTINGS.md. */
#ifndef LORD_HW_H
#define LORD_HW_H

#include <poll.h>
#include "display.h"
#include "power.h"

/* After dropping root. host->event receives every notice ("display", "idle", "power", "bt",
 * "bt-prompt", "bt-result", "ppd", "ppd-result", "timedate"). */
void hw_start(const DisplayHost *host, int with_bus);
int  hw_pollfds(struct pollfd *p, int max);
void hw_dispatch(const struct pollfd *p, int n);
void hw_events(void);                   /* once per loop turn (coalesced Bluetooth state, battery reads) */
double hw_timeout(double now_ms);       /* ms to the next battery read (CLOCK_MONOTONIC), or -1 */
const PowerState *hw_power(void);
void hw_emit(const char *kind, const char *json);
void hw_timedate_refresh(void);         /* asks timedated; answer as "timedate" */

#endif
