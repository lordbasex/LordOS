/* webback.h — the seam between lord-web and the way WPE WebKit shows a view (docs/WEBAPPS.md, "How a frame gets to
 * the screen"). Everything lord-web does with WebKit's frames and input goes through here; the rest of lord-web uses
 * WebKit's own API (webkit_web_view_*) and never names the backend's types.
 *
 * Two implementations, exactly one of them built (mk/web.mk picks it: LORD_WEB_BACKEND=legacy|platform):
 *   webback_legacy.c    libwpe + wpebackend-fdo, WPE WebKit's legacy API (Debian 13's WPE WebKit 2.48, libwpe 1.16,
 *                       wpebackend-fdo 1.16). WebKit's web process draws into wpebackend-fdo's nested compositor;
 *                       its exportable view backend hands lord-web each frame (webshm.c: its libwayland-server half).
 *   webback_platform.c  WPEPlatform (WPE WebKit 2.50 and later, built with ENABLE_WPE_PLATFORM; LordOS's 2.54
 *                       backport): lord-web's own WPEDisplay, WPEView, WPEToplevel and WPEScreen, no Wayland of
 *                       WPEPlatform's (wpe-platform-wayland is never used: lord-web keeps handing its frames to
 *                       lord-server on its own wl_surface). WebKit's web process hands each frame as a WPEBufferDMABuf
 *                       or a WPEBufferSHM over WebKit's own IPC.
 *
 * Either way the frame reaches webwin.c (the Wayland window) through WebBackSink and leaves as a wl_buffer of
 * lord-web's on lord-server: a dmabuf's planes passed on as they are, or shared memory copied into lord-web's own
 * wl_shm buffer. Coordinates given to webback_* are the window's surface coordinates (logical pixels); each backend
 * turns them into what its WebKit takes. */
#ifndef LORD_WEB_WEBBACK_H
#define LORD_WEB_WEBBACK_H

#include "webaxis.h"

#include <stddef.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <xkbcommon/xkbcommon.h>

typedef struct WebBack WebBack;

/* modifiers and held buttons, the backend's neutral form */
enum {
    WEBMOD_CTRL = 1 << 0, WEBMOD_SHIFT = 1 << 1, WEBMOD_ALT = 1 << 2, WEBMOD_META = 1 << 3,
    WEBMOD_BTN_LEFT = 1 << 8, WEBMOD_BTN_RIGHT = 1 << 9, WEBMOD_BTN_MIDDLE = 1 << 10,
};
/* buttons: libwpe's numbers, which webwin.c has always used */
enum { WEBBTN_LEFT = 1, WEBBTN_RIGHT = 2, WEBBTN_MIDDLE = 3 };
enum { WEBTOUCH_DOWN = 1, WEBTOUCH_MOTION, WEBTOUCH_UP, WEBTOUCH_CANCEL };

/* WEBFRAME_OTHER: a buffer in a form lord-web can't pass on (legacy: an EGL wl_buffer): release it (key) */
enum { WEBFRAME_SHM = 1, WEBFRAME_DMABUF, WEBFRAME_OTHER };
typedef struct {
    int kind;                       /* WEBFRAME_* */
    int w, h;                       /* in buffer pixels */
    /* WEBFRAME_SHM: the pixels, valid only while the sink's frame call runs (the backend gives the buffer back to
     * WebKit after it); format is a wl_shm format (WL_SHM_FORMAT_ARGB8888 or _XRGB8888: the same numbers as DRM's
     * for every other format) */
    const void *pixels;
    int stride;
    uint32_t format;
    /* WEBFRAME_DMABUF: the planes, owned by the backend (dup them to keep them). key is the backend's buffer, the
     * same pointer for as long as that buffer lives: a wl_buffer made for it can be shown again. fourcc is DRM's. */
    void *key;                      /* WEBFRAME_OTHER too */
    int n_planes;
    int fds[4];
    uint32_t offsets[4], strides[4];
    uint64_t modifier;
    uint32_t fourcc;
} WebFrame;

typedef struct {
    /* a frame to show. SHM: copy it now. DMABUF: webback_release(key) once the compositor no longer reads it. Either
     * way webback_frame_done() once it is on screen (or won't be): until then WebKit draws no next frame. */
    void (*frame)(void *d, const WebFrame *f);
    /* the page asks for this cursor (a CSS cursor name: "default", "pointer", "text", ...); only backends that know
     * it call this (WPEPlatform); otherwise webengine.c's hint from mouse-target-changed is all there is */
    void (*cursor)(void *d, const char *name);
} WebBackSink;

