/* monitor.c — the monitor in lord-sessiond: lends the screen to ONE session at a time.
 *
 * Like logind with "session devices": lord-sessiond (root) opens the video card and the
 * keyboard and mouse, and passes the fds to the session that has to be shown (RP_DISPLAY over the
 * session socket, with SCM_RIGHTS). The session draws straight on the card, like a
 * regular lord-server: GPU composition if available (Mesa), direct full-screen scanout
 * and hardware cursor. To switch sessions:
 *   1. the one leaving is notified (RP_UNDISPLAY);
 *   2. its input devices are revoked (EVIOCREVOKE: its fds stop working);
 *   3. its DRM master is dropped (DROP_MASTER on our copy of the same open file);
 *   4. the new one gets a freshly opened card, already master, and new input.
 * Sessions never open devices nor run as root. See docs/SESSIONS.md. */
#include "sessiond.h"
#include "proto.h"
#include "../drmcard.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <drm/drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* the card with a display (on the Pi 5 it's usually card1: card0 is v3d, without connectors; drmcard.h) */
static char card[64] = "/dev/dri/card0";
#define CARD card
#define MAX_IN 32

static int vt = -1, conn = -1, drm = -1;
static int ins[MAX_IN], nin;
static char inname[MAX_IN][16];        /* "eventN" of each one (to know which one was unplugged) */
static int hp = -1;                    /* inotify on /dev/input (hotplug) */
static unsigned char *rb;
static size_t rlen, rcap, skip;        /* skip: bytes of a large message being discarded */

#define LONG_BITS (8 * sizeof(long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)
#include "../inputkind.h"
#define HAS(bits, n) (((bits)[(n) / LONG_BITS] >> ((n) % LONG_BITS)) & 1UL)

