/* webwin.c — lord-web's Wayland window (see web.h, webwin.h).
 *
 * WebKit draws each frame through the backend (webback.h: libwpe + wpebackend-fdo, or WPEPlatform), which hands it to
 * this file, which shows it on LordOS's compositor:
 *   - dmabuf: when the compositor offers zwp_linux_dmabuf_v1 and EGL works on its display, the frame's planes become a
 *     wl_buffer of ours (no copy). A frame the compositor can't import, or one in a form we can't pass on, restarts
 *     lord-web once on the shared memory path.
 *   - shared memory (LordOS's CPU path; a computer with no GPU): the frame is copied into one of our own wl_shm
 *     buffers.
 * Either way the frame WebKit waits for is completed only after the compositor's frame callback. WebKit is told the view
 * is visible only while the window is mapped, configured and not suspended (xdg_toplevel's "suspended" state, which
 * lord-server sends for a minimized window and one on a hidden desktop): otherwise the page stops painting, its
 * requestAnimationFrame stops, its CSS animations pause and its timers are throttled (webengine.c turns on WebKit's
 * hidden page throttling), and frames are completed at once. Audio and the page's network keep going.
 * LORD_WEB_TRACE=1 logs, for each click or key, the time to the frame WebKit exports after it, to its commit and to
 * the compositor's frame callback (the frame on screen).
 *
 * Input goes to the backend's events (webback.h): the keyboard with the compositor's keymap and its key repeat, the
 * pointer, the wheel (webaxis.c: lines for a mouse wheel, pixels for a touchpad), touch. The clipboard is a normal wl_data_device: the
 * session's text is mirrored into WebKit's pasteboard whenever the selection changes while the window has the
 * keyboard, and the page's copies are offered to the session. WebKit's pasteboard holds text only, and reads back as
 * Latin-1 what it holds as UTF-8 (webclip.h), so when the person pastes (Ctrl+V, Shift+Insert, the Edit menu, the
 * context menu) lord-web looks first: a file list on the session's clipboard (text/uri-list, gnome-copied-files; Files'
 * copies) is read then (at most WEBDROP_LIST_MAX) and its local files, checked like a drop's (webdrop.c), go to the page
 * as a paste of Files (webxfer.c), its text when none passes; an image (image/png or image/jpeg, at most CLIP_IMAGE_MAX)
 * with no text goes as a File; a text that isn't ASCII is pasted by webxfer.c (WebKit would garble it); anything
 * else is WebKit's own paste. An image the page copies (webxfer.c) is offered from here.
 * The same data device takes drops: a drag that offers text/uri-list (Files) or text is accepted as a copy, its list
 * read at the drop (at most WEBDROP_LIST_MAX) and the local files on it (webdrop.c) given to webxfer.c; while it moves
 * over the window the page gets dragenter, dragover and dragleave (webxfer_drag).
 * text-input-v3 is WebKit's input method context. */
#include "webwin.h"
#include "webaxis.h"
#include "webback.h"
#include "webclip.h"
#include "webframe.h"

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define DEFAULT_W 1200
#define DEFAULT_H 800
#define MIN_W 360
#define MIN_H 300
#define SHM_BUFS 3
#define MAX_OUTPUTS 8
#define MAX_TOUCH 10
#define MAX_FORMATS 256
#define CLIP_MAX (16 * 1024 * 1024)
#define CLIP_IMAGE_MAX (32 * 1024 * 1024)
#define USER_ACTIVE_US (5 * G_USEC_PER_SEC)   /* how long after a key or a click the page may write the clipboard */
#define PB_TEXT "text/plain;charset=utf-8"
#define PB_HTML "text/html;charset=utf-8"

typedef struct {
    struct wl_buffer *buf;
    void *data;
    size_t size;
    int w, h, stride;
    unsigned format;
    int busy;
} ShmBuf;

typedef struct DmaBuf DmaBuf;
struct DmaBuf {
    WebWin *win;
    void *res;                          /* the backend's buffer (WebFrame.key) */
    struct wl_buffer *buf;              /* ours on LordOS's compositor, NULL until created */
    int w, h;
    int waiting;                        /* the newest frame: show it once created */
    int busy;                           /* attached, not released yet */
    int gone;                           /* its resource is destroyed: free on release */
    int pending;                        /* its params create is unanswered: the reply still points here */
    DmaBuf *next;
};

typedef struct { struct wl_output *o; uint32_t name; int scale, entered, w, h, refresh; } Output;

typedef struct { char *data; size_t len, off; } Outgoing;

typedef struct _LordWebIme LordWebIme;

struct WebWin {
    Web *w;
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wl_seat *seat;
    uint32_t seat_caps;                 /* what the seat has: only those devices are asked for */
    struct wl_data_device_manager *ddm;
    uint32_t ddm_ver;
    struct wl_data_device *dd;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *frac_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_activation_v1 *activation;
    struct zwp_text_input_manager_v3 *ti_mgr;
    struct wp_cursor_shape_manager_v1 *cursor_mgr;
    struct zwp_primary_selection_device_manager_v1 *psm;
    struct zwp_primary_selection_device_v1 *pdev;
    struct zwp_primary_selection_source_v1 *psrc;   /* ours, while it is the session's primary selection */
    struct zwp_primary_selection_offer_v1 *poffer;  /* the session's, kept only to be let go */
    char *prim;                                     /* the text psrc gives */
    Output outputs[MAX_OUTPUTS];
    int nout;

    struct wl_surface *surface;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *top;
    struct zxdg_toplevel_decoration_v1 *deco;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *frac;
    int width, height, cfg_w, cfg_h;
    int configured, shown;
    int suspended, cfg_suspended;       /* xdg_toplevel's suspended state: applied, and pending until the configure */
    double scale;
    struct wl_callback *frame_cb;
    char title[512];

    int gpu;
    EGLDisplay egl;
    char render_node[64];               /* the compositor's GPU (EGL_EXT_device_query), or "" */
    char primary_node[64];
    uint32_t dmabuf_formats[MAX_FORMATS];   /* what the compositor imports (linux-dmabuf's modifier events) */
    uint64_t dmabuf_modifiers[MAX_FORMATS];
    int dmabuf_n;
    WebBack *back;
    ShmBuf shm_bufs[SHM_BUFS];
    DmaBuf *dmabufs;

    struct wl_keyboard *kbd;
    struct wl_pointer *ptr;
    struct wl_touch *touch;
    struct wp_cursor_shape_device_v1 *cursor_dev;
    int cursor_shape;
    int cursor_named;                   /* the backend names the page's cursor: the hint from webengine.c is ignored */
    uint32_t enter_serial, input_serial;
    int focused;
    double px, py;
    uint32_t buttons, mods;             /* WEBMOD_BTN_*, WEBMOD_* */
    WebAxis axis;
    uint32_t axis_time;
    gint64 last_input_us;               /* the person's last key, click or touch in the window */
    int trace;                          /* LORD_WEB_TRACE: input to frame on screen */
    int trace_stage;                    /* 1 waiting for the frame, 2 for its commit, 3 for its frame callback */
    gint64 trace_in, trace_export, trace_commit, trace_copy;
    int repeat_rate, repeat_delay;
    uint32_t repeat_key;
    guint repeat_src;

    struct wl_data_offer *sel_offer;
    char *sel_mime;                     /* the best text type the session's selection offers */
    const char *sel_img;                /* its image type (image/png before image/jpeg), or NULL */
    const char *sel_list;               /* its file list type (WEBCLIP_URIS before WEBCLIP_GNOME), or NULL */
    struct wl_data_source *source;      /* our own selection */
    char *own_text, *own_html;
    GBytes *own_img;                    /* or an image the page copied */
    char *own_img_mime;

    struct wl_data_offer *drag_offer;   /* a drag over the window */
    const char *drag_mime;              /* what it gives us: text/uri-list, or a text type; NULL when nothing */
    double drag_x, drag_y;
    int drag_page;                      /* the page was told of this drag (dragenter): it is told when it leaves */
    char *known;                        /* the pasteboard's text as last mirrored or offered */
    guint pb_timer;

    struct zwp_text_input_v3 *ti;
    LordWebIme *ime;
    GSource *src;
};

/* ---- the Wayland connection in GLib's main loop ---- */

typedef struct { GSource s; WebWin *win; gpointer tag; } WlSource;

static gboolean wl_prepare(GSource *s, gint *timeout) {
    WebWin *win = ((WlSource *)s)->win;
    *timeout = -1;
    while (wl_display_prepare_read(win->dpy)) if (wl_display_dispatch_pending(win->dpy) < 0) break;
    wl_display_flush(win->dpy);
    return FALSE;
}

static gboolean wl_check(GSource *s) {
    WlSource *ws = (WlSource *)s;
    GIOCondition rev = g_source_query_unix_fd(s, ws->tag);
    if (rev & G_IO_IN) wl_display_read_events(ws->win->dpy);
    else wl_display_cancel_read(ws->win->dpy);
    return (rev & (G_IO_IN | G_IO_ERR | G_IO_HUP)) != 0;
}

