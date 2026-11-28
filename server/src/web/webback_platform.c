/* webback_platform.c — webback.h on WPEPlatform, WPE WebKit's new API (2.50 and later built with ENABLE_WPE_PLATFORM;
 * LordOS's own 2.54 backport for Debian 13 has it next to the legacy API).
 *
 * lord-web brings its own platform instead of WPEPlatform's Wayland one: wpe-platform-wayland would open a window of
 * its own on lord-server, and lord-web must keep its window (webwin.c: xdg-shell, the frame callbacks, the clipboard,
 * text-input-v3, drops, the suspended state) and hand lord-server its frames itself. So:
 *   - LordWebDisplay (a WPEDisplay): no connection of its own. Its DRM device is the compositor's GPU (the render node
 *     webwin.c read from EGL), its preferred buffer formats are the ones lord-server imports (linux-dmabuf), its
 *     keymap is WPEPlatform's xkb one fed with the compositor's keymap, its clipboard WPEPlatform's in-process one
 *     (webwin.c mirrors the session's clipboard into it, as it did into libwpe's pasteboard), and one screen. Without
 *     a GPU (cfg->gpu 0, LORD_GPU=0, LORD_WEB_SHM) it has no DRM device: WebKit's web process draws on the CPU into
 *     shared memory.
 *   - LordWebView (a WPEView): render_buffer hands each WPEBufferDMABuf or WPEBufferSHM to webwin.c (WebBackSink);
 *     buffer-rendered is emitted at webback_frame_done (lord-server's frame callback), buffer-released when
 *     lord-server releases the wl_buffer (dmabuf) or right after the copy (shared memory). The page's cursor names go
 *     to webwin.c (wp_cursor_shape).
 *   - LordWebToplevel (a WPEToplevel): one view; its size, scale and active state are the xdg_toplevel's. WebKit's own
 *     requests to resize, go fullscreen or maximize are refused (lord-server decides the window's size).
 *   - LordWebScreen (a WPEScreen): the output the window is on (its size and refresh rate when lord-server says them).
 * Input becomes WPEEvents on the view, in surface coordinates (WPEPlatform's are logical pixels, unlike libwpe's).
 * The input method stays webwin.c's WebKitInputMethodContext on text-input-v3, which replaces the one WebKit builds
 * on the display's WPEInputMethodContext (there is none here: WPEPlatform's "none" context). */
#include "webback.h"
#include "webframe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wpe/wpe-platform.h>

struct WebBack {
    const WebBackSink *sink;
    void *sd;
    WPEDisplay *display;
    WPEView *view;                      /* the WebKitWebView's (webkit_web_view_get_wpe_view), with a ref of ours */
    WPEToplevel *toplevel;
    WPEScreen *screen;
    WPEKeymap *keymap;
    WPEDRMDevice *drm;                  /* NULL: no GPU */
    WPEBufferFormats *formats;          /* NULL: WebKit's own choice (shared memory without a DRM device) */
    WPEBuffer *pending;                 /* the frame shown, until webback_frame_done */
    int width, height;
    double scale;
    double px, py;                      /* the pointer, for its move events' deltas */
    int touching[10];                   /* touch points down (WEBTOUCH ids 0..9) */
    double touch_x[10], touch_y[10];    /* and where each one was last: wl_touch.up and cancel say no place */
};

const char *webback_name(void) { return "platform"; }

void webback_process_init(void) {}

/* ---- the screen ---- */

#define LORD_TYPE_WEB_SCREEN (lord_web_screen_get_type())
G_DECLARE_FINAL_TYPE(LordWebScreen, lord_web_screen, LORD, WEB_SCREEN, WPEScreen)
struct _LordWebScreen { WPEScreen parent; };
G_DEFINE_TYPE(LordWebScreen, lord_web_screen, WPE_TYPE_SCREEN)
static void lord_web_screen_class_init(LordWebScreenClass *k) { (void)k; }
static void lord_web_screen_init(LordWebScreen *s) { (void)s; }