/* What webwin.c knows of the compositor and the GPU when the backend starts. */
typedef struct {
    int width, height;              /* the window's size, logical pixels */
    double scale;
    int gpu;                        /* 1: dmabuf frames wanted (EGL works on the compositor's display) */
    void *egl_display;              /* that EGLDisplay (legacy only: wpebackend-fdo's EGL) */
    /* platform only: the formats and modifiers the compositor imports (linux-dmabuf), n pairs; none: WebKit's choice */
    const uint32_t *dmabuf_formats;
    const uint64_t *dmabuf_modifiers;
    int dmabuf_n;
    const char *render_node;        /* platform only: /dev/dri/renderD* of the compositor's GPU, or NULL */
    const char *primary_node;       /* platform only: its /dev/dri/card*, or NULL */
    int screen_w, screen_h;         /* platform only: the output's size in logical pixels, 0 unknown */
    int refresh_mhz;                /* platform only: its refresh rate, 0 unknown (60 Hz) */
} WebBackConfig;

/* Starts the backend; NULL when it can't (legacy: wpebackend-fdo didn't start). Without a GPU (cfg->gpu 0) the web
 * process draws on the CPU into shared memory. The probe, which has no window, gives every frame back at once. */
WebBack *webback_new(const WebBackConfig *cfg, const WebBackSink *sink, void *sink_data);
/* the backend's name, for lord-web's log and the tests: "legacy" or "platform" */
const char *webback_name(void);
/* the WebKitWebView on this backend */
WebKitWebView *webback_view_new(WebBack *b, WebKitWebContext *context, WebKitNetworkSession *session, WebKitSettings *settings);
void webback_free(WebBack *b);

/* frames */
void webback_frame_done(WebBack *b);
void webback_release(WebBack *b, void *key);
/* gone(data) once, when the backend's buffer key is destroyed */
void webback_watch(WebBack *b, void *key, void (*gone)(void *data), void *data);

/* the window */
void webback_set_size(WebBack *b, int w, int h);
void webback_set_scale(WebBack *b, double scale);
void webback_set_visible(WebBack *b, int visible);   /* mapped, configured and not suspended: WebKit paints */
void webback_set_focused(WebBack *b, int focused);
void webback_set_title(WebBack *b, const char *title); /* the toplevel's, for backends that keep one */
/* the output the window is on: its size (logical pixels) and refresh rate (mHz), 0 when unknown; platform only */
void webback_set_screen(WebBack *b, int w, int h, int refresh_mhz);

/* the keyboard: the compositor's keymap (xkb text, len bytes), its modifiers, and keys (evdev codes) */
void webback_keymap(WebBack *b, const char *map, size_t len);
/* the modifiers now (wl_keyboard.modifiers); returns them as WEBMOD_* */
uint32_t webback_modifiers(WebBack *b, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
xkb_keysym_t webback_keysym(WebBack *b, uint32_t key);       /* the key's keysym in the keymap's state now, or 0 */
int webback_key_repeats(WebBack *b, uint32_t key);
void webback_key(WebBack *b, uint32_t time, uint32_t key, int pressed, uint32_t mods);
/* Ctrl+<keysym> (Shift too) pressed and released, as from the keyboard; 0 when the keymap has no such key */
int webback_shortcut(WebBack *b, xkb_keysym_t keysym, int shift);

/* the pointer, the wheel, touch; x and y in surface coordinates, mods WEBMOD_* with the held buttons */
void webback_motion(WebBack *b, uint32_t time, double x, double y, uint32_t mods);
void webback_button(WebBack *b, uint32_t time, double x, double y, int button, int pressed, uint32_t mods);
void webback_axis(WebBack *b, uint32_t time, double x, double y, const WebAxisOut *out, int n, uint32_t mods);
void webback_touch(WebBack *b, int type, int id, uint32_t time, double x, double y, uint32_t mods);

/* WebKit's pasteboard (legacy: libwpe's; platform: the display's WPEClipboard): its text of a type, newly
 * allocated (g_free) or NULL; and text written to it as text/plain;charset=utf-8 and text/plain */
char *webback_pb_read(WebBack *b, const char *type);
void webback_pb_write_text(WebBack *b, const char *text, size_t len);

/* main(), before anything of WebKit's: legacy loads wpebackend-fdo (wpe_loader_init); platform nothing */
void webback_process_init(void);

#endif
