/* xpopup_x11.c — an X11 app's menus under Xwayland (GitHub #209): an override-redirect window (what Qt's xcb plugin,
 * GTK 3 on X11, LibreOffice, Java and Electron use for a menu, a dropdown or a tooltip) and a window of type
 * _NET_WM_WINDOW_TYPE_POPUP_MENU are shown where X put them, take the pointer's clicks, and are gone once unmapped.
 * From 0.8.9 to 0.8.9.5 every one of them stayed invisible: a commit on an X popup returned before mapping, because it
 * has no xdg_popup (wl.c surface_commit_role). Against a headless build/lord-server-test with --test-hooks and --user,
 * run by tests/xpopup_headless.sh with DISPLAY and XAUTHORITY; the screen is read with the hooks' "frame output" (a PPM
 * in $XDG_RUNTIME_DIR). Prints ok/FAIL; exit 1 on a failure, 77 without the hooks. */
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
static xcb_screen_t *scr;
static xcb_window_t top, pop;
static int fails, checks, top_mapped, pop_mapped, pop_presses;

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
    case XCB_MAP_NOTIFY: { xcb_map_notify_event_t *v = (void *)e; if (v->window == top) top_mapped = 1; if (v->window == pop) pop_mapped = 1; break; }
    case XCB_UNMAP_NOTIFY: { xcb_unmap_notify_event_t *v = (void *)e; if (v->window == pop) pop_mapped = 0; break; }
    case XCB_BUTTON_PRESS: { xcb_button_press_event_t *v = (void *)e; if (v->event == pop) pop_presses++; break; }
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
        if (xcb_connection_has_error(X)) { fprintf(stderr, "xpopup-x11: connection lost\n"); exit(1); }
    }
}
static int wait_for(int *flag, int ms) {
    double end = now_ms() + ms;
    while (!*flag && now_ms() < end) pump(30);
    return *flag;
}

/* the screen's pixel at (x, y) as 0xRRGGBB, from the hooks' "frame output"; -1 when it can't be read */
static long pixel(int x, int y) {
    if (th("frame output")) return -1;
    char path[512];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || (size_t)snprintf(path, sizeof path, "%s/lordos-test-frame-output.ppm", rt) >= sizeof path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int w = 0, h = 0, max = 0;
    long r = -1;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) == 3 && fgetc(f) != EOF && x >= 0 && y >= 0 && x < w && y < h && max == 255 &&
        !fseek(f, ((long)y * w + x) * 3, SEEK_CUR)) {
        unsigned char p[3];
        if (fread(p, 1, 3, f) == 3) r = (long)p[0] << 16 | p[1] << 8 | p[2];
    }
    fclose(f);
    return r;
}
static int near(long c, long want) {
    if (c < 0) return 0;
    for (int sh = 0; sh < 24; sh += 8) { int d = (int)((c >> sh) & 255) - (int)((want >> sh) & 255); if (d < -8 || d > 8) return 0; }
    return 1;
}

static xcb_atom_t atom(const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(X, xcb_intern_atom(X, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

/* one popup: COLOR at (x, y) of the screen, override-redirect or a managed POPUP_MENU window; shown, clicked, hidden */
static void one(const char *kind, int override, uint32_t color, int x, int y) {
    char what[160];
    pop = xcb_generate_id(X);
    uint32_t vals[3] = { color, override ? 1u : 0u, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_create_window(X, XCB_COPY_FROM_PARENT, pop, scr->root, (int16_t)x, (int16_t)y, 120, 90, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      scr->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, vals);
    xcb_change_property(X, XCB_PROP_MODE_REPLACE, pop, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &top);
    xcb_atom_t t = atom(override ? "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU" : "_NET_WM_WINDOW_TYPE_POPUP_MENU");
    xcb_change_property(X, XCB_PROP_MODE_REPLACE, pop, atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32, 1, &t);
    pop_mapped = 0;
    xcb_map_window(X, pop);
    xcb_flush(X);
    snprintf(what, sizeof what, "%s: X maps it", kind);
    check(what, wait_for(&pop_mapped, 5000), "no MapNotify");
    long c = -1;
    for (int i = 0; i < 20 && !near(c, color); i++) { pump(150); c = pixel(x + 60, y + 45); }
    snprintf(what, sizeof what, "%s: shown where X put it (%d, %d)", kind, x, y);
    check(what, near(c, color), "the screen at its middle is #%06lx, want #%06x", c, (unsigned)color);

    char b[32];
    pop_presses = 0;
    snprintf(b, sizeof b, "ptr %d %d", x + 60, y + 45);
    th("%s", b);
    pump(200);
    th("btn 0 1");
    pump(100);
    th("btn 0 0");
    pump(400);
    snprintf(what, sizeof what, "%s: a click on it reaches it", kind);
    check(what, pop_presses >= 1, "%d presses", pop_presses);

    xcb_unmap_window(X, pop);
    xcb_flush(X);
    c = pixel(x + 60, y + 45);
    for (int i = 0; i < 20 && near(c, color); i++) { pump(150); c = pixel(x + 60, y + 45); }
    snprintf(what, sizeof what, "%s: gone once unmapped", kind);
    check(what, c >= 0 && !near(c, color), "the screen at its middle is still #%06lx", c);
    xcb_destroy_window(X, pop);
    xcb_flush(X);
    pump(200);
}

int main(void) {
    X = xcb_connect(NULL, NULL);
    if (!X || xcb_connection_has_error(X)) { fprintf(stderr, "xpopup-x11: no X display\n"); return 2; }
    if (th("ptr 3 3")) { printf("skipped: needs lord-server-test --test-hooks\n"); return 77; }
    scr = xcb_setup_roots_iterator(xcb_get_setup(X)).data;
    top = xcb_generate_id(X);
    uint32_t vals[2] = { scr->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_create_window(X, XCB_COPY_FROM_PARENT, top, scr->root, 0, 0, 500, 360, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    xcb_change_property(X, XCB_PROP_MODE_REPLACE, top, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 10, "xpopup-x11");
    xcb_map_window(X, top);
    xcb_flush(X);
    check("the X window is mapped", wait_for(&top_mapped, 5000), "no MapNotify");
    pump(1500);                                  /* Xwayland's surface reached the theme */
    xcb_translate_coordinates_reply_t *tr = xcb_translate_coordinates_reply(X, xcb_translate_coordinates(X, top, scr->root, 0, 0), NULL);
    int wx = tr ? tr->dst_x : 0, wy = tr ? tr->dst_y : 0;
    free(tr);

    one("an override-redirect menu", 1, 0xd02020, wx + 40, wy + 60);
    one("a POPUP_MENU window", 0, 0x2040d0, wx + 200, wy + 120);

    check("X11: still connected at the end", !xcb_connection_has_error(X), "connection error");
    printf("%d checks, %d failed\n", checks, fails);
    xcb_disconnect(X);
    return fails ? 1 : 0;
}
