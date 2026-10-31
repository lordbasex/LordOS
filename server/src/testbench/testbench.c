/* testbench.c — lordos-testbench: LordOS's visual test bench (docs/PAPER-VALIDATION.md, "the visual test
 * bench"). A plain Wayland client, so it tests the compositor from the outside like any app, and runs on
 * any compositor for comparison. What it shows:
 *
 *   - the test card (card.c), with a live clock and a marker that moves one step every frame, so a
 *     dropped or repeated frame is visible;
 *   - timing: frames a second, frame-to-frame time, and with wp_presentation when each frame really
 *     reached the screen, the display's refresh and the vblanks it skipped;
 *   - the keyboard: a full keyboard that lights each key while it is down and marks every key seen, the
 *     keysym from the keymap the compositor sent, modifiers, repeat rate and delay;
 *   - the pointer: a crosshair at the exact position, buttons, wheel (discrete and high resolution),
 *     axis source;
 *   - the Wayland globals the compositor advertises, with their versions.
 *
 *   lordos-testbench                    interactive (F5 runs the protocol checklist again, Pause freezes
 *                                       the picture: one still frame to compare or capture, Ctrl+L
 *                                       locks the pointer for 10 s)
 *   lordos-testbench --auto [seconds]   runs for a while (default 5 s), prints a JSON report and exits
 *   --gl                                 also starts the 3D view (G does it interactively)
 *
 * It reads no files and talks to nothing but the compositor. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "card.h"
#include "draw.h"
#include "tb.h"
#include "gl3d.h"
#include "lockkey.h"
#include "../bench/bench.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "client/xdg-shell-client-protocol.h"
#include "client/xdg-decoration-unstable-v1-client-protocol.h"
#include "client/presentation-time-client-protocol.h"
#include "client/pointer-constraints-unstable-v1-client-protocol.h"

/* ---- colours ---- */
#define C_BG       TB_RGB(18, 18, 26)
#define C_PANEL    TB_RGB(28, 28, 38)
#define C_TEXT     TB_RGB(225, 225, 235)
#define C_DIM      TB_RGB(140, 140, 160)
#define C_HEAD     TB_RGB(180, 150, 255)
#define C_GOOD     TB_RGB(120, 230, 150)
#define C_WARN     TB_RGB(255, 190, 70)
#define C_BAD      TB_RGB(255, 90, 80)
#define C_LAMP_ON  TB_RGB(242, 163, 58)
#define C_LAMP_OFF TB_RGB(55, 55, 55)
#define C_KEY      TB_RGB(44, 44, 56)
#define C_SEEN     TB_RGB(30, 80, 50)
#define C_CROSS    TB_RGB(0, 230, 255)
#define C_SYSTEM   TB_RGB(150, 110, 255)

/* keys LordOS's compositor keeps for itself (wl.c, wl_key): Print Screen, the media keys and ❖ on its own
 * (its press never reaches an app; ❖ combinations are LordOS's shortcuts) */
static int system_key(int code) { return code == KEY_SYSRQ || code == KEY_LEFTMETA || code == KEY_RIGHTMETA; }

/* ---- Wayland ---- */
struct wl_display *dpy;
struct wl_registry *registry;
struct wl_compositor *comp;
struct wl_shm *shm;
struct xdg_wm_base *wm;
struct wl_seat *seat;
struct wl_pointer *pointer;
struct wl_keyboard *keyboard;
static struct zxdg_decoration_manager_v1 *decoman;
static struct wp_presentation *presentation;
static struct wl_subcompositor *subcomp;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_locked_pointer_v1 *locked;
static int lock_on;
static double lock_t;
struct wl_surface *surf;
struct xdg_surface *xsurf;
struct xdg_toplevel *top;
uint32_t kb_serial, ptr_serial, toplevel_states;

Global globals[96];
int nglobals;

typedef struct { struct wl_buffer *b; uint32_t *px; size_t size; int w, h, busy; } Buf;
static Buf bufs[2];
int win_w = 1280, win_h = 800;
static int configured, closed, frame_pending;
static Card card;
static int card_x, card_y;

/* ---- measurements ---- */
#define now_ms bench_now_ms
static BenchFrames ft;                  /* frame times (src/bench, shared with LordBench) */
static BenchPresent pr;
static unsigned long no_buffer;
static uint32_t clock_id;

/* ---- input ---- */
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *kstate;
static unsigned char down[KEY_MAX + 1], seen[KEY_MAX + 1];
static int nseen, repeat_rate = -1, repeat_delay = -1, kb_focus;
static char last_key[160] = "-", mods[64] = "-", other_keys[96] = "";
static double ptr_x = -1, ptr_y = -1;
static int ptr_in, btn_down[16], btn_seen[16];
static long wheel_v, wheel_h, wheel120_v, wheel120_h;
static char axis_src[16] = "-";
static int rerun_checks, paused;
/* input to screen: the time of the last key or button, kept until the frame showing it is presented */
static uint32_t lat_ev;
static int lat_pending;
static BenchLatency lat = { .last = -1 };
static int clock_mono = 1;              /* wp_presentation's clock is CLOCK_MONOTONIC (the event times' base) */
/* generated video: content at 24/30/60 fps, and how many refreshes each content frame stayed */
static int video_fps;                   /* 0 off */
static long video_last = -1;
static int video_run;
static unsigned long video_hold[5];     /* frames held 1, 2, 3, 4, 5+ refreshes */
/* the 3D view's presentation */
static unsigned long g_presented, g_zero;          /* F5 runs the checklist again; Pause freezes the picture */

