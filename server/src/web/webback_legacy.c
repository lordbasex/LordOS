/* webback_legacy.c — webback.h on WPE WebKit's legacy API: libwpe and wpebackend-fdo (Debian 13's WPE WebKit 2.48).
 *
 * WebKit's web process draws into wpebackend-fdo's nested compositor (a libwayland-server of its own inside lord-web,
 * on a socket pair WebKit hands the web process); the exportable view backend gives lord-web each frame:
 *   - with EGL up on lord-server's display (wpe_fdo_initialize_for_egl_display): the web process's linux-dmabuf
 *     buffers, as wl_resources of that nested compositor (webshm.c watches them);
 *   - without (LordOS's CPU path, wpe_fdo_initialize_shm): wl_shm buffers, read from webshm.c.
 * Input goes to libwpe's events with libwpe's xkb context (the compositor's keymap); coordinates are the buffer's
 * (surface times the scale). The pasteboard is libwpe's singleton. */
#include "webback.h"
#include "webshm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/wpe.h>

#define MAX_TOUCH 10

struct WebBack {
    const WebBackSink *sink;
    void *sd;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    double scale;
    uint32_t activity;
    struct wpe_input_touch_event_raw touches[MAX_TOUCH];
};

const char *webback_name(void) { return "legacy"; }

void webback_process_init(void) { wpe_loader_init("libWPEBackend-fdo-1.0.so.1"); }

/* ---- frames ---- */

static void export_shm(void *d, struct wpe_fdo_shm_exported_buffer *buf) {
    WebBack *b = d;
    WebFrame f = { .kind = WEBFRAME_SHM };
    int ok = !webshm_begin(buf, &f.w, &f.h, &f.stride, &f.format, &f.pixels);
    if (ok) b->sink->frame(b->sd, &f);
    webshm_end(buf, ok);
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(b->exportable, buf);
    if (!ok) webback_frame_done(b);
}

static void export_dmabuf(void *d, struct wpe_view_backend_exportable_fdo_dmabuf_resource *r) {
    WebBack *b = d;
    WebFrame f = { .kind = WEBFRAME_DMABUF, .w = (int)r->width, .h = (int)r->height, .key = r->buffer_resource, .fourcc = r->format };
    f.n_planes = r->n_planes < 4 ? (int)r->n_planes : 4;
    for (int i = 0; i < f.n_planes; i++) {
        f.fds[i] = r->fds[i];
        f.offsets[i] = r->offsets[i];
        f.strides[i] = r->strides[i];
    }
    f.modifier = r->modifiers[0];
    b->sink->frame(b->sd, &f);
}

static void export_resource(void *d, struct wl_resource *res) {
    WebBack *b = d;
    WebFrame f = { .kind = WEBFRAME_OTHER, .key = res };
    b->sink->frame(b->sd, &f);
}

static const struct wpe_view_backend_exportable_fdo_client export_client = { export_resource, export_dmabuf, export_shm, NULL, NULL };

void webback_frame_done(WebBack *b) { if (b && b->exportable) wpe_view_backend_exportable_fdo_dispatch_frame_complete(b->exportable); }
void webback_release(WebBack *b, void *key) { if (b && b->exportable && key) wpe_view_backend_exportable_fdo_dispatch_release_buffer(b->exportable, key); }
void webback_watch(WebBack *b, void *key, void (*gone)(void *data), void *data) { (void)b; webshm_on_destroy(key, gone, data); }

/* ---- start ---- */

WebBack *webback_new(const WebBackConfig *cfg, const WebBackSink *sink, void *sd) {
    if (cfg->gpu && cfg->egl_display && wpe_fdo_initialize_for_egl_display(cfg->egl_display)) {}
    else {
        if (cfg->gpu) return NULL;                  /* webwin.c starts again without the GPU */
        if (!wpe_fdo_initialize_shm()) { fprintf(stderr, "lord-web: wpebackend-fdo could not start\n"); return NULL; }
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);     /* the web process draws on the CPU into shared memory */
    }
    WebBack *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->sink = sink;
    b->sd = sd;
    b->scale = 1;
    b->exportable = wpe_view_backend_exportable_fdo_create(&export_client, b, (uint32_t)cfg->width, (uint32_t)cfg->height);
    if (!b->exportable) { free(b); return NULL; }
    b->backend = wpe_view_backend_exportable_fdo_get_view_backend(b->exportable);
    return b;
}

