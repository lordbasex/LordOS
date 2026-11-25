/* vmlogind.c — lord-vm and logind over the system bus (vmlogind.h) */
#include "vmlogind.h"

#include <fcntl.h>
#include <stdint.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

static sd_bus *bus;
static VmlLog vlog;
static void (*shutdown_cb)(void);
static long long delay_ms = -1;

static int on_prepare_shutdown(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) >= 0 && start && shutdown_cb) shutdown_cb();
    return 0;
}

static void drop(const char *why) {
    if (vlog) vlog("logind: %s", why);
    sd_bus_flush_close_unref(bus);
    bus = NULL;
}

int vml_open(VmlLog log, void (*on_shutdown)(void)) {
    vlog = log;
    shutdown_cb = on_shutdown;
    if (sd_bus_open_system(&bus) < 0) { bus = NULL; return -1; }
    if (sd_bus_match_signal(bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                            "PrepareForShutdown", on_prepare_shutdown, NULL) < 0 && vlog)
        vlog("logind: PrepareForShutdown can't be followed: only SIGTERM stops the machine");
    sd_bus_error e = SD_BUS_ERROR_NULL;
    uint64_t us = 0;
    if (sd_bus_get_property_trivial(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                    "InhibitDelayMaxUSec", &e, 't', &us) >= 0)
        delay_ms = us > 3600ULL * 1000000 ? 3600LL * 1000 : (long long)(us / 1000);
    sd_bus_error_free(&e);
    return 0;
}

int vml_inhibit(const char *what, const char *why, const char *mode) {
    if (!bus) return -1;
    sd_bus_message *rep = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int fd = -1, got = -1;
    if (sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit", &e, &rep,
                           "ssss", what, "LordOS Machines", why, mode) >= 0 && sd_bus_message_read(rep, "h", &got) >= 0)
        fd = fcntl(got, F_DUPFD_CLOEXEC, 3);
    else if (vlog) vlog("logind: no %s inhibitor: %s", what, e.message ? e.message : "?");
    sd_bus_error_free(&e);
    sd_bus_message_unref(rep);
    return fd;
}

long long vml_delay_ms(void) { return delay_ms; }

void vml_process(void) {
    if (!bus) return;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) ;
    if (r < 0) drop("the system bus went away");
}

int vml_poll(struct pollfd *p, long long now_ms, long long *next) {
    (void)now_ms;
    vml_process();                      /* what a synchronous call queued (vmlogind.h): POLLIN only once it's empty */
    if (!bus) return 0;
    int fd = sd_bus_get_fd(bus), ev = sd_bus_get_events(bus);
    if (fd < 0 || ev < 0) { drop("the system bus went away"); return 0; }
    uint64_t us = UINT64_MAX;
    if (sd_bus_get_timeout(bus, &us) >= 0 && us != UINT64_MAX) {
        long long at = (long long)(us / 1000);
        if (*next < 0 || at < *next) *next = at;
    }
    *p = (struct pollfd){ fd, (short)ev, 0 };
    return 1;
}

void vml_close(void) {
    if (bus) sd_bus_flush_close_unref(bus);
    bus = NULL;
}
