/* moddrag_x11.c — the same as moddrag_client.c for an X11 app under Xwayland (GitHub #219): ❖ + left drag anywhere in
 * the window moves it and ❖ + right drag resizes it, and the X client sees neither press; a plain click and Alt + click
 * (with ❖ chosen) are the app's. Against a headless build/lord-server-test with --test-hooks and --user (its X socket),
 * run by tests/moddrag_headless.sh with DISPLAY and XAUTHORITY. Prints ok/FAIL; exit 1 on a failure, 77 without the hooks. */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <xcb/xcb.h>

static xcb_connection_t *X;
static xcb_window_t win;
static int fails, checks;
static int ptr_in, presses, releases, mapped, width = 400, height = 300;
static int lx, ly;                      /* the pointer on the window, X pixels */

static void check(const char *what, int ok, const char *fmt, ...) {
    checks++;
    char detail[200] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (ok) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: %s\n", what, detail); }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int th(const char *fmt, ...) {
    char cmd[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!rt || (size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/lordos-test-hooks.sock", rt) >= sizeof a.sun_path) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    char buf[64];
    struct pollfd p = { s, POLLIN, 0 };
    ssize_t n = -1;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && send(s, cmd, strlen(cmd), MSG_NOSIGNAL) >= 0 && poll(&p, 1, 3000) == 1)
        n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strncmp(buf, "ok", 2) ? -1 : 0;
}

static void handle(xcb_generic_event_t *e) {
    switch (e->response_type & 0x7f) {
    case XCB_MAP_NOTIFY: mapped = 1; break;
    case XCB_ENTER_NOTIFY: { xcb_enter_notify_event_t *v = (void *)e; ptr_in = v->event == win; lx = v->event_x; ly = v->event_y; break; }
    case XCB_LEAVE_NOTIFY: ptr_in = 0; break;
    case XCB_MOTION_NOTIFY: { xcb_motion_notify_event_t *v = (void *)e; if (v->event == win) { ptr_in = 1; lx = v->event_x; ly = v->event_y; } break; }
    case XCB_BUTTON_PRESS: presses++; break;
    case XCB_BUTTON_RELEASE: releases++; break;
    case XCB_CONFIGURE_NOTIFY: { xcb_configure_notify_event_t *v = (void *)e; if (v->window == win) { width = v->width; height = v->height; } break; }
    default: break;
    }
}
static void pump(int ms) {
    double end = now_ms() + ms;
    while (now_ms() < end) {
        xcb_flush(X);
        struct pollfd p = { xcb_get_file_descriptor(X), POLLIN, 0 };
        poll(&p, 1, 30);
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(X))) { handle(e); free(e); }
        if (xcb_connection_has_error(X)) { fprintf(stderr, "moddrag-x11: connection lost\n"); exit(1); }
    }
}
static int wait_for(int *flag, int ms) {
    double end = now_ms() + ms;
    while (!*flag && now_ms() < end) pump(30);
    return *flag;
}
static void step(const char *cmd) { if (th("%s", cmd)) { fprintf(stderr, "moddrag-x11: the test hooks refused \"%s\"\n", cmd); exit(1); } pump(60); }
static void click(int key, int button) {
    char b[32];
    if (key) { snprintf(b, sizeof b, "key %d 1", key); step(b); }
    snprintf(b, sizeof b, "btn %d 1", button); step(b);
    snprintf(b, sizeof b, "btn %d 0", button); step(b);
    if (key) { snprintf(b, sizeof b, "key %d 0", key); step(b); }
    pump(250);
}

int main(void) {
    X = xcb_connect(NULL, NULL);
    if (!X || xcb_connection_has_error(X)) { fprintf(stderr, "moddrag-x11: no X display\n"); return 2; }
    if (th("ptr 3 3")) { printf("skipped: needs lord-server-test --test-hooks\n"); return 77; }
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(X)).data;
    win = xcb_generate_id(X);
    uint32_t vals[2] = { scr->white_pixel, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
                         XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_create_window(X, XCB_COPY_FROM_PARENT, win, scr->root, 0, 0, 400, 300, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    xcb_change_property(X, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 11, "moddrag-x11");
    xcb_map_window(X, win);
    xcb_flush(X);
    check("the X window is mapped", wait_for(&mapped, 5000), "no MapNotify");
    pump(1500);                                  /* Xwayland's surface reached the theme */

    static const int pts[][2] = { { 640, 420 }, { 600, 380 }, { 700, 460 }, { 560, 340 }, { 740, 500 } };
    int px = -1, py = -1;
    for (size_t i = 0; i < sizeof pts / sizeof *pts && px < 0; i++) {
        th("ptr 3 3");
        pump(150);
        char b[32];
        snprintf(b, sizeof b, "ptr %d %d", pts[i][0], pts[i][1]);
        step(b);
        if (wait_for(&ptr_in, 800) && lx > 40 && ly > 40 && lx < width - 40 && ly < height - 40) { px = pts[i][0]; py = pts[i][1]; }
    }
    check("the pointer is over the X window", px >= 0, "it never entered the window's middle");
    if (px < 0) { printf("%d checks, %d failed\n", checks, fails); return 1; }
    int lx0 = lx, ly0 = ly;

    presses = releases = 0;
    click(0, 0);
    check("X11: a plain click reaches the app", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);

    presses = releases = 0;
    char b[48];
    step("key 125 1");
    step("btn 0 1");
    snprintf(b, sizeof b, "ptr %d %d", px + 40, py + 25); step(b);
    snprintf(b, sizeof b, "ptr %d %d", px + 80, py + 50); step(b);
    step("btn 0 0");
    step("key 125 0");
    pump(500);
    check("X11: ❖ + drag: the app gets no press and no release", presses == 0 && releases == 0, "%d presses, %d releases", presses, releases);
    snprintf(b, sizeof b, "ptr %d %d", px + 81, py + 51); step(b);
    pump(400);
    check("X11: ❖ + drag: the window moved with the pointer", ptr_in && lx > lx0 - 3 && lx < lx0 + 5 && ly > ly0 - 3 && ly < ly0 + 5,
          "at the dragged-to point the pointer is at (%d, %d) on the window, was (%d, %d)%s", lx, ly, lx0, ly0, ptr_in ? "" : ", not over it");
    px += 80;
    py += 50;
    int ox = px + 1 - lx, oy = py + 1 - ly;

    snprintf(b, sizeof b, "ptr %d %d", px, py); step(b);
    presses = releases = 0;
    click(56, 0);
    check("X11: Alt + click with ❖ chosen: the app's", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);

    int w0 = width, h0 = height;
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 - 25, oy + h0 - 25); step(b);
    presses = releases = 0;
    step("key 125 1");
    step("btn 2 1");
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 + 5, oy + h0 - 5); step(b);
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 + 35, oy + h0 + 15); step(b);
    step("btn 2 0");
    step("key 125 0");
    pump(800);
    check("X11: ❖ + right drag: the app gets no press", presses == 0 && releases == 0, "%d presses, %d releases", presses, releases);
    check("X11: ❖ + right drag: the window is resized by the drag", width >= w0 + 56 && width <= w0 + 64 && height >= h0 + 36 && height <= h0 + 44,
          "%d × %d, was %d × %d (want +60, +40)", width, height, w0, h0);

    check("X11: still connected at the end", !xcb_connection_has_error(X), "connection error");
    printf("%d checks, %d failed\n", checks, fails);
    xcb_disconnect(X);
    return fails ? 1 : 0;
}