/* ---- the keyboard drawn on screen: evdev codes, widths in quarter units ---- */
typedef struct { int code, w; const char *label; } Key;
#define GAP(w) { 0, w, NULL }
static const Key ROWS[6][24] = {
    { { KEY_ESC, 4, "ESC" }, GAP(4), { KEY_F1, 4, "F1" }, { KEY_F2, 4, "F2" }, { KEY_F3, 4, "F3" }, { KEY_F4, 4, "F4" }, GAP(2),
      { KEY_F5, 4, "F5" }, { KEY_F6, 4, "F6" }, { KEY_F7, 4, "F7" }, { KEY_F8, 4, "F8" }, GAP(2), { KEY_F9, 4, "F9" }, { KEY_F10, 4, "F10" },
      { KEY_F11, 4, "F11" }, { KEY_F12, 4, "F12" } },
    { { KEY_GRAVE, 4, "`" }, { KEY_1, 4, "1" }, { KEY_2, 4, "2" }, { KEY_3, 4, "3" }, { KEY_4, 4, "4" }, { KEY_5, 4, "5" }, { KEY_6, 4, "6" },
      { KEY_7, 4, "7" }, { KEY_8, 4, "8" }, { KEY_9, 4, "9" }, { KEY_0, 4, "0" }, { KEY_MINUS, 4, "-" }, { KEY_EQUAL, 4, "=" }, { KEY_BACKSPACE, 8, "BKSP" } },
    { { KEY_TAB, 6, "TAB" }, { KEY_Q, 4, "Q" }, { KEY_W, 4, "W" }, { KEY_E, 4, "E" }, { KEY_R, 4, "R" }, { KEY_T, 4, "T" }, { KEY_Y, 4, "Y" },
      { KEY_U, 4, "U" }, { KEY_I, 4, "I" }, { KEY_O, 4, "O" }, { KEY_P, 4, "P" }, { KEY_LEFTBRACE, 4, "[" }, { KEY_RIGHTBRACE, 4, "]" }, { KEY_BACKSLASH, 6, "\\" } },
    { { KEY_CAPSLOCK, 7, "CAPS" }, { KEY_A, 4, "A" }, { KEY_S, 4, "S" }, { KEY_D, 4, "D" }, { KEY_F, 4, "F" }, { KEY_G, 4, "G" }, { KEY_H, 4, "H" },
      { KEY_J, 4, "J" }, { KEY_K, 4, "K" }, { KEY_L, 4, "L" }, { KEY_SEMICOLON, 4, ";" }, { KEY_APOSTROPHE, 4, "'" }, { KEY_ENTER, 9, "ENTER" } },
    { { KEY_LEFTSHIFT, 9, "SHIFT" }, { KEY_102ND, 0, NULL }, { KEY_Z, 4, "Z" }, { KEY_X, 4, "X" }, { KEY_C, 4, "C" }, { KEY_V, 4, "V" }, { KEY_B, 4, "B" },
      { KEY_N, 4, "N" }, { KEY_M, 4, "M" }, { KEY_COMMA, 4, "," }, { KEY_DOT, 4, "." }, { KEY_SLASH, 4, "/" }, { KEY_RIGHTSHIFT, 11, "SHIFT" } },
    { { KEY_LEFTCTRL, 5, "CTRL" }, { KEY_LEFTMETA, 5, "SUPER" }, { KEY_LEFTALT, 5, "ALT" }, { KEY_SPACE, 25, "SPACE" }, { KEY_RIGHTALT, 5, "ALTGR" },
      { KEY_RIGHTMETA, 5, "SUPER" }, { KEY_COMPOSE, 5, "MENU" }, { KEY_RIGHTCTRL, 5, "CTRL" } },
};
static const Key NAV[6][4] = {
    { { KEY_SYSRQ, 4, "PRT" }, { KEY_SCROLLLOCK, 4, "SCR" }, { KEY_PAUSE, 4, "PAU" } },
    { { KEY_INSERT, 4, "INS" }, { KEY_HOME, 4, "HOME" }, { KEY_PAGEUP, 4, "PGUP" } },
    { { KEY_DELETE, 4, "DEL" }, { KEY_END, 4, "END" }, { KEY_PAGEDOWN, 4, "PGDN" } },
    { GAP(4) },
    { GAP(4), { KEY_UP, 4, "^" } },
    { { KEY_LEFT, 4, "<" }, { KEY_DOWN, 4, "V" }, { KEY_RIGHT, 4, ">" } },
};
static const Key PAD[6][4] = {
    { GAP(4) },
    { { KEY_NUMLOCK, 4, "NUM" }, { KEY_KPSLASH, 4, "/" }, { KEY_KPASTERISK, 4, "*" }, { KEY_KPMINUS, 4, "-" } },
    { { KEY_KP7, 4, "7" }, { KEY_KP8, 4, "8" }, { KEY_KP9, 4, "9" }, { KEY_KPPLUS, 4, "+" } },
    { { KEY_KP4, 4, "4" }, { KEY_KP5, 4, "5" }, { KEY_KP6, 4, "6" }, GAP(4) },
    { { KEY_KP1, 4, "1" }, { KEY_KP2, 4, "2" }, { KEY_KP3, 4, "3" }, { KEY_KPENTER, 4, "ENT" } },
    { { KEY_KP0, 8, "0" }, { KEY_KPDOT, 4, "." } },
};

/* every key on the drawn keyboard, for the "seen" count */
static int on_map(int code) {
    for (int r = 0; r < 6; r++) {
        for (int i = 0; i < 24 && (ROWS[r][i].w || ROWS[r][i].code); i++) if (ROWS[r][i].w && ROWS[r][i].code == code) return 1;
        for (int i = 0; i < 4; i++) if ((NAV[r][i].w && NAV[r][i].code == code) || (PAD[r][i].w && PAD[r][i].code == code)) return 1;
    }
    return 0;
}
static int map_total(void) {
    int n = 0;
    for (int c = 1; c <= KEY_MAX; c++) n += on_map(c);
    return n;
}

