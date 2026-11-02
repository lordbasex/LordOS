/* Simulate logind/seatd revocation without taking over a real seat. */
#include "platform/seat.h"
#include <libseat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

struct libseat { int unused; };
static struct libseat fake;
static const struct libseat_seat_listener *listener;
static void *userdata;
static int event, opens, releases, ack, enabled, disabled;
static int device = -1;
void libseat_set_log_level(enum libseat_log_level level) { (void)level; }
struct libseat *libseat_open_seat(const struct libseat_seat_listener *l, void *d) {
    listener = l; userdata = d; event = 1; return &fake;
}
int libseat_close_seat(struct libseat *s) { assert(s == &fake); return 0; }
int libseat_get_fd(struct libseat *s) { (void)s; return 42; }
int libseat_dispatch(struct libseat *s, int timeout) {
    (void)timeout;
    int e = event; event = 0;
    if (e == 1) listener->enable_seat(s, userdata);
    if (e == 2) listener->disable_seat(s, userdata);
    return e != 0;
}
int libseat_disable_seat(struct libseat *s) {
    (void)s;
    assert(disabled && device == -1); /* caller released devices before ack */
    ack++;
    return 0;
}
int libseat_open_device(struct libseat *s, const char *path, int *fd) {
    (void)s; (void)path;
    *fd = open("/dev/null", O_RDONLY);
    assert(*fd >= 0);
    opens++;
    return *fd + 100;
}
int libseat_close_device(struct libseat *s, int id) {
    (void)s;
    assert(fcntl(id - 100, F_GETFD) >= 0); /* libseat requires the fd still open */
    releases++;
    return 0;
}
int libseat_switch_session(struct libseat *s, int n) { (void)s; return n == 3 ? 0 : -1; }
static void enable(void) { enabled++; device = seat_device_open("/dev/input/event0", O_RDONLY | O_NONBLOCK); assert(device >= 0); }
static void disable(void) { disabled++; seat_device_close(device); device = -1; }

int main(void) {
    assert(!seat_managed());
    assert(seat_start() == 0 && seat_managed());
    assert(seat_fd() == 42);
    seat_callbacks(enable, disable);
    enable();
    assert(fcntl(device, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(device, F_GETFL) & O_NONBLOCK);
    event = 2;
    seat_dispatch();
    assert(ack == 1 && opens == releases);
    assert(seat_device_open("/dev/dri/card0", O_RDWR) == -1 && errno == EACCES);
    event = 1;
    seat_dispatch();
    assert(enabled == 2 && device >= 0);
    assert(seat_switch(3) == 0);
    seat_stop();
    assert(opens == releases && !seat_managed() && seat_fd() == -1);
    assert(fcntl(device, F_GETFD) == -1);
    puts("seat: devices released before disable, reopened on enable, closed on exit");
}