/* ---- the toplevel ---- */

#define LORD_TYPE_WEB_TOPLEVEL (lord_web_toplevel_get_type())
G_DECLARE_FINAL_TYPE(LordWebToplevel, lord_web_toplevel, LORD, WEB_TOPLEVEL, WPEToplevel)
struct _LordWebToplevel { WPEToplevel parent; WebBack *b; };
G_DEFINE_TYPE(LordWebToplevel, lord_web_toplevel, WPE_TYPE_TOPLEVEL)

static void top_set_title(WPEToplevel *t, const char *title) { (void)t; (void)title; }   /* webwin.c's xdg_toplevel has the page's title */
static WPEScreen *top_get_screen(WPEToplevel *t) { WebBack *b = LORD_WEB_TOPLEVEL(t)->b; return b ? b->screen : NULL; }
static gboolean top_resize(WPEToplevel *t, int w, int h) { (void)t; (void)w; (void)h; return FALSE; }
static gboolean top_fullscreen(WPEToplevel *t, gboolean on) { (void)t; (void)on; return FALSE; }
static gboolean top_maximized(WPEToplevel *t, gboolean on) { (void)t; (void)on; return FALSE; }
static gboolean top_minimized(WPEToplevel *t) { (void)t; return FALSE; }
static WPEBufferFormats *top_formats(WPEToplevel *t) {
    WebBack *b = LORD_WEB_TOPLEVEL(t)->b;
    return b && b->formats ? g_object_ref(b->formats) : NULL;
}

static void lord_web_toplevel_class_init(LordWebToplevelClass *k) {
    WPEToplevelClass *c = WPE_TOPLEVEL_CLASS(k);
    c->set_title = top_set_title;
    c->get_screen = top_get_screen;
    c->resize = top_resize;
    c->set_fullscreen = top_fullscreen;
    c->set_maximized = top_maximized;
    c->set_minimized = top_minimized;
    c->get_preferred_buffer_formats = top_formats;
}
static void lord_web_toplevel_init(LordWebToplevel *t) { (void)t; }

/* ---- the view ---- */

#define LORD_TYPE_WEB_VIEW (lord_web_view_get_type())
G_DECLARE_FINAL_TYPE(LordWebView, lord_web_view, LORD, WEB_VIEW, WPEView)
struct _LordWebView { WPEView parent; WebBack *b; };
G_DEFINE_TYPE(LordWebView, lord_web_view, WPE_TYPE_VIEW)

static gboolean view_render(WPEView *v, WPEBuffer *buf, const WPERectangle *damage, guint n_damage, GError **error) {
    (void)damage; (void)n_damage; (void)error;
    WebBack *b = LORD_WEB_VIEW(v)->b;
    if (!b) return FALSE;
    if (b->pending) g_object_unref(b->pending);
    b->pending = g_object_ref(buf);
    WebFrame f = { .w = wpe_buffer_get_width(buf), .h = wpe_buffer_get_height(buf), .key = buf };
    if (WPE_IS_BUFFER_DMA_BUF(buf)) {
        WPEBufferDMABuf *d = WPE_BUFFER_DMA_BUF(buf);
        f.kind = WEBFRAME_DMABUF;
        f.fourcc = wpe_buffer_dma_buf_get_format(d);
        f.modifier = wpe_buffer_dma_buf_get_modifier(d);
        guint32 n = wpe_buffer_dma_buf_get_n_planes(d);
        f.n_planes = n < 4 ? (int)n : 4;
        for (int i = 0; i < f.n_planes; i++) {
            f.fds[i] = wpe_buffer_dma_buf_get_fd(d, (guint)i);
            f.offsets[i] = wpe_buffer_dma_buf_get_offset(d, (guint)i);
            f.strides[i] = wpe_buffer_dma_buf_get_stride(d, (guint)i);
        }
        b->sink->frame(b->sd, &f);
        return TRUE;
    }
    if (WPE_IS_BUFFER_SHM(buf)) {
        WPEBufferSHM *s = WPE_BUFFER_SHM(buf);
        GBytes *bytes = wpe_buffer_shm_get_data(s);
        gsize len = 0;
        f.kind = WEBFRAME_SHM;
        f.pixels = bytes ? g_bytes_get_data(bytes, &len) : NULL;
        f.stride = (int)wpe_buffer_shm_get_stride(s);
        f.format = 0;                               /* WL_SHM_FORMAT_ARGB8888: WPE_PIXEL_FORMAT_ARGB8888 is the only one */
        if (f.pixels && webframe_shm_ok(f.w, f.h, f.stride, (size_t)len)) b->sink->frame(b->sd, &f);
        else webback_frame_done(b);
        return TRUE;
    }
    f.kind = WEBFRAME_OTHER;
    b->sink->frame(b->sd, &f);
    return TRUE;
}