/* ---- buffers ---- */
static void buf_release(void *data, struct wl_buffer *b) { (void)b; ((Buf *)data)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static void buf_free(Buf *b) {
    if (b->b) wl_buffer_destroy(b->b);
    if (b->px) munmap(b->px, b->size);
    memset(b, 0, sizeof *b);
}

static int buf_make(Buf *b, int w, int h) {
    buf_free(b);
    size_t stride = (size_t)w * 4, size = stride * (size_t)h;
    int fd = memfd_create("lordos-testbench", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { if (fd >= 0) close(fd); return -1; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    b->b = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->b, &buf_listener, b);
    b->px = p;
    b->size = size;
    b->w = w;
    b->h = h;
    return 0;
}

/* ---- drawing a frame ---- */
static int panel_w(void) { return win_w >= 1100 ? 420 : 300; }
static int kbd_h(void) { return 214; }

static void draw_keys(Surf *s, const Key *row, int n, int x, int y, int unit, int kh) {
    for (int i = 0; i < n; i++) {
        const Key *k = &row[i];
        if (!k->w) { if (!k->code) break; continue; }
        int w = k->w * unit / 4 - 3;
        if (k->code) {
            uint32_t bg = down[k->code] ? C_LAMP_ON : seen[k->code] ? C_SEEN : C_KEY;
            d_fill(s, x, y, w, kh, bg);
            if (seen[k->code] && !down[k->code]) d_frame(s, x, y, w, kh, 1, C_GOOD);
            else if (system_key(k->code)) d_frame(s, x, y, w, kh, 1, C_SYSTEM);
            d_text_c(s, x + w / 2, y + kh / 2, 1, down[k->code] ? TB_RGB(0, 0, 0) : C_TEXT, k->label);
        }
        x += k->w * unit / 4;
    }
}

static void draw_keyboard(Surf *s, int y0) {
    int total_units = 15 * 4 + 2 + 3 * 4 + 2 + 4 * 4;      /* main block, gap, nav, gap, keypad (quarter units) */
    int unit = (s->w - 24) * 4 / total_units;
    if (unit > 52) unit = 52;
    int kh = (kbd_h() - 40) / 6 - 3;
    int x0 = 12, xn = x0 + (15 * 4 + 2) * unit / 4, xp = xn + (3 * 4 + 2) * unit / 4;
    for (int r = 0; r < 6; r++) {
        int y = y0 + 28 + r * (kh + 3);
        draw_keys(s, ROWS[r], 24, x0, y, unit, kh);
        draw_keys(s, NAV[r], 4, xn, y, unit, kh);
        draw_keys(s, PAD[r], 4, xp, y, unit, kh);
    }
    char t[160];
    snprintf(t, sizeof t, "KEYBOARD  %d OF %d KEYS SEEN%s%s", nseen, map_total(), other_keys[0] ? "   OTHER: " : "", other_keys);
    d_text(s, s->w - 12 - d_text_w("PURPLE: KEPT BY LORDOS (PRINT, SUPER ALONE, MEDIA KEYS)", 1), y0 + 10, 1, C_SYSTEM, "PURPLE: KEPT BY LORDOS (PRINT, SUPER ALONE, MEDIA KEYS)");
    d_text(s, 12, y0 + 10, 1, nseen == map_total() ? C_GOOD : C_HEAD, t);
}

static void stat_line(Surf *s, int x, int *y, const char *label, const char *value, uint32_t c) {
    d_text(s, x, *y, 1, C_DIM, label);
    d_text(s, x + 110, *y, 1, c, value);
    *y += 12;
}

static void draw_panel(Surf *s, int x0) {
    int pw = panel_w();
    d_fill(s, x0, 0, pw, s->h - kbd_h(), C_PANEL);
    int x = x0 + 14, y = 14;
    d_text(s, x, y, 2, C_TEXT, "TEST BENCH");
    y += 26;
    char v[160];
    /* timing */
    d_text(s, x, y, 1, C_HEAD, "TIMING");
    y += 14;
    BenchStats st;
    bench_frames_stats(&ft, &st);
    snprintf(v, sizeof v, "%.1f", bench_frames_fps(&ft));
    stat_line(s, x, &y, "FRAMES/S", v, C_GOOD);
    snprintf(v, sizeof v, "%.2f MS AVG  %.2f P99  %.2f MAX", st.avg, st.p99, st.max);
    stat_line(s, x, &y, "FRAME TIME", v, st.p99 > (pr.refresh_ns ? pr.refresh_ns / 1e6 * 1.5 : 25) ? C_WARN : C_TEXT);
    snprintf(v, sizeof v, "%lu", ft.frames);
    stat_line(s, x, &y, "FRAMES", v, C_TEXT);
    if (presentation) {
        snprintf(v, sizeof v, "%lu SHOWN  %lu DISCARDED", pr.presented, pr.discarded);
        stat_line(s, x, &y, "PRESENTED", v, pr.discarded ? C_WARN : C_GOOD);
        if (pr.refresh_ns) snprintf(v, sizeof v, "%.2f HZ", bench_refresh_hz(&pr));
        else if (out_refresh_mhz) snprintf(v, sizeof v, "%.2f HZ (OUTPUT MODE; NONE IN FEEDBACK)", out_refresh_mhz / 1000.0);
        else snprintf(v, sizeof v, "UNKNOWN");
        stat_line(s, x, &y, "REFRESH", v, C_TEXT);
        snprintf(v, sizeof v, "%lu", pr.vblank_skips);
        stat_line(s, x, &y, "VBLANKS SKIPPED", v, pr.vblank_skips ? C_WARN : C_GOOD);
        snprintf(v, sizeof v, "%lu", pr.zero_copy);
        stat_line(s, x, &y, "ZERO-COPY", v, C_TEXT);
    } else stat_line(s, x, &y, "PRESENTED", "NO WP_PRESENTATION", C_WARN);
    snprintf(v, sizeof v, "%lu", no_buffer);
    stat_line(s, x, &y, "NO FREE BUFFER", v, no_buffer ? C_WARN : C_TEXT);
    snprintf(v, sizeof v, "%d X %d", win_w, win_h);
    stat_line(s, x, &y, "WINDOW", v, C_TEXT);
    if (lat.n) snprintf(v, sizeof v, "%.1f MS LAST  %.1f AVG  (%lu)", lat.last, bench_latency_avg(&lat), lat.n);
    else snprintf(v, sizeof v, lat.lost ? "NOT MEASURABLE (%lu FRAMES NOT PRESENTED)" : "PRESS A KEY OR CLICK", lat.lost);
    stat_line(s, x, &y, "INPUT TO SCREEN", v, lat.n ? (bench_latency_avg(&lat) > 50 ? C_WARN : C_GOOD) : C_DIM);
    y += 8;
    /* graphics: the 3D view (G) */
    d_text(s, x, y, 1, C_HEAD, "GRAPHICS  (G: 3D  V: VIDEO  CTRL+L: LOCK)");
    y += 14;
    if (gl3d_on()) {
        stat_line(s, x, &y, "RENDERER", gl3d.renderer, C_TEXT);
        stat_line(s, x, &y, "KIND", gl3d.kind, strstr(gl3d.kind, "SOFTWARE") ? C_WARN : C_GOOD);
        snprintf(v, sizeof v, "%.2f MS PER FRAME  %lu FRAMES", gl3d.gpu_ms, gl3d.frames);
        stat_line(s, x, &y, "DRAW TIME", v, C_TEXT);
        snprintf(v, sizeof v, "%lu SHOWN  %lu ZERO-COPY", g_presented, g_zero);
        stat_line(s, x, &y, "3D PRESENTED", v, C_TEXT);
    } else stat_line(s, x, &y, "3D", gl3d.error[0] ? gl3d.error : (gl3d.renderer[0] ? gl3d.renderer : "OFF"), gl3d.error[0] ? C_BAD : C_DIM);
    if (video_fps) {
        snprintf(v, sizeof v, "%d FPS: HELD 1X %lu  2X %lu  3X %lu  4X %lu  5+ %lu", video_fps, video_hold[0], video_hold[1], video_hold[2], video_hold[3], video_hold[4]);
        stat_line(s, x, &y, "VIDEO CADENCE", v, C_TEXT);
    }
    if (locked) stat_line(s, x, &y, "POINTER LOCK", lock_on ? "LOCKED (CTRL+L OR 10 S)" : "ASKED, NOT LOCKED YET", lock_on ? C_GOOD : C_WARN);
    y += 8;
    /* pointer */
    d_text(s, x, y, 1, C_HEAD, "POINTER");
    y += 14;
    if (ptr_in) snprintf(v, sizeof v, "%.2f, %.2f", ptr_x, ptr_y); else snprintf(v, sizeof v, "OUTSIDE");
    stat_line(s, x, &y, "POSITION", v, ptr_in ? C_TEXT : C_DIM);
    static const char *bn[8] = { "L", "R", "M", "SIDE", "EXTRA", "FWD", "BACK", "TASK" };
    int bx = x + 110;
    d_text(s, x, y, 1, C_DIM, "BUTTONS");
    for (int i = 0; i < 8; i++) {
        int w = d_text_w(bn[i], 1) + 8;
        d_fill(s, bx, y - 2, w, 11, btn_down[i] ? C_LAMP_ON : btn_seen[i] ? C_SEEN : C_KEY);
        d_text(s, bx + 4, y, 1, btn_down[i] ? TB_RGB(0, 0, 0) : C_TEXT, bn[i]);
        bx += w + 3;
    }
    y += 14;
    snprintf(v, sizeof v, "V %ld  H %ld  (120THS V %ld H %ld)", wheel_v, wheel_h, wheel120_v, wheel120_h);
    stat_line(s, x, &y, "WHEEL", v, C_TEXT);
    stat_line(s, x, &y, "AXIS SOURCE", axis_src, C_TEXT);
    y += 8;
    /* keyboard */
    d_text(s, x, y, 1, C_HEAD, "KEYBOARD");
    y += 14;
    stat_line(s, x, &y, "FOCUS", kb_focus ? "YES" : "NO", kb_focus ? C_GOOD : C_DIM);
    stat_line(s, x, &y, "LAST KEY", last_key, C_TEXT);
    stat_line(s, x, &y, "MODIFIERS", mods, C_TEXT);
    if (repeat_rate >= 0) snprintf(v, sizeof v, "%d/S AFTER %d MS", repeat_rate, repeat_delay); else snprintf(v, sizeof v, "NOT SENT");
    stat_line(s, x, &y, "REPEAT", v, C_TEXT);
    y += 8;
    /* the protocol checklist (checks.c), or the globals until it has run */
    if (nchecks) {
        int pass = 0, fail = 0;
        for (int i = 0; i < nchecks; i++) { pass += checks[i].state == CK_PASS; fail += checks[i].state == CK_FAIL; }
        snprintf(v, sizeof v, "WAYLAND  %d GLOBALS  CHECKS %d OK  %d FAILED  (F5 AGAIN)", nglobals, pass, fail);
        d_text(s, x, y, 1, fail ? C_BAD : C_HEAD, v);
        y += 14;
        for (int i = 0; i < nchecks && y < s->h - kbd_h() - 10; i++) {
            static const uint32_t col[4] = { TB_RGB(140, 140, 160), TB_RGB(120, 230, 150), TB_RGB(255, 90, 80), TB_RGB(110, 170, 255) };
            d_fill(s, x, y + 1, 6, 6, col[checks[i].state & 3]);
            char line[240];
            snprintf(line, sizeof line, "%.22s  %.*s", checks[i].id, (pw - 28) / 6 - 26, checks[i].detail);
            d_text(s, x + 10, y, 1, checks[i].state == CK_FAIL ? C_BAD : C_TEXT, line);
            y += 10;
        }
        return;
    }
    snprintf(v, sizeof v, "WAYLAND  %d GLOBALS", nglobals);
    d_text(s, x, y, 1, C_HEAD, v);
    y += 14;
    int col = 0, ystart = y, colw = (pw - 28) / 2;
    for (int i = 0; i < nglobals; i++) {
        if (y > s->h - kbd_h() - 14) {
            if (col) break;
            col = 1;
            y = ystart;
        }
        char g[96], ver[16];
        snprintf(ver, sizeof ver, " V%u", globals[i].version);
        int room = colw / 6 - (int)strlen(ver) - 1;    /* characters that fit the column */
        if (room < 4) room = 4;
        snprintf(g, sizeof g, "%.*s%s", room, globals[i].iface, ver);
        d_text(s, x + col * colw, y, 1, C_TEXT, g);
        y += 10;
    }
}

/* the input box of the card: a pad lit by the keyboard (arrows, Z X A S, Q W, shift, enter), as in
 * go-link-console's test pattern, and the frames per second */
static void draw_pad(Surf *s) {
    CardBox b = card.pad;
    b.x += card_x;
    b.y += card_y;
    int u = b.h / 8 > 4 ? b.h / 8 : 4;
#define AT(fx, fy, X, Y) int X = b.x + (int)((fx) * b.w), Y = b.y + (int)((fy) * b.h)
#define LAMP(k) (down[k] ? C_LAMP_ON : C_LAMP_OFF)
    AT(0.09, 0.08, l2x, l2y); d_fill(s, l2x - u, l2y, 2 * u, u, LAMP(KEY_Q)); d_text_c(s, l2x, l2y + u / 2, 1, C_TEXT, "ZL");
    AT(0.91, 0.08, r2x, r2y); d_fill(s, r2x - u, r2y, 2 * u, u, LAMP(KEY_W)); d_text_c(s, r2x, r2y + u / 2, 1, C_TEXT, "ZR");
    AT(0.17, 0.5, dx, dy);
    int arm = u + u / 3;
    d_fill(s, dx - arm / 2, dy - arm / 2, arm, arm, TB_RGB(120, 120, 120));
    d_fill(s, dx - arm / 2, dy - arm * 3 / 2, arm, arm, LAMP(KEY_UP));
    d_fill(s, dx - arm / 2, dy + arm / 2, arm, arm, LAMP(KEY_DOWN));
    d_fill(s, dx - arm * 3 / 2, dy - arm / 2, arm, arm, LAMP(KEY_LEFT));
    d_fill(s, dx + arm / 2, dy - arm / 2, arm, arm, LAMP(KEY_RIGHT));
    AT(0.83, 0.5, fx, fy);
    int fr = u * 2 / 3 + 1, off = u + u / 2;
    d_circle(s, fx, fy - off, fr, LAMP(KEY_S)); d_text_c(s, fx + 1, fy - off, 1, C_TEXT, "X");
    d_circle(s, fx - off, fy, fr, LAMP(KEY_A)); d_text_c(s, fx - off + 1, fy, 1, C_TEXT, "Y");
    d_circle(s, fx + off, fy, fr, LAMP(KEY_X)); d_text_c(s, fx + off + 1, fy, 1, C_TEXT, "A");
    d_circle(s, fx, fy + off, fr, LAMP(KEY_Z)); d_text_c(s, fx + 1, fy + off, 1, C_TEXT, "B");
    AT(0.40, 0.36, mx, my); d_circle(s, mx, my, u / 2 + 1, LAMP(KEY_RIGHTSHIFT)); d_text_c(s, mx + 1, my, 1, C_TEXT, "-");
    AT(0.60, 0.36, px, py); d_circle(s, px, py, u / 2 + 1, LAMP(KEY_ENTER)); d_text_c(s, px + 1, py, 1, C_TEXT, "+");
    char t[48];
    snprintf(t, sizeof t, "%.1f FPS", bench_frames_fps(&ft));
    AT(0.5, 0.82, tx, ty);
    d_text_c(s, tx, ty, u / 6 > 1 ? u / 6 : 1, C_GOOD, t);
#undef AT
#undef LAMP
}

static void draw(Buf *b) {
    Surf s = { b->px, b->w, b->h, b->w };
    d_fill(&s, 0, 0, b->w, b->h, C_BG);
    /* the card: 4:3 in the area left of the panel and above the keyboard */
    int aw = b->w - panel_w(), ah = b->h - kbd_h();
    int cw = aw, ch = cw * 3 / 4;
    if (ch > ah) { ch = ah; cw = ch * 4 / 3; }
    if (card.w != (cw & ~1) || card.h != (ch & ~1)) card_build(&card, cw, ch);
    card_x = (aw - card.w) / 2;
    card_y = (ah - card.h) / 2;
    for (int y = 0; y < card.h; y++) memcpy(b->px + (size_t)(card_y + y) * b->w + card_x, card.px + (size_t)y * card.w, (size_t)card.w * 4);
    /* clock and the per-frame marker in the top box */
    CardBox c = card.clock;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M:%S", &tm);
    int sc = c.h / 10 > 1 ? c.h / 10 : 1;
    d_text_c(&s, card_x + c.x + c.w / 2, card_y + c.y + c.h / 2 - sc, sc, TB_RGB(235, 235, 235), clk);
    int steps = 60, cell = c.w / steps;
    d_fill(&s, card_x + c.x + (int)(ft.frames % steps) * cell, card_y + c.y + c.h - 4, cell > 2 ? cell - 1 : 2, 3, C_LAMP_ON);
    draw_pad(&s);
    if (video_fps) {                     /* the generated video: a square that steps once per content frame */
        long idx = (long)(ft.last * video_fps / 1000.0);
        int track = card.w - 40, pos = (int)(idx % 60) * track / 60;
        d_fill(&s, card_x + 20, card_y + card.h - 26, track, 14, TB_RGB(20, 20, 20));
        d_fill(&s, card_x + 20 + pos, card_y + card.h - 26, track / 60 + 2, 14, TB_RGB(255, 255, 255));
        char vt[32];
        snprintf(vt, sizeof vt, "%d FPS", video_fps);
        d_text(&s, card_x + 24, card_y + card.h - 40, 1, TB_RGB(255, 255, 255), vt);
    }
    draw_panel(&s, b->w - panel_w());
    draw_keyboard(&s, b->h - kbd_h());
    /* the crosshair: the exact pointer position, on top of everything */
    if (ptr_in) {
        int px = (int)ptr_x, py = (int)ptr_y;
        d_fill(&s, px, 0, 1, b->h, C_CROSS);
        d_fill(&s, 0, py, b->w, 1, C_CROSS);
        d_frame(&s, px - 6, py - 6, 13, 13, 1, C_CROSS);
    }
}

/* ---- frames and presentation feedback ---- */
static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                         uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)d;
    bench_present(&pr, (((uint64_t)sec_hi << 32) | sec_lo) * 1e3 + nsec / 1e6, ((uint64_t)seq_hi << 32) | seq_lo, refresh,
                  !!(flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY));
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; pr.discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_listener = { fb_sync, fb_presented, fb_discarded };

/* the frame that shows an input: its presentation time minus the event's time */
static void lat_presented(void *d, struct wp_presentation_feedback *f, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                          uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)refresh; (void)seq_hi; (void)seq_lo; (void)flags;
    uint32_t ev = (uint32_t)(uintptr_t)d;
    uint64_t t = ((((uint64_t)sec_hi << 32) | sec_lo) * 1000ull) + nsec / 1000000u;
    double l = (double)(uint32_t)((uint32_t)t - ev);
    bench_latency_add(&lat, clock_mono ? l : -1);
    wp_presentation_feedback_destroy(f);
}
static void lat_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; lat.lost++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener lat_listener = { fb_sync, lat_presented, lat_discarded };
static void g_presented_cb(void *d, struct wp_presentation_feedback *f, uint32_t a, uint32_t b, uint32_t c, uint32_t r, uint32_t e, uint32_t g, uint32_t flags) {
    (void)d; (void)a; (void)b; (void)c; (void)r; (void)e; (void)g;
    g_presented++;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) g_zero++;
    wp_presentation_feedback_destroy(f);
}
static void g_discarded_cb(void *d, struct wp_presentation_feedback *f) { (void)d; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener g_listener = { fb_sync, g_presented_cb, g_discarded_cb };

static void redraw(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; wl_callback_destroy(cb); frame_pending = 0; if (!paused) redraw(); }
static const struct wl_callback_listener frame_listener = { frame_done };

static void redraw(void) {
    if (!configured || closed || frame_pending) return;
    Buf *b = NULL;
    for (int i = 0; i < 2; i++) if (!bufs[i].busy && bufs[i].b) { b = &bufs[i]; break; }
    if (b && (b->w != win_w || b->h != win_h)) { if (buf_make(b, win_w, win_h)) return; }
    if (!b) {
        for (int i = 0; i < 2; i++) if (!bufs[i].b) { if (buf_make(&bufs[i], win_w, win_h)) return; b = &bufs[i]; break; }
        if (!b) { no_buffer++; }
    }
    double t = now_ms();
    if (b) {
        bench_frames_add(&ft, t);
        draw(b);
    }
    if (video_fps) {                     /* how many refreshes each content frame stays */
        long idx = (long)(t * video_fps / 1000.0);
        if (idx != video_last) { if (video_last >= 0 && video_run) video_hold[video_run > 5 ? 4 : video_run - 1]++; video_last = idx; video_run = 1; }
        else video_run++;
    }
    if (gl3d_on()) {
        gl3d_place(card_x, card_y, card.w, card.h);
        if (presentation) wp_presentation_feedback_add_listener(wp_presentation_feedback(presentation, gl3d_surface()), &g_listener, NULL);
        gl3d_frame(t);
    }
    if (lat_pending && b && presentation) {
        wp_presentation_feedback_add_listener(wp_presentation_feedback(presentation, surf), &lat_listener, (void *)(uintptr_t)lat_ev);
        lat_pending = 0;
    }
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    frame_pending = 1;
    if (presentation) wp_presentation_feedback_add_listener(wp_presentation_feedback(presentation, surf), &fb_listener, NULL);
    if (b) {
        wl_surface_attach(surf, b->b, 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, b->w, b->h);
        b->busy = 1;
    }
    wl_surface_commit(surf);
}

/* ---- xdg ---- */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { (void)d; xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };
static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(x, serial);
    int first = !configured;
    configured = 1;
    if (first || !frame_pending) { frame_pending = 0; redraw(); }
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)d; (void)t;
    toplevel_states = 0;
    uint32_t *st;
    wl_array_for_each(st, states) if (*st < 32) toplevel_states |= 1u << *st;
    if (w >= 640 && h >= 480) { win_w = w; win_h = h; }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

