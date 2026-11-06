/* lock_seat.c — plays lord-sessiond's monitor part for a lock (issue #36; run by tests/lock_e2e.sh).
 *
 *   sudo build/lock-seat <session remote.sock> <greeter remote.sock> <card|-> <prefix> <wait_ms> [hold_ms]
 *
 * The same steps lord-sessiond takes (docs/SESSIONS.md): it lends the card to the session (RP_SEAT,
 * RP_DISPLAY), saves what the screen shows as <prefix>-1-desktop.bmp, and waits up to <wait_ms> for the
 * session's lock request (RP_SESSION {"a":"lock",...}). Then, after <hold_ms> (the time lord-sessiond
 * takes, to see that a sleep waits for it), it takes the monitor back (RP_UNDISPLAY, drop master),
 * lends it to the login screen in lock mode with the session's idle state (dark, off, sleep,
 * sleepLeft), answers its hello and saves <prefix>-2-lock.bmp; if the lock said dark, it then moves the
 * pointer (RP_PTR, as input does) and saves <prefix>-3-lock-woken.bmp.
 * With "-" for the card (a VM whose one card the running desktop holds) nothing is lent: the session and
 * the login screen run with LORD_IDLE_HEADLESS=1 and the pictures are their frames in memory, taken as a
 * remote viewer does (RP_VIDEO + RP_VMODE: the lossless tiles once the screen is still).
 * Prints one line per step with the milliseconds since it started. Exit 3: no lock came.
 * A test tool only: it is never installed. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

enum { RP_SIZE = 1, RP_RECT = 2, RP_SESSION = 9, RP_READY = 10, RP_PTR = 11, RP_SEAT = 18, RP_DISPLAY = 19, RP_GREETER = 20,
       RP_GREETER_IN = 21, RP_UNDISPLAY = 22, RP_VIDEO = 32, RP_VMODE = 35 };

static double t0;
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define say(...) do { printf("[%7.0f ms] ", now_ms() - t0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int send_msg(int sock, int type, const void *d, size_t n, int fd) {
    unsigned char h[5];
    uint32_t len = (uint32_t)(1 + n);
    memcpy(h, &len, 4);
    h[4] = (unsigned char)type;
    struct iovec iov[2] = { { h, 5 }, { (void *)d, n } };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = n ? 2 : 1 };
    if (fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return sendmsg(sock, &mh, MSG_NOSIGNAL) == (ssize_t)(5 + n) ? 0 : -1;
}
static int send_json(int sock, int type, const char *j) { return send_msg(sock, type, j, strlen(j), -1); }

static int connect_to(const char *path) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) { perror(path); exit(1); }
    return s;
}

/* Reads framed messages for up to ms; returns the type of the first one of `want` (its JSON in out), or 0. */
static unsigned char in[1 << 20];
static size_t in_n;
static int read_until(int sock, int ms, int want, const char *needle, char *out, size_t cap) {
    double end = now_ms() + ms;
    for (;;) {
        while (in_n >= 5) {
            uint32_t len;
            memcpy(&len, in, 4);
            if (len < 1 || len > sizeof in - 4) { fprintf(stderr, "lock-seat: bad frame\n"); exit(1); }
            if (in_n < 4 + len) break;
            int type = in[4];
            size_t n = len - 1;
            int hit = type == want && (!needle || memmem(in + 5, n, needle, strlen(needle)));
            if (hit && out) { size_t k = n < cap - 1 ? n : cap - 1; memcpy(out, in + 5, k); out[k] = 0; }
            memmove(in, in + 4 + len, in_n - 4 - len);
            in_n -= 4 + len;
            if (hit) return type;
        }
        double left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { sock, POLLIN, 0 };
        if (poll(&p, 1, (int)left + 1) <= 0) continue;
        ssize_t k = read(sock, in + in_n, sizeof in - in_n);
        if (k <= 0) { say("the connection closed"); return -1; }
        in_n += (size_t)k;
    }
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* What the first active CRTC shows, as a 24-bit BMP (as tests/seat_cycle.c). 0 if saved. */
static int snapshot(int card, const char *path) {
    uint32_t crtcs[16];
    struct drm_mode_card_res r = {0};
    if (ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &r)) return -1;
    r = (struct drm_mode_card_res){ .crtc_id_ptr = (uint64_t)(uintptr_t)crtcs, .count_crtcs = r.count_crtcs < 16 ? r.count_crtcs : 16 };
    if (ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &r)) return -1;
    for (uint32_t i = 0; i < r.count_crtcs && i < 16; i++) {
        struct drm_mode_crtc c = { .crtc_id = crtcs[i] };
        if (ioctl(card, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id) continue;
        struct drm_mode_fb_cmd2 fb = { .fb_id = c.fb_id };
        if (ioctl(card, DRM_IOCTL_MODE_GETFB2, &fb) || !fb.handles[0]) return -1;
        struct drm_mode_map_dumb md = { .handle = fb.handles[0] };
        int ret = -1;
        size_t size = (size_t)fb.pitches[0] * fb.height;
        if (!ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
            uint8_t *m = mmap(NULL, size + fb.offsets[0], PROT_READ, MAP_SHARED, card, (off_t)md.offset);
            FILE *f = m != MAP_FAILED ? fopen(path, "wb") : NULL;
            if (f) {
                unsigned w = fb.width, h = fb.height, row = (w * 3 + 3) & ~3u;
                fputc('B', f); fputc('M', f); put32(f, 54 + row * h); put32(f, 0); put32(f, 54);
                put32(f, 40); put32(f, w); put32(f, h); put16(f, 1); put16(f, 24); put32(f, 0);
                put32(f, row * h); put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
                for (unsigned y = h; y-- > 0;) {
                    const uint32_t *px = (const uint32_t *)(m + fb.offsets[0] + (size_t)y * fb.pitches[0]);
                    for (unsigned x = 0; x < w; x++) { fputc(px[x] & 255, f); fputc(px[x] >> 8 & 255, f); fputc(px[x] >> 16 & 255, f); }
                    for (unsigned x = w * 3; x < row; x++) fputc(0, f);
                }
                fclose(f);
                ret = 0;
            }
            if (m != MAP_FAILED) munmap(m, size + fb.offsets[0]);
        }
        struct drm_gem_close gc = { .handle = fb.handles[0] };
        ioctl(card, DRM_IOCTL_GEM_CLOSE, &gc);
        return ret;
    }
    return -1;
}