static void view_cursor(WPEView *v, const char *name) {
    WebBack *b = LORD_WEB_VIEW(v)->b;
    if (b && b->sink->cursor) b->sink->cursor(b->sd, name);
}

static void lord_web_view_class_init(LordWebViewClass *k) {
    WPEViewClass *c = WPE_VIEW_CLASS(k);
    c->render_buffer = view_render;
    c->set_cursor_from_name = view_cursor;
}
static void lord_web_view_init(LordWebView *v) { (void)v; }

/* ---- the display ---- */

#define LORD_TYPE_WEB_DISPLAY (lord_web_display_get_type())
G_DECLARE_FINAL_TYPE(LordWebDisplay, lord_web_display, LORD, WEB_DISPLAY, WPEDisplay)
struct _LordWebDisplay { WPEDisplay parent; WebBack *b; };
G_DEFINE_TYPE(LordWebDisplay, lord_web_display, WPE_TYPE_DISPLAY)

static gboolean display_connect(WPEDisplay *d, GError **error) { (void)d; (void)error; return TRUE; }
static WPEView *display_create_view(WPEDisplay *d) {
    LordWebView *v = g_object_new(LORD_TYPE_WEB_VIEW, "display", d, NULL);
    v->b = LORD_WEB_DISPLAY(d)->b;
    return WPE_VIEW(v);
}
static WPEKeymap *display_keymap(WPEDisplay *d) { WebBack *b = LORD_WEB_DISPLAY(d)->b; return b && b->keymap ? g_object_ref(b->keymap) : NULL; }
static WPEBufferFormats *display_formats(WPEDisplay *d) { WebBack *b = LORD_WEB_DISPLAY(d)->b; return b && b->formats ? g_object_ref(b->formats) : NULL; }
static guint display_n_screens(WPEDisplay *d) { WebBack *b = LORD_WEB_DISPLAY(d)->b; return b && b->screen ? 1 : 0; }
static WPEScreen *display_screen(WPEDisplay *d, guint i) { WebBack *b = LORD_WEB_DISPLAY(d)->b; return b && i == 0 ? b->screen : NULL; }
static WPEDRMDevice *display_drm(WPEDisplay *d) { WebBack *b = LORD_WEB_DISPLAY(d)->b; return b ? b->drm : NULL; }

static void lord_web_display_class_init(LordWebDisplayClass *k) {
    WPEDisplayClass *c = WPE_DISPLAY_CLASS(k);
    c->connect = display_connect;
    c->create_view = display_create_view;
    c->get_keymap = display_keymap;
    c->get_preferred_buffer_formats = display_formats;
    c->get_n_screens = display_n_screens;
    c->get_screen = display_screen;
    c->get_drm_device = display_drm;
}
static void lord_web_display_init(LordWebDisplay *d) { (void)d; }

/* ---- start ---- */