/* ---- keyboard ---- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d; (void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(xkb, m, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(m, size);
    if (!km) return;
    if (kstate) xkb_state_unref(kstate);
    if (keymap) xkb_keymap_unref(keymap);
    keymap = km;
    kstate = xkb_state_new(km);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d; (void)k; (void)sf; (void)keys; kb_focus = 1; kb_serial = s; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
    kb_focus = 0;
    memset(down, 0, sizeof down);                /* keys still down when focus left aren't down for us */
}
static void toggle_lock(void);
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k;
    kb_serial = serial;
    if (key > KEY_MAX) return;
    int pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    down[key] = (unsigned char)pressed;
    if (pressed && key == KEY_F5) rerun_checks = 1;
    if (pressed && key == KEY_PAUSE) { paused = !paused; if (!paused && !frame_pending) redraw(); }
    if (pressed) { lat_ev = t; lat_pending = 1; }
    if (pressed && key == KEY_G) { if (gl3d_on()) gl3d_stop(); else if (subcomp) gl3d_start(dpy, comp, subcomp, surf, card_x, card_y, card.w, card.h); }
    if (pressed && key == KEY_V) { video_fps = video_fps == 0 ? 24 : video_fps == 24 ? 30 : video_fps == 30 ? 60 : 0; memset(video_hold, 0, sizeof video_hold); video_last = -1; video_run = 0; }
    int ctrl = kstate && xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    if (tb_lock_key(key, pressed, ctrl, locked != NULL, ptr_in)) toggle_lock();
    if (pressed && !seen[key]) {
        seen[key] = 1;
        if (on_map((int)key)) nseen++;
        else {                                   /* a key the drawn keyboard doesn't have: list its code */
            size_t o = strlen(other_keys);
            if (o + 8 < sizeof other_keys) snprintf(other_keys + o, sizeof other_keys - o, "%s%u", o ? " " : "", key);
        }
    }
    if (pressed) {
        char sym[64] = "?", utf[16] = "";
        if (kstate) {
            xkb_keysym_t ks = xkb_state_key_get_one_sym(kstate, key + 8);
            xkb_keysym_get_name(ks, sym, sizeof sym);
            xkb_state_key_get_utf8(kstate, key + 8, utf, sizeof utf);
            for (char *q = utf; *q; q++) if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7e) { utf[0] = 0; break; }
        }
        snprintf(last_key, sizeof last_key, "%u  %.40s%s%s", key, sym, utf[0] ? "  " : "", utf);
    }
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)d; (void)k; (void)s;
    if (!kstate) return;
    xkb_state_update_mask(kstate, dep, lat, lock, 0, 0, group);
    static const char *names[] = { XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_NUM };
    static const char *label[] = { "SHIFT", "CTRL", "ALT", "SUPER", "CAPS", "NUM" };
    mods[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (xkb_state_mod_name_is_active(kstate, names[i], XKB_STATE_MODS_EFFECTIVE) > 0) {
            size_t o = strlen(mods);
            snprintf(mods + o, sizeof mods - o, "%s%s", o ? "+" : "", label[i]);
        }
    if (!mods[0]) snprintf(mods, sizeof mods, "-");
    char g[24];
    snprintf(g, sizeof g, " LAYOUT %u", group);
    size_t o = strlen(mods);
    snprintf(mods + o, sizeof mods - o, "%s", g);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; repeat_rate = rate; repeat_delay = delay; }
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

