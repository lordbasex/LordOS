/* web_backend_test.c — lord-web's view backend alone (src/web/webback.h), headless, on whichever backend mk/web.mk
 * built (legacy: libwpe + wpebackend-fdo; platform: WPEPlatform). Run from server/:
 *
 *   build/web-backend-test
 *
 * With a throwaway HOME (nothing of the person's is read or written) and WebKit's own sandbox (bubblewrap,
 * xdg-dbus-proxy), no compositor and no GPU (the shared memory path, LordOS's CPU path):
 *   - a local page renders: the frame's size is the view's and its pixels are the page's background;
 *   - a click and a key reach the page's own listeners (mousedown with the right button and position, keydown with
 *     the key and code of the keymap), and the page's new colour reaches the next frame;
 *   - the wheel reaches it (a notch down is deltaY > 0);
 *   - a resize and a scale reach the page (innerWidth, innerHeight, devicePixelRatio) and the frame (buffer pixels);
 *     a click at scale 2 lands at the same CSS position;
 *   - hiding the view makes the page hidden (visibilitychange);
 *   - on WPEPlatform, the page's cursor reaches the sink by its CSS name (cursor: pointer, then default).
 * Prints "ok <backend>" and exits 0, or the first failure and 1. */
#include "../src/web/webback.h"

#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WebBack *back;
static WebKitWebView *view;
static int frame_w, frame_h, frames;
static unsigned char *frame_px;         /* the last frame, BGRA rows of frame_w * 4 bytes */
static int failed;

static gboolean done_later(gpointer d) { (void)d; webback_frame_done(back); return G_SOURCE_REMOVE; }

static void on_frame(void *d, const WebFrame *f) {
    (void)d;
    if (f->kind == WEBFRAME_SHM) {
        free(frame_px);
        frame_px = malloc((size_t)f->w * 4 * (size_t)f->h);
        for (int y = 0; y < f->h; y++) memcpy(frame_px + (size_t)y * (size_t)f->w * 4, (const unsigned char *)f->pixels + (size_t)y * (size_t)f->stride, (size_t)f->w * 4);
        frame_w = f->w;
        frame_h = f->h;
        frames++;
    } else webback_release(back, f->key);
    g_idle_add(done_later, NULL);
}
static char cursor[32];                 /* the last cursor name the backend gave (WPEPlatform's only) */
static void on_cursor(void *d, const char *name) { (void)d; snprintf(cursor, sizeof cursor, "%s", name ? name : ""); }
static int cursor_is(const void *arg) { return !strcmp(cursor, arg); }
static const WebBackSink sink = { on_frame, on_cursor };

static gboolean timed_out;
static gboolean on_timeout(gpointer d) { (void)d; timed_out = TRUE; return G_SOURCE_REMOVE; }

typedef int (*Cond)(const void *arg);
/* runs the main loop until cond holds, at most secs seconds */
static int wait_for(Cond cond, const void *arg, int secs) {
    timed_out = FALSE;
    guint t = g_timeout_add_seconds((guint)secs, on_timeout, NULL);
    while (!cond(arg) && !timed_out) g_main_context_iteration(NULL, TRUE);
    if (!timed_out) g_source_remove(t);
    return cond(arg);
}

static int title_is(const void *arg) {
    const char *t = webkit_web_view_get_title(view);
    return t && !strcmp(t, arg);
}
static int title_starts(const void *arg) {
    const char *t = webkit_web_view_get_title(view);
    return t && !strncmp(t, arg, strlen(arg));
}

typedef struct { int w, h; unsigned rgb; } Want;
/* a frame of that size whose middle pixel is that colour (within 2 a channel: no colour management is expected) */
static int frame_is(const void *arg) {
    const Want *x = arg;
    if (!frame_px || frame_w != x->w || frame_h != x->h) return 0;
    const unsigned char *p = frame_px + ((size_t)(frame_h / 2) * (size_t)frame_w + (size_t)(frame_w / 2)) * 4;
    int r = (int)(x->rgb >> 16 & 255), g = (int)(x->rgb >> 8 & 255), b = (int)(x->rgb & 255);
    return abs(p[2] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[0] - b) <= 2;
}

static void check(int ok, const char *what) {
    if (ok) return;
    const char *t = webkit_web_view_get_title(view);
    unsigned px = 0;
    if (frame_px) {
        const unsigned char *p = frame_px + ((size_t)(frame_h / 2) * (size_t)frame_w + (size_t)(frame_w / 2)) * 4;
        px = (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0];
    }
    fprintf(stderr, "FAILED: %s (title \"%s\", last frame %dx%d, middle #%06x, %d frames)\n", what, t ? t : "", frame_w, frame_h, px, frames);
    failed = 1;
}

static int load_done;
static void on_load(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) { (void)v; (void)d; if (ev == WEBKIT_LOAD_FINISHED) load_done = 1; }
static int loaded(const void *arg) { (void)arg; return load_done; }

static const char PAGE[] =
    "<!doctype html><html><head><title>ready</title><style>html,body{margin:0;height:100%;background:#3366cc}</style></head><body>"
    "<div style='position:fixed;left:0;top:0;width:60px;height:60px;cursor:pointer'></div>"
    "<script>"
    "document.addEventListener('keydown', e => { document.body.style.background = '#00cc44'; document.title = 'key:' + e.key + ':' + e.code; });"
    "document.addEventListener('mousedown', e => { document.body.style.background = '#cc2200'; document.title = 'click:' + e.button + ':' + e.clientX + ',' + e.clientY; });"
    "document.addEventListener('wheel', e => { document.title = 'wheel:' + (e.deltaY > 0 ? 'down' : e.deltaY < 0 ? 'up' : 'none'); });"
    "window.addEventListener('resize', () => { document.title = 'size:' + innerWidth + 'x' + innerHeight + '@' + devicePixelRatio; });"
    "matchMedia('(resolution: 2dppx)').addEventListener('change', () => { document.title = 'size:' + innerWidth + 'x' + innerHeight + '@' + devicePixelRatio; });"
    "document.addEventListener('visibilitychange', () => { document.title = 'visibility:' + document.visibilityState; });"
    "</script></body></html>";