/* keyboard, mouse or tablet (same as input.c) */
static int useful(int fd) {
    unsigned long ev[NLONGS(EV_MAX + 1)] = {0}, keys[NLONGS(KEY_MAX + 1)] = {0}, abs[NLONGS(ABS_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    unsigned long props[NLONGS(INPUT_PROP_MAX + 1)] = {0};
    ioctl(fd, EVIOCGPROP(sizeof props), props);
    return (HAS(ev, EV_KEY) && HAS(keys, KEY_A) && HAS(keys, KEY_ENTER)) || (HAS(ev, EV_REL) && HAS(keys, BTN_LEFT)) ||
           input_is_abs_pointer(ev, abs, keys, props);
}

/* Nothing can be shown. The caller reports it on tty1 (sessiond.c tell_no_display); the file makes the
 * boot splash end for real instead of keeping its last picture over that message (lordos-splash). */
static int no_display(void) {
    mkdir(SD_RUN_DIR, 0755);
    int fd = open(SD_NO_DISPLAY, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
    return -1;
}

int sd_mon_open(void) {
    /* On ARM boards the display driver can show up well after boot (on a Raspberry Pi 5's first
     * boot vc4 may come after lordos.service has started): wait up to 30 s for a card with KMS
     * before giving up, instead of falling back to the text console for good. */
    int tries = 0, none;
    unlink(SD_NO_DISPLAY);
    while ((none = drm_pick_card(card, sizeof card)) && tries < 150) {
        if (!tries) sd_log("no card with KMS yet; waiting up to 30 s");
        struct timespec ts = { 0, 200000000 };
        nanosleep(&ts, NULL);
        tries++;
    }
    /* A card that opens but has no KMS (a render-only GPU) can't show anything either: lord-server
     * would fail on it, quietly, once per attempt, while this process and the unit look fine. */
    if (none) {
        sd_log("no usable display driver: no /dev/dri/card* with connectors and CRTCs after 30 s, the login screen will not start "
               "(lspci -k shows the graphics driver; once it is fixed, restart lordos.service)");
        return no_display();
    }
    sd_log("display: %s%s", card, tries ? " (after waiting)" : "");
    int fd = open(CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0) { sd_log("no usable display: %s: %s, the login screen will not start", CARD, strerror(errno)); return no_display(); }
    close(fd);
    hp = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (hp >= 0 && inotify_add_watch(hp, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) { close(hp); hp = -1; }
    if (hp < 0) sd_log("no inotify on /dev/input: new devices will show up on the next session switch");
    /* the text console stops drawing on top */
    vt = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (vt >= 0 && ioctl(vt, KDSETMODE, KD_GRAPHICS)) { close(vt); vt = -1; }
    return 0;
}

static int send_msg(int type, const void *d, size_t n, const int *fds, int nfds) {
    if (conn < 0) return -1;
    unsigned char h[5];
    uint32_t len = (uint32_t)(1 + n);
    memcpy(h, &len, 4);
    h[4] = (unsigned char)type;
    struct iovec iov[2] = { { h, 5 }, { (void *)d, n } };
    char cbuf[CMSG_SPACE(sizeof(int) * (MAX_IN + 1))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = n ? 2 : 1 };
    if (nfds > 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
    }
    ssize_t r;
    while ((r = sendmsg(conn, &mh, MSG_NOSIGNAL)) < 0 && errno == EINTR) {}
    return r == (ssize_t)(5 + n) ? 0 : -1;
}

/* Removes every framebuffer of the leaving session. They belong to the open file we lent (ours
 * is the same one), and the next DRM master could otherwise read them back (GETFB2 + map): the
 * last frame of an unlocked desktop. The session itself frees them when it handles RP_UNDISPLAY,
 * but that is asynchronous and a stuck or hostile session never does. Removing the one on the
 * screen turns that output off, which is what a switch shows anyway. */
/* Before the leaving session's framebuffers go: every lit CRTC shows a black frame of its own mode, let go with CLOSEFB.
 * Removing a framebuffer that is still on the screen makes the kernel turn the plane off, which amdgpu refuses ("atomic
 * remove_fb failed with -22" at every lock and logout) and which drops a DisplayPort link: a monitor that went to deep
 * sleep that way may not wake up again until it is unplugged. A black frame of the same mode keeps the signal, shows
 * nothing of the session, and stays (closed, it no longer belongs to the file) until the next one shows its own. With no
 * CLOSEFB (a kernel before 6.9) the black frame is removed with the rest, as before. Returns the CRTCs blacked out. */
static int black_out(int fd) {
    uint32_t crtcs[16], conns[32];
    struct drm_mode_card_res r = { .crtc_id_ptr = (uint64_t)(uintptr_t)crtcs, .count_crtcs = 16,
                                   .connector_id_ptr = (uint64_t)(uintptr_t)conns, .count_connectors = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) return 0;
    uint32_t nc = r.count_crtcs < 16 ? r.count_crtcs : 16, nk = r.count_connectors < 32 ? r.count_connectors : 32;
    int done = 0;
    for (uint32_t i = 0; i < nc; i++) {
        struct drm_mode_crtc c = { .crtc_id = crtcs[i] };
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id || !c.mode_valid || !c.mode.hdisplay || !c.mode.vdisplay) continue;
        /* the connectors this CRTC drives */
        uint32_t on[8];
        uint32_t n = 0;
        for (uint32_t j = 0; j < nk && n < 8; j++) {
            struct drm_mode_get_connector k = { .connector_id = conns[j] };
            if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &k) || !k.encoder_id) continue;
            struct drm_mode_get_encoder e = { .encoder_id = k.encoder_id };
            if (!ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) && e.crtc_id == crtcs[i]) on[n++] = conns[j];
        }
        if (!n) continue;
        struct drm_mode_create_dumb d = { .width = c.mode.hdisplay, .height = c.mode.vdisplay, .bpp = 32 };
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d)) continue;       /* a new dumb buffer is zeroed: black */
        struct drm_mode_fb_cmd f = { .width = d.width, .height = d.height, .pitch = d.pitch, .bpp = 32, .depth = 24, .handle = d.handle };
        int ok = !ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f);
        if (ok) {
            struct drm_mode_crtc set = { .crtc_id = crtcs[i], .fb_id = f.fb_id, .set_connectors_ptr = (uint64_t)(uintptr_t)on,
                                         .count_connectors = n, .mode = c.mode, .mode_valid = 1 };
            if (!ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) {
                struct drm_mode_closefb cf = { .fb_id = f.fb_id };
                if (ioctl(fd, DRM_IOCTL_MODE_CLOSEFB, &cf)) { /* an older kernel: drop_framebuffers removes it */ }
                done++;
            } else ioctl(fd, DRM_IOCTL_MODE_RMFB, &f.fb_id);
        }
        struct drm_gem_close g = { .handle = d.handle };                /* the framebuffer keeps the buffer */
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &g);
    }
    return done;
}

static int drop_framebuffers(int fd) {
    int total = 0;
    for (int pass = 0; pass < 16; pass++) {
        uint32_t ids[64];
        struct drm_mode_card_res r = {0};
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) || !r.count_fbs) return total;
        uint32_t n = r.count_fbs < 64 ? r.count_fbs : 64;
        r = (struct drm_mode_card_res){ .fb_id_ptr = (uint64_t)(uintptr_t)ids, .count_fbs = n };
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) return total;
        if (r.count_fbs < n) n = r.count_fbs;
        int removed = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t id = ids[i];
            if (!ioctl(fd, DRM_IOCTL_MODE_RMFB, &id)) removed++;
        }
        total += removed;
        if (!removed) {
            sd_log("warning: %u framebuffer(s) of the leaving session could not be removed", n);
            return total;
        }
    }
    return total;
}