/* ---- pointer ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)sf;
    ptr_serial = s;
    ptr_in = 1;
    ptr_x = wl_fixed_to_double(x);
    ptr_y = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; ptr_in = 0; memset(btn_down, 0, sizeof btn_down); }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y); }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t button, uint32_t state) {
    (void)d; (void)p;
    ptr_serial = s;
    int i = (int)button - BTN_LEFT;              /* BTN_LEFT, RIGHT, MIDDLE, SIDE, EXTRA, FORWARD, BACK, TASK */
    if (i < 0 || i >= 8) return;
    btn_down[i] = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (btn_down[i]) { btn_seen[i] = 1; lat_ev = t; lat_pending = 1; }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d; (void)p; (void)t;
    long n = wl_fixed_to_int(v);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) wheel_v += n; else wheel_h += n;
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_source(void *d, struct wl_pointer *p, uint32_t src) {
    (void)d; (void)p;
    static const char *n[] = { "WHEEL", "FINGER", "CONTINUOUS", "WHEEL TILT" };
    snprintf(axis_src, sizeof axis_src, "%s", src < 4 ? n[src] : "?");
}
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis) { (void)d; (void)p; (void)t; (void)axis; }
static void p_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t steps) { (void)d; (void)p; (void)axis; (void)steps; }
static void p_value120(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) {
    (void)d; (void)p;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) wheel120_v += v; else wheel120_h += v;
}
static void p_reldir(void *d, struct wl_pointer *p, uint32_t axis, uint32_t dir) { (void)d; (void)p; (void)axis; (void)dir; }
static const struct wl_pointer_listener p_listener = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete, p_value120, p_reldir };