/* lord-server's formats, ARGB and XRGB first (what WebKit draws), each with the modifiers lord-server listed */
static WPEBufferFormats *formats_new(const WebBackConfig *cfg, WPEDRMDevice *drm) {
    static const uint32_t want[] = { 0x34325241 /* AR24 */, 0x34325258 /* XR24 */, 0x34324241 /* AB24 */, 0x34324258 /* XB24 */ };
    WPEBufferFormatsBuilder *bld = wpe_buffer_formats_builder_new(drm);
    wpe_buffer_formats_builder_append_group(bld, NULL, WPE_BUFFER_FORMAT_USAGE_RENDERING);
    int any = 0;
    for (size_t f = 0; f < sizeof want / sizeof want[0]; f++)
        for (int i = 0; i < cfg->dmabuf_n; i++) if (cfg->dmabuf_formats[i] == want[f]) {
            wpe_buffer_formats_builder_append_format(bld, want[f], cfg->dmabuf_modifiers[i]);
            any = 1;
        }
    WPEBufferFormats *r = wpe_buffer_formats_builder_end(bld);
    wpe_buffer_formats_builder_unref(bld);
    if (!any && r) { g_object_unref(r); r = NULL; }
    return r;
}

WebBack *webback_new(const WebBackConfig *cfg, const WebBackSink *sink, void *sd) {
    if (cfg->gpu && (!cfg->render_node || !cfg->primary_node)) return NULL;     /* webwin.c starts again without the GPU */
    WebBack *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->sink = sink;
    b->sd = sd;
    b->width = cfg->width;
    b->height = cfg->height;
    b->scale = cfg->scale > 0 ? cfg->scale : 1;
    if (cfg->gpu) {
        b->drm = wpe_drm_device_new(cfg->primary_node, cfg->render_node);
        b->formats = cfg->dmabuf_n ? formats_new(cfg, b->drm) : NULL;
    } else setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);  /* the web process draws on the CPU into shared memory */
    b->keymap = wpe_keymap_xkb_new();
    LordWebDisplay *d = g_object_new(LORD_TYPE_WEB_DISPLAY, NULL);
    d->b = b;
    b->display = WPE_DISPLAY(d);
    GError *err = NULL;
    if (!wpe_display_connect(b->display, &err)) {
        fprintf(stderr, "lord-web: WPEPlatform display: %s\n", err ? err->message : "could not connect");
        g_clear_error(&err);
        webback_free(b);
        return NULL;
    }
    wpe_display_set_primary(b->display);            /* WebKit's pasteboard and screens are the primary display's */
    b->screen = g_object_new(LORD_TYPE_WEB_SCREEN, "id", 1, NULL);
    wpe_screen_set_size(b->screen, cfg->screen_w > 0 ? cfg->screen_w : 1920, cfg->screen_h > 0 ? cfg->screen_h : 1080);
    wpe_screen_set_scale(b->screen, b->scale);
    wpe_screen_set_refresh_rate(b->screen, cfg->refresh_mhz > 0 ? cfg->refresh_mhz : 60000);
    wpe_display_screen_added(b->display, b->screen);
    return b;
}

int webback_native_scheme(void) { return 1; }

/* WPE_SETTING_DARK_MODE on the display's settings: WebKit answers prefers-color-scheme with it, for CSS and matchMedia
 * alike, in every view of this display (lord-web has one), and a change reaches open pages at once */
void webback_set_dark(WebBack *b, int dark) {
    WPESettings *s = b && b->display ? wpe_display_get_settings(b->display) : NULL;
    if (!s) return;
    GError *err = NULL;
    if (!wpe_settings_set_boolean(s, WPE_SETTING_DARK_MODE, dark ? TRUE : FALSE, WPE_SETTINGS_SOURCE_APPLICATION, &err)) {
        fprintf(stderr, "lord-web: dark mode setting: %s\n", err ? err->message : "refused");
        g_clear_error(&err);
    }
}