static void write_bmp(const char *path, const uint32_t *px, unsigned w, unsigned h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned row = (w * 3 + 3) & ~3u;
    fputc('B', f); fputc('M', f); put32(f, 54 + row * h); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, w); put32(f, h); put16(f, 1); put16(f, 24); put32(f, 0);
    put32(f, row * h); put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (unsigned y = h; y-- > 0;) {
        for (unsigned x = 0; x < w; x++) { uint32_t p = px[(size_t)y * w + x]; fputc(p & 255, f); fputc(p >> 8 & 255, f); fputc(p >> 16 & 255, f); }
        for (unsigned x = w * 3; x < row; x++) fputc(0, f);
    }
    fclose(f);
}

/* The frame in memory of the session behind sock, as a remote viewer gets it: the lossless tiles sent
 * once the screen is still (video mode). Collected for ms. 0 if saved. */
static int view_snapshot(const char *sock, const char *path, int ms) {
    int v = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (v < 0 || connect(v, (struct sockaddr *)&a, sizeof a)) { if (v >= 0) close(v); return -1; }
    unsigned char vid[8] = { 2, 0, 30, 0, 0x88, 0x13, 0, 0 };     /* VP8, no sound, 30 Hz, 5000 kbit/s */
    unsigned char vm[1] = { 1 };
    send_msg(v, RP_VIDEO, vid, sizeof vid, -1);
    send_msg(v, RP_VMODE, vm, sizeof vm, -1);
    send_msg(v, RP_READY, NULL, 0, -1);
    static unsigned char buf[1 << 22];
    size_t n = 0;
    unsigned W = 0, H = 0, rects = 0;
    uint32_t *px = NULL;
    double end = now_ms() + ms;
    while (now_ms() < end) {
        struct pollfd p = { v, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        ssize_t k = read(v, buf + n, sizeof buf - n);
        if (k <= 0) break;
        n += (size_t)k;
        while (n >= 5) {
            uint32_t len;
            memcpy(&len, buf, 4);
            if (len < 1 || len > sizeof buf - 4) { n = 0; break; }
            if (n < 4 + len) break;
            const unsigned char *d = buf + 5;
            size_t dn = len - 1;
            if (buf[4] == RP_SIZE && dn >= 4) {
                uint16_t w, h;
                memcpy(&w, d, 2);
                memcpy(&h, d + 2, 2);
                if (w && h && (w != W || h != H)) { free(px); W = w; H = h; px = calloc((size_t)W * H, 4); }
            } else if (buf[4] == RP_RECT && dn >= 8 && px) {
                uint16_t r[4];
                memcpy(r, d, 8);
                if ((size_t)r[2] * r[3] * 4 + 8 <= dn && r[0] + r[2] <= W && r[1] + r[3] <= H) {
                    for (unsigned y = 0; y < r[3]; y++) memcpy(px + (size_t)(r[1] + y) * W + r[0], d + 8 + (size_t)y * r[2] * 4, (size_t)r[2] * 4);
                    rects++;
                }
            }
            if (buf[4] != RP_SIZE && buf[4] != RP_RECT) send_msg(v, RP_READY, NULL, 0, -1);
            memmove(buf, buf + 4 + len, n - 4 - len);
            n -= 4 + len;
        }
    }
    close(v);
    if (!px || !rects) { free(px); return -1; }
    write_bmp(path, px, W, H);
    free(px);
    return 0;
}

static const char *view_sock;           /* card "-": the pictures come from this session's frame */
static void shot(int card, const char *prefix, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s-%s.bmp", prefix, name);
    int r = card >= 0 ? snapshot(card, path) : view_snapshot(view_sock, path, 2500);
    if (r) say("no picture for %s", name);
    else say("saved %s", path);
}

static int lend(int sock, const char *cardpath) {
    if (!strcmp(cardpath, "-")) return -1;
    int card = open(cardpath, O_RDWR | O_CLOEXEC);
    if (card < 0) { perror(cardpath); exit(1); }
    ioctl(card, DRM_IOCTL_SET_MASTER, 0);
    unsigned char d[2] = { 1, 0 };
    if (send_msg(sock, RP_DISPLAY, d, sizeof d, card)) { perror("RP_DISPLAY"); exit(1); }
    return card;
}

static int jint(const char *j, const char *key) {
    char k[40];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(j, k);
    return p ? atoi(p + strlen(k)) : 0;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <session.sock> <greeter.sock> <card> <prefix> <wait_ms> [hold_ms]\n", argv[0]);
        return 2;
    }
    t0 = now_ms();
    const char *prefix = argv[4];
    int wait = atoi(argv[5]), hold = argc > 6 ? atoi(argv[6]) : 0;
    char lock[512] = "";

    int s = connect_to(argv[1]);
    send_msg(s, RP_SEAT, NULL, 0, -1);
    view_sock = argv[1];
    int card = lend(s, argv[3]);
    say("the session has the monitor");
    int got = read_until(s, 1500, RP_SESSION, "\"lock\"", lock, sizeof lock);
    if (got != RP_SESSION) {
        shot(card, prefix, "1-desktop");
        got = read_until(s, wait, RP_SESSION, "\"lock\"", lock, sizeof lock);
    }
    if (got != RP_SESSION) {
        say("NO LOCK within %d ms", wait);
        return 3;
    }
    say("the session asked to lock: %s", lock);
    if (hold > 0) { usleep((useconds_t)hold * 1000); say("held %d ms (lord-sessiond's work)", hold); }
    send_msg(s, RP_UNDISPLAY, NULL, 0, -1);
    close(s);                           /* lord-sessiond closes right after RP_UNDISPLAY */
    if (card >= 0) { ioctl(card, DRM_IOCTL_DROP_MASTER, 0); close(card); }
    say("the monitor was taken from the session");

    int dark = strstr(lock, "\"dark\":true") != NULL;
    int g = connect_to(argv[2]);
    send_msg(g, RP_SEAT, NULL, 0, -1);
    view_sock = argv[2];
    card = lend(g, argv[3]);
    char mode[256];
    snprintf(mode, sizeof mode, "{\"t\":\"mode\",\"mode\":\"lock\",\"user\":\"lord\",\"dark\":%s,\"off\":%d,\"sleep\":%d,\"sleepLeft\":%d}",
             dark ? "true" : "false", jint(lock, "off"), jint(lock, "sleep"), jint(lock, "sleepLeft"));
    send_json(g, RP_GREETER_IN, mode);
    say("the login screen has the monitor, in lock mode: %s", mode);
    if (read_until(g, 8000, RP_GREETER, "\"hello\"", NULL, 0) == RP_GREETER) {
        send_json(g, RP_GREETER_IN, "{\"t\":\"hello\",\"mode\":\"lock\",\"user\":\"lord\",\"last\":\"lord\",\"host\":\"lordos\",\"layouts\":\"us\","
                                    "\"variants\":\"\",\"lang\":\"en_US.UTF-8\",\"users\":[{\"name\":\"lord\",\"full\":\"Lord\",\"color\":\"#0a84ff\","
                                    "\"image\":\"\",\"admin\":true,\"open\":true}]}");
        say("answered the login screen's hello");
    }
    read_until(g, 3000, -1, NULL, NULL, 0);
    shot(card, prefix, "2-lock");
    if (dark) {
        int16_t pt[2] = { 400, 300 };
        send_msg(g, RP_PTR, pt, sizeof pt, -1);
        say("moved the pointer");
        read_until(g, 2500, -1, NULL, NULL, 0);
        shot(card, prefix, "3-lock-woken");
    }
    close(g);
    if (card >= 0) close(card);
    return 0;
}