/* Closing an input device can be slow: the last close of a USB HID device stops its transfers, about
 * 0.7 s for VMware's virtual USB mouse, which held up every lock and switch. Access is already cut by
 * EVIOCREVOKE, so the close happens on a short-lived thread and the hand-off doesn't wait for it. */
static void *close_inputs(void *arg) {
    int *fds = arg;
    for (int i = 0; fds[i] >= 0; i++) close(fds[i]);
    free(fds);
    return NULL;
}
static void close_later(const int *fds, int n) {
    int *list = n > 0 ? malloc(sizeof(int) * ((size_t)n + 1)) : NULL;
    pthread_t th;
    pthread_attr_t at;
    if (list) {
        memcpy(list, fds, sizeof(int) * (size_t)n);
        list[n] = -1;
        if (!pthread_attr_init(&at)) {
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            int ok = !pthread_create(&th, &at, close_inputs, list);
            pthread_attr_destroy(&at);
            if (ok) return;
        }
        free(list);
    }
    for (int i = 0; i < n; i++) close(fds[i]);         /* no thread: closed here, as before */
}

/* Takes the monitor away from the session that has it. */
static void take_back(void) {
    double t0 = rd_now();
    if (conn >= 0) send_msg(RP_UNDISPLAY, NULL, 0, NULL, 0);
    double tsend = rd_now();
    char slow[160] = "";
    for (int i = 0; i < nin; i++) {
        double a = rd_now();
        ioctl(ins[i], EVIOCREVOKE, NULL);
        double e = rd_now();
        if (e - a > 0.05 && strlen(slow) < sizeof slow - 40)
            snprintf(slow + strlen(slow), sizeof slow - strlen(slow), " %.15s revoke %.0f", inname[i], (e - a) * 1000);
    }
    close_later(ins, nin);
    nin = 0;
    double t1 = rd_now(), t2 = t1;
    int fbs = 0, had = drm >= 0;
    if (drm >= 0) {
        black_out(drm);                 /* nothing of the session on the screen, and no display turned off */
        fbs = drop_framebuffers(drm);
        t2 = rd_now();
        ioctl(drm, DRM_IOCTL_DROP_MASTER, NULL);
        close(drm);
    }
    double t3 = rd_now();
    /* a slow hand-off says where the time went (vmwgfx turns the output off on RMFB: about 1 s) */
    if (had && t3 - t0 > 0.25)
        sd_log("monitor: take back %.0f ms (notice %.0f, input %.0f%s, %d framebuffer(s) removed %.0f, release %.0f)",
               (t3 - t0) * 1000, (tsend - t0) * 1000, (t1 - tsend) * 1000, slow, fbs, (t2 - t1) * 1000, (t3 - t2) * 1000);
    drm = -1;
    if (conn >= 0) close(conn);
    conn = -1;
    rlen = skip = 0;
}

void sd_mon_close(void) {
    take_back();
    if (vt >= 0) { ioctl(vt, KDSETMODE, KD_TEXT); close(vt); }
    vt = -1;
}

int sd_mon_fd(void) { return conn; }

int sd_mon_show(int fd, const char *who, int keep) {
    take_back();
    conn = fd;
    drm = open(CARD, O_RDWR | O_CLOEXEC);
    if (drm < 0) { sd_log("%s: %s", CARD, strerror(errno)); return -1; }
    if (ioctl(drm, DRM_IOCTL_SET_MASTER, NULL)) sd_log("warning: DRM master: %s", strerror(errno));
    for (int i = 0; i < 64 && nin < MAX_IN; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) continue;
        if (useful(f)) { snprintf(inname[nin], sizeof inname[nin], "event%d", i); ins[nin++] = f; } else close(f);
    }
    int fds[MAX_IN + 1];
    fds[0] = drm;
    memcpy(fds + 1, ins, sizeof(int) * (size_t)nin);
    char hello[64];
    snprintf(hello, sizeof hello, "monitor %s", who);
    /* keep: the boot splash is still on the screen; the session leaves it there until its first
     * frame instead of showing black, and answers RP_SHOWN */
    unsigned char disp[2] = { (unsigned char)(nin + 1), keep ? RP_DISPLAY_KEEP : 0 };
    if (send_msg(RP_SEAT, NULL, 0, NULL, 0) || send_msg(RP_HELLO, hello, strlen(hello), NULL, 0) ||
        send_msg(RP_DISPLAY, disp, sizeof disp, fds, nin + 1)) {
        sd_log("the session did not get the monitor");
        return -1;
    }
    return 0;
}

