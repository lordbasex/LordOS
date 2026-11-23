/* uevent.c — kernel uevents (see uevent.h). */
#include "uevent.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <linux/netlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fd = -1;

int uevent_open(void) {
    if (fd >= 0) return 0;
    fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };      /* group 1: the kernel's */
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); fd = -1; return -1; }
    return 0;
}

int uevent_fd(void) { return fd; }

/* the card lord-server drives (uevent_set_card); -1: every card's uevents count */
static int own_card = -1;

void uevent_set_card(int n) { own_card = n < 0 ? -1 : n; }

/* N of "cardN" at s, ending at end or '/' or '-' (a connector: card0-DP-1); -1 if it isn't one */
static int card_at(const char *s, const char *end) {
    if (end - s < 5 || strncmp(s, "card", 4)) return -1;
    int n = 0, digits = 0;
    for (s += 4; s < end && *s >= '0' && *s <= '9'; s++) { if (++digits > 4) return -1; n = n * 10 + (*s - '0'); }
    if (!digits || (s < end && *s != '/' && *s != '-')) return -1;
    return n;
}

/* the card of a devpath ("/devices/.../drm/card0" or ".../drm/card0/card0-DP-1"), or -1 */
static int card_of_path(const char *p, const char *end) {
    for (const char *q = p; q + 5 <= end; q++)
        if (!strncmp(q, "/drm/", 5)) return card_at(q + 5, end);
    return -1;
}

int uevent_card_of_fd(int dfd) {
    struct stat st;
    if (dfd < 0 || fstat(dfd, &st) || !S_ISCHR(st.st_mode)) return -1;
    char sys[64], link[PATH_MAX];
    snprintf(sys, sizeof sys, "/sys/dev/char/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    ssize_t n = readlink(sys, link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    const char *base = strrchr(link, '/');
    base = base ? base + 1 : link;
    return card_at(base, base + strlen(base));
}

int uevent_parse(const char *buf, int len) {
    const char *at = len > 0 ? memchr(buf, '@', (size_t)len) : NULL;
    if (!at) return 0;                                          /* udev's own (libudev) messages have no '@' header */
    int kind = 0, seen = 0, card = -1, card_name = -1, card_path = -1;
    for (int off = 0; off < len;) {
        const char *s = buf + off;
        size_t n = strnlen(s, (size_t)(len - off));
        if (off == 0) card = card_of_path(at + 1, s + n);       /* the header: "ACTION@devpath" */
        else if (n > 10 && !strncmp(s, "SUBSYSTEM=", 10)) {
            const char *v = s + 10;
            seen = 1;
            kind = !strcmp(v, "drm") ? UEV_DRM : !strcmp(v, "power_supply") ? UEV_POWER : !strcmp(v, "backlight") ? UEV_BACKLIGHT
                 : !strcmp(v, "video4linux") ? UEV_VIDEO : 0;
        } else if (n > 8 && !strncmp(s, "DEVNAME=", 8)) {
            if (n > 12 && !strncmp(s + 8, "dri/", 4)) card_name = card_at(s + 12, s + n);
        } else if (n > 8 && !strncmp(s, "DEVPATH=", 8)) card_path = card_of_path(s + 8, s + n);
        off += (int)n + 1;
    }
    if (!seen || kind != UEV_DRM) return kind;
    /* a drm uevent of another card (an Intel iGPU a GVT-g machine starts on) isn't a change of the displays shown */
    if (card_name >= 0) card = card_name;
    else if (card_path >= 0) card = card_path;
    if (own_card >= 0 && card >= 0 && card != own_card) return 0;
    return UEV_DRM;
}

int uevent_dispatch(void) {
    int mask = 0;
    char buf[8192];
    for (;;) {
        struct sockaddr_nl from;
        struct iovec iov = { buf, sizeof buf - 1 };
        struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t r = recvmsg(fd, &m, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (from.nl_pid != 0) continue;                             /* only the kernel */
        buf[r] = 0;
        mask |= uevent_parse(buf, (int)r);
    }
    return mask;
}