/* pointer lock (Ctrl+L, lockkey.h): the pointer stays put and only relative motion arrives (games, 3D viewers) */
static void lk_locked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; lock_on = 1; }
static void lk_unlocked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; lock_on = 0; }
static const struct zwp_locked_pointer_v1_listener lk_l = { lk_locked, lk_unlocked };
static void toggle_lock(void) {
    if (locked) { zwp_locked_pointer_v1_destroy(locked); locked = NULL; lock_on = 0; return; }
    if (!constraints || !pointer) return;
    locked = zwp_pointer_constraints_v1_lock_pointer(constraints, surf, pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    lock_t = now_ms();
    zwp_locked_pointer_v1_add_listener(locked, &lk_l, NULL);
}

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) { pointer = wl_seat_get_pointer(s); wl_pointer_add_listener(pointer, &p_listener, NULL); }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) { keyboard = wl_seat_get_keyboard(s); wl_keyboard_add_listener(keyboard, &kb_listener, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---- globals ---- */
static uint32_t vmin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    (void)d;
    if (nglobals < (int)(sizeof globals / sizeof globals[0])) {
        Global *g = &globals[nglobals++];
        g->name = name;
        g->version = version;
        snprintf(g->iface, sizeof g->iface, "%s", iface);
    }
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, vmin(version, 4));
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, vmin(version, 5)); xdg_wm_base_add_listener(wm, &wm_listener, NULL); }
    else if (!strcmp(iface, wl_seat_interface.name) && !seat) { seat = wl_registry_bind(r, name, &wl_seat_interface, vmin(version, 9)); wl_seat_add_listener(seat, &seat_listener, NULL); }
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) decoman = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) presentation = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) constraints = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void pres_clock(void *d, struct wp_presentation *p, uint32_t id) { (void)d; (void)p; clock_id = id; clock_mono = id == CLOCK_MONOTONIC; }
static const struct wp_presentation_listener pres_listener = { pres_clock };