void sd_mon_greeter(const char *json) { send_msg(RP_GREETER_IN, json, strlen(json), NULL, 0); }
void sd_mon_notice(const char *json) { send_msg(RP_NOTICE, json, strlen(json), NULL, 0); }

/* Requests from the session being shown (RP_SESSION, RP_GREETER: JSON up to 4 KB; RP_SHOWN with no
 * data). Everything else is discarded without looking at it. -1 if the session closed. */
int sd_mon_read(void (*fn)(int type, const char *json)) {
    for (;;) {
        if (rcap - rlen < 8192) {
            size_t cap = rcap ? rcap * 2 : 65536;
            if (cap > (1u << 20)) return -1;
            unsigned char *nb = realloc(rb, cap);
            if (!nb) return -1;
            rb = nb;
            rcap = cap;
        }
        ssize_t n = recv(conn, rb + rlen, rcap - rlen, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) return -1;
        rlen += (size_t)n;
        size_t off = 0;
        if (skip) {                         /* rest of a message we don't care about */
            size_t k = skip < rlen ? skip : rlen;
            skip -= k;
            off = k;
        }
        while (rlen - off >= 5) {
            uint32_t len;
            memcpy(&len, rb + off, 4);
            if (!len || len > RP_MAX_MSG) return -1;
            if (len > 8192) {               /* screen (before the session reads RP_SEAT): skipped */
                size_t have = rlen - off;
                if (have >= 4 + (size_t)len) { off += 4 + len; continue; }
                skip = 4 + (size_t)len - have;
                off = rlen;
                break;
            }
            if (rlen - off < 4 + (size_t)len) break;
            int type = rb[off + 4];
            if (type == RP_SHOWN) fn(type, "");
            else if ((type == RP_SESSION || type == RP_GREETER) && len - 1 < 4096) {
                char j[4096];
                memcpy(j, rb + off + 5, len - 1);
                j[len - 1] = 0;
                if (!memchr(j, 0, len - 1)) fn(type, j);
                explicit_bzero(j, sizeof j);
            }
            off += 4 + len;
        }
        explicit_bzero(rb, off);
        memmove(rb, rb + off, rlen - off);
        rlen -= off;
    }
    return 0;
}

void sd_mon_lost(void) { take_back(); }

/* ---- hotplug ----
 * A new keyboard, mouse or touchpad is opened here (root) and passed ONLY to the session that is on
 * the monitor (RP_INPUT_ADD); background ones get nothing and see it when they return to the monitor
 * (sd_mon_show opens everything again). When it's unplugged our copy is closed; the session sees
 * ENODEV on its own and closes it by itself, without leaving the monitor. */
int sd_mon_hotplug_fd(void) { return hp; }

static int is_event(const char *n) {
    if (strncmp(n, "event", 5) || !n[5]) return 0;
    for (const char *p = n + 5; *p; p++) if (*p < '0' || *p > '9') return 0;
    return strlen(n) < 16;
}

static void plug(const char *name) {
    for (int i = 0; i < nin; i++) if (!strcmp(inname[i], name)) return;     /* already there */
    if (conn < 0 || drm < 0 || nin >= MAX_IN) return;                         /* nobody on the monitor */
    char p[40];
    snprintf(p, sizeof p, "/dev/input/%s", name);
    int f = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) return;
    if (!useful(f)) { close(f); return; }
    unsigned char count = 1;
    if (send_msg(RP_INPUT_ADD, &count, 1, &f, 1)) { close(f); return; }
    snprintf(inname[nin], sizeof inname[nin], "%s", name);
    ins[nin++] = f;
    char dev[80] = "";
    ioctl(f, EVIOCGNAME(sizeof dev), dev);
    sd_log("input plugged in: %s (%s)", name, dev);
}

static void unplug(const char *name) {
    for (int i = 0; i < nin; i++) {
        if (strcmp(inname[i], name)) continue;
        close(ins[i]);
        ins[i] = ins[nin - 1];
        memcpy(inname[i], inname[nin - 1], sizeof inname[i]);
        nin--;
        sd_log("input removed: %s", name);
        return;
    }
}

void sd_mon_hotplug(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(hp, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            if (e->len && is_event(e->name)) {
                if (e->mask & IN_DELETE) unplug(e->name);
                else plug(e->name);
            }
            p += sizeof *e + e->len;
        }
    }
}

/* In a child process (web connection, worker): drops the copies of the card, the input and the
 * monitor socket without touching them (they never stay in a process that later stops being root). */
void sd_mon_child(void) {
    for (int i = 0; i < nin; i++) close(ins[i]);
    nin = 0;
    if (drm >= 0) close(drm);
    if (conn >= 0) close(conn);
    if (vt >= 0) close(vt);
    if (hp >= 0) close(hp);
    drm = conn = vt = hp = -1;
}