WebKitWebView *webback_view_new(WebBack *b, WebKitWebContext *context, WebKitNetworkSession *session, WebKitSettings *settings) {
    WebKitWebView *v = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", b->display, "web-context", context, "network-session", session, "settings", settings, "website-policies", webback_policies(), NULL);
    if (!v) return NULL;
    b->view = g_object_ref(webkit_web_view_get_wpe_view(v));   /* kept until webback_free, after the WebKitWebView's last unref */
    LordWebToplevel *t = g_object_new(LORD_TYPE_WEB_TOPLEVEL, "display", b->display, "max-views", 1, NULL);
    t->b = b;
    b->toplevel = WPE_TOPLEVEL(t);
    wpe_toplevel_scale_changed(b->toplevel, b->scale);
    wpe_toplevel_resized(b->toplevel, b->width, b->height);
    wpe_view_set_toplevel(b->view, b->toplevel);
    wpe_view_resized(b->view, b->width, b->height);
    return v;
}

void webback_free(WebBack *b) {
    if (!b) return;
    if (b->view && b->toplevel) wpe_view_set_toplevel(b->view, NULL);
    if (b->pending) g_object_unref(b->pending);
    if (b->toplevel) { LORD_WEB_TOPLEVEL(b->toplevel)->b = NULL; g_object_unref(b->toplevel); }
    if (b->view) {
        if (LORD_IS_WEB_VIEW(b->view)) LORD_WEB_VIEW(b->view)->b = NULL;
        g_object_unref(b->view);                    /* ours since webback_view_new: the WebKitWebView may be gone */
    }
    if (b->display) LORD_WEB_DISPLAY(b->display)->b = NULL;
    /* the display, screen and keymap stay: WebKit's web view and WPEPlatform's singletons may still use them */
    if (b->formats) g_object_unref(b->formats);
    if (b->drm) wpe_drm_device_unref(b->drm);
    free(b);
}

/* ---- frames ---- */

void webback_frame_done(WebBack *b) {
    if (!b || !b->pending || !b->view) return;
    WPEBuffer *buf = b->pending;
    b->pending = NULL;
    wpe_view_buffer_rendered(b->view, buf);
    if (WPE_IS_BUFFER_SHM(buf)) wpe_view_buffer_released(b->view, buf);    /* copied: WebKit may draw into it again */
    g_object_unref(buf);
}

void webback_release(WebBack *b, void *key) { if (b && b->view && key && WPE_IS_BUFFER(key)) wpe_view_buffer_released(b->view, WPE_BUFFER(key)); }

typedef struct { void (*gone)(void *data); void *data; } Watch;
static void watch_fire(gpointer d, GObject *where) { (void)where; Watch *w = d; w->gone(w->data); free(w); }
void webback_watch(WebBack *b, void *key, void (*gone)(void *data), void *data) {
    (void)b;
    Watch *w = calloc(1, sizeof *w);
    if (!w) return;
    w->gone = gone;
    w->data = data;
    g_object_weak_ref(G_OBJECT(key), watch_fire, w);
}

/* ---- the window ---- */

void webback_set_size(WebBack *b, int w, int h) {
    b->width = w;
    b->height = h;
    if (!b->toplevel) return;
    wpe_toplevel_resized(b->toplevel, w, h);
    wpe_view_resized(b->view, w, h);
}

void webback_set_scale(WebBack *b, double s) {
    b->scale = s;
    if (b->screen) wpe_screen_set_scale(b->screen, s);
    if (b->toplevel) wpe_toplevel_scale_changed(b->toplevel, s);
}

void webback_set_visible(WebBack *b, int visible) {
    if (!b->view) return;
    if (visible) { wpe_view_set_visible(b->view, TRUE); wpe_view_map(b->view); }
    else wpe_view_unmap(b->view);
}

void webback_set_focused(WebBack *b, int focused) {
    if (!b->view) return;
    WPEToplevelState st = wpe_toplevel_get_state(b->toplevel);
    wpe_toplevel_state_changed(b->toplevel, focused ? (WPEToplevelState)(st | WPE_TOPLEVEL_STATE_ACTIVE) : (WPEToplevelState)(st & ~WPE_TOPLEVEL_STATE_ACTIVE));
    if (focused) wpe_view_focus_in(b->view);
    else wpe_view_focus_out(b->view);
}

