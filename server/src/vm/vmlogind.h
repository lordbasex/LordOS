/* vmlogind.h — lord-vm and logind (docs/VIRTUALIZATION.md 6.1, 6.14), over the system bus (libsystemd's sd-bus):
 * the shutdown delay inhibitor, the idle block inhibitor, logind's InhibitDelayMaxUSec and the PrepareForShutdown
 * signal. Built into lord-vm only with VM_LOGIND (server/mk/vm.mk); tests/vmlogind_test.c runs it against a fake
 * logind on a private bus.
 *
 * Every synchronous call (AddMatch, Inhibit, Get) reads whatever else the bus sends meanwhile into sd-bus's own queue
 * (the bus's NameAcquired at the very least). While that queue isn't empty, sd_bus_get_events() leaves POLLIN out and
 * sd_bus_get_timeout() says "now": a loop that polls the descriptor with those events and no timeout never wakes for
 * it again, and PrepareForShutdown was handled only when the bus hung up at the end of the shutdown, after logind's
 * delay had run out (2026-11-23). vml_poll() empties that queue before every poll. */
#ifndef LORD_VM_LOGIND_H
#define LORD_VM_LOGIND_H

#include <poll.h>

typedef void (*VmlLog)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* connects to the system bus ($DBUS_SYSTEM_BUS_ADDRESS when set, as sd-bus does), subscribes to PrepareForShutdown
 * (on_shutdown is called on PrepareForShutdown(true)) and reads logind's InhibitDelayMaxUSec. 0 ok, -1 no bus */
int vml_open(VmlLog log, void (*on_shutdown)(void));
/* an inhibitor ("shutdown"/"idle", mode "delay"/"block"): its descriptor (close it to let go), or -1 */
int vml_inhibit(const char *what, const char *why, const char *mode);
/* logind's InhibitDelayMaxUSec in ms, as read by vml_open; -1 unknown */
long long vml_delay_ms(void);
/* before each poll: dispatches what's queued already (handlers may run here), then fills *p with the bus's descriptor
 * and the events it waits for and lowers *next (an absolute CLOCK_MONOTONIC ms, -1 none) to sd-bus's own deadline.
 * 1 when *p is to be polled, 0 no bus */
int vml_poll(struct pollfd *p, long long now_ms, long long *next);
/* after poll said the descriptor is ready: everything that arrived is dispatched */
void vml_process(void);
void vml_close(void);

#endif