static gboolean wl_dispatch(GSource *s, GSourceFunc cb, gpointer data) {
    (void)cb; (void)data;
    WlSource *ws = (WlSource *)s;
    GIOCondition rev = g_source_query_unix_fd(s, ws->tag);
    if ((rev & (G_IO_ERR | G_IO_HUP)) || wl_display_dispatch_pending(ws->win->dpy) < 0) {
        fprintf(stderr, "lord-web: the compositor went away\n");
        web_quit(ws->win->w, 0);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs wl_funcs = { wl_prepare, wl_check, wl_dispatch, NULL, NULL, NULL };

/* ---- globals ---- */

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void update_scale(WebWin *win);

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *mk, const char *md, int32_t t) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)t;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    WebWin *win = d;
    if (!(f & WL_OUTPUT_MODE_CURRENT)) return;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].o == o) { win->outputs[i].w = w; win->outputs[i].h = h; win->outputs[i].refresh = r; }
}
static void out_done(void *d, struct wl_output *o) { (void)o; update_scale(d); }
static void out_scale(void *d, struct wl_output *o, int32_t s) {
    WebWin *win = d;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].o == o) win->outputs[i].scale = s > 0 && s <= 8 ? s : 1;
}
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener out_listener = { out_geometry, out_mode, out_done, out_scale, out_name, out_desc };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps);
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static uint32_t min_u(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* linux-dmabuf 3: the formats and modifiers the compositor imports (the WPEPlatform backend offers WebKit only these) */
static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *m, uint32_t f) { (void)d; (void)m; (void)f; }
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *m, uint32_t f, uint32_t hi, uint32_t lo) {
    (void)m;
    WebWin *win = d;
    if (win->dmabuf_n >= MAX_FORMATS) return;
    win->dmabuf_formats[win->dmabuf_n] = f;
    win->dmabuf_modifiers[win->dmabuf_n++] = (uint64_t)hi << 32 | lo;
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = { dmabuf_format, dmabuf_modifier };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    WebWin *win = d;
    if (!strcmp(iface, wl_compositor_interface.name)) win->comp = wl_registry_bind(r, name, &wl_compositor_interface, min_u(ver, 4));
    else if (!strcmp(iface, wl_shm_interface.name)) win->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        win->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, min_u(ver, 6));    /* 6: suspended */
        xdg_wm_base_add_listener(win->wm, &wm_listener, win);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) win->deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !win->seat) {
        win->seat = wl_registry_bind(r, name, &wl_seat_interface, min_u(ver, 8));
        wl_seat_add_listener(win->seat, &seat_listener, win);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        win->ddm_ver = min_u(ver, 3);
        win->ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, win->ddm_ver);
    }
    else if (!strcmp(iface, wp_viewporter_interface.name)) win->viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) win->frac_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && ver >= 3) {
        win->dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(win->dmabuf, &dmabuf_listener, win);
    }
    else if (!strcmp(iface, xdg_activation_v1_interface.name)) win->activation = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
    else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name)) win->ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) win->psm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) win->cursor_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && win->nout < MAX_OUTPUTS && ver >= 2) {
        Output *o = &win->outputs[win->nout++];
        o->name = name;
        o->scale = 1;
        o->o = wl_registry_bind(r, name, &wl_output_interface, min_u(ver, 4));
        wl_output_add_listener(o->o, &out_listener, win);
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)r;
    WebWin *win = d;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].name == name) {
        wl_output_destroy(win->outputs[i].o);
        win->outputs[i] = win->outputs[--win->nout];
        update_scale(win);
        return;
    }
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ---- scale ---- */

static void set_scale(WebWin *win, double s) {
    if (s < 0.5 || s > 8) s = 1;
    if (s == win->scale) return;
    win->scale = s;
    if (win->back) webback_set_scale(win->back, s);
}

/* the output the window is on (the first one it entered), for the backend's screen: its size and refresh rate */
static void update_screen(WebWin *win) {
    if (!win->back) return;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].entered && win->outputs[i].w > 0) {
        int s = win->outputs[i].scale > 0 ? win->outputs[i].scale : 1;
        webback_set_screen(win->back, win->outputs[i].w / s, win->outputs[i].h / s, win->outputs[i].refresh);
        return;
    }
}

/* without fractional-scale: the largest integer scale of the outputs the window is on */
static void update_scale(WebWin *win) {
    update_screen(win);
    if (win->frac) return;
    int s = 1;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].entered && win->outputs[i].scale > s) s = win->outputs[i].scale;
    set_scale(win, s);
}

static void frac_preferred(void *d, struct wp_fractional_scale_v1 *f, uint32_t s120) { (void)f; set_scale(d, s120 / 120.0); }
static const struct wp_fractional_scale_v1_listener frac_listener = { frac_preferred };

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) {
    (void)s;
    WebWin *win = d;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].o == o) win->outputs[i].entered = 1;
    update_scale(win);
}
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) {
    (void)s;
    WebWin *win = d;
    for (int i = 0; i < win->nout; i++) if (win->outputs[i].o == o) win->outputs[i].entered = 0;
    update_scale(win);
}
static const struct wl_surface_listener surf_listener = { surf_enter, surf_leave, NULL, NULL };

/* ---- frames ---- */

static gboolean complete_later(gpointer d) {
    WebWin *win = d;
    webback_frame_done(win->back);
    return G_SOURCE_REMOVE;
}

/* ---- LORD_WEB_TRACE ---- */

static void trace_input(WebWin *win) {
    if (!win->trace || win->trace_stage) return;
    win->trace_stage = 1;
    win->trace_in = g_get_monotonic_time();
}
static void trace_frame(WebWin *win, gint64 copy_us) {
    if (win->trace_stage != 1) return;
    win->trace_stage = 2;
    win->trace_export = g_get_monotonic_time();
    win->trace_copy = copy_us;
}
static void trace_presented(WebWin *win) {
    if (win->trace_stage != 3) return;
    win->trace_stage = 0;
    gint64 now = g_get_monotonic_time();
    fprintf(stderr, "lord-web: trace: input to frame %.1f ms, to commit %.1f ms (copy %.1f), to frame callback %.1f ms\n",
            (win->trace_export - win->trace_in) / 1e3, (win->trace_commit - win->trace_in) / 1e3, win->trace_copy / 1e3,
            (now - win->trace_in) / 1e3);
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)t;
    WebWin *win = d;
    wl_callback_destroy(cb);
    win->frame_cb = NULL;
    trace_presented(win);
    webback_frame_done(win->back);
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void present(WebWin *win, struct wl_buffer *buf, int bw, int bh) {
    wl_surface_attach(win->surface, buf, 0, 0);
    if (win->viewport) wp_viewport_set_destination(win->viewport, win->width, win->height);
    else {
        int s = win->width > 0 && bw % win->width == 0 && bh % (bw / win->width ? bw / win->width : 1) == 0 ? bw / win->width : 1;
        wl_surface_set_buffer_scale(win->surface, s > 0 && s <= 8 ? s : 1);
    }
    wl_surface_damage_buffer(win->surface, 0, 0, bw, bh);
    if (win->frame_cb) wl_callback_destroy(win->frame_cb);
    win->frame_cb = wl_surface_frame(win->surface);
    wl_callback_add_listener(win->frame_cb, &frame_listener, win);
    wl_surface_commit(win->surface);
    if (win->trace_stage == 2) { win->trace_stage = 3; win->trace_commit = g_get_monotonic_time(); }
}

static int can_show(WebWin *win) { return win->shown && win->configured; }

static void shm_release(void *d, struct wl_buffer *b) { (void)b; ((ShmBuf *)d)->busy = 0; }
static const struct wl_buffer_listener shm_buf_listener = { shm_release };

static void shm_buf_free(ShmBuf *s) {
    if (s->buf) wl_buffer_destroy(s->buf);
    if (s->data) munmap(s->data, s->size);
    memset(s, 0, sizeof *s);
}

static ShmBuf *shm_buf_get(WebWin *win, int w, int h, unsigned format) {
    ShmBuf *free_one = NULL;
    for (int i = 0; i < SHM_BUFS; i++) {
        ShmBuf *s = &win->shm_bufs[i];
        if (s->busy) continue;
        if (s->buf && s->w == w && s->h == h && s->format == format) return s;
        if (!free_one) free_one = s;
    }
    if (!free_one) return NULL;
    /* the backend checked these already; checked again here so a wrapped w * 4 can never size the buffer */
    if (w <= 0 || h <= 0 || w > WEBFRAME_MAX_SIDE || h > WEBFRAME_MAX_SIDE) return NULL;
    shm_buf_free(free_one);
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = memfd_create("lord-web-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size)) { close(fd); return NULL; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(win->shm, fd, (int32_t)size);
    free_one->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(free_one->buf, &shm_buf_listener, free_one);
    free_one->data = data;
    free_one->size = size;
    free_one->w = w;
    free_one->h = h;
    free_one->stride = stride;
    free_one->format = format;
    return free_one;
}

/* once per kind: what the web process really hands over (with EGL up it may still draw into shared memory) */
static void first_frame(WebWin *win, int dmabuf) {
    static int seen[2];
    if (seen[dmabuf]) return;
    seen[dmabuf] = 1;
    fprintf(stderr, "lord-web: frames by %s (EGL %s)\n", dmabuf ? "dmabuf" : "shared memory", win->gpu ? "up" : "off");
}

/* a frame in shared memory: copied into one of our own wl_shm buffers (the backend gives its buffer back after) */
static void show_shm(WebWin *win, const WebFrame *f) {
    first_frame(win, 0);
    ShmBuf *s = NULL;
    gint64 t0 = win->trace ? g_get_monotonic_time() : 0;
    if (can_show(win) && (s = shm_buf_get(win, f->w, f->h, f->format))) {
        /* every row clamped to both widths and strides, every row count to both heights and our buffer's size */
        webframe_copy(s->data, s->size, s->w, s->h, s->stride, f->pixels, f->w, f->h, f->stride);
    }
    if (s) trace_frame(win, win->trace ? g_get_monotonic_time() - t0 : 0);
    if (!s) { g_idle_add(complete_later, win); return; }
    s->busy = 1;
    present(win, s->buf, s->w, s->h);
}

/* a frame we can't pass on: start again on the shared memory path, once */
static void fall_back_to_shm(WebWin *win, const char *why) {
    if (getenv("LORD_WEB_SHM")) return;
    fprintf(stderr, "lord-web: %s; starting again on the shared memory path\n", why);
    setenv("LORD_WEB_SHM", "1", 1);
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) web_quit(win->w, 1);
    exe[n] = 0;
    char *argv[] = { "lord-web", "--app", win->w->app.id, "--profile", (char *)win->w->profile->id, NULL };
    execv(exe, argv);
    web_quit(win->w, 1);
}

