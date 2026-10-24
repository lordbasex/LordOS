#include "seat.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef LORD_LIBSEAT
#include <libseat.h>
#include <stdio.h>

static struct libseat *seat;
static int active;
static void (*on_enable)(void), (*on_disable)(void);
static struct { int fd, id; } devices[128];
static int count;

static void enable(struct libseat *s, void *data) {
    (void)s; (void)data;
    active = 1;
    if (on_enable) on_enable();
}
static void disable(struct libseat *s, void *data) {
    (void)data;
    active = 0;
    /* Release GPU resources and grabbed input before acknowledging the switch. */
    if (on_disable) on_disable();
    libseat_disable_seat(s);
}
static const struct libseat_seat_listener listener = { enable, disable };

int seat_start(void) {
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);
    seat = libseat_open_seat(&listener, NULL);
    if (!seat) { perror("lord-de: opening seat (logind or seatd required)"); return -1; }
    /* Enable may already be queued by open_seat. Dispatch it before opening DRM. */
    for (int i = 0; !active && i < 5; i++)
        if (libseat_dispatch(seat, 1000) < 0) break;
    if (!active) { seat_stop(); errno = EACCES; return -1; }
    return 0;
}
int seat_managed(void) { return seat != NULL; }
int seat_fd(void) { return seat ? libseat_get_fd(seat) : -1; }
int seat_dispatch(void) { return seat ? libseat_dispatch(seat, 0) : 0; }
void seat_callbacks(void (*e)(void), void (*d)(void)) { on_enable = e; on_disable = d; }
int seat_switch(int vt) { return seat ? libseat_switch_session(seat, vt) : -1; }

int seat_device_open(const char *path, int flags) {
    if (!seat) return open(path, flags);
    if (!active || count == 128) { errno = EACCES; return -1; }
    int fd, id = libseat_open_device(seat, path, &fd);
    if (id < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | (flags & O_NONBLOCK));
    devices[count].fd = fd;
    devices[count++].id = id;
    return fd;
}
void seat_device_close(int fd) {
    for (int i = 0; i < count; i++) if (devices[i].fd == fd) {
        libseat_close_device(seat, devices[i].id);
        devices[i] = devices[--count];
        break;
    }
    close(fd);
}
void seat_stop(void) {
    on_enable = on_disable = NULL;
    while (count) seat_device_close(devices[count - 1].fd);
    if (seat) libseat_close_seat(seat);
    seat = NULL;
    active = 0;
}
#else
int seat_start(void) { return 0; }
void seat_stop(void) { }
int seat_fd(void) { return -1; }
int seat_dispatch(void) { return 0; }
int seat_managed(void) { return 0; }
int seat_device_open(const char *path, int flags) { return open(path, flags); }
void seat_device_close(int fd) { close(fd); }
void seat_callbacks(void (*e)(void), void (*d)(void)) { (void)e; (void)d; }
int seat_switch(int vt) { (void)vt; return -1; }
#endif