void webback_set_title(WebBack *b, const char *title) { if (b->toplevel) wpe_toplevel_set_title(b->toplevel, title); }

void webback_set_screen(WebBack *b, int w, int h, int refresh_mhz) {
    if (!b->screen) return;
    if (w > 0 && h > 0) wpe_screen_set_size(b->screen, w, h);
    if (refresh_mhz > 0) wpe_screen_set_refresh_rate(b->screen, refresh_mhz);
}

/* ---- the keyboard ---- */

static struct xkb_state *xkb_st(WebBack *b) { return wpe_keymap_xkb_get_xkb_state(WPE_KEYMAP_XKB(b->keymap)); }
static struct xkb_keymap *xkb_km(WebBack *b) { return wpe_keymap_xkb_get_xkb_keymap(WPE_KEYMAP_XKB(b->keymap)); }

void webback_keymap(WebBack *b, const char *map, size_t len) {
    /* WPEPlatform reads the keymap from an fd, as the compositor gave it: a sealed memfd with the text and its NUL */
    int fd = memfd_create("lord-web-keymap", MFD_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, map, len) != (ssize_t)len || write(fd, "", 1) != 1) { close(fd); return; }
    wpe_keymap_xkb_update(WPE_KEYMAP_XKB(b->keymap), XKB_KEYMAP_FORMAT_TEXT_V1, fd, (guint)len + 1);    /* closes fd */
}

static uint32_t from_wpe_mods(WPEModifiers m) {
    uint32_t r = 0;
    if (m & WPE_MODIFIER_KEYBOARD_CONTROL) r |= WEBMOD_CTRL;
    if (m & WPE_MODIFIER_KEYBOARD_SHIFT) r |= WEBMOD_SHIFT;
    if (m & WPE_MODIFIER_KEYBOARD_ALT) r |= WEBMOD_ALT;
    if (m & WPE_MODIFIER_KEYBOARD_META) r |= WEBMOD_META;
    return r;
}

/* the event's modifiers: the keyboard's from the keymap's state (Caps Lock too), the held buttons from mods */
static WPEModifiers to_wpe_mods(WebBack *b, uint32_t mods) {
    unsigned r = xkb_st(b) ? wpe_keymap_get_modifiers(b->keymap) : 0;
    if (!xkb_st(b)) {
        if (mods & WEBMOD_CTRL) r |= WPE_MODIFIER_KEYBOARD_CONTROL;
        if (mods & WEBMOD_SHIFT) r |= WPE_MODIFIER_KEYBOARD_SHIFT;
        if (mods & WEBMOD_ALT) r |= WPE_MODIFIER_KEYBOARD_ALT;
        if (mods & WEBMOD_META) r |= WPE_MODIFIER_KEYBOARD_META;
    }
    if (mods & WEBMOD_BTN_LEFT) r |= WPE_MODIFIER_POINTER_BUTTON1;
    if (mods & WEBMOD_BTN_MIDDLE) r |= WPE_MODIFIER_POINTER_BUTTON2;
    if (mods & WEBMOD_BTN_RIGHT) r |= WPE_MODIFIER_POINTER_BUTTON3;
    return (WPEModifiers)r;
}

uint32_t webback_modifiers(WebBack *b, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    struct xkb_state *st = xkb_st(b);
    if (!st) return 0;
    xkb_state_update_mask(st, dep, lat, lock, 0, 0, group);
    return from_wpe_mods(wpe_keymap_get_modifiers(b->keymap));
}

xkb_keysym_t webback_keysym(WebBack *b, uint32_t key) {
    struct xkb_state *st = xkb_st(b);
    return st ? xkb_state_key_get_one_sym(st, key + 8) : 0;
}

int webback_key_repeats(WebBack *b, uint32_t key) {
    struct xkb_keymap *km = xkb_km(b);
    return km && xkb_keymap_key_repeats(km, key + 8);
}