/* ---- the --auto report ---- */
static void report(FILE *o) {
    BenchStats fs;
    bench_frames_stats(&ft, &fs);
    double hz = bench_refresh_hz(&pr) > 0 ? bench_refresh_hz(&pr) : out_refresh_mhz / 1000.0;
    fprintf(o, "{\"tool\":\"lordos-testbench\",\"v\":1,\"at\":%lld,\"window\":[%d,%d],\"frames\":%lu,\"fps\":%.2f,\"frameMsAvg\":%.3f,\"frameMsMax\":%.3f,"
               "\"frameMsP50\":%.3f,\"frameMsP99\":%.3f,\"missedBudget\":%d,"
               "\"noFreeBuffer\":%lu,\"presentation\":%s,\"presented\":%lu,\"discarded\":%lu,\"refreshHz\":%.3f,\"vblankSkips\":%lu,\"zeroCopy\":%lu,"
               "\"keyboard\":%s,\"pointer\":%s,\"repeat\":[%d,%d],\"globals\":[",
            (long long)time(NULL) * 1000, win_w, win_h, ft.frames, bench_frames_fps(&ft), fs.avg, fs.max,
            fs.p50, fs.p99, hz > 0 ? bench_frames_missed(&ft, 1000.0 / hz) : 0,
            no_buffer, presentation ? "true" : "false", pr.presented, pr.discarded, bench_refresh_hz(&pr), pr.vblank_skips, pr.zero_copy,
            keyboard ? "true" : "false", pointer ? "true" : "false", repeat_rate, repeat_delay);
    for (int i = 0; i < nglobals; i++) fprintf(o, "%s{\"interface\":\"%s\",\"version\":%u}", i ? "," : "", globals[i].iface, globals[i].version);
    static const char *st[4] = { "untested", "pass", "fail", "info" };
    fprintf(o, "],\"outputRefreshHz\":%.3f,\"latency\":{\"n\":%lu,\"avgMs\":%.2f,\"notPresented\":%lu},", out_refresh_mhz / 1000.0, lat.n, bench_latency_avg(&lat), lat.lost);
    fprintf(o, "\"gl\":{\"renderer\":");
    bench_json_str(o, gl3d.renderer);
    fprintf(o, ",\"vendor\":");
    bench_json_str(o, gl3d.vendor);
    fprintf(o, ",\"version\":");
    bench_json_str(o, gl3d.version);
    fprintf(o, ",\"kind\":\"%s\",\"error\":\"%s\",\"drawMs\":%.3f,\"frames\":%lu,\"presented\":%lu,\"zeroCopy\":%lu},\"checks\":[",
            gl3d.kind, gl3d.error, gl3d.gpu_ms, gl3d.frames, g_presented, g_zero);
    for (int i = 0; i < nchecks; i++) {
        fprintf(o, "%s{\"id\":\"%s\",\"state\":\"%s\",\"detail\":", i ? "," : "", checks[i].id, st[checks[i].state & 3]);
        bench_json_str(o, checks[i].detail);
        fprintf(o, "}");
    }
    fprintf(o, "]}\n");
}