static void dma_free(DmaBuf *b) {
    WebWin *win = b->win;
    for (DmaBuf **p = &win->dmabufs; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
    if (b->buf) wl_buffer_destroy(b->buf);
    free(b);
}

static void dma_release(void *d, struct wl_buffer *buf) {
    (void)buf;
    DmaBuf *b = d;
    b->busy = 0;
    if (b->gone) { dma_free(b); return; }
    webback_release(b->win->back, b->res);
}
static const struct wl_buffer_listener dma_buf_listener = { dma_release };

static void dma_gone(void *d) {
    DmaBuf *b = d;
    b->gone = 1;
    b->res = NULL;
    if (!b->busy && !b->pending) dma_free(b);      /* else params_created/failed or dma_release frees it */
}

static void dma_show(DmaBuf *b) {
    WebWin *win = b->win;
    if (!can_show(win)) {
        webback_release(win->back, b->res);
        g_idle_add(complete_later, win);
        return;
    }
    b->busy = 1;
    trace_frame(win, 0);
    present(win, b->buf, b->w, b->h);
}

static void params_created(void *d, struct zwp_linux_buffer_params_v1 *p, struct wl_buffer *buf) {
    DmaBuf *b = d;
    zwp_linux_buffer_params_v1_destroy(p);
    b->pending = 0;
    b->buf = buf;
    wl_buffer_add_listener(buf, &dma_buf_listener, b);
    if (b->gone) { dma_free(b); return; }
    if (b->waiting) { b->waiting = 0; dma_show(b); }
}
static void params_failed(void *d, struct zwp_linux_buffer_params_v1 *p) {
    DmaBuf *b = d;
    zwp_linux_buffer_params_v1_destroy(p);
    b->pending = 0;
    WebWin *win = b->win;
    if (b->gone) { dma_free(b); b = NULL; }        /* its resource went while we waited: nothing to release */
    else {
        webback_release(win->back, b->res);
        b->waiting = 0;
    }
    g_idle_add(complete_later, win);
    fall_back_to_shm(win, "the compositor could not import a frame");
}
static const struct zwp_linux_buffer_params_v1_listener params_listener = { params_created, params_failed };

/* a frame on the GPU: its planes become a wl_buffer of ours once (kept for as long as the backend's buffer lives) */
static void show_dmabuf(WebWin *win, const WebFrame *f) {
    first_frame(win, 1);
    DmaBuf *b = win->dmabufs;
    while (b && b->res != f->key) b = b->next;
    if (b && b->buf) { dma_show(b); return; }
    if (b) { b->waiting = 1; return; }
    b = calloc(1, sizeof *b);
    if (!b) { webback_release(win->back, f->key); g_idle_add(complete_later, win); return; }
    b->win = win;
    b->res = f->key;
    b->w = f->w;
    b->h = f->h;
    b->waiting = 1;
    b->pending = 1;
    b->next = win->dmabufs;
    win->dmabufs = b;
    webback_watch(win->back, f->key, dma_gone, b);
    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(win->dmabuf);
    for (int i = 0; i < f->n_planes && i < 4; i++)
        zwp_linux_buffer_params_v1_add(p, f->fds[i], (uint32_t)i, f->offsets[i], f->strides[i], (uint32_t)(f->modifier >> 32), (uint32_t)f->modifier);
    zwp_linux_buffer_params_v1_add_listener(p, &params_listener, b);
    zwp_linux_buffer_params_v1_create(p, (int32_t)f->w, (int32_t)f->h, f->fourcc, 0);
}

static void on_frame(void *d, const WebFrame *f) {
    WebWin *win = d;
    if (f->kind == WEBFRAME_SHM) show_shm(win, f);
    else if (f->kind == WEBFRAME_DMABUF && win->dmabuf) show_dmabuf(win, f);
    else {
        webback_release(win->back, f->key);
        g_idle_add(complete_later, win);
        fall_back_to_shm(win, "a frame in a form that can't be passed on");
    }
}

static void set_cursor_name(WebWin *win, int shape);
/* the page's cursor, by its CSS name (WPEPlatform's view says it; legacy has only webwin_set_cursor_hint) */
static void on_cursor(void *d, const char *name) {
    static const struct { const char *css; int shape; } map[] = {
        { "default", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT }, { "pointer", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER },
        { "text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT }, { "vertical-text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT },
        { "wait", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT }, { "progress", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS },
        { "help", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP }, { "crosshair", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR },
        { "move", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE }, { "all-scroll", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL },
        { "not-allowed", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED }, { "no-drop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP },
        { "grab", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB }, { "grabbing", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING },
        { "copy", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY }, { "alias", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS },
        { "context-menu", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU }, { "cell", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL },
        { "col-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE }, { "row-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE },
        { "n-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE }, { "s-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE },
        { "e-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE }, { "w-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE },
        { "ne-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE }, { "nw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE },
        { "se-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE }, { "sw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE },
        { "ew-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE }, { "ns-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE },
        { "nesw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE }, { "nwse-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE },
        { "zoom-in", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN }, { "zoom-out", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT },
    };
    WebWin *win = d;
    int shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    for (size_t i = 0; name && i < sizeof map / sizeof map[0]; i++) if (!strcmp(name, map[i].css)) shape = map[i].shape;
    win->cursor_named = 1;
    if (shape != win->cursor_shape) set_cursor_name(win, shape);
}

static const WebBackSink sink = { on_frame, on_cursor };

/* ---- the toplevel ---- */

/* WebKit's visible and in-window states follow the window (webpage_view_visible) */
static void update_activity(WebWin *win) {
    if (win->back) webback_set_visible(win->back, webpage_view_visible(win->shown, win->configured, win->suspended));
}

static void xsurf_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    WebWin *win = d;
    xdg_surface_ack_configure(xs, serial);
    int w = win->cfg_w > 0 ? win->cfg_w : win->width, h = win->cfg_h > 0 ? win->cfg_h : win->height;
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    int changed = w != win->width || h != win->height;
    win->width = w;
    win->height = h;
    if (changed && win->back) webback_set_size(win->back, w, h);
    win->configured = 1;
    win->suspended = win->cfg_suspended;
    update_activity(win);
}
static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)t;
    WebWin *win = d;
    win->cfg_w = w;
    win->cfg_h = h;
    win->cfg_suspended = webpage_toplevel_suspended(states->data, states->size / sizeof(uint32_t));
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)t; web_close_requested(((WebWin *)d)->w); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void act_done(void *d, struct xdg_activation_token_v1 *t, const char *token) {
    WebWin *win = d;
    if (win->activation && win->surface) xdg_activation_v1_activate(win->activation, token, win->surface);
    xdg_activation_token_v1_destroy(t);
}
static const struct xdg_activation_token_v1_listener act_listener = { act_done };

static void activate(WebWin *win, const char *token) {
    if (!win->activation) return;
    if (token && token[0]) { xdg_activation_v1_activate(win->activation, token, win->surface); return; }
    struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(win->activation);
    xdg_activation_token_v1_add_listener(t, &act_listener, win);
    xdg_activation_token_v1_set_surface(t, win->surface);
    if (win->input_serial && win->seat) xdg_activation_token_v1_set_serial(t, win->input_serial, win->seat);
    xdg_activation_token_v1_set_app_id(t, win->w->window_id);
    xdg_activation_token_v1_commit(t);
}