int webback_native_scheme(void) { return 0; }
void webback_set_dark(WebBack *b, int dark) { (void)b; (void)dark; }   /* 2.48: no setting; webscheme.js only */

WebKitWebView *webback_view_new(WebBack *b, WebKitWebContext *context, WebKitNetworkSession *session, WebKitSettings *settings) {
    WebKitWebViewBackend *backend = webkit_web_view_backend_new(b->backend, NULL, NULL);
    return g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "web-context", context, "network-session", session, "settings", settings, "website-policies", webback_policies(), NULL);
}

void webback_free(WebBack *b) { free(b); }

/* ---- the window ---- */

void webback_set_size(WebBack *b, int w, int h) { wpe_view_backend_dispatch_set_size(b->backend, (uint32_t)w, (uint32_t)h); }

void webback_set_scale(WebBack *b, double s) {
    b->scale = s;
    wpe_view_backend_dispatch_set_device_scale_factor(b->backend, (float)s);
}

void webback_set_visible(WebBack *b, int visible) {
    const uint32_t both = wpe_view_activity_state_visible | wpe_view_activity_state_in_window;
    uint32_t want = visible ? both : 0;
    if (want == b->activity) return;
    if (want) wpe_view_backend_add_activity_state(b->backend, both);
    else wpe_view_backend_remove_activity_state(b->backend, both);
    b->activity = want;
}

void webback_set_focused(WebBack *b, int focused) {
    if (focused) wpe_view_backend_add_activity_state(b->backend, wpe_view_activity_state_focused);
    else wpe_view_backend_remove_activity_state(b->backend, wpe_view_activity_state_focused);
}

void webback_set_title(WebBack *b, const char *title) { (void)b; (void)title; }
void webback_set_screen(WebBack *b, int w, int h, int refresh_mhz) { (void)b; (void)w; (void)h; (void)refresh_mhz; }

/* ---- the keyboard ---- */

static uint32_t to_wpe_mods(uint32_t m) {
    uint32_t r = 0;
    if (m & WEBMOD_CTRL) r |= wpe_input_keyboard_modifier_control;
    if (m & WEBMOD_SHIFT) r |= wpe_input_keyboard_modifier_shift;
    if (m & WEBMOD_ALT) r |= wpe_input_keyboard_modifier_alt;
    if (m & WEBMOD_META) r |= wpe_input_keyboard_modifier_meta;
    if (m & WEBMOD_BTN_LEFT) r |= wpe_input_pointer_modifier_button1;
    if (m & WEBMOD_BTN_RIGHT) r |= wpe_input_pointer_modifier_button2;
    if (m & WEBMOD_BTN_MIDDLE) r |= wpe_input_pointer_modifier_button3;
    return r;
}

void webback_keymap(WebBack *b, const char *map, size_t len) {
    (void)b;
    struct wpe_input_xkb_context *x = wpe_input_xkb_context_get_default();
    struct xkb_keymap *km = xkb_keymap_new_from_buffer(wpe_input_xkb_context_get_context(x), map, len, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) return;
    wpe_input_xkb_context_set_keymap(x, km);
    xkb_keymap_unref(km);
}

uint32_t webback_modifiers(WebBack *b, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)b;
    struct wpe_input_xkb_context *x = wpe_input_xkb_context_get_default();
    struct xkb_state *st = wpe_input_xkb_context_get_state(x);
    if (st) xkb_state_update_mask(st, dep, lat, lock, 0, 0, group);
    uint32_t m = wpe_input_xkb_context_get_modifiers(x, dep, lat, lock, group), r = 0;
    if (m & wpe_input_keyboard_modifier_control) r |= WEBMOD_CTRL;
    if (m & wpe_input_keyboard_modifier_shift) r |= WEBMOD_SHIFT;
    if (m & wpe_input_keyboard_modifier_alt) r |= WEBMOD_ALT;
    if (m & wpe_input_keyboard_modifier_meta) r |= WEBMOD_META;
    return r;
}

xkb_keysym_t webback_keysym(WebBack *b, uint32_t key) {
    (void)b;
    struct xkb_state *st = wpe_input_xkb_context_get_state(wpe_input_xkb_context_get_default());
    return st ? xkb_state_key_get_one_sym(st, key + 8) : 0;
}

int webback_key_repeats(WebBack *b, uint32_t key) {
    (void)b;
    struct xkb_keymap *km = wpe_input_xkb_context_get_keymap(wpe_input_xkb_context_get_default());
    return km && xkb_keymap_key_repeats(km, key + 8);
}

