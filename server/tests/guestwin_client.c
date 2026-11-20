/* guestwin_client.c — apps mode's guest side against a headless lord-server (tests/guestwin_headless.sh).
 *
 *   guestwin-client --direct PATH   connects to lord-server's endpoint (js_guestwin.c) like lordos-guest does
 *   guestwin-client --host PATH     listens on PATH as the host's end of org.lordos.agent would be, for lordos-guest
 *                                   started with --port PATH (the relay, tools/guest/lordos-guest.c)
 * Then: the hello says "windows", the app list and the window list arrive; launching a token that isn't listed opens
 * nothing; launching a listed app opens its window; a move puts its content where asked; an action on an id that
 * isn't listed changes nothing; close takes it away. Exit 0 when all of it held. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/vm/vmagent.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fd = -1, fails;
static uint8_t buf[AG_HEAD + LAG_APPLIST_BYTES + 4096];
static size_t have;
static int hello_windows;
static LagWinList wl;
static int have_list;
static LagAppList al;
static int have_apps, icons;

#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } else { printf("ok: "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void send_frame(uint16_t kind, const uint8_t *p, size_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, (uint32_t)n);
    if (write(fd, h, AG_HEAD) != AG_HEAD || (n && write(fd, p, n) != (ssize_t)n)) { fprintf(stderr, "write failed\n"); exit(1); }
}

/* reads frames for up to ms; returns early once done() says so */
static int pump(int ms, int (*done)(void)) {
    long long end = now_ms() + ms;
    for (;;) {
        if (done && done()) return 1;
        long long left = end - now_ms();
        if (left <= 0) return done ? done() : 0;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) continue;
        ssize_t r = read(fd, buf + have, sizeof buf - have);
        if (r <= 0) { fprintf(stderr, "the peer went away\n"); exit(1); }
        have += (size_t)r;
        for (;;) {
            AgFrame f;
            int k = ag_frame_next(buf, have, &f);
            if (k < 0) { fprintf(stderr, "bad frame\n"); exit(1); }
            if (!k) break;
            if (f.kind == AGK_HELLO) hello_windows = memmem(f.data, f.len, " windows", 8) != NULL;
            else if (f.kind == LAG_WINDOWS) { if (lag_windows_parse(f.data, f.len, &wl)) { fprintf(stderr, "a window list that doesn't parse\n"); fails++; } else have_list = 1; }
            else if (f.kind == LAG_APPLIST) { if (lag_applist_parse(f.data, f.len, &al)) { fprintf(stderr, "an app list that doesn't parse\n"); fails++; } else have_apps = 1; }
            else if (f.kind == LAG_WINICON) { uint32_t id; int w, h; const uint8_t *px; if (lag_icon_parse(f.data, f.len, &id, &w, &h, &px)) fails++; else icons++; }
            memmove(buf, buf + k, have - (size_t)k);
            have -= (size_t)k;
        }
    }
}

static int ready(void) { return hello_windows && have_list && have_apps; }
static int base_n;
static uint32_t new_id;
static int grew(void) {
    if (wl.n <= base_n) return 0;
    for (int i = 0; i < wl.n; i++) if (wl.w[i].flags & LAGW_FOCUSED) { new_id = wl.w[i].id; return 1; }
    new_id = wl.w[0].id;
    return 1;
}
static const LagWin *find(uint32_t id) { for (int i = 0; i < wl.n; i++) if (wl.w[i].id == id) return &wl.w[i]; return NULL; }
static int tx = 100, ty = 140, tw = 420, th = 300;      /* where the move puts the content (another place through the relay) */
static int moved(void) { const LagWin *w = find(new_id); return w && abs(w->x - tx) <= 2 && abs(w->y - ty) <= 2 && abs(w->w - tw) <= 2 && abs(w->h - th) <= 2; }
static int gone(void) { return !find(new_id); }

static void launch(const char *tok) { uint8_t b[LAG_TOKEN_MAX + 2]; size_t n = lag_launch_write(tok, b, sizeof b); send_frame(LAG_LAUNCH, b, n); }
static void act(uint32_t id, int action, int x, int y, int w, int h) {
    LagAct a = { id, action, x, y, w, h };
    uint8_t b[32];
    size_t n = lag_act_write(&a, b, sizeof b);
    send_frame(LAG_WINACT, b, n);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: guestwin-client --direct|--host PATH\n"); return 2; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", argv[2]);
    if (!strcmp(argv[1], "--direct")) {
        for (int i = 0; i < 160 && fd < 0; i++) {
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); fd = -1; usleep(250000); }
        }
        if (fd < 0) { fprintf(stderr, "no endpoint at %s\n", argv[2]); return 1; }
    } else {
        int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        unlink(argv[2]);
        if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) { perror("listen"); return 1; }
        struct pollfd p = { l, POLLIN, 0 };
        if (poll(&p, 1, 20000) <= 0 || (fd = accept(l, NULL, NULL)) < 0) { fprintf(stderr, "lordos-guest didn't connect\n"); return 1; }
        close(l);
    }
    CHECK(pump(20000, ready), "hello with \"windows\", the app list (%d apps) and the window list (%d windows)", al.n, wl.n);
    if (!ready()) return 1;
    const char *tok = lag_applist_has(&al, "calculator") ? "calculator" : NULL;
    CHECK(tok != NULL, "the launcher lists Calculator");
    if (!tok) return 1;
    base_n = wl.n;
    launch("not-listed-app");
    CHECK(!pump(1500, grew), "a token that isn't listed opens nothing");
    launch(tok);
    CHECK(pump(10000, grew), "launching a listed app opens its window (id %u, %d windows)", new_id, wl.n);
    const LagWin *w = find(new_id);
    if (w) printf("  window: \"%s\" (%s) %d,%d %dx%d flags %u\n", w->title, w->app, w->x, w->y, w->w, w->h, w->flags);
    if (strcmp(argv[1], "--direct")) { tx = 230; ty = 210; tw = 480; th = 360; }
    act(new_id, LAGA_MOVE, tx, ty, tw, th);
    CHECK(pump(5000, moved), "a move puts the content at %d,%d %dx%d", tx, ty, tw, th);
    w = find(new_id);
    if (w) printf("  now: %d,%d %dx%d\n", w->x, w->y, w->w, w->h);
    int before = wl.n;
    act(424242, LAGA_CLOSE, 0, 0, 0, 0);
    pump(800, NULL);
    CHECK(wl.n == before, "an action on an id that isn't listed changes nothing");
    uint8_t on = 1;
    send_frame(LAG_APPS, &on, 1);
    pump(500, NULL);
    act(new_id, LAGA_CLOSE, 0, 0, 0, 0);
    CHECK(pump(5000, gone), "close takes the window away");
    on = 0;
    send_frame(LAG_APPS, &on, 1);
    pump(300, NULL);
    printf("guestwin-client: %d icons seen, %d failed\n", icons, fails);
    return fails ? 1 : 0;
}