void webwin_show(WebWin *win, const char *token) {
    if (win->shown) { activate(win, token); wl_display_flush(win->dpy); return; }
    win->shown = 1;
    win->configured = 0;
    win->cfg_suspended = 0;
    win->xsurf = xdg_wm_base_get_xdg_surface(win->wm, win->surface);
    xdg_surface_add_listener(win->xsurf, &xsurf_listener, win);
    win->top = xdg_surface_get_toplevel(win->xsurf);
    xdg_toplevel_add_listener(win->top, &top_listener, win);
    xdg_toplevel_set_app_id(win->top, win->w->window_id);
    xdg_toplevel_set_title(win->top, win->title[0] ? win->title : win->w->app.name);
    xdg_toplevel_set_min_size(win->top, MIN_W, MIN_H);
    if (win->deco_mgr) {
        win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(win->deco_mgr, win->top);
        zxdg_toplevel_decoration_v1_set_mode(win->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (token && token[0] && win->activation) xdg_activation_v1_activate(win->activation, token, win->surface);
    wl_surface_commit(win->surface);
    wl_display_flush(win->dpy);
}

void webwin_hide(WebWin *win) {
    if (!win->shown) return;
    win->shown = 0;
    win->configured = 0;
    update_activity(win);
    if (win->back) webback_set_focused(win->back, 0);
    if (win->frame_cb) { wl_callback_destroy(win->frame_cb); win->frame_cb = NULL; g_idle_add(complete_later, win); }
    if (win->deco) { zxdg_toplevel_decoration_v1_destroy(win->deco); win->deco = NULL; }
    xdg_toplevel_destroy(win->top);
    xdg_surface_destroy(win->xsurf);
    win->top = NULL;
    win->xsurf = NULL;
    wl_surface_attach(win->surface, NULL, 0, 0);
    wl_surface_commit(win->surface);
    wl_display_flush(win->dpy);
}

int webwin_visible(WebWin *win) { return win->shown; }
int webwin_focused(WebWin *win) { return win->focused; }

void webwin_set_title(WebWin *win, const char *title) {
    snprintf(win->title, sizeof win->title, "%s", title && title[0] ? title : win->w->app.name);
    if (win->top) xdg_toplevel_set_title(win->top, win->title);
    if (win->back) webback_set_title(win->back, win->title);
}

/* ---- the clipboard ---- */

static char *pb_read(WebWin *win, const char *type) { return webback_pb_read(win->back, type); }
static void pb_write(WebWin *win, const char *text, size_t len) { webback_pb_write_text(win->back, text, len); }

static gboolean out_write(gint fd, GIOCondition c, gpointer d) {
    Outgoing *o = d;
    if (!(c & G_IO_ERR) && !(c & G_IO_HUP)) {
        while (o->off < o->len) {
            ssize_t k = write(fd, o->data + o->off, o->len - o->off);
            if (k < 0 && errno == EINTR) continue;
            if (k < 0 && errno == EAGAIN) return G_SOURCE_CONTINUE;
            if (k <= 0) break;
            o->off += (size_t)k;
        }
    }
    close(fd);
    g_free(o->data);
    g_free(o);
    return G_SOURCE_REMOVE;
}

static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)s;
    WebWin *win = d;
    const void *data = NULL;
    gsize len = 0;
    if (win->own_img) {
        if (win->own_img_mime && !strcmp(mime, win->own_img_mime)) data = g_bytes_get_data(win->own_img, &len);
    } else {
        const char *t = !strncmp(mime, "text/html", 9) ? win->own_html : win->own_text;
        if (t) { data = t; len = strlen(t); }
    }
    if (!data) { close(fd); return; }
    Outgoing *o = g_new0(Outgoing, 1);
    o->data = g_memdup2(data, len);
    o->len = len;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_unix_fd_add(fd, G_IO_OUT | G_IO_ERR | G_IO_HUP, out_write, o);
}
static void src_cancelled(void *d, struct wl_data_source *s) {
    WebWin *win = d;
    if (win->source == s) win->source = NULL;
    wl_data_source_destroy(s);
}
static void src_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_listener = { src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_finished, src_action };

static void clear_own(WebWin *win) {
    g_free(win->own_text);
    g_free(win->own_html);
    win->own_text = win->own_html = NULL;
    if (win->own_img) g_bytes_unref(win->own_img);
    win->own_img = NULL;
    g_free(win->own_img_mime);
    win->own_img_mime = NULL;
}

/* our own selection, with what own_* holds */
static void new_source(WebWin *win) {
    if (win->source) wl_data_source_destroy(win->source);
    win->source = wl_data_device_manager_create_data_source(win->ddm);
    wl_data_source_add_listener(win->source, &src_listener, win);
    if (win->own_img) wl_data_source_offer(win->source, win->own_img_mime);
    else {
        static const char *const types[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" };
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) wl_data_source_offer(win->source, types[i]);
        if (win->own_html) wl_data_source_offer(win->source, "text/html");
    }
    wl_data_device_set_selection(win->dd, win->source, win->input_serial);
    wl_display_flush(win->dpy);
}

/* ---- the primary selection: the page's selection, offered to the session (webwin_set_primary) ---- */

static void psrc_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *mime, int32_t fd) {
    WebWin *win = d;
    (void)mime;
    if (s != win->psrc || !win->prim) { close(fd); return; }
    Outgoing *o = g_new0(Outgoing, 1);
    o->len = strlen(win->prim);
    o->data = g_memdup2(win->prim, o->len);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_unix_fd_add(fd, G_IO_OUT | G_IO_ERR | G_IO_HUP, out_write, o);
}
static void psrc_cancelled(void *d, struct zwp_primary_selection_source_v1 *s) {
    WebWin *win = d;
    if (win->psrc == s) { win->psrc = NULL; g_free(win->prim); win->prim = NULL; }
    zwp_primary_selection_source_v1_destroy(s);
}
static const struct zwp_primary_selection_source_v1_listener psrc_listener = { psrc_send, psrc_cancelled };

static void pdev_offer(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o) { (void)d; (void)dev; (void)o; }
static void pdev_selection(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o) {
    (void)dev;
    WebWin *win = d;
    if (win->poffer && win->poffer != o) zwp_primary_selection_offer_v1_destroy(win->poffer);
    win->poffer = o;
}
static const struct zwp_primary_selection_device_v1_listener pdev_listener = { pdev_offer, pdev_selection };

void webwin_set_primary(WebWin *win, const char *text) {
    if (!win->psm || !win->pdev || !win->input_serial || !text || !text[0]) return;
    if (win->psrc && win->prim && !strcmp(win->prim, text)) return;     /* the same selection again */
    if (win->psrc) zwp_primary_selection_source_v1_destroy(win->psrc);
    g_free(win->prim);
    win->prim = g_strdup(text);
    win->psrc = zwp_primary_selection_device_manager_v1_create_source(win->psm);
    zwp_primary_selection_source_v1_add_listener(win->psrc, &psrc_listener, win);
    static const char *const types[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) zwp_primary_selection_source_v1_offer(win->psrc, types[i]);
    zwp_primary_selection_device_v1_set_selection(win->pdev, win->psrc, win->input_serial);
    wl_display_flush(win->dpy);
}

void web_note(const char *fmt, ...) {
    static gint64 window_us;
    static int lines;
    gint64 now = g_get_monotonic_time();
    if (now - window_us > 10 * G_USEC_PER_SEC) { window_us = now; lines = 0; }
    if (++lines > 20) return;                       /* at most 20 lines in 10 seconds */
    va_list ap;
    va_start(ap, fmt);
    fputs("lord-web: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* the page copied something: offer it to the session (its HTML too, may be NULL) */
static void offer_own(WebWin *win, char *text, char *html, const char *how) {
    if (!win->ddm || !win->dd || !win->input_serial) {
        web_note("page copy refused (%zu bytes, %s): %s", text ? strlen(text) : 0, how, !win->input_serial ? "no key or click in the window yet" : "no data device");
        g_free(text);
        g_free(html);
        return;
    }
    clear_own(win);
    win->own_text = text;
    win->own_html = html;
    new_source(win);
    web_note("page copy offered (%zu bytes%s, %s)", strlen(text), html ? " and HTML" : "", how);
}

void webwin_offer(WebWin *win, const char *mime, GBytes *data) {
    gsize n = 0;
    const char *p = g_bytes_get_data(data, &n);
    if (!mime) {
        char *text = g_strndup(p, n);
        g_bytes_unref(data);
        pb_write(win, text, strlen(text));
        g_free(win->known);
        win->known = g_strdup(text);
        offer_own(win, text, NULL, "clipboard API");
        return;
    }
    if (!win->ddm || !win->dd || !win->input_serial || !n) { g_bytes_unref(data); return; }
    clear_own(win);
    win->own_img = data;
    win->own_img_mime = g_strdup(mime);
    new_source(win);
    web_note("page copy offered (%zu bytes, %s)", (size_t)n, mime);
}

void webwin_offer_text(WebWin *win, char *text, char *html) {
    if (!text) { g_free(html); return; }
    /* WebKit's pasteboard gets the text too: a paste back into the page pastes what was copied, and the timer that
     * watches the pasteboard doesn't offer the older text again */
    pb_write(win, text, strlen(text));
    g_free(win->known);
    win->known = g_strdup(text);
    offer_own(win, text, html, "copy event");
}

/* an editing command ran (the Edit menu, the context menu): WebKit writes its pasteboard a moment later, from the web
 * process, and the window may not have the keyboard (the menu had it), so it is looked at now rather than by the timer */
static gboolean pb_watch(gpointer d);

static gboolean pb_check_once(gpointer d) {
    WebWin *win = d;
    pb_watch(win);
    return G_SOURCE_REMOVE;
}

/* Ctrl+<letter> (with Shift) as the page would get it from the keyboard: the key that makes that letter in the
 * compositor's keymap, pressed and released. 0 when the keymap has no such key. */
int webwin_send_shortcut(WebWin *win, uint32_t keysym, int shift) { return win->back ? webback_shortcut(win->back, keysym, shift) : 0; }

void webwin_after_edit(WebWin *win) {
    g_timeout_add(150, pb_check_once, win);
    g_timeout_add(600, pb_check_once, win);
}

int webwin_user_active(WebWin *win) {
    return win->focused && win->last_input_us && g_get_monotonic_time() - win->last_input_us < USER_ACTIVE_US;
}

static gboolean pb_watch(gpointer d) {
    WebWin *win = d;
    char *t = pb_read(win, PB_TEXT);
    if (t && (!win->known || strcmp(t, win->known))) {
        g_free(win->known);
        win->known = g_strdup(t);
        offer_own(win, t, pb_read(win, PB_HTML), "editing command");
    } else g_free(t);
    return G_SOURCE_CONTINUE;
}

/* ---- reading what another client offers: a pipe, non-blocking, in the main loop, at most max bytes ---- */

/* data is NULL when the read failed or passed max (the offer pointer only to compare: it may be gone) */
typedef void (*ReadDone)(WebWin *win, struct wl_data_offer *o, GByteArray *data, void *d);
typedef struct { WebWin *win; struct wl_data_offer *offer; GByteArray *buf; size_t max; int over; ReadDone done; void *d; } Incoming;

static gboolean in_read(gint fd, GIOCondition c, gpointer d) {
    Incoming *in = d;
    char chunk[65536];
    if (c & G_IO_IN) {
        ssize_t k = read(fd, chunk, sizeof chunk);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
        if (k > 0 && in->buf->len + (size_t)k <= in->max) { g_byte_array_append(in->buf, (const guint8 *)chunk, (guint)k); return G_SOURCE_CONTINUE; }
        if (k != 0) in->over = 1;                       /* too big, or an error: nothing */
    }
    close(fd);
    in->done(in->win, in->offer, in->over ? NULL : in->buf, in->d);
    g_byte_array_unref(in->buf);
    g_free(in);
    return G_SOURCE_REMOVE;
}

static void read_offer(WebWin *win, struct wl_data_offer *o, const char *mime, size_t max, ReadDone done, void *d) {
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK)) { done(win, o, NULL, d); return; }
    wl_data_offer_receive(o, mime, p[1]);
    close(p[1]);
    wl_display_flush(win->dpy);
    Incoming *in = g_new0(Incoming, 1);
    in->win = win;
    in->offer = o;
    in->buf = g_byte_array_new();
    in->max = max;
    in->done = done;
    in->d = d;
    g_unix_fd_add(p[0], G_IO_IN | G_IO_HUP | G_IO_ERR, in_read, in);
}