void webback_key(WebBack *b, uint32_t time, uint32_t key, int pressed, uint32_t mods) {
    struct wpe_input_xkb_context *x = wpe_input_xkb_context_get_default();
    uint32_t code = key + 8;
    struct wpe_input_keyboard_event e = { time, wpe_input_xkb_context_get_key_code(x, code, pressed), code, pressed != 0, to_wpe_mods(mods) };
    wpe_view_backend_dispatch_keyboard_event(b->backend, &e);
}

int webback_shortcut(WebBack *b, xkb_keysym_t keysym, int shift) {
    struct wpe_input_xkb_context *x = wpe_input_xkb_context_get_default();
    struct xkb_keymap *km = wpe_input_xkb_context_get_keymap(x);
    if (!km) return 0;
    xkb_keycode_t code = 0;
    for (xkb_keycode_t k = xkb_keymap_min_keycode(km); k <= xkb_keymap_max_keycode(km) && !code; k++) {
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(km, k, 0, 0, &syms);
        for (int i = 0; i < n; i++) if (syms[i] == keysym) code = k;
    }
    if (!code) return 0;
    uint32_t mods = wpe_input_keyboard_modifier_control | (shift ? wpe_input_keyboard_modifier_shift : 0);
    uint32_t t = (uint32_t)(g_get_monotonic_time() / 1000);
    for (int pressed = 1; pressed >= 0; pressed--) {
        struct wpe_input_keyboard_event e = { t, shift ? xkb_keysym_to_upper(keysym) : keysym, code, pressed != 0, mods };
        wpe_view_backend_dispatch_keyboard_event(b->backend, &e);
    }
    return 1;
}

/* ---- the pointer ---- */

/* A motion names the button held (left first, then right, then middle), as GTK's does: WebKit 2.48 turns a libwpe
 * motion's button into the mouse event's, and WebCore drags a selection (or anything else) only while a motion says
 * the left button is down; with 0 a drag over text selected nothing. The click count (double and triple clicks) is
 * WebKit's own, from the presses' times and places. */
static uint32_t motion_button(uint32_t mods) {
    if (mods & WEBMOD_BTN_LEFT) return 1;
    if (mods & WEBMOD_BTN_RIGHT) return 2;
    if (mods & WEBMOD_BTN_MIDDLE) return 3;
    return 0;
}

void webback_motion(WebBack *b, uint32_t time, double x, double y, uint32_t mods) {
    uint32_t btn = motion_button(mods);
    struct wpe_input_pointer_event e = { wpe_input_pointer_event_type_motion, time, (int)(x * b->scale), (int)(y * b->scale), btn, btn != 0, to_wpe_mods(mods) };
    wpe_view_backend_dispatch_pointer_event(b->backend, &e);
}

void webback_button(WebBack *b, uint32_t time, double x, double y, int button, int pressed, uint32_t mods) {
    struct wpe_input_pointer_event e = { wpe_input_pointer_event_type_button, time, (int)(x * b->scale), (int)(y * b->scale), (uint32_t)button, (uint32_t)(pressed != 0), to_wpe_mods(mods) };
    wpe_view_backend_dispatch_pointer_event(b->backend, &e);
}

static void send_axis(WebBack *b, enum wpe_input_axis_event_type type, uint32_t time, int px, int py, uint32_t axis, int32_t value, double x, double y, uint32_t mods) {
    if (type & wpe_input_axis_event_type_mask_2d) {
        struct wpe_input_axis_2d_event e = { { type, time, px, py, 0, 0, mods }, x, y };
        wpe_view_backend_dispatch_axis_event(b->backend, &e.base);
    } else {
        struct wpe_input_axis_event e = { type, time, px, py, axis, value, mods };
        wpe_view_backend_dispatch_axis_event(b->backend, &e);
    }
}

