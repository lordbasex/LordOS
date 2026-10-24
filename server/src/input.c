/* input.c — evdev device detection and event reading. */
#include "input.h"
#include "platform/seat.h"
#include "tp/tp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define LONG_BITS (8 * sizeof(long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)
#define HAS(bits, n) (((bits)[(n) / LONG_BITS] >> ((n) % LONG_BITS)) & 1UL)

/* Adds the device if it is a keyboard, mouse or tablet (and grabs it); otherwise closes it. */
static void add_device(Input *in, int fd, const char *path) {
    unsigned long ev[NLONGS(EV_MAX + 1)] = {0};
    unsigned long keys[NLONGS(KEY_MAX + 1)] = {0};
    unsigned long abs[NLONGS(ABS_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);

    int keyboard = HAS(ev, EV_KEY) && HAS(keys, KEY_A) && HAS(keys, KEY_ENTER);
    int mouse    = HAS(ev, EV_REL) && HAS(keys, BTN_LEFT);
    int tablet   = HAS(ev, EV_ABS) && HAS(abs, ABS_X) && HAS(abs, ABS_Y);
    if ((!keyboard && !mouse && !tablet) || in->count >= INPUT_MAX_DEVICES) { seat_device_close(fd); return; }

    InputDevice *d = &in->dev[in->count++];
    memset(d, 0, sizeof *d);                /* the slot may have belonged to one that was unplugged */
    d->fd = fd;
    ioctl(fd, EVIOCGNAME(sizeof d->name), d->name);
    unsigned long rel[NLONGS(REL_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    d->wheel_hires = HAS(rel, REL_WHEEL_HI_RES);
    d->hwheel_hires = HAS(rel, REL_HWHEEL_HI_RES);
    /* touchpad or touchscreen (multitouch): handled by our own driver (tp/tp.c) */
    d->tp = tp_open(fd, d->name);
    if (d->tp) tablet = 0;
    if (tablet) {
        d->has_abs = 1;
        ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
    }
    /* Exclusive: keys do not reach the text console underneath. */
    if (ioctl(fd, EVIOCGRAB, 1))
        fprintf(stderr, "input: could not grab %s: %s\n", d->name, strerror(errno));
    fprintf(stderr, "input: %s -> %s%s%s\n", path, d->name,
            keyboard ? " [keyboard]" : "", d->tp ? (tp_kind(d->tp) == TP_KIND_TOUCHSCREEN ? " [touchscreen]" : " [touchpad]")
            : mouse ? " [mouse]" : (tablet ? " [tablet]" : ""));
}

static void scan_devices(Input *in) {
    for (int i = 0; i < 64 && in->count < INPUT_MAX_DEVICES; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        struct stat st, existing;
        if (stat(path, &st)) continue;
        int found = 0;
        for (int k = 0; k < in->count; k++)
            if (!fstat(in->dev[k].fd, &existing) && existing.st_rdev == st.st_rdev && existing.st_ino == st.st_ino) { found = 1; break; }
        if (found) continue;
        int fd = seat_device_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) add_device(in, fd, path);
    }
}

int input_open(Input *in) {
    memset(in, 0, sizeof *in);
    scan_devices(in);
    return in->count > 0 ? 0 : -1;
}

/* lord-sessiond used to forward new devices. An independent session observes
 * /dev/input itself; all access still goes through the seat provider. */
static int watch = -1;
int input_watch_fd(void) {
    if (watch >= 0) return watch;
    watch = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watch >= 0 && inotify_add_watch(watch, "/dev/input", IN_CREATE | IN_MOVED_TO) < 0) {
        close(watch); watch = -1;
    }
    return watch;
}
int input_watch_dispatch(Input *in, int active) {
    char events[4096];
    while (read(watch, events, sizeof events) > 0) { }
    int before = in->count;
    if (active) scan_devices(in);
    return before != in->count;
}

int input_open_fds(Input *in, const int *fds, int n) {
    memset(in, 0, sizeof *in);
    for (int i = 0; i < n; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        add_device(in, fds[i], "(lord-sessiond)");
    }
    return in->count > 0 ? 0 : -1;
}

int input_add_fds(Input *in, const int *fds, int n) {
    int before = in->count;
    for (int i = 0; i < n; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        add_device(in, fds[i], "(enchufado)");
    }
    return in->count - before;
}

void input_remove(Input *in, int idx) {
    if (idx < 0 || idx >= in->count) return;
    InputDevice *d = &in->dev[idx];
    fprintf(stderr, "input: %s is gone; closing it\n", d->name);
    seat_device_close(d->fd);
    tp_free(d->tp);
    memmove(&in->dev[idx], &in->dev[idx + 1], sizeof in->dev[0] * (size_t)(in->count - idx - 1));
    in->count--;
}

void input_close(Input *in) {
    for (int i = 0; i < in->count; i++) {
        ioctl(in->dev[i].fd, EVIOCGRAB, 0);
        seat_device_close(in->dev[i].fd);
        tp_free(in->dev[i].tp);
        in->dev[i].tp = NULL;
    }
    in->count = 0;
}

void input_dispatch(Input *in, int idx, InputHandler cb, void *ud) {
    InputDevice *d = &in->dev[idx];
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(d->fd, evs, sizeof evs);
        if (n <= 0) break;
        for (size_t i = 0; i < (size_t)n / sizeof evs[0]; i++) cb(ud, d, &evs[i]);
    }
}