static void sel_text_read(WebWin *win, struct wl_data_offer *o, GByteArray *b, void *d) {
    (void)d;
    if (!b || !b->len || o != win->sel_offer || !win->sel_mime) return;
    /* as UTF-8: a UTF-8 type only when it is valid, STRING and a text/plain that isn't UTF-8 from Latin-1 */
    size_t n = 0;
    char *t = webclip_text_utf8(win->sel_mime, (const char *)b->data, b->len, &n);
    if (!t) { web_note("clipboard text refused (%u bytes, %s): not UTF-8", b->len, win->sel_mime); return; }
    pb_write(win, t, n);
    g_free(win->known);
    win->known = g_strndup(t, n);
    free(t);
}

static void mirror_selection(WebWin *win) {
    if (!win->sel_offer || !win->sel_mime || win->source) return;
    read_offer(win, win->sel_offer, win->sel_mime, CLIP_MAX, sel_text_read, NULL);
}

static void sel_image_read(WebWin *win, struct wl_data_offer *o, GByteArray *b, void *d) {
    const char *mime = d;
    if (b && b->len && o == win->sel_offer) webxfer_paste_image(win->w, mime, g_bytes_new(b->data, b->len));
}

/* the clipboard's text as WebKit's pasteboard holds it (mirrored from the session, or the page's own copy): one that
 * isn't ASCII goes to the page from webxfer.c, since WebKit's paste would read its UTF-8 as Latin-1. Else WebKit's
 * own paste: 0 to let the key or the command go on, or with run, the Paste command now. 1 when it was pasted. */
static int paste_text(WebWin *win, int run) {
    const char *t = win->known;
    size_t n = t ? strlen(t) : 0;
    const char *type = win->source ? "the page's own copy" : win->sel_mime ? win->sel_mime : "WebKit's pasteboard";
    if (n && !webclip_webkit_reads(t, n) && webxfer_paste_text(win->w, t)) {
        web_note("paste: text (%zu bytes, %s)", n, type);
        return 1;
    }
    if (n) web_note("paste: text (%zu bytes, %s; WebKit's own paste)", n, type);
    if (!run) return 0;
    if (win->w->view) webkit_web_view_execute_editing_command(win->w->view, WEBKIT_EDITING_COMMAND_PASTE);
    return 1;
}

/* the session's file list, read at the person's paste: its local files (the same checks and limits as a drop's) to
 * the page; when none passes, its text as before */
static void paste_list_read(WebWin *win, struct wl_data_offer *o, GByteArray *b, void *d) {
    const char *mime = d;
    if (o != win->sel_offer) { web_note("paste refused: the clipboard changed while its file list was read"); return; }
    if (!b || !b->len) {
        web_note("paste: the clipboard's file list %s; pasting its text", b ? "is empty" : "could not be read, or it is too big");
        paste_text(win, 1);
        return;
    }
    const char *list = (const char *)b->data;
    size_t at = webclip_list_body(mime, list, b->len);
    int lines = webclip_list_count(list + at, b->len - at);
    WebDropFile files[WEBDROP_FILES_MAX];
    int n = webdrop_files(list + at, b->len - at, g_get_home_dir(), files, WEBDROP_FILES_MAX);
    static const char why[] = "not a local regular file in the home folder or a mount, hidden, too big, or past the 20";
    if (n) {
        web_note("paste: %d files from the clipboard accepted%s%s", n, n < lines ? "; others refused: " : "", n < lines ? why : "");
        webxfer_paste_files(win->w, files, n);
        return;
    }
    web_note("paste: no file from the clipboard accepted (%d on its list, %s); pasting its text", lines, why);
    if (win->sel_mime) { paste_text(win, 1); return; }
    /* a list and no text: the list is the text (text/plain), as a browser pastes it */
    char *t = webclip_utf8_valid(list + at, b->len - at) ? g_strndup(list + at, b->len - at) : NULL;
    if (t && t[0] && webxfer_paste_text(win->w, t)) web_note("paste: text (%zu bytes, %s)", strlen(t), mime);
    g_free(t);
}

/* The person pastes (a key, the Edit menu, the context menu): the session's file list, else its image when it has no
 * text, else its text (paste_text). Only from here: nothing is read from the clipboard for the page without the
 * person's paste. 1 when lord-web pastes it (the key or WebKit's command must not then). */
int webwin_paste(WebWin *win) {
    if (win->sel_offer && win->sel_list && !win->source) {
        read_offer(win, win->sel_offer, win->sel_list, WEBDROP_LIST_MAX, paste_list_read, (void *)win->sel_list);
        return 1;
    }
    if (win->sel_offer && win->sel_img && !win->sel_mime) {
        read_offer(win, win->sel_offer, win->sel_img, CLIP_IMAGE_MAX, sel_image_read, (void *)win->sel_img);
        return 1;
    }
    return paste_text(win, 0);
}

void webwin_paste_plain(WebWin *win) {
    const char *t = win->known;
    if (t && t[0] && webxfer_paste_text(win->w, t)) { web_note("paste as plain text (%zu bytes)", strlen(t)); return; }
    if (win->w->view) webkit_web_view_execute_editing_command(win->w->view, WEBKIT_EDITING_COMMAND_PASTE_AS_PLAIN_TEXT);
}

/* Ctrl+V or Shift+Insert: webwin_paste; Ctrl+Shift+V: webwin_paste_plain. 1 when lord-web pasted (the key doesn't go to WebKit). */
static int paste_key(WebWin *win, uint32_t key) {
    xkb_keysym_t sym = webback_keysym(win->back, key);
    if (!sym) return 0;
    int ctrl = (win->mods & WEBMOD_CTRL) != 0, alt = (win->mods & WEBMOD_ALT) != 0;
    int shift = (win->mods & WEBMOD_SHIFT) != 0;
    if (!((ctrl && !alt && (sym == XKB_KEY_v || sym == XKB_KEY_V)) || (shift && !ctrl && !alt && sym == XKB_KEY_Insert))) return 0;
    if (ctrl && shift) { webwin_paste_plain(win); return 1; }
    return webwin_paste(win);
}

/* each offer keeps the best text type it listed (webclip_text_rank), its image type, whether it has a text/uri-list
 * (a drag's files) and its best file list type (the clipboard's); the selection or enter event that follows names
 * the offer */
typedef struct { char *mime; int rank; const char *img; int uri; const char *list; } OfferInfo;

static void offer_mime(void *d, struct wl_data_offer *o, const char *m) {
    (void)o;
    OfferInfo *in = d;
    int r = webclip_text_rank(m);
    if (r > in->rank) { g_free(in->mime); in->mime = g_strdup(m); in->rank = r; }
    int l = webclip_list_rank(m);
    if (l > webclip_list_rank(in->list)) in->list = l == 2 ? WEBCLIP_URIS : WEBCLIP_GNOME;
    if (!strcmp(m, "image/png")) in->img = "image/png";
    else if (!strcmp(m, "image/jpeg") && !in->img) in->img = "image/jpeg";
    else if (!strcmp(m, "text/uri-list")) in->uri = 1;
}
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = { offer_mime, offer_actions, offer_action };

static void offer_free(struct wl_data_offer *o) {
    OfferInfo *in = wl_data_offer_get_user_data(o);
    if (in) { g_free(in->mime); g_free(in); }
    wl_data_offer_destroy(o);
}

static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; wl_data_offer_add_listener(o, &offer_listener, g_new0(OfferInfo, 1)); }