static char *us_keymap(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .layout = "us" };
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *s = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (km) xkb_keymap_unref(km);
    if (ctx) xkb_context_unref(ctx);
    return s;
}

static uint32_t now_ms(void) { return (uint32_t)(g_get_monotonic_time() / 1000); }

static void click(double x, double y) {
    webback_motion(back, now_ms(), x, y, 0);
    webback_button(back, now_ms(), x, y, WEBBTN_LEFT, 1, WEBMOD_BTN_LEFT);
    webback_button(back, now_ms(), x, y, WEBBTN_LEFT, 0, 0);
}

int main(void) {
    char home[] = "/tmp/lord-web-backend-test-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    char sub[sizeof home + 32];
    snprintf(sub, sizeof sub, "%s/cache", home);
    setenv("XDG_CACHE_HOME", sub, 1);
    snprintf(sub, sizeof sub, "%s/data", home);
    setenv("XDG_DATA_HOME", sub, 1);
    snprintf(sub, sizeof sub, "%s/config", home);
    setenv("XDG_CONFIG_HOME", sub, 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);

    webback_process_init();
    WebBackConfig cfg = { .width = 640, .height = 480, .scale = 1 };
    back = webback_new(&cfg, &sink, NULL);
    if (!back) { fprintf(stderr, "FAILED: the %s backend did not start\n", webback_name()); return 1; }
    WebKitWebContext *ctx = webkit_web_context_new();
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    WebKitSettings *settings = webkit_settings_new();
    view = webback_view_new(back, ctx, session, settings);
    g_object_unref(settings);
    if (!view) { fprintf(stderr, "FAILED: no view\n"); return 1; }
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    webback_set_visible(back, 1);
    webback_set_focused(back, 1);
    char *km = us_keymap();
    if (!km) { fprintf(stderr, "FAILED: no us keymap (xkeyboard-config)\n"); return 1; }
    webback_keymap(back, km, strlen(km));
    free(km);
    webback_modifiers(back, 0, 0, 0, 0);
    webkit_web_view_load_html(view, PAGE, "http://127.0.0.1/");

    check(wait_for(loaded, NULL, 30), "the page loads");
    Want blue = { 640, 480, 0x3366cc };
    check(wait_for(frame_is, &blue, 15), "a 640x480 frame in the page's blue");

    if (!strcmp(webback_name(), "platform")) {
        webback_motion(back, now_ms(), 20, 20, 0);
        check(wait_for(cursor_is, "pointer", 10), "the page's cursor: pointer reaches the sink as \"pointer\"");
        webback_motion(back, now_ms(), 200, 200, 0);
        check(wait_for(cursor_is, "default", 10), "and \"default\" outside it");
    }
    click(100, 50);
    check(wait_for(title_is, "click:0:100,50", 10), "a click at 100,50 reaches mousedown");
    Want red = { 640, 480, 0xcc2200 };
    check(wait_for(frame_is, &red, 10), "the click's colour reaches the next frame");

    webback_key(back, now_ms(), KEY_A, 1, 0);
    webback_key(back, now_ms(), KEY_A, 0, 0);
    check(wait_for(title_is, "key:a:KeyA", 10), "the key A reaches keydown as a and KeyA");
    Want green = { 640, 480, 0x00cc44 };
    check(wait_for(frame_is, &green, 10), "the key's colour reaches the next frame");

    uint32_t mods = webback_modifiers(back, 1, 0, 0, 0);      /* Shift held, in the us keymap's first modifier */
    webback_key(back, now_ms(), KEY_B, 1, mods);
    webback_key(back, now_ms(), KEY_B, 0, mods);
    webback_modifiers(back, 0, 0, 0, 0);
    check(mods == WEBMOD_SHIFT, "Shift is WEBMOD_SHIFT");
    check(wait_for(title_is, "key:B:KeyB", 10), "Shift+B reaches keydown as B");

    WebAxisOut down = { WEBAXIS_OUT_LINES, 0, -1 };
    webback_axis(back, now_ms(), 200, 200, &down, 1, 0);
    check(wait_for(title_is, "wheel:down", 10), "a wheel notch down reaches the page as deltaY > 0");

    webback_set_size(back, 500, 400);
    check(wait_for(title_is, "size:500x400@1", 10), "a resize to 500x400 reaches the page");
    Want small = { 500, 400, 0x00cc44 };
    check(wait_for(frame_is, &small, 10), "a 500x400 frame");

    webback_set_scale(back, 2);
    check(wait_for(title_is, "size:500x400@2", 10), "scale 2 reaches devicePixelRatio, the CSS size unchanged");
    Want big = { 1000, 800, 0x00cc44 };
    check(wait_for(frame_is, &big, 10), "a 1000x800 frame at scale 2");
    click(30, 40);
    check(wait_for(title_is, "click:0:30,40", 10), "a click at 30,40 at scale 2 lands at 30,40 in CSS pixels");

    webback_set_visible(back, 0);
    check(wait_for(title_starts, "visibility:hidden", 10), "a hidden view makes the page hidden");
    webback_set_visible(back, 1);
    check(wait_for(title_starts, "visibility:visible", 10), "shown again, the page is visible");

    g_object_unref(view);
    char cmd[sizeof home + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    if (failed) return 1;
    printf("ok %s (WPE WebKit %u.%u.%u)\n", webback_name(), webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    return 0;
}