static void send_key(WebBack *b, uint32_t time, guint code, guint keyval, int pressed, WPEModifiers m) {
    /* a modifier key's own event already has (down) or no longer has (up) its modifier, as WPEPlatform's Wayland does */
    unsigned mm = m, bit = 0;
    switch (keyval) {
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: bit = WPE_MODIFIER_KEYBOARD_CONTROL; break;
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: bit = WPE_MODIFIER_KEYBOARD_SHIFT; break;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: bit = WPE_MODIFIER_KEYBOARD_ALT; break;
    case XKB_KEY_Meta_L: case XKB_KEY_Meta_R: bit = WPE_MODIFIER_KEYBOARD_META; break;
    default: break;
    }
    if (bit) mm = pressed ? mm | bit : mm & ~bit;
    WPEEvent *e = wpe_event_keyboard_new(pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, b->view, WPE_INPUT_SOURCE_KEYBOARD, time, (WPEModifiers)mm, code, keyval);
    wpe_view_event(b->view, e);
    wpe_event_unref(e);
}

void webback_key(WebBack *b, uint32_t time, uint32_t key, int pressed, uint32_t mods) {
    if (!b->view) return;
    xkb_keysym_t sym = webback_keysym(b, key);
    if (sym == XKB_KEY_NoSymbol) return;
    send_key(b, time, key + 8, sym, pressed, to_wpe_mods(b, mods));
}

int webback_shortcut(WebBack *b, xkb_keysym_t keysym, int shift) {
    struct xkb_keymap *km = xkb_km(b);
    if (!km || !b->view) return 0;
    xkb_keycode_t code = 0;
    for (xkb_keycode_t k = xkb_keymap_min_keycode(km); k <= xkb_keymap_max_keycode(km) && !code; k++) {
        const xkb_keysym_t *syms;
        int n = xkb_keymap_key_get_syms_by_level(km, k, 0, 0, &syms);
        for (int i = 0; i < n; i++) if (syms[i] == keysym) code = k;
    }
    if (!code) return 0;
    WPEModifiers m = (WPEModifiers)(WPE_MODIFIER_KEYBOARD_CONTROL | (shift ? WPE_MODIFIER_KEYBOARD_SHIFT : 0));
    uint32_t t = (uint32_t)(g_get_monotonic_time() / 1000);
    xkb_keysym_t sym = shift ? xkb_keysym_to_upper(keysym) : keysym;
    for (int pressed = 1; pressed >= 0; pressed--) send_key(b, t, code, sym, pressed, m);
    return 1;
}

/* ---- the pointer ---- */

void webback_motion(WebBack *b, uint32_t time, double x, double y, uint32_t mods) {
    if (!b->view) return;
    WPEEvent *e = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, b->view, WPE_INPUT_SOURCE_MOUSE, time, to_wpe_mods(b, mods), x, y, x - b->px, y - b->py);
    b->px = x;
    b->py = y;
    wpe_view_event(b->view, e);
    wpe_event_unref(e);
}

void webback_button(WebBack *b, uint32_t time, double x, double y, int button, int pressed, uint32_t mods) {
    if (!b->view) return;
    guint wb = button == WEBBTN_LEFT ? WPE_BUTTON_PRIMARY : button == WEBBTN_RIGHT ? WPE_BUTTON_SECONDARY : WPE_BUTTON_MIDDLE;
    guint count = pressed ? wpe_view_compute_press_count(b->view, x, y, wb, time) : 0;
    WPEEvent *e = wpe_event_pointer_button_new(pressed ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, b->view, WPE_INPUT_SOURCE_MOUSE, time, to_wpe_mods(b, mods), wb, x, y, count);
    wpe_view_event(b->view, e);
    wpe_event_unref(e);
}

/* webaxis.c's lines are notches in WebKit's sign (WPEPlatform's too: not precise); its pixels are buffer pixels,
 * WPEPlatform's precise deltas are logical ones */