/* ---- drops: WPE's view has no drop target of its own, so lord-web is the target and webxfer.c hands the files over ---- */

static void dd_enter(void *d, struct wl_data_device *dd, uint32_t serial, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)dd;
    WebWin *win = d;
    if (win->drag_offer) offer_free(win->drag_offer);
    win->drag_offer = o;
    win->drag_mime = NULL;
    if (!o) return;
    OfferInfo *in = wl_data_offer_get_user_data(o);
    if (sf == win->surface && in && (in->uri || in->mime)) win->drag_mime = in->uri ? "text/uri-list" : in->mime;
    win->drag_x = wl_fixed_to_double(x);
    win->drag_y = wl_fixed_to_double(y);
    wl_data_offer_accept(o, serial, win->drag_mime);
    if (win->drag_mime) {
        win->drag_page = 1;
        webxfer_drag(win->w, win->drag_x, win->drag_y, in->uri);
    }
    if (win->ddm_ver >= 3) {
        uint32_t a = win->drag_mime ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
        wl_data_offer_set_actions(o, a, a);
    }
    wl_display_flush(win->dpy);
}
static void dd_leave(void *d, struct wl_data_device *dd) {
    (void)dd;
    WebWin *win = d;
    if (win->drag_page) webxfer_drag_leave(win->w);
    win->drag_page = 0;
    if (win->drag_offer) offer_free(win->drag_offer);
    win->drag_offer = NULL;
    win->drag_mime = NULL;
}
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)dd; (void)t;
    WebWin *win = d;
    win->drag_x = wl_fixed_to_double(x);
    win->drag_y = wl_fixed_to_double(y);
    if (win->drag_offer && win->drag_mime && win->drag_page) webxfer_drag(win->w, win->drag_x, win->drag_y, !strcmp(win->drag_mime, "text/uri-list"));
}

typedef struct { double x, y; int uri; char *mime; } DropAt;

static void drop_read(WebWin *win, struct wl_data_offer *o, GByteArray *b, void *d) {
    DropAt *at = d;
    if (b && win->ddm_ver >= 3) wl_data_offer_finish(o);
    offer_free(o);
    wl_display_flush(win->dpy);
    int given = 0;
    if (b && b->len) {
        WebDropFile files[WEBDROP_FILES_MAX];
        int lines = 0;
        for (guint i = 0; at->uri && i < b->len; i++) if (b->data[i] == '\n' || i + 1 == b->len) lines++;
        int n = at->uri ? webdrop_files((const char *)b->data, b->len, g_get_home_dir(), files, WEBDROP_FILES_MAX) : 0;
        if (at->uri) web_note("drop received (text/uri-list, %d lines, %d local files accepted%s)", lines, n,
                              n < lines ? "; the others refused: not a regular file in the home folder or a mount, hidden, or too big" : "");
        else web_note("drop received (text, %u bytes)", b->len);
        if (n) { webxfer_drop(win->w, at->x, at->y, files, n, NULL, NULL); given = 1; }
        else {
            /* as UTF-8, like a pasted text (webclip_text_utf8); a list must be UTF-8 already */
            char *t = at->uri ? (g_utf8_validate((const char *)b->data, (gssize)b->len, NULL) ? g_strndup((const char *)b->data, b->len) : NULL)
                              : webclip_text_utf8(at->mime, (const char *)b->data, b->len, NULL);
            if (t) {
                webxfer_drop(win->w, at->x, at->y, NULL, 0, at->uri ? t : NULL, t);
                given = 1;
            } else web_note("drop refused: the text is not UTF-8");
            if (at->uri) g_free(t);
            else free(t);
        }
    } else web_note("drop refused: %s", b ? "nothing in it" : "it could not be read, or it is too big");
    if (!given) webxfer_drag_leave(win->w);          /* the page's drop area, if it showed one, goes away */
    g_free(at->mime);
    g_free(at);
}

static void dd_drop(void *d, struct wl_data_device *dd) {
    (void)dd;
    WebWin *win = d;
    struct wl_data_offer *o = win->drag_offer;
    const char *mime = win->drag_mime;
    win->drag_offer = NULL;                         /* the read owns it now: the leave that follows leaves it alone */
    win->drag_mime = NULL;
    win->drag_page = 0;                             /* and the page gets the drop, not dragleave */
    if (!o) return;
    if (!mime) { offer_free(o); web_note("drop refused: it offers neither files nor text"); return; }
    DropAt *at = g_new0(DropAt, 1);
    at->x = win->drag_x;
    at->y = win->drag_y;
    at->uri = !strcmp(mime, "text/uri-list");
    at->mime = g_strdup(mime);
    read_offer(win, o, mime, WEBDROP_LIST_MAX, drop_read, at);
}

static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)dd;
    WebWin *win = d;
    if (win->sel_offer && win->sel_offer != o) offer_free(win->sel_offer);
    win->sel_offer = o;
    g_free(win->sel_mime);
    win->sel_mime = NULL;
    win->sel_img = NULL;
    win->sel_list = NULL;
    if (!o) return;
    OfferInfo *in = wl_data_offer_get_user_data(o);
    if (in && in->mime) win->sel_mime = g_strdup(in->mime);
    if (in) { win->sel_img = in->img; win->sel_list = in->list; }
    if (!win->sel_mime && !win->source) {
        /* no text (an image, a file list only): WebKit's pasteboard must not paste the text copied before it */
        pb_write(win, "", 0);
        g_free(win->known);
        win->known = NULL;
    }
    mirror_selection(win);
}
static const struct wl_data_device_listener dd_listener = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* ---- the input method: WebKit's input method context on text-input-v3 ---- */

#define LORD_TYPE_WEB_IME (lord_web_ime_get_type())
G_DECLARE_FINAL_TYPE(LordWebIme, lord_web_ime, LORD, WEB_IME, WebKitInputMethodContext)
struct _LordWebIme {
    WebKitInputMethodContext parent;
    WebWin *win;
    int enabled;                        /* focus is in an editable element */
    char *preedit, *pend_preedit, *pend_commit;
    int preedit_cursor, pend_cursor, has_pend_preedit;
};
G_DEFINE_TYPE(LordWebIme, lord_web_ime, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static void ime_send_state(LordWebIme *ime) {
    WebWin *win = ime->win;
    if (!win->ti) return;
    if (ime->enabled && win->focused) {
        zwp_text_input_v3_enable(win->ti);
        zwp_text_input_v3_set_content_type(win->ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    } else zwp_text_input_v3_disable(win->ti);
    zwp_text_input_v3_commit(win->ti);
    wl_display_flush(win->dpy);
}

static void ime_focus_in(WebKitInputMethodContext *c) { LordWebIme *ime = LORD_WEB_IME(c); ime->enabled = 1; ime_send_state(ime); }
static void ime_focus_out(WebKitInputMethodContext *c) { LordWebIme *ime = LORD_WEB_IME(c); ime->enabled = 0; ime_send_state(ime); }
static void ime_cursor_area(WebKitInputMethodContext *c, int x, int y, int w, int h) {
    LordWebIme *ime = LORD_WEB_IME(c);
    WebWin *win = ime->win;
    if (!win->ti || !ime->enabled) return;
    double s = win->scale > 0 ? win->scale : 1;
    zwp_text_input_v3_set_cursor_rectangle(win->ti, (int)(x / s), (int)(y / s), (int)(w / s) > 0 ? (int)(w / s) : 1, (int)(h / s) > 0 ? (int)(h / s) : 1);
    zwp_text_input_v3_commit(win->ti);
}
static void ime_surrounding(WebKitInputMethodContext *c, const gchar *text, guint length, guint cursor, guint anchor) {
    LordWebIme *ime = LORD_WEB_IME(c);
    WebWin *win = ime->win;
    if (!win->ti || !ime->enabled || !text) return;
    /* the protocol limits the text to 4000 bytes: send a window around the cursor */
    if (length > 3900 || cursor > length || anchor > length) return;
    char *t = g_strndup(text, length);
    zwp_text_input_v3_set_surrounding_text(win->ti, t, (int32_t)cursor, (int32_t)anchor);
    zwp_text_input_v3_commit(win->ti);
    g_free(t);
}
static void ime_get_preedit(WebKitInputMethodContext *c, gchar **text, GList **underlines, guint *cursor) {
    LordWebIme *ime = LORD_WEB_IME(c);
    const char *p = ime->preedit ? ime->preedit : "";
    if (text) *text = g_strdup(p);
    if (underlines) {
        *underlines = NULL;
        glong n = g_utf8_strlen(p, -1);
        if (n) *underlines = g_list_prepend(NULL, webkit_input_method_underline_new(0, (guint)n));
    }
    if (cursor) *cursor = (guint)ime->preedit_cursor;
}
static gboolean ime_filter(WebKitInputMethodContext *c, gpointer e) { (void)c; (void)e; return FALSE; }
static void ime_reset(WebKitInputMethodContext *c) { (void)c; }

static void lord_web_ime_finalize(GObject *o) {
    LordWebIme *ime = LORD_WEB_IME(o);
    g_free(ime->preedit);
    g_free(ime->pend_preedit);
    g_free(ime->pend_commit);
    G_OBJECT_CLASS(lord_web_ime_parent_class)->finalize(o);
}
static void lord_web_ime_class_init(LordWebImeClass *k) {
    WebKitInputMethodContextClass *c = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(k);
    c->notify_focus_in = ime_focus_in;
    c->notify_focus_out = ime_focus_out;
    c->notify_cursor_area = ime_cursor_area;
    c->notify_surrounding = ime_surrounding;
    c->get_preedit = ime_get_preedit;
    c->filter_key_event = ime_filter;
    c->reset = ime_reset;
    G_OBJECT_CLASS(k)->finalize = lord_web_ime_finalize;
}
static void lord_web_ime_init(LordWebIme *ime) { (void)ime; }

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)t; (void)s; WebWin *win = d; if (win->ime) ime_send_state(win->ime); }
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)d; (void)s; zwp_text_input_v3_disable(t); zwp_text_input_v3_commit(t); }
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text, int32_t begin, int32_t end) {
    (void)t; (void)end;
    LordWebIme *ime = ((WebWin *)d)->ime;
    if (!ime) return;
    g_free(ime->pend_preedit);
    ime->pend_preedit = g_strdup(text ? text : "");
    ime->pend_cursor = begin >= 0 && text && (size_t)begin <= strlen(text) ? (int)g_utf8_strlen(text, begin) : 0;
    ime->has_pend_preedit = 1;
}
static void ti_commit(void *d, struct zwp_text_input_v3 *t, const char *text) {
    (void)t;
    LordWebIme *ime = ((WebWin *)d)->ime;
    if (!ime) return;
    g_free(ime->pend_commit);
    ime->pend_commit = g_strdup(text ? text : "");
}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t before, uint32_t after) { (void)d; (void)t; (void)before; (void)after; }
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial) {
    (void)t; (void)serial;
    LordWebIme *ime = ((WebWin *)d)->ime;
    if (!ime) return;
    WebKitInputMethodContext *c = WEBKIT_INPUT_METHOD_CONTEXT(ime);
    const char *now = ime->has_pend_preedit ? ime->pend_preedit : "";
    int had = ime->preedit && ime->preedit[0];
    if (had && !now[0]) {
        g_free(ime->preedit);
        ime->preedit = NULL;
        g_signal_emit_by_name(c, "preedit-changed");
        g_signal_emit_by_name(c, "preedit-finished");
    }
    if (ime->pend_commit && ime->pend_commit[0]) g_signal_emit_by_name(c, "committed", ime->pend_commit);
    if (now[0]) {
        if (!had) g_signal_emit_by_name(c, "preedit-started");
        g_free(ime->preedit);
        ime->preedit = g_strdup(now);
        ime->preedit_cursor = ime->pend_cursor;
        g_signal_emit_by_name(c, "preedit-changed");
    }
    g_free(ime->pend_commit);
    ime->pend_commit = NULL;
    g_free(ime->pend_preedit);
    ime->pend_preedit = NULL;
    ime->has_pend_preedit = 0;
}
/* designated: newer protocol headers add events (text-input-v3 2's action) lord-web never binds */
static const struct zwp_text_input_v3_listener ti_listener = {
    .enter = ti_enter, .leave = ti_leave, .preedit_string = ti_preedit, .commit_string = ti_commit,
    .delete_surrounding_text = ti_delete, .done = ti_done,
};