/* The last report, for System Information's Checks: ~/.local/state/lordos/testbench.json (bench_save). */
static void save_report(void) { bench_save("testbench", report); }

int main(int argc, char **argv) {
    double auto_s = 0;
    int auto_gl = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gl")) auto_gl = 1;
        else if (!strcmp(argv[i], "--auto")) {
            auto_s = 5;
            if (i + 1 < argc && atof(argv[i + 1]) > 0) auto_s = atof(argv[++i]);
            if (auto_s > 600) auto_s = 600;
        } else {
            fprintf(stderr, "usage: lordos-testbench [--gl] [--auto [seconds]]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);           /* a clipboard reader that hangs up must not end the bench (toolkits do the same) */
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "lordos-testbench: no Wayland compositor (WAYLAND_DISPLAY)\n"); return 1; }
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "lordos-testbench: the compositor lacks wl_compositor, wl_shm or xdg_wm_base\n"); return 1; }
    if (presentation) wp_presentation_add_listener(presentation, &pres_listener, NULL);
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_listener, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, "Test Bench");
    xdg_toplevel_set_app_id(top, "lordos-testbench");
    xdg_toplevel_set_min_size(top, 640, 480);
    if (decoman) {
        struct zxdg_toplevel_decoration_v1 *deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decoman, top);
        zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(surf);
    double start = now_ms();
    int ran = 0;
    while (!closed && wl_display_dispatch(dpy) != -1) {
        if (tb_lock_over(locked != NULL, lock_t, now_ms())) toggle_lock();
        if ((!ran && ft.frames > 2) || rerun_checks) {
            ran = 1;
            rerun_checks = 0;
            if (checks_run() < 0) { save_report(); break; }   /* the compositor ended the connection */
            save_report();
            if (auto_gl && subcomp && !gl3d_on()) gl3d_start(dpy, comp, subcomp, surf, card_x, card_y, card.w, card.h);
        }
        if (auto_s > 0 && ft.frames && now_ms() - start > auto_s * 1000) break;
    }
    if (wl_display_get_error(dpy) && !checks_done) fprintf(stderr, "lordos-testbench: the connection ended (error %d)\n", wl_display_get_error(dpy));
    if (auto_s > 0) report(stdout);
    if (ft.frames) save_report();
    for (int i = 0; i < 2; i++) buf_free(&bufs[i]);
    card_free(&card);
    wl_display_disconnect(dpy);
    return 0;
}