void webback_axis(WebBack *b, uint32_t time, double x, double y, const WebAxisOut *out, int n, uint32_t mods) {
    if (!b->view) return;
    WPEModifiers m = to_wpe_mods(b, mods);
    double s = b->scale > 0 ? b->scale : 1;
    for (int i = 0; i < n; i++) {
        WPEEvent *e;
        if (out[i].kind == WEBAXIS_OUT_LINES) e = wpe_event_scroll_new(b->view, WPE_INPUT_SOURCE_MOUSE, time, m, out[i].x, out[i].y, FALSE, FALSE, x, y);
        else if (out[i].kind == WEBAXIS_OUT_PIXELS) e = wpe_event_scroll_new(b->view, WPE_INPUT_SOURCE_TOUCHPAD, time, m, out[i].x / s, out[i].y / s, TRUE, FALSE, x, y);
        else e = wpe_event_scroll_new(b->view, WPE_INPUT_SOURCE_TOUCHPAD, time, m, 0, 0, TRUE, TRUE, x, y);
        wpe_view_event(b->view, e);
        wpe_event_unref(e);
    }
}

/* ---- touch ---- */

#define MAX_TOUCH 10

void webback_touch(WebBack *b, int type, int id, uint32_t time, double x, double y, uint32_t mods) {
    if (!b->view) return;
    WPEModifiers m = to_wpe_mods(b, mods);
    if (type == WEBTOUCH_CANCEL) {
        for (int i = 0; i < MAX_TOUCH; i++) if (b->touching[i]) {
            b->touching[i] = 0;
            WPEEvent *e = wpe_event_touch_new(WPE_EVENT_TOUCH_CANCEL, b->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, m, (guint32)i, b->touch_x[i], b->touch_y[i]);
            wpe_view_event(b->view, e);
            wpe_event_unref(e);
        }
        return;
    }
    if (id < 0 || id >= MAX_TOUCH) return;
    if (type != WEBTOUCH_DOWN && !b->touching[id]) return;
    WPEEventType t = type == WEBTOUCH_DOWN ? WPE_EVENT_TOUCH_DOWN : type == WEBTOUCH_MOTION ? WPE_EVENT_TOUCH_MOVE : WPE_EVENT_TOUCH_UP;
    b->touching[id] = type != WEBTOUCH_UP;
    if (type == WEBTOUCH_UP) { x = b->touch_x[id]; y = b->touch_y[id]; }
    else { b->touch_x[id] = x; b->touch_y[id] = y; }
    WPEEvent *e = wpe_event_touch_new(t, b->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, m, (guint32)id, x, y);
    wpe_view_event(b->view, e);
    wpe_event_unref(e);
}

/* ---- the pasteboard: the primary display's WPEClipboard ---- */

char *webback_pb_read(WebBack *b, const char *type) {
    WPEClipboard *c = wpe_display_get_clipboard(b->display);
    if (!c) return NULL;
    gsize n = 0;
    char *t = wpe_clipboard_read_text(c, type, &n);
    if (!t) {
        /* WPEPlatform names its types without the charset: text/plain;charset=utf-8 is offered, text/html is not */
        const char *semi = strchr(type, ';');
        if (semi) {
            char *base = g_strndup(type, (gsize)(semi - type));
            t = wpe_clipboard_read_text(c, base, &n);
            g_free(base);
        }
    }
    if (!t || !n) { g_free(t); return NULL; }
    char *r = g_strndup(t, n);
    g_free(t);
    return r;
}

void webback_pb_write_text(WebBack *b, const char *text, size_t len) {
    WPEClipboard *c = wpe_display_get_clipboard(b->display);
    if (!c) return;
    WPEClipboardContent *content = wpe_clipboard_content_new();
    char *t = g_strndup(text, len);
    wpe_clipboard_content_set_text(content, t);
    g_free(t);
    wpe_clipboard_set_content(c, content);
    wpe_clipboard_content_unref(content);
}