void webback_axis(WebBack *b, uint32_t time, double x, double y, const WebAxisOut *out, int n, uint32_t mods) {
    int px = (int)(x * b->scale), py = (int)(y * b->scale);
    uint32_t m = to_wpe_mods(mods);
    enum wpe_input_axis_event_type smooth = (enum wpe_input_axis_event_type)(wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
    for (int i = 0; i < n; i++) {
        if (out[i].kind == WEBAXIS_OUT_LINES) {
            /* one line event per notch, on its own axis only */
            for (int k = 0; k < (int)(out[i].y < 0 ? -out[i].y : out[i].y); k++) send_axis(b, wpe_input_axis_event_type_motion, time, px, py, WEBAXIS_V, out[i].y > 0 ? 1 : -1, 0, 0, m);
            for (int k = 0; k < (int)(out[i].x < 0 ? -out[i].x : out[i].x); k++) send_axis(b, wpe_input_axis_event_type_motion, time, px, py, WEBAXIS_H, out[i].x > 0 ? 1 : -1, 0, 0, m);
        } else if (out[i].kind == WEBAXIS_OUT_PIXELS) send_axis(b, smooth, time, px, py, 0, 0, out[i].x, out[i].y, m);
        else send_axis(b, smooth, time, px, py, 0, 0, 0, 0, m);
    }
}

/* ---- touch ---- */

static void touch_send(WebBack *b, enum wpe_input_touch_event_type type, int id, uint32_t time, uint32_t mods) {
    struct wpe_input_touch_event_raw pts[MAX_TOUCH];
    int n = 0;
    for (int i = 0; i < MAX_TOUCH; i++) if (b->touches[i].type != wpe_input_touch_event_type_null) pts[n++] = b->touches[i];
    struct wpe_input_touch_event e = { pts, (uint64_t)n, type, id, time, to_wpe_mods(mods) };
    wpe_view_backend_dispatch_touch_event(b->backend, &e);
}

void webback_touch(WebBack *b, int type, int id, uint32_t time, double x, double y, uint32_t mods) {
    if (type == WEBTOUCH_CANCEL) {
        for (int i = 0; i < MAX_TOUCH; i++) if (b->touches[i].type != wpe_input_touch_event_type_null) {
            b->touches[i].type = wpe_input_touch_event_type_up;
            touch_send(b, wpe_input_touch_event_type_up, i, b->touches[i].time, mods);
            b->touches[i].type = wpe_input_touch_event_type_null;
        }
        return;
    }
    if (id < 0 || id >= MAX_TOUCH) return;
    if (type == WEBTOUCH_DOWN) {
        b->touches[id] = (struct wpe_input_touch_event_raw){ wpe_input_touch_event_type_down, time, id, (int32_t)(x * b->scale), (int32_t)(y * b->scale) };
        touch_send(b, wpe_input_touch_event_type_down, id, time, mods);
        return;
    }
    if (b->touches[id].type == wpe_input_touch_event_type_null) return;
    if (type == WEBTOUCH_MOTION) {
        b->touches[id] = (struct wpe_input_touch_event_raw){ wpe_input_touch_event_type_motion, time, id, (int32_t)(x * b->scale), (int32_t)(y * b->scale) };
        touch_send(b, wpe_input_touch_event_type_motion, id, time, mods);
    } else {
        b->touches[id].type = wpe_input_touch_event_type_up;
        b->touches[id].time = time;
        touch_send(b, wpe_input_touch_event_type_up, id, time, mods);
        b->touches[id].type = wpe_input_touch_event_type_null;
    }
}

/* ---- the pasteboard ---- */

char *webback_pb_read(WebBack *b, const char *type) {
    (void)b;
    struct wpe_pasteboard_string s = { 0 };
    wpe_pasteboard_get_string(wpe_pasteboard_get_singleton(), type, &s);
    char *r = s.data && s.length ? g_strndup(s.data, (gsize)s.length) : NULL;
    wpe_pasteboard_string_free(&s);
    return r;
}

void webback_pb_write_text(WebBack *b, const char *text, size_t len) {
    (void)b;
    /* zeroed: libwpe's wpe_pasteboard_string_initialize leaves a string that already has data alone, so stack garbage
     * was written to the pasteboard (or read: a crash) and the session's text never reached the page */
    static const char utf8[] = "text/plain;charset=utf-8";
    struct wpe_pasteboard_string_pair pairs[2];
    memset(pairs, 0, sizeof pairs);
    wpe_pasteboard_string_initialize(&pairs[0].type, utf8, strlen(utf8));
    wpe_pasteboard_string_initialize(&pairs[0].string, text, len);
    wpe_pasteboard_string_initialize(&pairs[1].type, "text/plain", 10);
    wpe_pasteboard_string_initialize(&pairs[1].string, text, len);
    struct wpe_pasteboard_string_map map = { pairs, 2 };
    wpe_pasteboard_write(wpe_pasteboard_get_singleton(), &map);
    for (int i = 0; i < 2; i++) { wpe_pasteboard_string_free(&pairs[i].type); wpe_pasteboard_string_free(&pairs[i].string); }
}