/* ---- keyboard ---- */

static void send_key(WebWin *win, uint32_t time, uint32_t key, int pressed) { webback_key(win->back, time, key, pressed, win->mods); }

static gboolean repeat_tick(gpointer d) {
    WebWin *win = d;
    send_key(win, (uint32_t)(g_get_monotonic_time() / 1000), win->repeat_key, 1);
    return G_SOURCE_CONTINUE;
}
static gboolean repeat_start(gpointer d) {
    WebWin *win = d;
    send_key(win, (uint32_t)(g_get_monotonic_time() / 1000), win->repeat_key, 1);
    win->repeat_src = g_timeout_add(1000 / (guint)win->repeat_rate, repeat_tick, win);
    return G_SOURCE_REMOVE;
}
static void repeat_stop(WebWin *win) {
    if (win->repeat_src) g_source_remove(win->repeat_src);
    win->repeat_src = 0;
}

/* lord-web's own keys: reload, history, zoom. 1 when the key was one of them. */
static int own_key(WebWin *win, uint32_t key) {
    xkb_keysym_t sym = webback_keysym(win->back, key);
    if (!sym) return 0;
    int ctrl = (win->mods & WEBMOD_CTRL) != 0, alt = (win->mods & WEBMOD_ALT) != 0;
    WebKitWebView *v = win->w->view;
    if (sym == XKB_KEY_F5 || (ctrl && !alt && (sym == XKB_KEY_r || sym == XKB_KEY_R))) { web_reload(win->w); return 1; }
    if (alt && !ctrl && sym == XKB_KEY_Left) { webkit_web_view_go_back(v); return 1; }
    if (alt && !ctrl && sym == XKB_KEY_Right) { webkit_web_view_go_forward(v); return 1; }
    if (ctrl && !alt) {
        double z = webkit_web_view_get_zoom_level(v);
        if (sym == XKB_KEY_plus || sym == XKB_KEY_equal || sym == XKB_KEY_KP_Add) { webkit_web_view_set_zoom_level(v, z < 3 ? z + 0.1 : 3); return 1; }
        if (sym == XKB_KEY_minus || sym == XKB_KEY_KP_Subtract) { webkit_web_view_set_zoom_level(v, z > 0.4 ? z - 0.1 : 0.3); return 1; }
        if (sym == XKB_KEY_0 || sym == XKB_KEY_KP_0) { webkit_web_view_set_zoom_level(v, 1); return 1; }
    }
    return 0;
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)k;
    WebWin *win = d;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    webback_keymap(win->back, map, strnlen(map, size));
    munmap(map, size);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)k; (void)s; (void)keys;
    WebWin *win = d;
    win->focused = 1;
    win->input_serial = serial;
    webback_set_focused(win->back, 1);
    if (!win->pb_timer) win->pb_timer = g_timeout_add(400, pb_watch, win);
    if (win->ime && win->ime->enabled) ime_send_state(win->ime);
    web_menu_ai_focused(win->w);        /* ✨ AI's Replace waits for the keyboard (webmenu.c) */
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)k; (void)serial; (void)s;
    WebWin *win = d;
    win->focused = 0;
    repeat_stop(win);
    webback_set_focused(win->back, 0);
    if (win->pb_timer) { pb_watch(win); g_source_remove(win->pb_timer); win->pb_timer = 0; }
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)k;
    WebWin *win = d;
    int pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (pressed) { win->input_serial = serial; win->last_input_us = g_get_monotonic_time(); trace_input(win); }
    if (pressed && (own_key(win, key) || paste_key(win, key))) return;
    send_key(win, time, key, pressed);
    if (pressed) {
        repeat_stop(win);
        if (win->repeat_rate > 0 && webback_key_repeats(win->back, key)) {
            win->repeat_key = key;
            win->repeat_src = g_timeout_add((guint)win->repeat_delay, repeat_start, win);
        }
    } else if (key == win->repeat_key) repeat_stop(win);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)k; (void)serial;
    WebWin *win = d;
    win->mods = webback_modifiers(win->back, dep, lat, lock, group);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)k;
    WebWin *win = d;
    win->repeat_rate = rate > 0 && rate <= 100 ? rate : 0;
    win->repeat_delay = delay > 0 && delay < 5000 ? delay : 400;
}
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

/* ---- pointer ---- */

static void set_cursor(WebWin *win, int shape) {
    win->cursor_shape = shape;
    if (win->cursor_dev && win->enter_serial) wp_cursor_shape_device_v1_set_shape(win->cursor_dev, win->enter_serial, (uint32_t)shape);
}
static void set_cursor_name(WebWin *win, int shape) { set_cursor(win, shape); }

void webwin_set_cursor_hint(WebWin *win, int link, int text) {
    if (win->cursor_named) return;
    int shape = link ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : text ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (shape != win->cursor_shape) set_cursor(win, shape);
}

static void send_motion(WebWin *win, uint32_t time) { webback_motion(win->back, time, win->px, win->py, win->mods | win->buttons); }

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)s;
    WebWin *win = d;
    win->enter_serial = serial;
    win->px = wl_fixed_to_double(x);
    win->py = wl_fixed_to_double(y);
    set_cursor(win, win->cursor_shape ? win->cursor_shape : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    send_motion(win, 0);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)p; (void)serial; (void)s; ((WebWin *)d)->buttons = 0; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    WebWin *win = d;
    win->px = wl_fixed_to_double(x);
    win->py = wl_fixed_to_double(y);
    send_motion(win, time);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)p;
    WebWin *win = d;
    win->input_serial = serial;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) { win->last_input_us = g_get_monotonic_time(); trace_input(win); }
    int b = button == BTN_LEFT ? WEBBTN_LEFT : button == BTN_RIGHT ? WEBBTN_RIGHT : button == BTN_MIDDLE ? WEBBTN_MIDDLE : 0;
    if (!b) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_SIDE) webkit_web_view_go_back(win->w->view);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_EXTRA) webkit_web_view_go_forward(win->w->view);
        return;
    }
    uint32_t mask = b == WEBBTN_LEFT ? WEBMOD_BTN_LEFT : b == WEBBTN_RIGHT ? WEBMOD_BTN_RIGHT : WEBMOD_BTN_MIDDLE;
    int pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed) win->buttons |= mask; else win->buttons &= ~mask;
    webback_button(win->back, time, win->px, win->py, b, pressed, win->mods | win->buttons);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {
    (void)p;
    WebWin *win = d;
    win->axis_time = time;
    webaxis_value(&win->axis, axis, wl_fixed_to_double(v));
}
/* the frame ends the axis events that belong together (webaxis.c decides what they make) */
static void ptr_frame(void *d, struct wl_pointer *p) {
    (void)p;
    WebWin *win = d;
    WebAxisOut out[2];
    int n = webaxis_frame(&win->axis, win->scale, out);
    if (n) webback_axis(win->back, win->axis_time, win->px, win->py, out, n, win->mods);
}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)p; webaxis_source(&((WebWin *)d)->axis, s); }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)p;
    WebWin *win = d;
    win->axis_time = t;
    webaxis_stop(&win->axis, a);
}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) { (void)p; webaxis_discrete(&((WebWin *)d)->axis, axis, v); }
static void ptr_axis_120(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) { (void)p; webaxis_value120(&((WebWin *)d)->axis, axis, v); }
static void ptr_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t r) { (void)d; (void)p; (void)a; (void)r; }
/* designated: newer libwayland adds events (wl_pointer 11's warp) of versions lord-web never binds (wl_seat 8 at most) */
static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_120,
    .axis_relative_direction = ptr_axis_rel,
};

/* ---- touch ---- */

static void touch_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)s;
    WebWin *win = d;
    win->input_serial = serial;
    win->last_input_us = g_get_monotonic_time();
    webback_touch(win->back, WEBTOUCH_DOWN, id, time, wl_fixed_to_double(x), wl_fixed_to_double(y), win->mods);
}
static void touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
    (void)t; (void)serial;
    WebWin *win = d;
    webback_touch(win->back, WEBTOUCH_UP, id, time, 0, 0, win->mods);
}
static void touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t;
    WebWin *win = d;
    webback_touch(win->back, WEBTOUCH_MOTION, id, time, wl_fixed_to_double(x), wl_fixed_to_double(y), win->mods);
}
static void touch_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void touch_cancel(void *d, struct wl_touch *t) {
    (void)t;
    WebWin *win = d;
    webback_touch(win->back, WEBTOUCH_CANCEL, -1, 0, 0, 0, win->mods);
}
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b) { (void)d; (void)t; (void)id; (void)a; (void)b; }
static void touch_orient(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) { (void)d; (void)t; (void)id; (void)o; }
static const struct wl_touch_listener touch_listener = { touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orient };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    WebWin *win = d;
    win->seat_caps = caps;
    if (!win->back) return;                         /* webwin_attach calls this again */
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !win->kbd) { win->kbd = wl_seat_get_keyboard(s); wl_keyboard_add_listener(win->kbd, &kb_listener, win); }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !win->ptr) {
        win->ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(win->ptr, &ptr_listener, win);
        if (win->cursor_mgr) win->cursor_dev = wp_cursor_shape_manager_v1_get_pointer(win->cursor_mgr, win->ptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !win->touch) { win->touch = wl_seat_get_touch(s); wl_touch_add_listener(win->touch, &touch_listener, win); }
}

/* ---- setup ---- */

WebWin *webwin_new(Web *w) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return NULL;
    WebWin *win = calloc(1, sizeof *win);
    if (!win) { wl_display_disconnect(dpy); return NULL; }
    win->w = w;
    win->dpy = dpy;
    win->scale = 1;
    win->width = DEFAULT_W;
    win->height = DEFAULT_H;
    win->repeat_delay = 400;
    win->trace = getenv("LORD_WEB_TRACE") != NULL;
    webaxis_reset(&win->axis);
    win->reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(win->reg, &reg_listener, win);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!win->comp || !win->wm || !win->shm) {
        fprintf(stderr, "lord-web: the compositor has no xdg-shell\n");
        wl_display_disconnect(dpy);
        free(win);
        return NULL;
    }
    win->surface = wl_compositor_create_surface(win->comp);
    wl_surface_add_listener(win->surface, &surf_listener, win);
    if (win->viewporter) win->viewport = wp_viewporter_get_viewport(win->viewporter, win->surface);
    if (win->viewporter && win->frac_mgr) {
        win->frac = wp_fractional_scale_manager_v1_get_fractional_scale(win->frac_mgr, win->surface);
        wp_fractional_scale_v1_add_listener(win->frac, &frac_listener, win);
    }
    WlSource *ws = (WlSource *)g_source_new(&wl_funcs, sizeof *ws);
    ws->win = win;
    ws->tag = g_source_add_unix_fd(&ws->s, wl_display_get_fd(dpy), G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_set_priority(&ws->s, G_PRIORITY_DEFAULT);
    g_source_attach(&ws->s, NULL);
    win->src = &ws->s;
    w->win = win;
    return win;
}

/* EGL on the compositor's own display, for the backend's linux-dmabuf: 0 when it works. Also the GPU's render node
 * (EGL_EXT_device_query), which the WPEPlatform backend tells WebKit. */
static int egl_up(WebWin *win) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    win->egl = get ? get(EGL_PLATFORM_WAYLAND_KHR, win->dpy, NULL) : eglGetDisplay((EGLNativeDisplayType)win->dpy);
    if (win->egl == EGL_NO_DISPLAY || !eglInitialize(win->egl, NULL, NULL)) { win->egl = EGL_NO_DISPLAY; return -1; }
    const char *ext = eglQueryString(win->egl, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import")) { eglTerminate(win->egl); win->egl = EGL_NO_DISPLAY; return -1; }
    PFNEGLQUERYDISPLAYATTRIBEXTPROC qd = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qs = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLAttrib dev = 0;
    if (qd && qs && qd(win->egl, EGL_DEVICE_EXT, &dev) && dev) {
        const char *node = qs((EGLDeviceEXT)dev, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (node && !strncmp(node, "/dev/dri/renderD", 16) && strlen(node) < sizeof win->render_node) snprintf(win->render_node, sizeof win->render_node, "%s", node);
        const char *card = qs((EGLDeviceEXT)dev, EGL_DRM_DEVICE_FILE_EXT);
        if (card && !strncmp(card, "/dev/dri/card", 13) && strlen(card) < sizeof win->primary_node) snprintf(win->primary_node, sizeof win->primary_node, "%s", card);
    }
    return 0;
}

static void egl_down(WebWin *win) {
    if (win->egl != EGL_NO_DISPLAY && win->egl) eglTerminate(win->egl);
    win->egl = EGL_NO_DISPLAY;
    win->render_node[0] = win->primary_node[0] = 0;
}

int webwin_init_backend(WebWin *win) {
    const char *gpu = getenv("LORD_GPU");
    int try_gpu = win->dmabuf && !getenv("LORD_WEB_SHM") && !(gpu && !strcmp(gpu, "0"));
    WebBackConfig cfg = { .width = win->width, .height = win->height, .scale = win->scale };
    if (try_gpu && !egl_up(win)) {
        cfg.gpu = 1;
        cfg.egl_display = win->egl;
        cfg.render_node = win->render_node[0] ? win->render_node : NULL;
        cfg.primary_node = win->primary_node[0] ? win->primary_node : NULL;
        cfg.dmabuf_formats = win->dmabuf_formats;
        cfg.dmabuf_modifiers = win->dmabuf_modifiers;
        cfg.dmabuf_n = win->dmabuf_n;
        win->back = webback_new(&cfg, &sink, win);
        if (win->back) win->gpu = 1;
    }
    if (!win->back) {
        egl_down(win);
        WebBackConfig cpu = { .width = win->width, .height = win->height, .scale = win->scale };
        win->back = webback_new(&cpu, &sink, win);
        if (!win->back) return -1;
    }
    win->w->back = win->back;
    update_screen(win);
    fprintf(stderr, "lord-web: %s backend (WPE WebKit %u.%u.%u), %s\n", webback_name(), webkit_get_major_version(), webkit_get_minor_version(),
            webkit_get_micro_version(), win->gpu ? "dmabuf" : "shared memory");
    return 0;
}

static void ime_attach(WebWin *win) {
    if (!win->ti_mgr || !win->seat) return;
    win->ti = zwp_text_input_manager_v3_get_text_input(win->ti_mgr, win->seat);
    zwp_text_input_v3_add_listener(win->ti, &ti_listener, win);
    win->ime = g_object_new(LORD_TYPE_WEB_IME, NULL);
    win->ime->win = win;
    webkit_web_view_set_input_method_context(win->w->view, WEBKIT_INPUT_METHOD_CONTEXT(win->ime));
}

void webwin_attach(WebWin *win) {
    if (win->scale != 1) webback_set_scale(win->back, win->scale);
    if (win->seat) {
        seat_caps(win, win->seat, win->seat_caps);
        if (win->ddm) { win->dd = wl_data_device_manager_get_data_device(win->ddm, win->seat); wl_data_device_add_listener(win->dd, &dd_listener, win); }
        if (win->psm) { win->pdev = zwp_primary_selection_device_manager_v1_get_device(win->psm, win->seat); zwp_primary_selection_device_v1_add_listener(win->pdev, &pdev_listener, win); }
    }
    ime_attach(win);
    wl_display_flush(win->dpy);
}

void webwin_free(WebWin *win) {
    if (!win) return;
    repeat_stop(win);
    if (win->pb_timer) g_source_remove(win->pb_timer);
    if (win->src) { g_source_destroy(win->src); g_source_unref(win->src); }
    webback_free(win->back);
    win->w->back = NULL;
    wl_display_flush(win->dpy);
    wl_display_disconnect(win->dpy);
    free(win);
}
