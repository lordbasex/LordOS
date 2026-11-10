/* wl.c — LordOS's own Wayland compositor: Linux apps (GTK, Qt, foot…) open as LordOS
 * windows. It only uses libwayland-server (it serializes the protocol; the dependency policy
 * allows it since M1) and the code wayland-scanner generates for xdg-shell and xdg-decoration.
 *
 * Globals: wl_compositor 6, wl_subcompositor, wl_shm (from libwayland), wl_output 4, wl_seat 7
 * (pointer and keyboard with the XKB keymap from keyboard.c), wl_data_device_manager 3 (clipboard
 * shared with LordOS's, and drag and drop), xdg_wm_base 6, zxdg_decoration_manager_v1, and for dialogs over their window
 * zxdg_exporter_v2/zxdg_importer_v2 (xdg-foreign) and xdg_wm_dialog_v1.
 *
 * Each surface copies its shared-memory buffer on commit (and releases it right away):
 * the UI draws it as one more node (NODE_SURFACE) inside the theme's window frame. Windows
 * with server-side decoration (Qt, foot) use the theme's frame; those that draw their
 * own (GTK) go without a frame and the shadow is clipped with their window geometry. Popup
 * menus (xdg_popup) are drawn on top of everything.
 *
 * X11 apps go through Xwayland (xwayland.c starts it on demand, xwm.c is the X window manager):
 * its X windows are surfaces of one more client, with the toplevel or popup role given here
 * (wlx_attach). Not there yet: drag and drop with X11 apps (XDND), text-input (LordOS input methods inside
 * Wayland apps).
 *
 * xdg_activation_v1: a window asks to come forward with a token; only one LordOS issued when it
 * launched the app, or one the app in front asked for, raises it; any other marks it as wanting
 * attention (xdgact.c holds the tokens). */
#include "wl.h"
#include "keygrab.h"
#include "../keylag.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-protocol.h"
#include "xdg-decoration-unstable-v1-protocol.h"
#include "linux-dmabuf-v1-protocol.h"
#include "presentation-time-protocol.h"
#include "viewporter-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "cursor-shape-v1-protocol.h"
#include "fractional-scale-v1-protocol.h"
#include "idle-inhibit-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "primary-selection-unstable-v1-protocol.h"
#include "xdg-activation-v1-protocol.h"
#include "xdgact.h"
#include "dndgate.h"
#include "urilist.h"
#include "tiutf8.h"
#include "xdg-foreign-unstable-v2-protocol.h"
#include "xdg-dialog-v1-protocol.h"
#include "xwm.h"
#include "xwayland.h"
#include "../outputs.h"
#include "../cursor.h"
#include "ptrlock.h"
#include "wl_input.h"
#include "../gpu/gpu.h"
#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include "../term/term.h"
#include "../ui/ui.h"
#include "../sec/broker.h"
#include "wlregion.h"
#include "urilist.h"

enum { R_NONE, R_TOPLEVEL, R_POPUP, R_SUB, R_CURSOR, R_DNDICON };

/* An app's dmabuf buffer (zwp_linux_dmabuf_v1). Refcounted: 1 for the wl_buffer, 1 for the
 * surface showing it and 1 while it is on screen via direct scanout. */
typedef struct Dmabuf {
    GpuDmabuf d;
    struct wl_resource *res;            /* NULL if the app already destroyed it */
    GpuTex *tex;                        /* EGLImage (GPU path); stale once its context is gone */
    int refs, scanned;
    unsigned no_import;                 /* the GPU context (generation) where EGL refused it: mapped and copied */
} Dmabuf;

typedef struct Surf Surf;
struct Surf {
    struct wl_resource *res;
    struct wl_client *client;
    int id;
    /* pending until commit */
    struct wl_resource *p_buf;
    struct wl_listener p_buf_destroy;
    int p_attached, p_gx, p_gy, p_gw, p_gh, p_geom;
    struct wl_list p_frames, frames;
    /* current */
    uint32_t *px;
    int w, h, opaque;
    int role;
    struct wl_resource *xdg, *top, *popup, *deco, *sub;
    int sent_configure, mapped, conf_w, conf_h;
    int gx, gy, gw, gh, has_geom;
    char title[256], app_id[128];
    int maximized, fullscreen, activated, csd;
    int min_w, min_h, max_w, max_h;             /* xdg_toplevel: its size limits */
    /* the window it belongs to: xdg_toplevel.set_parent, or xdg-foreign's set_parent_of (owner_by =
     * the zxdg_imported_v2 that set it); modal (xdg-dialog-v1): the owner takes no input while it is up */
    Surf *owner;
    void *owner_by;
    struct wl_resource *dlg;
    int modal;
    int kde_ssd;                    /* asked for a server frame via org_kde_kwin_server_decoration (GTK 3) */
    struct wl_resource *kde;
    /* popup menu */
    Surf *parent;
    int pop_x, pop_y, grab;
    /* subsurface */
    int sx, sy, p_sx, p_sy;
    Surf *kids[16];
    int nkids;
    int node;                   /* UI node that shows it (windows) */
    /* content: shm buffer copied into px, or dmabuf in use (GPU path) */
    int content;
    Dmabuf *dmabuf;
    GpuTex *tex;                /* uploaded px (GPU path) */
    int tex_dirty;
    int lw, lh;                 /* logical size (viewporter; otherwise the buffer's) */
    /* wp_viewporter: pending and current (vsw < 0 = no crop, vdw <= 0 = no scaling) */
    struct wl_resource *viewport;
    float p_vsx, p_vsy, p_vsw, p_vsh, vsx, vsy, vsw, vsh;
    int p_vdw, p_vdh, vdw, vdh, p_vp;
    /* wp_presentation */
    struct wl_list p_feedback, feedback;
    int cursor_hx, cursor_hy, p_hx, p_hy;   /* cursor role: hotspot */
    int dnd_x, dnd_y, p_dnd_x, p_dnd_y;     /* drag icon role: where it goes from the pointer (attach and offset add up) */
    unsigned hole_sig;          /* GPU path: shape of the hole the UI left last time */
    struct wl_list sent_feedback;   /* presentation: already in a frame, flip pending */
    int fs_saved_w, fs_saved_h;     /* size before fullscreen */
    uint32_t conf_serial, acked_serial;   /* xdg_surface: the last configure sent, the last one acked */
    int parked, fs_resume;          /* on a hidden desktop (wl_set_parked); fullscreen waits for its return */
    /* HiDPI (docs/DISPLAYS.md): wl_surface.set_buffer_scale, the displays it is on (wl_surface
     * enter/leave, bit = output slot) and the scale it should render at (fractional-scale-v1,
     * preferred_buffer_scale) */
    int bscale, p_bscale;
    struct wl_resource *frac;
    unsigned outmask;
    int pref_scale;
    int logged_ratio;               /* buffer px per logical px ×100, last logged (docs/DISPLAYS.md) */
    /* an X11 window (Xwayland): its window manager record, and for a menu or tooltip its place */
    XWin *xw;
    int xpop, xx, xy, xmin;
    char app_alt[128];              /* X11: the WM_CLASS instance (the class is app_id) */
    /* wl_surface.set_input_region: without one the whole surface takes input */
    WlRgn input, p_input;
    int has_input, p_input_set, p_has_input;
};

/* Shape of a surface's hole in the UI: if it changes, the UI must be repainted. */
static unsigned hole_sig(const Surf *s) {
    return (unsigned)s->lw * 2654435761u ^ (unsigned)s->lh * 40503u ^ (unsigned)(s->gx + 7 * s->gy) * 97u ^
           (unsigned)(s->gw * 13 + s->gh) * 31u ^ (unsigned)(s->sx * 5 + s->sy) ^ (unsigned)s->content << 30;
}

#define MAX_SURF 256
static Surf *surfs[MAX_SURF];
static int next_id = 1;

static struct wl_display *dpy;
static struct wl_client *xwl_client;    /* Xwayland (X11 apps), or NULL */
static struct wl_event_loop *loop;
static char socket_name[64];

/* per-client resources */
static struct wl_list pointers, keyboards, data_devices, outputs;

static Surf *pointer_focus, *key_focus;
static int buttons_held;
static float last_x, last_y, press_x, press_y;     /* where it was pressed: base for move/resize */
static uint32_t mods_sent[4];

/* visible popup menus, bottom to top */
static Surf *popups[32];
static int npopups;

/* GPU path: what is drawn below (layer 0) and above (layer 1) the UI, in order */
typedef struct { int sid; float x, y; Rect clip; float op; float sx, sy; } Draw;   /* sx, sy: a transformed window's scale */
static Draw draws[2][128];
static int ndraws[2];
static int need_frame;              /* an app changed its content: must present */
static Dmabuf *to_release[64];      /* replaced: released after the next frame */
static int nrelease;

/* fullscreen (presented by the compositor, on top of everything) */
static int oslots_used(int slot);
static void oslot_rect(int slot, float *x, float *y, float *w, float *h);
static Surf *fs_surf;
static int fs_slot = -1;                /* the display it covers (output slot), -1 = the whole space */

/* Logical rect of the display the fullscreen window covers. */
static void fs_rect(float *x, float *y, float *w, float *h) {
    if (fs_slot >= 0 && fs_slot < OUT_MAX && oslots_used(fs_slot)) { oslot_rect(fs_slot, x, y, w, h); return; }
    *x = 0; *y = 0; *w = ui.width; *h = ui.height;
}

/* cursor of the app with the pointer: 0 LordOS arrow, 1 app image, 2 hidden */
static Surf *cursor_surf;
static int cursor_mode;
static unsigned cursor_gen;

/* zwp_pointer_constraints / zwp_relative_pointer */
static struct wl_list rel_pointers;
typedef struct { struct wl_resource *res; Surf *surf; int lock, active, oneshot, spent; } Constraint;
static Constraint *constraint;      /* the only active (or pending) one */
static void constraint_update(void);
static void constraint_escape(void);
static dev_t drm_dev;
/* linux-dmabuf feedback objects (re-sent when the GPU comes or goes), the signature of the table
 * last sent, and the formats the last GPU context imported (accepted off the GPU, see dmabuf_usable) */
static struct wl_list feedbacks;
static uint64_t feedback_sig;
static GpuFormat gpu_fmt_cache[256];
static int ngpu_fmt;

static unsigned now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ---- events for JS ---- */

typedef struct { int id; char type[16]; char json[600]; } Ev;
static Ev evq[64];
static int nev;

static void json_str(char *out, size_t size, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, size - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void emit(Surf *s, const char *type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void emit(Surf *s, const char *type, const char *fmt, ...) {
    if (nev == 64) return;
    Ev *e = &evq[nev++];
    e->id = s->id;
    snprintf(e->type, sizeof e->type, "%s", type);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->json, sizeof e->json, fmt, ap);
    va_end(ap);
}

/* An event that belongs to no window (id 0: LordOS's own, such as files an app dropped on it). */
static void emit_id(int id, const char *type, const char *json) {
    if (nev == 64) return;
    Ev *e = &evq[nev++];
    e->id = id;
    snprintf(e->type, sizeof e->type, "%s", type);
    snprintf(e->json, sizeof e->json, "%s", json);
}

void wl_events(WlEventFn fn) {
    for (int i = 0; i < nev; i++) fn(evq[i].id, evq[i].type, evq[i].json);
    nev = 0;
}

/* ---- utilities ---- */

static Surf *find(int id) {
    for (int i = 0; i < MAX_SURF; i++) if (surfs[i] && surfs[i]->id == id) return surfs[i];
    return NULL;
}

/* what cast.c asked to be told of (wl_content_track) */
static int content_on;
static Box content_box;

static void content_note(Box b) {
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    if (content_box.x1 <= content_box.x0 || content_box.y1 <= content_box.y0) { content_box = b; return; }
    if (b.x0 < content_box.x0) content_box.x0 = b.x0;
    if (b.y0 < content_box.y0) content_box.y0 = b.y0;
    if (b.x1 > content_box.x1) content_box.x1 = b.x1;
    if (b.y1 > content_box.y1) content_box.y1 = b.y1;
}

void wl_content_track(int on) { content_on = on; content_box = (Box){ 0, 0, 0, 0 }; }

int wl_content_take(float *x0, float *y0, float *x1, float *y1) {
    if (content_box.x1 <= content_box.x0 || content_box.y1 <= content_box.y0) return 0;
    *x0 = content_box.x0; *y0 = content_box.y0; *x1 = content_box.x1; *y1 = content_box.y1;
    content_box = (Box){ 0, 0, 0, 0 };
    return 1;
}

static void damage_node(Surf *s) {
    Surf *top = s;
    while (top->parent && top->role != R_TOPLEVEL) top = top->parent;
    if (content_on) {               /* a menu or a fullscreen app may be anywhere: all of it */
        Node *tn = top->node ? node_get(top->node) : NULL;
        if (top->role == R_POPUP || top == fs_surf || !tn) content_note((Box){ 0, 0, ui.width, ui.height });
        else content_note(tn->prev_box);
    }
    /* GPU path: if only the content changed, presenting again is enough */
    if (gpu_active() && (top->role == R_POPUP || top == fs_surf || hole_sig(s) == s->hole_sig)) { need_frame = 1; return; }
    Node *n = top->node ? node_get(top->node) : NULL;
    if (n && n->prev_box.x1 > n->prev_box.x0) damage_add(n->prev_box);
    ui.dirty = 1;
}

/* On-screen origin of a surface's buffer (window or menu). 0 if unknown. */
static int origin(Surf *s, float *x, float *y) {
    if (s == fs_surf) {
        float fx, fy, fw, fh;
        fs_rect(&fx, &fy, &fw, &fh);
        *x = fx - (float)s->gx;
        *y = fy - (float)s->gy;
        return 1;
    }
    if (s->role == R_TOPLEVEL) {
        Node *n = s->node ? node_get(s->node) : NULL;
        if (!n || n->prev_box.x1 <= n->prev_box.x0) return 0;
        *x = n->ax - s->gx;
        *y = n->ay - s->gy;
        return 1;
    }
    if (s->role == R_POPUP && s->xpop) {      /* an X11 menu or tooltip: where X put it */
        *x = (float)s->xx;
        *y = (float)s->xy;
        return 1;
    }
    if (s->role == R_POPUP && s->parent) {
        float px, py;
        if (!origin(s->parent, &px, &py)) return 0;
        *x = px + s->parent->gx + s->pop_x - s->gx;
        *y = py + s->parent->gy + s->pop_y - s->gy;
        return 1;
    }
    if (s->role == R_SUB && s->parent) {
        float px, py;
        if (!origin(s->parent, &px, &py)) return 0;
        *x = px + s->sx;
        *y = py + s->sy;
        return 1;
    }
    return 0;
}

static void popup_box_damage(Surf *s) {
    float x, y;
    if (gpu_active()) { need_frame = 1; return; }          /* menus go above the UI */
    if (s->lw > 0 && origin(s, &x, &y)) damage_add((Box){ x - 2, y - 2, x + s->lw + 2, y + s->lh + 2 });
    ui.dirty = 1;
}

/* resource of the same client in a resource list */
#define FOR_CLIENT(list, client, r) wl_resource_for_each(r, list) if (wl_resource_get_client(r) == (client))

/* ---- configure ---- */

/* An X11 window: its size (and in fullscreen its place) is set through the window manager. */
static void x_configure(Surf *s) {
    if (s == fs_surf && s->fullscreen) {
        float fx, fy, fw, fh;
        fs_rect(&fx, &fy, &fw, &fh);
        xwm_place(s->xw, (int)fx, (int)fy);
    }
    if (s->conf_w > 0 && s->conf_h > 0) xwm_resize(s->xw, s->conf_w, s->conf_h);
    xwm_set_state(s->xw, s->maximized, s->fullscreen, s->xmin);
    s->sent_configure = 1;
}

static void send_toplevel_configure(Surf *s) {
    if (s->xw) { x_configure(s); return; }
    if (!s->top || !s->xdg) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *p;
    if (s->maximized && (p = wl_array_add(&states, sizeof *p))) *p = XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (s->fullscreen && (p = wl_array_add(&states, sizeof *p))) *p = XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (s->activated && (p = wl_array_add(&states, sizeof *p))) *p = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (s->maximized && wl_resource_get_version(s->top) >= 2) {
        static const uint32_t tiled[] = { XDG_TOPLEVEL_STATE_TILED_LEFT, XDG_TOPLEVEL_STATE_TILED_RIGHT,
                                          XDG_TOPLEVEL_STATE_TILED_TOP, XDG_TOPLEVEL_STATE_TILED_BOTTOM };
        for (int i = 0; i < 4; i++) if ((p = wl_array_add(&states, sizeof *p))) *p = tiled[i];
    }
    xdg_toplevel_send_configure(s->top, s->conf_w, s->conf_h, &states);
    wl_array_release(&states);
    s->conf_serial = wl_display_next_serial(dpy);
    xdg_surface_send_configure(s->xdg, s->conf_serial);
    s->sent_configure = 1;
}

/* ---- wl_buffer → own pixels ---- */

static void buffer_destroyed(struct wl_listener *l, void *data) {
    (void)data;
    Surf *s = wl_container_of(l, s, p_buf_destroy);
    s->p_buf = NULL;
    wl_list_remove(&l->link);
    wl_list_init(&l->link);
}

static void copy_buffer(Surf *s, struct wl_resource *buf) {
    struct wl_shm_buffer *b = buf ? wl_shm_buffer_get(buf) : NULL;
    if (!b) {                               /* no buffer (or one that is not shared memory) */
        free(s->px);
        s->px = NULL;
        s->w = s->h = 0;
        s->content = 0;
        if (buf) wl_buffer_send_release(buf);
        return;
    }
    s->content = 1;
    int w = wl_shm_buffer_get_width(b), h = wl_shm_buffer_get_height(b), stride = wl_shm_buffer_get_stride(b);
    uint32_t fmt = wl_shm_buffer_get_format(b);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { wl_buffer_send_release(buf); return; }
    if (w != s->w || h != s->h || !s->px) {
        uint32_t *np = realloc(s->px, (size_t)w * (size_t)h * 4);
        if (!np) { wl_buffer_send_release(buf); return; }
        s->px = np;
        s->w = w;
        s->h = h;
    }
    s->opaque = fmt == WL_SHM_FORMAT_XRGB8888;
    wl_shm_buffer_begin_access(b);
    const uint8_t *d = wl_shm_buffer_get_data(b);
    for (int y = 0; y < h; y++) {
        uint32_t *dst = s->px + (size_t)y * w;
        memcpy(dst, d + (size_t)y * stride, (size_t)w * 4);
        if (s->opaque) for (int x = 0; x < w; x++) dst[x] |= 0xff000000u;
    }
    wl_shm_buffer_end_access(b);
    wl_buffer_send_release(buf);
}

/* ---- dmabuf ---- */

static void dm_unref(Dmabuf *d) {
    if (!d || --d->refs > 0) return;
    gpu_tex_free(d->tex);
    for (int i = 0; i < d->d.nplanes; i++) if (d->d.fd[i] >= 0) close(d->d.fd[i]);
    free(d);
}

static const struct wl_buffer_interface dmabuf_buffer_impl;
static Dmabuf *dmabuf_of(struct wl_resource *buf) {
    return buf && wl_resource_instance_of(buf, &wl_buffer_interface, &dmabuf_buffer_impl) ? wl_resource_get_user_data(buf) : NULL;
}

/* The buffer's texture in the live GPU context. The import is cached per wl_buffer, but only for
 * one context: after the GPU is re-created (the session back on the monitor, a remote viewer
 * leaving, a display change) the old texture is dropped and the buffer imported again. */
static GpuTex *dm_tex(Dmabuf *d) {
    if (!gpu_active()) return NULL;
    if (d->tex && !gpu_tex_current(d->tex)) { gpu_tex_free(d->tex); d->tex = NULL; }
    if (!d->tex && d->no_import != gpu_generation()) {
        d->tex = gpu_tex_dmabuf(&d->d);
        if (!d->tex) d->no_import = gpu_generation();
    }
    return d->tex;
}

/* Linear ARGB/XRGB8888 in one plane: the CPU path can map and copy it. */
static int dm_linear(const Dmabuf *d) {
    const GpuDmabuf *g = &d->d;
    return g->nplanes == 1 && (g->modifier == DRM_FORMAT_MOD_LINEAR || g->modifier == DRM_FORMAT_MOD_INVALID) &&
           (g->fourcc == DRM_FORMAT_ARGB8888 || g->fourcc == DRM_FORMAT_XRGB8888);
}

/* CPU path: a linear dmabuf (ARGB/XRGB8888, one plane) is mapped and copied to px. */
static int map_linear(Surf *s, Dmabuf *d) {
    const GpuDmabuf *g = &d->d;
    if (g->nplanes != 1 || (g->modifier != DRM_FORMAT_MOD_LINEAR && g->modifier != DRM_FORMAT_MOD_INVALID) ||
        (g->fourcc != DRM_FORMAT_ARGB8888 && g->fourcc != DRM_FORMAT_XRGB8888) || g->width > 8192 || g->height > 8192 ||
        g->stride[0] < (uint32_t)g->width * 4)
        return -1;
    size_t size = (size_t)g->offset[0] + (size_t)g->stride[0] * (size_t)g->height;
    uint8_t *m = mmap(NULL, size, PROT_READ, MAP_SHARED, g->fd[0], 0);
    if (m == MAP_FAILED) return -1;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(g->fd[0], DMA_BUF_IOCTL_SYNC, &sync);
    if (g->width != s->w || g->height != s->h || !s->px) {
        uint32_t *np = realloc(s->px, (size_t)g->width * (size_t)g->height * 4);
        if (!np) { munmap(m, size); return -1; }
        s->px = np;
        s->w = g->width;
        s->h = g->height;
    }
    s->opaque = g->fourcc == DRM_FORMAT_XRGB8888;
    for (int y = 0; y < g->height; y++) {
        uint32_t *dst = s->px + (size_t)y * s->w;
        memcpy(dst, m + g->offset[0] + (size_t)y * g->stride[0], (size_t)s->w * 4);
        if (s->opaque) for (int x = 0; x < s->w; x++) dst[x] |= 0xff000000u;
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(g->fd[0], DMA_BUF_IOCTL_SYNC, &sync);
    munmap(m, size);
    return 0;
}

static void queue_release(Dmabuf *d) {
    if (nrelease < 64) { to_release[nrelease++] = d; return; }
    if (d->res && !d->scanned) wl_buffer_send_release(d->res);
    dm_unref(d);
}

/* The buffer the app just sent becomes the surface's content. */
static void take_buffer(Surf *s, struct wl_resource *buf) {
    Dmabuf *dm = dmabuf_of(buf), *old = s->dmabuf;
    s->dmabuf = NULL;
    if (dm && s->role != R_CURSOR && dm_tex(dm)) {
        dm->refs++;
        s->dmabuf = dm;
        s->w = dm->d.width;
        s->h = dm->d.height;
        s->opaque = dm->d.fourcc == DRM_FORMAT_XRGB8888 || dm->d.fourcc == DRM_FORMAT_XBGR8888;
        s->content = 1;
        free(s->px);
        s->px = NULL;
    } else if (dm && map_linear(s, dm) == 0) {
        s->content = 1;
        s->tex_dirty = 1;
        wl_buffer_send_release(buf);
    } else if (dm && !gpu_active() && s->role != R_CURSOR) {
        /* Off the GPU (the session left the monitor, or a remote viewer) with a buffer the CPU
         * can't read (tiled, or a driver that doesn't map it): keep showing the last good frame
         * (px, read back from the GPU) and hold the buffer, so it is imported as soon as the GPU
         * is back without waiting for the app's next frame. */
        dm->refs++;
        s->dmabuf = dm;
        s->content = s->px != NULL;
    } else if (dm) {
        s->content = 0;
        free(s->px);
        s->px = NULL;
        s->w = s->h = 0;
        wl_buffer_send_release(buf);
    } else {
        copy_buffer(s, buf);
        s->tex_dirty = 1;
    }
    if (old) queue_release(old);
}

/* Logical size: the viewport destination, else the crop, else the buffer. */
static void apply_viewport(Surf *s) {
    if (s->p_vp) {
        s->vsx = s->p_vsx; s->vsy = s->p_vsy; s->vsw = s->p_vsw; s->vsh = s->p_vsh;
        s->vdw = s->p_vdw; s->vdh = s->p_vdh;
        s->p_vp = 0;
    }
    if (s->viewport && s->vdw > 0 && s->vdh > 0) { s->lw = s->vdw; s->lh = s->vdh; }
    else if (s->viewport && s->vsw > 0 && s->vsh > 0) { s->lw = (int)s->vsw; s->lh = (int)s->vsh; }
    else { int b = s->bscale > 0 ? s->bscale : 1; s->lw = s->w / b; s->lh = s->h / b; }
}

/* ---- wl_surface ---- */

static void surface_commit_role(Surf *s);
static void dnd_icon_damage(void);

static void srf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void srf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (wl_resource_get_version(r) < 5) { s->p_dnd_x += x; s->p_dnd_y += y; }   /* from version 5 that is wl_surface.offset */
    if (s->p_buf) { wl_list_remove(&s->p_buf_destroy.link); wl_list_init(&s->p_buf_destroy.link); }
    s->p_buf = buf;
    s->p_attached = 1;
    if (buf) {
        s->p_buf_destroy.notify = buffer_destroyed;
        wl_resource_add_destroy_listener(buf, &s->p_buf_destroy);
    }
}

static void srf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void callback_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }

static void srf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Surf *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(cb, NULL, NULL, callback_gone);
    wl_list_insert(s->p_frames.prev, wl_resource_get_link(cb));
}

static void srf_opaque(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) {
    (void)c; (void)r; (void)region;
}

static void srf_input(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) {
    Surf *s = wl_resource_get_user_data(r);
    static const WlRgn none;
    const WlRgn *from = region ? wl_resource_get_user_data(region) : &none;
    if (wlrgn_copy(&s->p_input, from)) { wl_client_post_no_memory(c); return; }
    s->p_input_set = 1;
    s->p_has_input = region != NULL;
}

static void srf_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (keylag_enabled && key_focus && c == key_focus->client) keylag_commit();
    if (s->p_attached) {
        take_buffer(s, s->p_buf);
        if (s->p_buf) { wl_list_remove(&s->p_buf_destroy.link); wl_list_init(&s->p_buf_destroy.link); }
        s->p_buf = NULL;
        s->p_attached = 0;
    }
    s->bscale = s->p_bscale;
    apply_viewport(s);
    if (s->p_input_set) {
        WlRgn t = s->input;                 /* the pending one is overwritten by the next request */
        s->input = s->p_input;
        s->p_input = t;
        s->has_input = s->p_has_input;
        s->p_input_set = 0;
    }
    wl_list_insert_list(s->frames.prev, &s->p_frames);
    wl_list_init(&s->p_frames);
    wl_list_insert_list(s->feedback.prev, &s->p_feedback);
    wl_list_init(&s->p_feedback);
    if (s->role == R_CURSOR) {
        s->cursor_hx = s->p_hx;
        s->cursor_hy = s->p_hy;
        if (s == cursor_surf) cursor_gen++;
        return;
    }
    if (s->role == R_DNDICON) {
        dnd_icon_damage();
        s->dnd_x += s->p_dnd_x;
        s->dnd_y += s->p_dnd_y;
        s->p_dnd_x = s->p_dnd_y = 0;
        dnd_icon_damage();
        return;
    }
    s->p_dnd_x = s->p_dnd_y = 0;
    if (s->p_geom) {
        s->gx = s->p_gx; s->gy = s->p_gy; s->gw = s->p_gw; s->gh = s->p_gh;
        s->has_geom = 1;
        s->p_geom = 0;
    }
    if (s->role == R_SUB) { s->sx = s->p_sx; s->sy = s->p_sy; }
    surface_commit_role(s);
}

static void srf_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }
static void srf_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (sc < 1) { wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_SCALE, "buffer scale must be positive"); return; }
    if (s) s->p_bscale = sc > 8 ? 8 : sc;
}
static void srf_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    s->p_dnd_x += x;
    s->p_dnd_y += y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = srf_destroy, .attach = srf_attach, .damage = srf_damage, .frame = srf_frame,
    .set_opaque_region = srf_opaque, .set_input_region = srf_input, .commit = srf_commit,
    .set_buffer_transform = srf_transform, .set_buffer_scale = srf_scale, .damage_buffer = srf_damage,
    .offset = srf_offset,
};

static void unmap_popup(Surf *s) {
    for (int i = 0; i < npopups; i++)
        if (popups[i] == s) {
            popup_box_damage(s);
            memmove(popups + i, popups + i + 1, sizeof *popups * (size_t)(npopups - i - 1));
            npopups--;
            break;
        }
}

/* ---- windows that belong to another: dialogs (xdg_toplevel.set_parent, xdg-foreign-v2, xdg-dialog-v1) ----
 * JS learns each toplevel's owner and whether it is modal ("parent" event, and in "map"): the theme
 * opens a dialog over its owner, moves it with it and keeps a modal one above it (sdk/dialogmodel.js).
 * Here a modal dialog takes its owner's clicks, wheel and keyboard. */

static void emit_owner(Surf *s) {
    emit(s, "parent", "{\"parent\":%d,\"modal\":%s}", s->owner ? s->owner->id : 0, s->modal ? "true" : "false");
}

/* by: the zxdg_imported_v2 that asks (NULL: the window itself, with xdg_toplevel.set_parent). A
 * parent that would make a loop is ignored. */
static void set_owner(Surf *s, Surf *p, void *by) {
    for (Surf *q = p; q; q = q->owner) if (q == s) return;
    s->owner_by = p ? by : NULL;
    if (s->owner == p) return;
    s->owner = p;
    if (s->mapped) emit_owner(s);
}

static void set_modal(Surf *s, int on) {
    if (!s || s->modal == !!on) return;
    s->modal = !!on;
    if (s->mapped) emit_owner(s);
}

/* The modal dialog that holds this window's input (the last of a chain of them), or NULL. */
static Surf *modal_over(Surf *s) {
    Surf *m = NULL;
    for (int depth = 0; s && depth < 8; depth++) {
        Surf *next = NULL;
        for (int i = 0; i < MAX_SURF && !next; i++) {
            Surf *k = surfs[i];
            if (k && k->owner == s && k->modal && k->mapped && k->role == R_TOPLEVEL) next = k;
        }
        if (next) m = next;
        s = next;
    }
    return m;
}

/* A window that goes away leaves its dialogs to its own owner (xdg-shell's rule for set_parent). */
static void orphan_dialogs(Surf *s) {
    for (int i = 0; i < MAX_SURF; i++)
        if (surfs[i] && surfs[i] != s && surfs[i]->owner == s) set_owner(surfs[i], s->owner, NULL);
}

/* xdg-foreign-v2: an app exports its toplevel as a handle and hands it to another process (a portal's
 * file dialog, a helper of its own), which imports it and puts its window over it. The handle is 128
 * random bits in hex: it can't be guessed, and it stops working when the surface goes away. */
typedef struct { struct wl_resource *res; Surf *s; char handle[33]; struct wl_list link; } Export;
typedef struct { struct wl_resource *res; Export *e; struct wl_list link; } Import;
static struct wl_list exports, imports;

/* What an import set stops counting: its dialogs go back to having no owner. */
static void import_unset(Import *im) {
    for (int i = 0; i < MAX_SURF; i++)
        if (surfs[i] && surfs[i]->owner_by == im) set_owner(surfs[i], NULL, NULL);
}

/* The exported surface or the export is gone: every import of it is told, and is inert from then on. */
static void export_drop(Export *e) {
    Import *im;
    wl_list_for_each(im, &imports, link)
        if (im->e == e) {
            im->e = NULL;
            import_unset(im);
            zxdg_imported_v2_send_destroyed(im->res);
        }
    e->s = NULL;
}

static void foreign_surface_gone(Surf *s) {
    Export *e;
    wl_list_for_each(e, &exports, link) if (e->s == s) export_drop(e);
}

int wl_foreign_surface(const char *handle) {
    if (!dpy || !handle || !handle[0]) return 0;
    Export *e;
    wl_list_for_each(e, &exports, link)
        if (e->s && !strcmp(e->handle, handle)) return e->s->role == R_TOPLEVEL ? e->s->id : 0;
    return 0;
}

static void exported_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zxdg_exported_v2_interface exported_impl = { .destroy = exported_destroy };
static void exported_gone(struct wl_resource *r) {
    Export *e = wl_resource_get_user_data(r);
    export_drop(e);
    wl_list_remove(&e->link);
    free(e);
}
static void exporter_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void exporter_export(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    Surf *s = wl_resource_get_user_data(surface);
    if (!s || s->role != R_TOPLEVEL || !s->top) {
        wl_resource_post_error(r, ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE, "surface is not an xdg_toplevel");
        return;
    }
    unsigned char rnd[16];
    Export *e = calloc(1, sizeof *e);
    struct wl_resource *x = e && getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd
        ? wl_resource_create(c, &zxdg_exported_v2_interface, wl_resource_get_version(r), id) : NULL;
    if (!x) { free(e); wl_client_post_no_memory(c); return; }
    for (int i = 0; i < 16; i++) snprintf(e->handle + 2 * i, 3, "%02x", rnd[i]);
    e->res = x;
    e->s = s;
    wl_list_insert(&exports, &e->link);
    wl_resource_set_implementation(x, &exported_impl, e, exported_gone);
    zxdg_exported_v2_send_handle(x, e->handle);
}
static const struct zxdg_exporter_v2_interface exporter_impl = { .destroy = exporter_destroy, .export_toplevel = exporter_export };
static void bind_exporter(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zxdg_exporter_v2_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &exporter_impl, NULL, NULL);
}

static void imported_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void imported_parent_of(struct wl_client *c, struct wl_resource *r, struct wl_resource *surface) {
    (void)c;
    Import *im = wl_resource_get_user_data(r);
    Surf *s = wl_resource_get_user_data(surface);
    if (!s || s->role != R_TOPLEVEL || !s->top) {
        wl_resource_post_error(r, ZXDG_IMPORTED_V2_ERROR_INVALID_SURFACE, "surface is not an xdg_toplevel");
        return;
    }
    if (im->e && im->e->s) set_owner(s, im->e->s, im);
}
static const struct zxdg_imported_v2_interface imported_impl = { .destroy = imported_destroy, .set_parent_of = imported_parent_of };
static void imported_gone(struct wl_resource *r) {
    Import *im = wl_resource_get_user_data(r);
    import_unset(im);
    wl_list_remove(&im->link);
    free(im);
}
static void importer_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void importer_import(struct wl_client *c, struct wl_resource *r, uint32_t id, const char *handle) {
    Import *im = calloc(1, sizeof *im);
    struct wl_resource *x = im ? wl_resource_create(c, &zxdg_imported_v2_interface, wl_resource_get_version(r), id) : NULL;
    if (!x) { free(im); wl_client_post_no_memory(c); return; }
    im->res = x;
    Export *e;
    wl_list_for_each(e, &exports, link) if (e->s && !strcmp(e->handle, handle)) { im->e = e; break; }
    wl_list_insert(&imports, &im->link);
    wl_resource_set_implementation(x, &imported_impl, im, imported_gone);
    if (!im->e) zxdg_imported_v2_send_destroyed(x);    /* unknown or no longer valid: inert */
}
static const struct zxdg_importer_v2_interface importer_impl = { .destroy = importer_destroy, .import_toplevel = importer_import };
static void bind_importer(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zxdg_importer_v2_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &importer_impl, NULL, NULL);
}

/* xdg-dialog-v1: the window says it is a dialog, and whether it is modal (GTK 4 and Qt 6 do). */
static void dlg_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void dlg_modal(struct wl_client *c, struct wl_resource *r) { (void)c; set_modal(wl_resource_get_user_data(r), 1); }
static void dlg_unmodal(struct wl_client *c, struct wl_resource *r) { (void)c; set_modal(wl_resource_get_user_data(r), 0); }
static const struct xdg_dialog_v1_interface dialog_impl = { .destroy = dlg_destroy, .set_modal = dlg_modal, .unset_modal = dlg_unmodal };
static void dlg_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s || s->dlg != r) return;
    s->dlg = NULL;
    set_modal(s, 0);
}
static void wmd_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wmd_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *toplevel) {
    Surf *s = wl_resource_get_user_data(toplevel);
    if (s && s->dlg) { wl_resource_post_error(r, XDG_WM_DIALOG_V1_ERROR_ALREADY_USED, "the toplevel already has a dialog object"); return; }
    struct wl_resource *d = wl_resource_create(c, &xdg_dialog_v1_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &dialog_impl, s, dlg_gone);
    if (s) s->dlg = d;
}
static const struct xdg_wm_dialog_v1_interface wm_dialog_impl = { .destroy = wmd_destroy, .get_xdg_dialog = wmd_get };
static void bind_wm_dialog(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_dialog_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_dialog_impl, NULL, NULL);
}

static void dnd_surface_gone(Surf *s);
static void surface_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (s->xw) { xwm_surface_gone(s->xw); s->xw = NULL; }
    if (pointer_focus == s) { pointer_focus = NULL; buttons_held = 0; }
    if (key_focus == s) { key_focus = NULL; wl_textinput_gone(); }
    dnd_surface_gone(s);
    unmap_popup(s);
    if (s->role == R_TOPLEVEL && s->mapped) emit(s, "unmap", "{}");
    orphan_dialogs(s);
    foreign_surface_gone(s);
    if (s->dlg) wl_resource_set_user_data(s->dlg, NULL);
    if (s->parent) {
        Surf *p = s->parent;
        for (int i = 0; i < p->nkids; i++) if (p->kids[i] == s) { p->kids[i] = p->kids[--p->nkids]; break; }
        damage_node(p);
    }
    for (int i = 0; i < s->nkids; i++) s->kids[i]->parent = NULL;
    for (int i = 0; i < MAX_SURF; i++) {
        if (surfs[i] == s) surfs[i] = NULL;
        else if (surfs[i] && surfs[i]->parent == s) surfs[i]->parent = NULL;
    }
    if (s->p_buf) wl_list_remove(&s->p_buf_destroy.link);
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->p_frames) wl_resource_destroy(cb);
    wl_resource_for_each_safe(cb, tmp, &s->frames) wl_resource_destroy(cb);
    wl_resource_for_each_safe(cb, tmp, &s->p_feedback) { wp_presentation_feedback_send_discarded(cb); wl_resource_destroy(cb); }
    wl_resource_for_each_safe(cb, tmp, &s->feedback) { wp_presentation_feedback_send_discarded(cb); wl_resource_destroy(cb); }
    wl_resource_for_each_safe(cb, tmp, &s->sent_feedback) { wp_presentation_feedback_send_discarded(cb); wl_resource_destroy(cb); }
    if (s->viewport) wl_resource_set_user_data(s->viewport, NULL);
    if (s->sub) wl_resource_set_user_data(s->sub, NULL);
    if (s->frac) wl_resource_set_user_data(s->frac, NULL);
    if (s->kde) wl_resource_set_user_data(s->kde, NULL);
    if (s->deco) wl_resource_set_user_data(s->deco, NULL);
    /* a client may destroy the wl_surface before its role objects: they must not keep the freed Surf, or set_parent,
     * get_xdg_dialog and the rest would read and write it (every handler takes a NULL as "gone") */
    if (s->top) wl_resource_set_user_data(s->top, NULL);
    if (s->popup) wl_resource_set_user_data(s->popup, NULL);
    if (s->xdg) wl_resource_set_user_data(s->xdg, NULL);
    if (s->node) damage_node(s);
    if (fs_surf == s) { fs_surf = NULL; ui.dirty = 1; damage_add((Box){ 0, 0, ui.width, ui.height }); }
    if (cursor_surf == s) { cursor_surf = NULL; cursor_mode = 0; cursor_gen++; }
    if (constraint && constraint->surf == s) { constraint->surf = NULL; constraint->active = 0; }
    constraint_update();                /* s may have been the window with the keyboard */
    if (s->dmabuf) queue_release(s->dmabuf);
    gpu_tex_free(s->tex);
    wlrgn_free(&s->input);
    wlrgn_free(&s->p_input);
    free(s->px);
    free(s);
}

/* ---- wl_compositor / wl_region ---- */

static void rgn_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void rgn_op(struct wl_client *c, struct wl_resource *r, int sub, int x, int y, int w, int h) {
    if (wlrgn_push(wl_resource_get_user_data(r), sub, x, y, w, h)) wl_client_post_no_memory(c);
}
static void rgn_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { rgn_op(c, r, 0, x, y, w, h); }
static void rgn_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { rgn_op(c, r, 1, x, y, w, h); }
static const struct wl_region_interface region_impl = { .destroy = rgn_destroy, .add = rgn_add, .subtract = rgn_sub };
static void rgn_gone(struct wl_resource *r) {
    WlRgn *g = wl_resource_get_user_data(r);
    wlrgn_free(g);
    free(g);
}

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    int slot = -1;
    for (int i = 0; i < MAX_SURF; i++) if (!surfs[i]) { slot = i; break; }
    Surf *s = slot >= 0 ? calloc(1, sizeof *s) : NULL;
    struct wl_resource *res = s ? wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(s); wl_client_post_no_memory(c); return; }
    s->res = res;
    s->client = c;
    s->id = next_id++;
    wl_list_init(&s->p_frames);
    wl_list_init(&s->frames);
    wl_list_init(&s->p_feedback);
    wl_list_init(&s->feedback);
    wl_list_init(&s->sent_feedback);
    wl_list_init(&s->p_buf_destroy.link);
    s->vsw = s->p_vsw = -1;
    s->bscale = s->p_bscale = 1;
    surfs[slot] = s;
    wl_resource_set_implementation(res, &surface_impl, s, surface_gone);
    if (c == xwl_client) xwm_surface_created(id);   /* WL_SURFACE_ID may have come first */
}

static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    WlRgn *g = calloc(1, sizeof *g);
    struct wl_resource *res = g ? wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(g); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &region_impl, g, rgn_gone);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = comp_create_surface, .create_region = comp_create_region,
};

static void bind_compositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---- wl_subcompositor ---- */

static void sub_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void sub_position(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (s) { s->p_sx = x; s->p_sy = y; }
}
static void sub_place(struct wl_client *c, struct wl_resource *r, struct wl_resource *sibling) { (void)c; (void)r; (void)sibling; }
static void sub_sync(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_destroy, .set_position = sub_position, .place_above = sub_place, .place_below = sub_place,
    .set_sync = sub_sync, .set_desync = sub_sync,
};

static void sub_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    if (s->parent) {
        Surf *p = s->parent;
        for (int i = 0; i < p->nkids; i++) if (p->kids[i] == s) { p->kids[i] = p->kids[--p->nkids]; break; }
        damage_node(p);
    }
    s->parent = NULL;
    s->sub = NULL;
    s->role = R_NONE;
}

static void subc_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void subc_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *parent) {
    Surf *s = wl_resource_get_user_data(surface), *p = wl_resource_get_user_data(parent);
    /* one wl_subsurface per wl_surface, and not for a surface that has another role (cursor, drag icon) */
    if (s->sub || s->role == R_CURSOR || s->role == R_DNDICON || s == p) {
        wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "the wl_surface already has a role");
        return;
    }
    struct wl_resource *res = wl_resource_create(c, &wl_subsurface_interface, wl_resource_get_version(r), id);
    if (!res) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &subsurface_impl, s, sub_gone);
    s->role = R_SUB;
    s->sub = res;
    s->parent = p;
    if (p->nkids < 16) p->kids[p->nkids++] = s;
}
static const struct wl_subcompositor_interface subcompositor_impl = { .destroy = subc_destroy, .get_subsurface = subc_get };

static void bind_subcompositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, NULL, NULL);
}

/* ---- wl_output: one per display (docs/DISPLAYS.md) ----
 * Each display is a slot with its own global. Resources keep the slot index + 1 as user data (0
 * once the display is gone). Geometry and xdg-output carry the position in the global logical
 * space; mode is the physical one; scale is the integer one for legacy clients (they render at it
 * and LordOS downscales), and fractional-scale-v1 tells the others the exact one. */
typedef struct {
    WlOutput o;
    struct wl_global *global;
    struct wl_list res, xdg;            /* wl_output and zxdg_output_v1 resources */
    int used;
} OutSlot;
static OutSlot oslots[OUT_MAX];
static int out_main;

static int int_scale(int pct) { return (pct + 99) / 100; }

static void out_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { .release = out_release };
static void out_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }

static OutSlot *slot_of(struct wl_resource *r) {
    intptr_t i = (intptr_t)wl_resource_get_user_data(r);
    return i >= 1 && i <= OUT_MAX && oslots[i - 1].used ? &oslots[i - 1] : NULL;
}

static void send_output(struct wl_resource *r, const WlOutput *o) {
    int ver = wl_resource_get_version(r);
    wl_output_send_geometry(r, o->x, o->y, o->mm_w, o->mm_h, WL_OUTPUT_SUBPIXEL_UNKNOWN, "LordOS", o->desc[0] ? o->desc : "lord-server",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, o->pw, o->ph, o->refresh > 0 ? o->refresh : 60000);
    if (ver >= 2) wl_output_send_scale(r, int_scale(o->scale));
    if (ver >= 4) { wl_output_send_name(r, o->name); wl_output_send_description(r, o->desc[0] ? o->desc : o->name); }
}

static void send_xdg_output(struct wl_resource *x, const WlOutput *o) {
    zxdg_output_v1_send_logical_position(x, o->x, o->y);
    zxdg_output_v1_send_logical_size(x, o->w, o->h);
    int ver = wl_resource_get_version(x);
    if (ver >= 2 && ver < 3) { zxdg_output_v1_send_name(x, o->name); zxdg_output_v1_send_description(x, o->desc[0] ? o->desc : o->name); }
    if (ver < 3) zxdg_output_v1_send_done(x);
}

static void bind_output(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    intptr_t i = (intptr_t)data;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &output_impl, (void *)i, out_gone);
    OutSlot *o = i >= 1 && i <= OUT_MAX ? &oslots[i - 1] : NULL;
    if (!o || !o->used) { wl_list_init(wl_resource_get_link(r)); return; }
    wl_list_insert(&o->res, wl_resource_get_link(r));
    send_output(r, &o->o);
    if (ver >= 2) wl_output_send_done(r);
    /* a surface of this client already on that display */
    for (int k = 0; k < MAX_SURF; k++)
        if (surfs[k] && surfs[k]->client == c && (surfs[k]->outmask >> (i - 1) & 1)) wl_surface_send_enter(surfs[k]->res, r);
}

/* xdg-output (zxdg_output_manager_v1 v3): logical position and size of each wl_output */
static void xo_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zxdg_output_v1_interface xdg_output_impl = { .destroy = xo_destroy };
static void xo_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void xom_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xom_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *output) {
    struct wl_resource *x = wl_resource_create(c, &zxdg_output_v1_interface, wl_resource_get_version(r), id);
    if (!x) { wl_client_post_no_memory(c); return; }
    OutSlot *o = slot_of(output);
    wl_resource_set_implementation(x, &xdg_output_impl, wl_resource_get_user_data(output), xo_gone);
    if (!o) { wl_list_init(wl_resource_get_link(x)); return; }
    wl_list_insert(&o->xdg, wl_resource_get_link(x));
    send_xdg_output(x, &o->o);
    if (wl_resource_get_version(x) >= 3 && wl_resource_get_version(output) >= 2) wl_output_send_done(output);
}
static const struct zxdg_output_manager_v1_interface xdg_output_mgr_impl = { .destroy = xom_destroy, .get_xdg_output = xom_get };
static void bind_xdg_output_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zxdg_output_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xdg_output_mgr_impl, NULL, NULL);
}

/* fractional-scale-v1: the exact scale (×120) for each surface */
static void fs1_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wp_fractional_scale_v1_interface frac_impl = { .destroy = fs1_destroy };
static void frac_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (s) s->frac = NULL;
}
static int pointer_scale = 100;         /* the display under the pointer: where a new window opens */
static void fsm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void fsm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    Surf *s = wl_resource_get_user_data(surface);
    if (s && s->frac) { wl_resource_post_error(r, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS, "already has one"); return; }
    struct wl_resource *f = wl_resource_create(c, &wp_fractional_scale_v1_interface, wl_resource_get_version(r), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &frac_impl, s, frac_gone);
    if (!s) return;
    s->frac = f;
    int sc = s->pref_scale ? s->pref_scale : pointer_scale;
    wp_fractional_scale_v1_send_preferred_scale(f, (uint32_t)(sc * 120 / 100));
}
static const struct wp_fractional_scale_manager_v1_interface frac_mgr_impl = { .destroy = fsm_destroy, .get_fractional_scale = fsm_get };
static void bind_frac_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_fractional_scale_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &frac_mgr_impl, NULL, NULL);
}

static void surface_leave_slot(int slot);
static void fs_follow_outputs(void);
static int oslots_used(int slot) { return oslots[slot].used; }
static void oslot_rect(int slot, float *x, float *y, float *w, float *h) {
    const WlOutput *o = &oslots[slot].o;
    *x = (float)o->x; *y = (float)o->y; *w = (float)o->w; *h = (float)o->h;
}
/* The display with most of a logical box (a window going fullscreen, a menu's parent). */
static int slot_for_box(float x0, float y0, float x1, float y1) {
    int best = -1;
    float area = 0;
    for (int i = 0; i < OUT_MAX; i++) {
        if (!oslots[i].used) continue;
        const WlOutput *o = &oslots[i].o;
        float w = fminf(x1, (float)(o->x + o->w)) - fmaxf(x0, (float)o->x), h = fminf(y1, (float)(o->y + o->h)) - fmaxf(y0, (float)o->y);
        if (w > 0 && h > 0 && w * h > area) { area = w * h; best = i; }
    }
    return best;
}

void wl_set_outputs(const WlOutput *list, int n, int main) {
    if (!dpy) return;
    int keep[OUT_MAX] = { 0 };
    for (int j = 0; j < n && j < OUT_MAX; j++) {
        int slot = -1;
        for (int i = 0; i < OUT_MAX && slot < 0; i++) if (oslots[i].used && !strcmp(oslots[i].o.name, list[j].name)) slot = i;
        if (slot >= 0 && keep[slot]) slot = -1;             /* two with the same name: a new slot */
        if (slot < 0) for (int i = 0; i < OUT_MAX && slot < 0; i++) if (!oslots[i].used && !keep[i]) slot = i;
        if (slot < 0) continue;
        keep[slot] = 1;
        OutSlot *o = &oslots[slot];
        if (j == main) out_main = slot;
        if (o->used && !memcmp(&o->o, &list[j], sizeof o->o)) continue;
        o->o = list[j];
        if (!o->used) {
            o->used = 1;
            wl_list_init(&o->res);
            wl_list_init(&o->xdg);
            o->global = wl_global_create(dpy, &wl_output_interface, 4, (void *)(intptr_t)(slot + 1), bind_output);
            continue;
        }
        struct wl_resource *r;
        wl_resource_for_each(r, &o->res) send_output(r, &o->o);
        wl_resource_for_each(r, &o->xdg) send_xdg_output(r, &o->o);
        wl_resource_for_each(r, &o->res) if (wl_resource_get_version(r) >= 2) wl_output_send_done(r);
    }
    for (int i = 0; i < OUT_MAX; i++) {
        if (!oslots[i].used || keep[i]) continue;
        surface_leave_slot(i);
        struct wl_resource *r, *t;
        wl_resource_for_each_safe(r, t, &oslots[i].res) { wl_resource_set_user_data(r, NULL); wl_list_remove(wl_resource_get_link(r)); wl_list_init(wl_resource_get_link(r)); }
        wl_resource_for_each_safe(r, t, &oslots[i].xdg) { wl_resource_set_user_data(r, NULL); wl_list_remove(wl_resource_get_link(r)); wl_list_init(wl_resource_get_link(r)); }
        if (oslots[i].global) wl_global_destroy(oslots[i].global);
        memset(&oslots[i], 0, sizeof oslots[i]);
    }
    fs_follow_outputs();
}

/* ---- which displays a surface is on (enter/leave, preferred scale), once per frame ---- */

static void send_enter_leave(Surf *s, unsigned mask) {
    for (int i = 0; i < OUT_MAX; i++) {
        unsigned bit = 1u << i;
        if (!oslots[i].used || (mask & bit) == (s->outmask & bit)) continue;
        struct wl_resource *r;
        FOR_CLIENT(&oslots[i].res, s->client, r) {
            if (mask & bit) wl_surface_send_enter(s->res, r);
            else wl_surface_send_leave(s->res, r);
        }
    }
    s->outmask = mask;
}

static void set_pref_scale(Surf *s, int pct) {
    if (s->pref_scale == pct) return;
    s->pref_scale = pct;
    if (s->frac) wp_fractional_scale_v1_send_preferred_scale(s->frac, (uint32_t)(pct * 120 / 100));
    if (wl_resource_get_version(s->res) >= 6) wl_surface_send_preferred_buffer_scale(s->res, int_scale(pct));
}

static void surface_outputs(Surf *s, float x, float y, int depth) {
    if (depth > 8) return;
    unsigned mask = 0;
    int best = 0;
    for (int i = 0; i < OUT_MAX; i++) {
        const WlOutput *o = &oslots[i].o;
        if (!oslots[i].used || s->lw <= 0) continue;
        if (x < o->x + o->w && x + s->lw > o->x && y < o->y + o->h && y + s->lh > o->y) {
            mask |= 1u << i;
            if (o->scale > best) best = o->scale;           /* sharp on the densest display it touches */
        }
    }
    if (mask != s->outmask) send_enter_leave(s, mask);
    if (best) set_pref_scale(s, best);
    else if (!s->pref_scale) set_pref_scale(s, pointer_scale);
    for (int k = 0; k < s->nkids; k++) surface_outputs(s->kids[k], x + s->kids[k]->sx, y + s->kids[k]->sy, depth + 1);
}

static void update_surface_outputs(void) {
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s || s->parent || (s->role != R_TOPLEVEL && s->role != R_POPUP) || !s->content) continue;
        float x, y;
        if (origin(s, &x, &y)) surface_outputs(s, x, y, 0);
    }
    for (int i = 0; i < npopups; i++) {
        float x, y;
        if (origin(popups[i], &x, &y)) surface_outputs(popups[i], x, y, 0);
    }
}

static void surface_leave_slot(int slot) {
    for (int i = 0; i < MAX_SURF; i++)
        if (surfs[i] && (surfs[i]->outmask >> slot & 1)) send_enter_leave(surfs[i], surfs[i]->outmask & ~(1u << slot));
}

/* ---- wl_seat: pointer and keyboard ---- */

/* App cursor: only valid if the pointer is over one of its surfaces. */
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surface, int32_t x, int32_t y) {
    (void)r; (void)serial;
    if (!pointer_focus || pointer_focus->client != c) return;
    Surf *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (s && s->role != R_NONE && s->role != R_CURSOR) return;
    if (s) { s->role = R_CURSOR; s->cursor_hx = s->p_hx = x; s->cursor_hy = s->p_hy = y; }
    cursor_surf = s;
    cursor_mode = s ? 1 : 2;
    cursor_gen++;
}
static void res_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

/* cursor-shape-v1: the app asks for a shape by name (Chromium, GTK 4, Qt 6) and LordOS draws its own
 * (cursor.c); cursor_mode 3. Only valid if the pointer is over a surface of that app. */
static unsigned cursor_shape;
static void csd_set_shape(struct wl_client *c, struct wl_resource *r, uint32_t serial, uint32_t shape) {
    (void)serial;
    if (shape < 1 || shape > 36) {
        wl_resource_post_error(r, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE, "invalid shape %u", shape);
        return;
    }
    if (!pointer_focus || pointer_focus->client != c) return;
    cursor_surf = NULL;
    cursor_mode = 3;
    cursor_shape = shape;
    cursor_gen++;
}
static const struct wp_cursor_shape_device_v1_interface csd_impl = { .destroy = res_release, .set_shape = csd_set_shape };
static void csm_device(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &csd_impl, NULL, NULL);
}
static void csm_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *pointer) {
    (void)pointer;
    csm_device(c, r, id);
}
static void csm_get_tablet(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *tool) {
    (void)tool;
    csm_device(c, r, id);               /* no tablets: the device exists but is unused */
}
static const struct wp_cursor_shape_manager_v1_interface csm_impl = {
    .destroy = res_release, .get_pointer = csm_get_pointer, .get_tablet_tool_v2 = csm_get_tablet,
};
static void bind_csm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_cursor_shape_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &csm_impl, NULL, NULL);
}
static const struct wl_pointer_interface pointer_impl = { .set_cursor = ptr_set_cursor, .release = res_release };
static const struct wl_keyboard_interface keyboard_impl = { .release = res_release };
static const struct wl_touch_interface touch_impl = { .release = res_release };
static void link_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }

/* ---- idle-inhibit: a video player or a presentation keeps the screen on while its surface is shown ---- */
static struct wl_list inhibitors;
static void inh_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_idle_inhibitor_v1_interface inh_impl = { .destroy = inh_destroy };
static void iim_create(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    struct wl_resource *i = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface, wl_resource_get_version(r), id);
    if (!i) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(i, &inh_impl, surface, link_gone);   /* the surface, only compared below */
    wl_list_insert(&inhibitors, wl_resource_get_link(i));
}
static void iim_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_idle_inhibit_manager_v1_interface iim_impl = { .destroy = iim_destroy, .create_inhibitor = iim_create };
static void bind_iim(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_idle_inhibit_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &iim_impl, NULL, NULL);
}

/* ---- xdg-activation: a window comes forward only with a token LordOS or the app in front gave (xdgact.c) ---- */
typedef struct { uint32_t serial; int has_serial, committed; char app_id[128]; } ActReq;
static void tok_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
static void tok_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void tok_serial(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *seat) {
    (void)c; (void)seat;                /* one seat */
    ActReq *q = wl_resource_get_user_data(r);
    q->serial = serial;
    q->has_serial = 1;
}
static void tok_app_id(struct wl_client *c, struct wl_resource *r, const char *app_id) {
    (void)c;
    ActReq *q = wl_resource_get_user_data(r);
    snprintf(q->app_id, sizeof q->app_id, "%s", app_id);
}
static void tok_surface(struct wl_client *c, struct wl_resource *r, struct wl_resource *surface) { (void)c; (void)r; (void)surface; }
/* Granted when the asking app has the keyboard, or the serial it names is a click or key LordOS
 * sent it a moment ago; otherwise the token exists but only asks for attention. The grant ends
 * when the keyboard leaves that app (wl_set_focus) or after XACT_CLIENT_MS. */
static void tok_commit(struct wl_client *c, struct wl_resource *r) {
    ActReq *q = wl_resource_get_user_data(r);
    if (q->committed) { wl_resource_post_error(r, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED, "token already committed"); return; }
    q->committed = 1;
    double now = xact_now();
    int granted = (key_focus && key_focus->client == c) || (q->has_serial && xact_serial_recent(q->serial, c, now));
    const char *t = xact_issue_for(now, XACT_CLIENT_MS, granted, q->app_id, c);
    xdg_activation_token_v1_send_done(r, t ? t : "none");
}
static const struct xdg_activation_token_v1_interface tok_impl = {
    .set_serial = tok_serial, .set_app_id = tok_app_id, .set_surface = tok_surface, .commit = tok_commit, .destroy = tok_destroy,
};
static void act_get_token(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    ActReq *q = calloc(1, sizeof *q);
    struct wl_resource *t = q ? wl_resource_create(c, &xdg_activation_token_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!t) { free(q); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(t, &tok_impl, q, tok_gone);
}
static void act_activate(struct wl_client *c, struct wl_resource *r, const char *token, struct wl_resource *surface) {
    (void)c; (void)r;
    int res = xact_take(token, xact_now(), NULL, 0);
    Surf *s = wl_resource_get_user_data(surface);
    while (s && s->role != R_TOPLEVEL && s->parent) s = s->parent;     /* a menu or a subsurface: its window */
    if (!s || s->role != R_TOPLEVEL || !s->mapped) return;
    if (getenv("LORD_WL_DEBUG")) fprintf(stderr, "wayland: activate %d: %s\n", s->id, res == XACT_OK ? "granted" : "attention");
    emit(s, res == XACT_OK ? "activate" : "attention", "{}");
}
static void act_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct xdg_activation_v1_interface act_impl = { .destroy = act_destroy, .get_activation_token = act_get_token, .activate = act_activate };
static void bind_activation(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_activation_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &act_impl, NULL, NULL);
}
/* An input serial for this client: remembered so its activation requests can name it. */
static uint32_t input_serial(struct wl_client *c) {
    uint32_t s = wl_display_next_serial(dpy);
    xact_note_serial(s, c, xact_now());
    return s;
}
/* A client that left: its serials stop counting (another one may get the same address). What is known of a
 * client from its connection: sandboxed (dndg_pid_sandboxed), looked at once, while its pid is still the one
 * that connected. */
typedef struct { struct wl_listener gone; int sandboxed; } ClientInfo;
static struct wl_listener client_created;
static void dnd_client_gone(struct wl_client *c);
static pid_t client_pid(struct wl_client *c);
static void client_destroyed(struct wl_listener *l, void *data) {
    xact_client_gone(data);
    dnd_client_gone(data);
    wl_list_remove(&l->link);
    free(wl_container_of(l, (ClientInfo *)NULL, gone));
}
static void on_client_created(struct wl_listener *l, void *data) {
    (void)l;
    ClientInfo *ci = calloc(1, sizeof *ci);
    if (!ci) return;
    ci->sandboxed = dndg_pid_sandboxed((int)client_pid(data));
    ci->gone.notify = client_destroyed;
    wl_client_add_destroy_listener(data, &ci->gone);
}
/* 1 if the client runs in a sandbox (Flatpak), or nothing is known of it */
static int client_sandboxed(struct wl_client *c) {
    struct wl_listener *l = c ? wl_client_get_destroy_listener(c, client_destroyed) : NULL;
    if (!l) return 1;
    ClientInfo *ci = wl_container_of(l, (ClientInfo *)NULL, gone);
    return ci->sandboxed;
}

static void send_keymap(struct wl_resource *kb) {
    char *km = kb_keymap_string();
    if (!km) return;
    size_t size = strlen(km) + 1;
    int fd = memfd_create("lord-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
        void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            memcpy(m, km, size);
            munmap(m, size);
            fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
            wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
        }
    }
    if (fd >= 0) close(fd);
    free(km);
}

static void send_enter_keyboard(struct wl_resource *kb, Surf *s) {
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(kb, input_serial(s->client), s->res, &keys);
    wl_array_release(&keys);
    uint32_t d, l, k, g;
    kb_mods(&d, &l, &k, &g);
    wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), d, l, k, g);
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &pointer_impl, NULL, link_gone);
    wl_list_insert(&pointers, wl_resource_get_link(p));
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(k, &keyboard_impl, NULL, link_gone);
    wl_list_insert(&keyboards, wl_resource_get_link(k));
    if (wl_resource_get_version(k) >= 4) wl_keyboard_send_repeat_info(k, 30, 500);
    send_keymap(k);
    if (key_focus && key_focus->client == c) send_enter_keyboard(k, key_focus);
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    (void)touch_impl;
    wl_input_get_touch(c, r, id);       /* touchscreens: wl_input.c */
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard, .get_touch = seat_get_touch, .release = res_release,
};

static void bind_seat(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_input_seat_caps(r);              /* pointer, keyboard and touch if there is a touchscreen */
    if (ver >= 2) wl_seat_send_name(r, "seat0");
}

void wl_keymap_changed(void) {
    if (!dpy) return;
    struct wl_resource *k;
    wl_resource_for_each(k, &keyboards) send_keymap(k);
}

/* ---- clipboard (wl_data_device_manager) ----
 * An app's text selection is read in full (text/plain) and goes to the LordOS
 * clipboard; if it is only an image (image/png), it is kept here, so it stays available when the app
 * closes and LordOS shows it in the history. While the owning app lives, the other apps
 * receive from it any format it offers (the request is forwarded to it). The LordOS
 * selection (terminal, text fields, an image from the history) is offered to the focused app. */

#define MAX_MIMES 32
#define CLIP_MAX (16u << 20)
typedef struct DOffer DOffer;
/* used: 1 once it was a selection, 2 once it started a drag (a source serves one of the two); actions: its
 * set_actions (drag and drop); offer: its drag offer that may still read it (the one under the pointer, or the
 * dropped one until it finishes) */
typedef struct { struct wl_resource *res; char mimes[MAX_MIMES][256]; int nmimes; uint32_t actions; int actions_set, used; DOffer *offer; } Source;
static Source *selection;           /* source of the app owning the selection, or NULL (LordOS) */
/* Bumped whenever the selection changes: an offer is only good for the selection it was made for,
 * and only while its client has the keyboard (an app can't keep an old offer to read later copies). */
static unsigned clip_offer_gen = 1;
typedef struct { Source *src; unsigned gen; } OfferRef;
static unsigned clip_seen;          /* LordOS clipboard generation already offered */
static unsigned char *clip_png;     /* last copied image */
static size_t clip_png_len;
static unsigned clip_img_gen;
static int clip_is_image;           /* the last thing copied was that image (and not text) */
static char clip_png_path[160];
static char clip_img_origin[128] = "linux";
static int clip_img_sensitive;
/* Rich text: an HTML version of the clipboard's text, valid while the text is the one it came with (its generation).
 * Read from an app that offers text/html beside its text, or set by Machines (a guest's rich text, sanitized by
 * lord-vm); offered again as text/html to Wayland and X11 apps. Never for a secret. */
static char *clip_html;
static size_t clip_html_len;
static unsigned clip_html_gen;          /* term_clipboard_gen() it belongs to (0: none) */
static char *clip_html_pend;            /* read from an app before its text arrived */
static size_t clip_html_pend_len;
static unsigned clip_html_pend_offer, clip_text_offer;
/* Files: the paths copied in Files (lord.clipboard.setFiles; its text is the paths) or in a Linux file manager
 * (its text/uri-list or x-special/gnome-copied-files, read beside its text and checked by urilist.c), valid
 * like the HTML while the text is the one it came with. Offered again in both formats. A cut counts only
 * when Files made it: what another app cut is pasted as a copy. Pasting them in Files copies real files, so
 * they are kept only from an app outside a sandbox (never an isolated LordOS app nor a Flatpak one, which could
 * name files they can't read for Files to copy where they can), and only when the selection's text is absent
 * or names exactly those paths (urilist.h uril_accept). Files asks before pasting them (apps/files/clipfiles.js).
 * X11 apps' lists are never read: an X client's sandbox can't be told, so their copy is text only (xwm.c). */
typedef struct { char *list; size_t len; int count, cut, own, src /* URIL_FROM_* */; unsigned gen; char from[256]; } ClipFiles;   /* list: urilist.h plist; from: the app's name */
static ClipFiles clip_files, clip_files_pend;
static unsigned clip_files_pend_offer;
#define MIME_URIS "text/uri-list"
#define MIME_GNOME_FILES "x-special/gnome-copied-files"

static int text_mime(const char *m) {
    return !strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "text/plain") || !strcmp(m, "UTF8_STRING") ||
           !strcmp(m, "STRING") || !strcmp(m, "TEXT");
}

static int client_from(struct wl_client *c);

static void src_offer(struct wl_client *c, struct wl_resource *r, const char *mime) {
    (void)c;
    Source *s = wl_resource_get_user_data(r);
    /* Offered back as written: an app can recognize its own selection by the whole name, so a
     * shortened one reads as someone else's and it drops the data. A name that doesn't fit is left out. */
    if (s->nmimes < MAX_MIMES && strlen(mime) < sizeof s->mimes[0]) snprintf(s->mimes[s->nmimes++], sizeof s->mimes[0], "%s", mime);
}
static void src_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
#define DND_ALL (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
static void src_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) {
    (void)c;
    Source *s = wl_resource_get_user_data(r);
    if (a & ~(uint32_t)DND_ALL) { wl_resource_post_error(r, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, "invalid dnd actions 0x%x", a); return; }
    if (s->used) return;                /* only before start_drag (lenient: ignored, not an error) */
    s->actions = a;
    s->actions_set = 1;
}
static const struct wl_data_source_interface source_impl = { .offer = src_offer, .destroy = src_destroy, .set_actions = src_actions };
static void offer_to_focus(void);
static void dnd_source_gone(Source *s);
static void src_gone(struct wl_resource *r) {
    Source *s = wl_resource_get_user_data(r);
    dnd_source_gone(s);
    if (selection == s) {
        selection = NULL;
        clip_offer_gen++;
        if (clip_is_image) offer_to_focus();    /* the image stays, now LordOS offers it */
    }
    free(s);
}

/* The image is also saved as a file in $XDG_RUNTIME_DIR/lordos (user-only),
 * for the history preview; the last 10 are kept. */
static void store_png(const unsigned char *png, size_t n) {
    unsigned char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, png, n);
    free(clip_png);
    clip_png = copy;
    clip_png_len = n;
    clip_img_gen++;
    clip_is_image = 1;
    clip_png_path[0] = 0;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return;
    char dir[128], old[160];
    snprintf(dir, sizeof dir, "%s/lordos", rt);
    mkdir(dir, 0700);
    if (clip_img_gen > 10) { snprintf(old, sizeof old, "%s/clip-%u.png", dir, clip_img_gen - 10); unlink(old); }
    snprintf(clip_png_path, sizeof clip_png_path, "%s/clip-%u.png", dir, clip_img_gen);
    int fd = open(clip_png_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    size_t off = 0;
    while (fd >= 0 && off < n) { ssize_t w = write(fd, png + off, n - off); if (w <= 0) break; off += (size_t)w; }
    if (fd < 0 || close(fd) || off != n) clip_png_path[0] = 0;
}

static void html_adopt(char *h, size_t n) {
    free(clip_html);
    clip_html = h;
    clip_html_len = n;
    clip_html_gen = h ? term_clipboard_gen() : 0;
}

/* takes f's list (f is left empty): it goes with the clipboard's current text */
static void files_adopt(ClipFiles *f) {
    free(clip_files.list);
    clip_files = *f;
    clip_files.gen = f->list ? term_clipboard_gen() : 0;
    memset(f, 0, sizeof *f);
}

/* reading an app's selection into LordOS (files: 1 its text/uri-list, 2 its gnome-copied-files) */
typedef struct {
    int fd, image, sensitive, primary, html, html_only, files, files_only, full /* cut short */, from /* URIL_FROM_* */;
    pid_t pid;
    unsigned offer;
    struct wl_event_source *ev;
    char *buf;
    size_t len, cap;
    char origin[128], name[256];    /* name: the app's app_id or window title, for Files' question ("" if none) */
} Reader;
static void prim_store(const char *t, size_t n, pid_t pid);
static unsigned prim_req;           /* bumped by each new primary selection: a slower, older read is dropped */

static int reader_ready(int fd, uint32_t mask, void *data) {
    Reader *rd = data;
    (void)mask;
    for (;;) {
        if (rd->len + 4096 > rd->cap) {
            size_t cap = rd->cap ? rd->cap * 2 : 8192;
            if (cap > CLIP_MAX) { rd->full = 1; break; }
            char *nb = realloc(rd->buf, cap);
            if (!nb) { rd->full = 1; break; }
            rd->buf = nb;
            rd->cap = cap;
        }
        ssize_t n = read(fd, rd->buf + rd->len, rd->cap - rd->len);
        if (n > 0) { rd->len += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (n < 0) rd->full = 1;            /* an error: what came may be cut short (a file list is then dropped) */
        break;                              /* end (or error): done */
    }
    if (rd->primary) {
        if (rd->len && rd->offer == prim_req) prim_store(rd->buf, rd->len, rd->pid);   /* a newer selection wins */
    } else if (rd->html) {
        /* the text/html beside an app's text: kept with that text (whichever of the two arrives last) */
        char *h = rd->len && rd->offer == clip_offer_gen ? malloc(rd->len) : NULL;
        if (h) memcpy(h, rd->buf, rd->len);
        if (h && rd->html_only) {
            /* an app that offers rich text only: its text is the HTML without tags (a few entities back) */
            char *t = malloc(rd->len + 1);
            size_t k = 0;
            int tag = 0;
            for (size_t i = 0; t && i < rd->len; i++) {
                char c = rd->buf[i];
                if (tag) { if (c == '>') tag = 0; continue; }
                if (c == '<') { tag = 1; continue; }
                if (c == '&' && i + 3 < rd->len && !strncmp(rd->buf + i, "&lt;", 4)) { t[k++] = '<'; i += 3; continue; }
                if (c == '&' && i + 3 < rd->len && !strncmp(rd->buf + i, "&gt;", 4)) { t[k++] = '>'; i += 3; continue; }
                if (c == '&' && i + 4 < rd->len && !strncmp(rd->buf + i, "&amp;", 5)) { t[k++] = '&'; i += 4; continue; }
                t[k++] = c;
            }
            if (t && k) {
                term_set_clipboard(t, k);
                term_clipboard_mark(rd->origin, rd->sensitive);
                clip_seen = term_clipboard_gen();
                clip_is_image = 0;
                clip_text_offer = rd->offer;
                if (!rd->sensitive) { html_adopt(h, rd->len); h = NULL; }
            }
            free(t);
            free(h);
        } else if (h && clip_text_offer == rd->offer) html_adopt(h, rd->len);
        else if (h) { free(clip_html_pend); clip_html_pend = h; clip_html_pend_len = rd->len; clip_html_pend_offer = rd->offer; }
    } else if (rd->files) {
        /* the files beside an app's text, kept with that text like the HTML; another app's cut is a copy */
        int cut = 0, count = 0;
        size_t pl = 0;
        ClipFiles f = { 0 };
        f.list = rd->len && !rd->full && rd->offer == clip_offer_gen && !rd->sensitive ? uril_parse(rd->buf, rd->len, rd->files == 2, &cut, &pl, &count) : NULL;
        f.len = pl;
        f.count = count;
        f.src = rd->from;
        snprintf(f.from, sizeof f.from, "%s", rd->name);
        if (f.list && rd->files_only) {
            /* an app that offers only files: its text is their paths (the files too, if the app isn't sandboxed) */
            size_t tn = 0;
            char *t = uril_encode(f.list, f.len, URIL_PLAIN, 0, &tn);
            if (t) {
                term_set_clipboard(t, tn);
                term_clipboard_mark(rd->origin, 0);
                clip_seen = term_clipboard_gen();
                clip_is_image = 0;
                clip_text_offer = rd->offer;
                html_adopt(NULL, 0);
                if (uril_accept(f.src, NULL, 0, f.list, f.len)) files_adopt(&f);
            }
            free(t);
        } else if (f.list && clip_text_offer == rd->offer) {
            const char *t = term_clipboard();
            if (uril_accept(f.src, t, strlen(t), f.list, f.len)) files_adopt(&f);
        } else if (f.list) { free(clip_files_pend.list); clip_files_pend = f; clip_files_pend_offer = rd->offer; f.list = NULL; }
        free(f.list);
    } else if (rd->len && rd->image) {
        if (rd->len > 8 && !memcmp(rd->buf, "\x89PNG\r\n\x1a\n", 8)) {
            store_png((unsigned char *)rd->buf, rd->len);
            snprintf(clip_img_origin, sizeof clip_img_origin, "%s", rd->origin);
            clip_img_sensitive = rd->sensitive;
        }
    } else if (rd->len) {
        term_set_clipboard(rd->buf, rd->len);
        term_clipboard_mark(rd->origin, rd->sensitive);
        clip_seen = term_clipboard_gen();       /* not a change from LordOS */
        clip_is_image = 0;
        clip_text_offer = rd->offer;
        if (clip_html_pend && clip_html_pend_offer == rd->offer && !rd->sensitive) { html_adopt(clip_html_pend, clip_html_pend_len); clip_html_pend = NULL; }
        else html_adopt(NULL, 0);
        if (clip_files_pend.list && clip_files_pend_offer == rd->offer) {
            if (uril_accept(clip_files_pend.src, rd->buf, rd->len, clip_files_pend.list, clip_files_pend.len)) files_adopt(&clip_files_pend);
            else { free(clip_files_pend.list); memset(&clip_files_pend, 0, sizeof clip_files_pend); }
        }
    }
    wl_event_source_remove(rd->ev);
    close(rd->fd);
    free(rd->buf);
    free(rd);
    return 0;
}

/* a second format of the same selection (files: 0 its HTML, else the kind of file list), kept with its text;
 * main: the reader of that text (where the selection comes from) */
static void read_beside(Source *s, const char *mime, int files, const Reader *main) {
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK)) return;
    wl_data_source_send_send(s->res, mime, p[1]);
    close(p[1]);
    Reader *rd = calloc(1, sizeof *rd);
    if (!rd) { close(p[0]); return; }
    rd->fd = p[0];
    rd->html = !files;
    rd->files = files;
    rd->from = main->from;
    snprintf(rd->name, sizeof rd->name, "%s", main->name);
    rd->offer = clip_offer_gen;
    rd->ev = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, reader_ready, rd);
    if (!rd->ev) { close(p[0]); free(rd); }
}

static void read_selection(Source *s) {
    if (getenv("LORD_WL_DEBUG")) for (int i = 0; i < s->nmimes; i++) fprintf(stderr, "wayland: selection %s\n", s->mimes[i]);
    const char *mime = NULL;
    int image = 0;
    for (int i = 0; i < s->nmimes && !mime; i++) if (!strcmp(s->mimes[i], "text/plain;charset=utf-8")) mime = s->mimes[i];
    for (int i = 0; i < s->nmimes && !mime; i++) if (text_mime(s->mimes[i])) mime = s->mimes[i];
    /* files: the standard list first; the gnome one only says copy or cut, and a cut is pasted as a copy anyway */
    const char *files = NULL;
    for (int i = 0; i < s->nmimes && !files; i++) if (!strcmp(s->mimes[i], MIME_URIS)) files = s->mimes[i];
    for (int i = 0; i < s->nmimes && !files; i++) if (!strcmp(s->mimes[i], MIME_GNOME_FILES)) files = s->mimes[i];
    int files_only = !mime && files;
    if (files_only) mime = files;
    for (int i = 0; i < s->nmimes && !mime; i++) if (!strcmp(s->mimes[i], "image/png")) { mime = s->mimes[i]; image = 1; }
    int html_only = 0;
    for (int i = 0; i < s->nmimes && !mime; i++) if (!strcmp(s->mimes[i], "text/html")) { mime = s->mimes[i]; html_only = 1; }
    if (!mime) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK)) return;
    wl_data_source_send_send(s->res, mime, p[1]);
    close(p[1]);
    Reader *rd = calloc(1, sizeof *rd);
    if (!rd) { close(p[0]); return; }
    rd->fd = p[0];
    rd->image = image;
    rd->html = rd->html_only = html_only;
    rd->files_only = files_only;
    if (files_only) rd->files = strcmp(files, MIME_URIS) ? 2 : 1;
    /* origin: the app_id of a window of that app; sensitive if it is a password manager
     * (KDE/GNOME convention: offers x-kde-passwordManagerHint = secret) */
    struct wl_client *cl = wl_resource_get_client(s->res);
    for (int i = 0; i < MAX_SURF && !rd->origin[0]; i++)
        if (surfs[i] && surfs[i]->client == cl && surfs[i]->app_id[0]) snprintf(rd->origin, sizeof rd->origin, "%s", surfs[i]->app_id);
    if (!rd->origin[0]) snprintf(rd->origin, sizeof rd->origin, "linux");
    snprintf(rd->name, sizeof rd->name, "%s", strcmp(rd->origin, "linux") ? rd->origin : "");
    for (int i = 0; i < MAX_SURF && !rd->name[0]; i++)
        if (surfs[i] && surfs[i]->client == cl && surfs[i]->title[0]) snprintf(rd->name, sizeof rd->name, "%s", surfs[i]->title);
    rd->from = client_from(cl);
    for (int i = 0; i < s->nmimes; i++) if (!strcmp(s->mimes[i], "x-kde-passwordManagerHint")) rd->sensitive = 1;
    rd->offer = clip_offer_gen;
    rd->ev = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, reader_ready, rd);
    if (!rd->ev) { close(p[0]); free(rd); return; }
    /* rich text and files: their text/html and file list too, beside the text (not for an image, nor a secret) */
    if (image || html_only || files_only || rd->sensitive) return;
    int has_html = 0;
    for (int i = 0; i < s->nmimes; i++) if (!strcmp(s->mimes[i], "text/html")) has_html = 1;
    if (has_html) read_beside(s, "text/html", 0, rd);
    /* a sandboxed app's file list is not even read: its copy is text only */
    if (files && rd->from == URIL_FROM_APP) read_beside(s, files, strcmp(files, MIME_URIS) ? 2 : 1, rd);
}

/* writing to an app, without blocking the server if the app reads slowly */
typedef struct { int fd; struct wl_event_source *ev; unsigned char *buf; size_t len, off; } Writer;

static int writer_ready(int fd, uint32_t mask, void *data) {
    Writer *w = data;
    (void)mask;
    while (w->off < w->len) {
        ssize_t n = write(fd, w->buf + w->off, w->len - w->off);
        if (n > 0) { w->off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        break;
    }
    wl_event_source_remove(w->ev);
    close(w->fd);
    explicit_bzero(w->buf, w->len);       /* may be a password copied from Passwords */
    free(w->buf);
    free(w);
    return 0;
}

static void send_bytes(int fd, const void *data, size_t n) {
    Writer *w = calloc(1, sizeof *w);
    if (!w || !(w->buf = malloc(n ? n : 1))) { free(w); close(fd); return; }
    memcpy(w->buf, data, n);
    w->len = n;
    w->fd = fd;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    w->ev = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, writer_ready, w);
    if (!w->ev) { close(fd); free(w->buf); free(w); }
}

/* offers: from another app (forwarded) or from the LordOS clipboard (written) */
static void ofr_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime) {
    (void)c; (void)r; (void)serial; (void)mime;
}
static void ofr_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd) {
    OfferRef *ref = wl_resource_get_user_data(r);
    if (!ref || ref->gen != clip_offer_gen || !key_focus || key_focus->client != c) { close(fd); return; }   /* stale, or not focused */
    Source *src = ref->src;
    if (src && src == selection) { wl_data_source_send_send(src->res, mime, fd); close(fd); return; }
    if (!src && clip_is_image && clip_png && !strcmp(mime, "image/png")) { send_bytes(fd, clip_png, clip_png_len); return; }
    if (!src && !clip_is_image && text_mime(mime)) { const char *t = term_clipboard(); send_bytes(fd, t, strlen(t)); return; }
    size_t hn = 0;
    const char *html = !src ? wl_clip_html(&hn) : NULL;
    if (html && !strcmp(mime, "text/html")) { send_bytes(fd, html, hn); return; }
    int gnome = !strcmp(mime, MIME_GNOME_FILES);
    char *files = !src && (gnome || !strcmp(mime, MIME_URIS)) ? wl_clip_files_as(gnome ? URIL_GNOME : URIL_URI_LIST, &hn) : NULL;
    if (files) { send_bytes(fd, files, hn); free(files); return; }
    /* KDE/GNOME convention: clipboard managers skip an offer carrying this hint */
    if (!src && !clip_is_image && !strcmp(mime, "x-kde-passwordManagerHint") && term_clipboard_sensitive()) { send_bytes(fd, "secret", 6); return; }
    close(fd);
}
static void ofr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ofr_finish(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void ofr_actions(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t p) { (void)c; (void)r; (void)a; (void)p; }
static const struct wl_data_offer_interface offer_impl = {
    .accept = ofr_accept, .receive = ofr_receive, .destroy = ofr_destroy, .finish = ofr_finish, .set_actions = ofr_actions,
};

static void offer_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

static void offer_selection(struct wl_resource *dev) {
    struct wl_client *c = wl_resource_get_client(dev);
    /* the focused client gets every new selection, its own included (the protocol says so; its receive is
     * forwarded to its own source) */
    const char *t = term_clipboard();
    int image = !selection && clip_is_image && clip_png;
    if (!selection && !image && !t[0]) { wl_data_device_send_selection(dev, NULL); return; }
    struct wl_resource *o = wl_resource_create(c, &wl_data_offer_interface, wl_resource_get_version(dev), 0);
    if (!o) return;
    OfferRef *ref = malloc(sizeof *ref);
    if (!ref) { wl_resource_destroy(o); return; }
    ref->src = selection;
    ref->gen = clip_offer_gen;
    wl_resource_set_implementation(o, &offer_impl, ref, offer_gone);
    wl_data_device_send_data_offer(dev, o);
    if (selection) for (int i = 0; i < selection->nmimes; i++) wl_data_offer_send_offer(o, selection->mimes[i]);
    else if (image) wl_data_offer_send_offer(o, "image/png");
    else {
        wl_data_offer_send_offer(o, "text/plain;charset=utf-8");
        wl_data_offer_send_offer(o, "text/plain");
        wl_data_offer_send_offer(o, "UTF8_STRING");
        size_t hn = 0;
        if (wl_clip_html(&hn)) wl_data_offer_send_offer(o, "text/html");
        int count, cut, own;
        if (wl_clip_files(&hn, &count, &cut, &own)) { wl_data_offer_send_offer(o, MIME_URIS); wl_data_offer_send_offer(o, MIME_GNOME_FILES); }
        /* a secret copied in LordOS (Passwords, password fields): Linux clipboard managers must not keep it */
        if (term_clipboard_sensitive()) wl_data_offer_send_offer(o, "x-kde-passwordManagerHint");
    }
    wl_data_device_send_selection(dev, o);
}

/* Clipboard image for JS (history) and copying one from the history again. */
unsigned wl_clip_image_gen(void) { return clip_img_gen; }
const char *wl_clip_image_path(void) { return clip_is_image ? clip_png_path : ""; }
int wl_clip_is_image(void) { return clip_is_image; }
const unsigned char *wl_clip_png(size_t *len) {
    *len = clip_is_image && clip_png ? clip_png_len : 0;
    return *len ? clip_png : NULL;
}
const char *wl_clip_image_origin(void) { return clip_img_origin; }
const char *wl_clip_html(size_t *len) {
    int ok = clip_html && !clip_is_image && clip_html_gen == term_clipboard_gen() && !term_clipboard_sensitive();
    *len = ok ? clip_html_len : 0;
    return ok ? clip_html : NULL;
}
void wl_clip_set_html(const char *html, size_t n) {
    char *h = html && n ? malloc(n) : NULL;
    if (h) memcpy(h, html, n);
    html_adopt(h, h ? n : 0);
    if (dpy) offer_to_focus();
}
int wl_clip_image_sensitive(void) { return clip_img_sensitive; }
const char *wl_clip_files(size_t *len, int *count, int *cut, int *own) {
    int ok = clip_files.list && !clip_is_image && clip_files.gen == term_clipboard_gen() && !term_clipboard_sensitive();
    *len = ok ? clip_files.len : 0;
    *count = ok ? clip_files.count : 0;
    *cut = ok && clip_files.own && clip_files.cut;
    *own = ok && clip_files.own;
    return ok ? clip_files.list : NULL;
}
const char *wl_clip_files_from(void) {
    size_t n;
    int count, cut, own;
    return wl_clip_files(&n, &count, &cut, &own) && !own ? clip_files.from : "";
}
char *wl_clip_files_as(int fmt, size_t *len) {
    int count, cut, own;
    size_t n;
    const char *list = wl_clip_files(&n, &count, &cut, &own);
    return list ? uril_encode(list, n, fmt, cut, len) : NULL;
}
int wl_clip_set_files(const char *list, size_t len, int count, int cut) {
    ClipFiles f = { 0 };
    if (!list || !len || count <= 0 || !(f.list = malloc(len))) return -1;
    memcpy(f.list, list, len);
    f.len = len;
    f.count = count;
    f.cut = cut != 0;
    f.own = 1;
    files_adopt(&f);
    return 0;
}

/* Only two places: the compositor's previews ($XDG_RUNTIME_DIR/lordos/clip-N.png) and the
 * saved history (~/.local/share/lordos/clipboard/img-*.png). */
static int clip_path_ok(const char *path) {
    const char *rt = getenv("XDG_RUNTIME_DIR"), *home = getenv("HOME");
    char dir[PATH_MAX];
    size_t n = strlen(path);
    if (n < 6 || strcmp(path + n - 4, ".png") || strstr(path, "..")) return 0;
    for (int k = 0; k < 2; k++) {
        if (k == 0 && rt) snprintf(dir, sizeof dir, "%s/lordos/clip-", rt);
        else if (k == 1 && home) snprintf(dir, sizeof dir, "%s/.local/share/lordos/clipboard/img-", home);
        else continue;
        size_t dl = strlen(dir);
        if (!strncmp(path, dir, dl) && n > dl + 4 && !strchr(path + dl, '/')) return 1;
    }
    return 0;
}

int wl_clip_use_image(const char *path) {
    if (!dpy || !clip_path_ok(path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > CLIP_MAX) { if (fd >= 0) close(fd); return -1; }
    unsigned char *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) { ssize_t r = read(fd, buf + got, (size_t)st.st_size - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (!buf || got != (size_t)st.st_size) { free(buf); return -1; }
    if (selection) { wl_data_source_send_cancelled(selection->res); selection = NULL; }
    clip_offer_gen++;
    store_png(buf, got);
    snprintf(clip_img_origin, sizeof clip_img_origin, "lordos");
    clip_img_sensitive = 0;
    free(buf);
    offer_to_focus();
    return 0;
}

static void offer_to_focus(void) {
    if (!key_focus) return;
    struct wl_resource *d;
    FOR_CLIENT(&data_devices, key_focus->client, d) offer_selection(d);
}

static void dev_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, struct wl_resource *origin_,
                     struct wl_resource *icon, uint32_t serial);
static void dev_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial) {
    (void)c; (void)serial;
    Source *s = src ? wl_resource_get_user_data(src) : NULL;
    if (s && s->used == 2) { wl_resource_post_error(r, WL_DATA_DEVICE_ERROR_USED_SOURCE, "the source started a drag"); return; }
    if (s) s->used = 1;
    if (selection && selection != s) wl_data_source_send_cancelled(selection->res);
    selection = s;
    clip_offer_gen++;
    if (s) read_selection(s);
    offer_to_focus();                   /* the new selection, to the client with the keyboard (the owner, usually) */
}
static void dev_gone(struct wl_resource *r);
static const struct wl_data_device_interface device_impl = { .start_drag = dev_drag, .set_selection = dev_set_selection, .release = res_release };

static void ddm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Source *s = calloc(1, sizeof *s);
    struct wl_resource *res = s ? wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(s); wl_client_post_no_memory(c); return; }
    s->res = res;
    wl_resource_set_implementation(res, &source_impl, s, src_gone);
}
static void ddm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &device_impl, NULL, dev_gone);
    wl_list_insert(&data_devices, wl_resource_get_link(d));
    if (key_focus && key_focus->client == c) offer_selection(d);
}
static const struct wl_data_device_manager_interface ddm_impl = { .create_data_source = ddm_create_source, .get_data_device = ddm_get_device };

static void bind_ddm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

/* ---- primary selection (zwp_primary_selection_v1: select to copy, middle click to paste) ----
 * Like the clipboard, but text only and kept here: what the focused app selects is read once
 * (text/plain) and offered to the app that gets the keyboard next, and to X11 apps through
 * Xwayland's PRIMARY (xwm.c). Only the app with the keyboard may set it, with the serial of a
 * recent click or key of its own. LordOS's own text fields and the Terminal set it too
 * (wl_set_primary). With middle-click paste off (Settings → Mouse & Trackpad, wl_primary_paste)
 * it is still kept, but offered to no app. */
static struct wl_list prim_devices;
static int prim_paste = 1;
typedef struct { struct wl_resource *res; char mime[64]; int rank; } PSource;
static PSource *prim_src;
static char *prim_text;
static size_t prim_len;
static unsigned prim_gen;
static pid_t prim_pid;          /* the app that made it: ❖Esc reads it aloud only in front of that app (wl_focus_primary) */

static pid_t client_pid(struct wl_client *c) {
    pid_t pid = 0;
    uid_t uid;
    gid_t gid;
    if (c) wl_client_get_credentials(c, &pid, &uid, &gid);
    return pid;
}

static int prim_rank(const char *m) {
    return !strcmp(m, "text/plain;charset=utf-8") ? 4 : !strcmp(m, "UTF8_STRING") ? 3 : !strcmp(m, "text/plain") ? 2 : text_mime(m) ? 1 : 0;
}
static void prim_offer_to(struct wl_resource *dev);
static void prim_offer_focus(void) {
    if (!key_focus) return;
    struct wl_resource *d;
    FOR_CLIENT(&prim_devices, key_focus->client, d) prim_offer_to(d);
}
static void prim_store(const char *t, size_t n, pid_t pid) {
    char *nb = malloc(n + 1);
    if (!nb) return;
    memcpy(nb, t, n);
    nb[n] = 0;
    if (prim_text) explicit_bzero(prim_text, prim_len);
    free(prim_text);
    prim_text = nb;
    prim_len = strlen(nb);
    prim_pid = pid;
    prim_gen++;
    prim_offer_focus();
}

static void psrc_offer(struct wl_client *c, struct wl_resource *r, const char *mime) {
    (void)c;
    PSource *s = wl_resource_get_user_data(r);
    int rk = prim_rank(mime);
    if (s && rk > s->rank) { s->rank = rk; snprintf(s->mime, sizeof s->mime, "%s", mime); }
}
static const struct zwp_primary_selection_source_v1_interface psrc_impl = { .offer = psrc_offer, .destroy = res_release };
static void psrc_gone(struct wl_resource *r) {
    PSource *s = wl_resource_get_user_data(r);
    if (prim_src == s) prim_src = NULL;                     /* its text stays */
    free(s);
}

/* An offer carries the generation it was made for: it reads that selection only, and only while
 * its client has the keyboard. */
static void poffer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd) {
    unsigned *gen = wl_resource_get_user_data(r);
    if (gen && *gen == prim_gen && prim_paste && key_focus && key_focus->client == c && prim_text && prim_rank(mime)) { send_bytes(fd, prim_text, prim_len); return; }
    close(fd);
}
static const struct zwp_primary_selection_offer_v1_interface poffer_impl = { .receive = poffer_receive, .destroy = res_release };
static void poffer_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

static void prim_offer_to(struct wl_resource *dev) {
    struct wl_client *c = wl_resource_get_client(dev);
    if (!prim_text || !prim_len || !prim_paste) { zwp_primary_selection_device_v1_send_selection(dev, NULL); return; }
    struct wl_resource *o = wl_resource_create(c, &zwp_primary_selection_offer_v1_interface, wl_resource_get_version(dev), 0);
    if (!o) return;
    unsigned *gen = malloc(sizeof *gen);
    if (!gen) { wl_resource_destroy(o); return; }
    *gen = prim_gen;
    wl_resource_set_implementation(o, &poffer_impl, gen, poffer_gone);
    zwp_primary_selection_device_v1_send_data_offer(dev, o);
    zwp_primary_selection_offer_v1_send_offer(o, "text/plain;charset=utf-8");
    zwp_primary_selection_offer_v1_send_offer(o, "text/plain");
    zwp_primary_selection_offer_v1_send_offer(o, "UTF8_STRING");
    zwp_primary_selection_device_v1_send_selection(dev, o);
}

static void pdev_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial) {
    (void)r;
    if (!key_focus || key_focus->client != c) return;      /* only the app with the keyboard */
    if (!xact_serial_recent(serial, c, xact_now())) return;   /* and for a click or key it got (not a made-up serial) */
    PSource *s = src ? wl_resource_get_user_data(src) : NULL;
    if (prim_src && prim_src != s) zwp_primary_selection_source_v1_send_cancelled(prim_src->res);
    prim_src = s;
    prim_req++;
    if (!s || !s->rank) return;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK)) return;
    zwp_primary_selection_source_v1_send_send(s->res, s->mime, p[1]);
    close(p[1]);
    Reader *rd = calloc(1, sizeof *rd);
    if (!rd) { close(p[0]); return; }
    rd->fd = p[0];
    rd->primary = 1;
    rd->offer = prim_req;
    rd->pid = client_pid(c);
    rd->ev = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, reader_ready, rd);
    if (!rd->ev) { close(p[0]); free(rd); }
}
static const struct zwp_primary_selection_device_v1_interface pdev_impl = { .set_selection = pdev_set_selection, .destroy = res_release };

static void pdm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    PSource *s = calloc(1, sizeof *s);
    struct wl_resource *res = s ? wl_resource_create(c, &zwp_primary_selection_source_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(s); wl_client_post_no_memory(c); return; }
    s->res = res;
    wl_resource_set_implementation(res, &psrc_impl, s, psrc_gone);
}
static void pdm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *d = wl_resource_create(c, &zwp_primary_selection_device_v1_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &pdev_impl, NULL, link_gone);
    wl_list_insert(&prim_devices, wl_resource_get_link(d));
    if (key_focus && key_focus->client == c) prim_offer_to(d);
}
static const struct zwp_primary_selection_device_manager_v1_interface pdm_impl = {
    .create_source = pdm_create_source, .get_device = pdm_get_device, .destroy = res_release,
};
static void bind_pdm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_primary_selection_device_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pdm_impl, NULL, NULL);
}

/* ---- xdg_positioner ---- */

typedef struct {
    int w, h, ax, ay, aw, ah, anchor, gravity, constraint, ox, oy;
} Positioner;

static void pos_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c; Positioner *p = wl_resource_get_user_data(r); p->w = w; p->h = h;
}
static void pos_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; Positioner *p = wl_resource_get_user_data(r); p->ax = x; p->ay = y; p->aw = w; p->ah = h;
}
static void pos_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; ((Positioner *)wl_resource_get_user_data(r))->anchor = (int)a; }
static void pos_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) { (void)c; ((Positioner *)wl_resource_get_user_data(r))->gravity = (int)g; }
static void pos_constraint(struct wl_client *c, struct wl_resource *r, uint32_t k) { (void)c; ((Positioner *)wl_resource_get_user_data(r))->constraint = (int)k; }
static void pos_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    (void)c; Positioner *p = wl_resource_get_user_data(r); p->ox = x; p->oy = y;
}
static void pos_reactive(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void pos_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void pos_parent_conf(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy, .set_size = pos_size, .set_anchor_rect = pos_rect, .set_anchor = pos_anchor,
    .set_gravity = pos_gravity, .set_constraint_adjustment = pos_constraint, .set_offset = pos_offset,
    .set_reactive = pos_reactive, .set_parent_size = pos_parent_size, .set_parent_configure = pos_parent_conf,
};
static void pos_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

static int has_left(int e) { return e == XDG_POSITIONER_ANCHOR_LEFT || e == XDG_POSITIONER_ANCHOR_TOP_LEFT || e == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT; }
static int has_right(int e) { return e == XDG_POSITIONER_ANCHOR_RIGHT || e == XDG_POSITIONER_ANCHOR_TOP_RIGHT || e == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT; }
static int has_top(int e) { return e == XDG_POSITIONER_ANCHOR_TOP || e == XDG_POSITIONER_ANCHOR_TOP_LEFT || e == XDG_POSITIONER_ANCHOR_TOP_RIGHT; }
static int has_bottom(int e) { return e == XDG_POSITIONER_ANCHOR_BOTTOM || e == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT || e == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT; }
/* (anchor and gravity use the same values) */
static int flip_v(int e) {
    switch (e) {
    case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
    case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    default: return e;
    }
}
static int flip_h(int e) {
    switch (e) {
    case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
    case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    default: return e;
    }
}

static void place(const Positioner *p, int anchor, int gravity, int *x, int *y) {
    int ax = p->ax + (has_left(anchor) ? 0 : has_right(anchor) ? p->aw : p->aw / 2);
    int ay = p->ay + (has_top(anchor) ? 0 : has_bottom(anchor) ? p->ah : p->ah / 2);
    *x = ax - (has_left(gravity) ? p->w : has_right(gravity) ? 0 : p->w / 2) + p->ox;
    *y = ay - (has_top(gravity) ? p->h : has_bottom(gravity) ? 0 : p->h / 2) + p->oy;
}

/* Menu position (relative to the parent's window geometry), within the screen. */
static void position_popup(Surf *s, const Positioner *p) {
    int x, y;
    place(p, p->anchor, p->gravity, &x, &y);
    float ox = 0, oy = 0;
    if (s->parent && origin(s->parent, &ox, &oy)) {
        ox += s->parent->gx;
        oy += s->parent->gy;
        /* within the display the parent is on (its rect, in the parent's terms) */
        float bx = 0, by = 0, W = ui.width, H = ui.height;
        int pslot = slot_for_box(ox, oy, ox + 1, oy + 1);
        if (pslot >= 0) { float rw, rh; oslot_rect(pslot, &bx, &by, &rw, &rh); W = bx + rw; H = by + rh; }
        ox -= bx; oy -= by; W -= bx; H -= by;
        if ((oy + y + p->h > H || oy + y < 0) && (p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y)) {
            int fx, fy;
            place(p, flip_v(p->anchor), flip_v(p->gravity), &fx, &fy);
            if (oy + fy >= 0 && oy + fy + p->h <= H) y = fy;
        }
        if ((ox + x + p->w > W || ox + x < 0) && (p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X)) {
            int fx, fy;
            place(p, flip_h(p->anchor), flip_h(p->gravity), &fx, &fy);
            if (ox + fx >= 0 && ox + fx + p->w <= W) x = fx;
        }
        if (ox + x + p->w > W) x = (int)(W - p->w - ox);       /* swipe */
        if (oy + y + p->h > H) y = (int)(H - p->h - oy);
        if (ox + x < 0) x = (int)-ox;
        if (oy + y < 0) y = (int)-oy;
    }
    s->pop_x = x;
    s->pop_y = y;
    s->conf_w = p->w;
    s->conf_h = p->h;
}

/* ---- xdg_toplevel ---- */

static void top_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void top_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r), *o = p ? wl_resource_get_user_data(p) : NULL;
    if (s) set_owner(s, o && o->role == R_TOPLEVEL && o->top ? o : NULL, NULL);     /* only a live toplevel parents */
}
static void top_title(struct wl_client *c, struct wl_resource *r, const char *t) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    snprintf(s->title, sizeof s->title, "%s", t);
    if (s->mapped) { char e[520]; json_str(e, sizeof e, s->title); emit(s, "title", "{\"title\":\"%s\"}", e); }
}
static void top_app_id(struct wl_client *c, struct wl_resource *r, const char *a) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (s) snprintf(s->app_id, sizeof s->app_id, "%s", a);
}
/* The app's own bar asked for the window menu (right click on Chromium's tab strip, a GTK
 * headerbar): JS gets where, in screen coordinates, and the theme shows its window menu there. */
static void top_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
    (void)c; (void)seat; (void)serial;
    Surf *s = wl_resource_get_user_data(r);
    float ox, oy;
    if (!s || !s->mapped || !origin(s, &ox, &oy)) return;
    emit(s, "menu", "{\"x\":%.0f,\"y\":%.0f}", ox + (float)x, oy + (float)y);
}
static void top_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial;
    Surf *s = wl_resource_get_user_data(r);
    if (s && s->mapped) { buttons_held = 0; emit(s, "move", "{\"x\":%.0f,\"y\":%.0f}", press_x, press_y); }
}
static void top_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)c; (void)seat; (void)serial;
    Surf *s = wl_resource_get_user_data(r);
    if (s && s->mapped) { buttons_held = 0; emit(s, "resize", "{\"edges\":%u,\"x\":%.0f,\"y\":%.0f}", edges, press_x, press_y); }
}
static void top_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (s) { s->max_w = w; s->max_h = h; }
}
static void top_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (s) { s->min_w = w; s->min_h = h; }
}
static void top_maximize(struct wl_client *c, struct wl_resource *r) {
    (void)c; Surf *s = wl_resource_get_user_data(r);
    if (s && s->mapped) emit(s, "maximize", "{}");
}
static void top_unmaximize(struct wl_client *c, struct wl_resource *r) {
    (void)c; Surf *s = wl_resource_get_user_data(r);
    if (s && s->mapped) emit(s, "unmaximize", "{}");
}
/* Fullscreen: presented by the compositor on top of everything (and straight to the card if possible). */
static int fs_refused;                  /* wl_fullscreen_allow */
static void leave_fullscreen(Surf *s);

/* A fullscreen surface is drawn over every layer, the sharing indicator included, so a share and a fullscreen surface can't
 * be on together: it goes back to its window, and no new one comes until the share ends. */
void wl_fullscreen_allow(int on) {
    if (fs_refused == !on) return;
    fs_refused = !on;
    if (fs_refused && dpy && fs_surf) leave_fullscreen(fs_surf);
}

static void enter_fullscreen(Surf *s) {
    if (!s || (!s->top && !s->xw) || s == fs_surf) return;
    if (fs_refused) { send_toplevel_configure(s); return; }
    if (s->parked || s->xmin) {      /* a hidden or minimized window is presented on nothing: it waits to come back */
        s->fs_resume = 1;
        send_toplevel_configure(s);
        return;
    }
    if (fs_surf) { fs_surf->fullscreen = 0; send_toplevel_configure(fs_surf); }
    fs_surf = s;
    s->fullscreen = 1;
    s->fs_saved_w = s->conf_w;
    s->fs_saved_h = s->conf_h;
    Node *fn = s->node ? node_get(s->node) : NULL;
    fs_slot = fn ? slot_for_box(fn->ax, fn->ay, fn->ax + fn->w, fn->ay + fn->h) : out_main;
    if (fs_slot < 0) fs_slot = out_main;
    float fx, fy, fw, fh;
    fs_rect(&fx, &fy, &fw, &fh);
    s->conf_w = (int)fw;
    s->conf_h = (int)fh;
    send_toplevel_configure(s);
    if (s->mapped) emit(s, "fullscreen", "{}");
    damage_add((Box){ 0, 0, ui.width, ui.height });
    ui.dirty = 1;
}

static void leave_fullscreen(Surf *s) {
    if (s) s->fs_resume = 0;
    if (!s || s != fs_surf) return;
    fs_surf = NULL;
    s->fullscreen = 0;
    if (s->fs_saved_w > 16) { s->conf_w = s->fs_saved_w; s->conf_h = s->fs_saved_h; }
    send_toplevel_configure(s);
    if (s->mapped) emit(s, "unfullscreen", "{}");
    damage_add((Box){ 0, 0, ui.width, ui.height });
    ui.dirty = 1;
}

static void top_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *out) {
    (void)c; (void)out;
    enter_fullscreen(wl_resource_get_user_data(r));
}
static void top_unfullscreen(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    leave_fullscreen(wl_resource_get_user_data(r));
}
static void top_minimize(struct wl_client *c, struct wl_resource *r) {
    (void)c; Surf *s = wl_resource_get_user_data(r);
    if (s && s->mapped) emit(s, "minimize", "{}");
}
static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = top_destroy, .set_parent = top_parent, .set_title = top_title, .set_app_id = top_app_id,
    .show_window_menu = top_menu, .move = top_move, .resize = top_resize, .set_max_size = top_max_size,
    .set_min_size = top_min_size, .set_maximized = top_maximize, .unset_maximized = top_unmaximize,
    .set_fullscreen = top_fullscreen, .unset_fullscreen = top_unfullscreen, .set_minimized = top_minimize,
};
static void top_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s || s->top != r) return;
    if (s->mapped) emit(s, "unmap", "{}");
    s->mapped = 0;
    s->top = NULL;
    orphan_dialogs(s);
    if (key_focus == s) { key_focus = NULL; wl_textinput_gone(); }
}

/* ---- xdg_popup ---- */

static void pop_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pop_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial;
    Surf *s = wl_resource_get_user_data(r);
    if (s) s->grab = 1;
}
static void pop_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *positioner, uint32_t token) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    popup_box_damage(s);
    position_popup(s, wl_resource_get_user_data(positioner));
    xdg_popup_send_repositioned(r, token);
    xdg_popup_send_configure(r, s->pop_x, s->pop_y, s->conf_w, s->conf_h);
    xdg_surface_send_configure(s->xdg, wl_display_next_serial(dpy));
}
static const struct xdg_popup_interface popup_impl = { .destroy = pop_destroy, .grab = pop_grab, .reposition = pop_reposition };
static void pop_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s || s->popup != r) return;
    unmap_popup(s);
    s->popup = NULL;
    s->mapped = 0;
    if (pointer_focus == s) pointer_focus = NULL;
}

/* ---- xdg_surface / xdg_wm_base ---- */

static void xs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xs_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "the wl_surface is gone"); return; }
    if (s->top || s->popup) { wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "the xdg_surface already has a role object"); return; }
    struct wl_resource *t = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    if (!t) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(t, &toplevel_impl, s, top_gone);
    s->top = t;
    s->role = R_TOPLEVEL;
    s->csd = !s->kde_ssd;                   /* until it asks for server decoration */
    if (wl_resource_get_version(t) >= 5) {
        struct wl_array caps;
        wl_array_init(&caps);
        uint32_t *p;
        if ((p = wl_array_add(&caps, sizeof *p))) *p = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
        if ((p = wl_array_add(&caps, sizeof *p))) *p = XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
        xdg_toplevel_send_wm_capabilities(t, &caps);
        wl_array_release(&caps);
    }
}
static void xs_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "the wl_surface is gone"); return; }
    if (s->top || s->popup) { wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "the xdg_surface already has a role object"); return; }
    struct wl_resource *p = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &popup_impl, s, pop_gone);
    s->popup = p;
    s->role = R_POPUP;
    s->parent = parent ? wl_resource_get_user_data(parent) : NULL;
    position_popup(s, wl_resource_get_user_data(positioner));
}
static void xs_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->p_gx = x; s->p_gy = y; s->p_gw = w; s->p_gh = h;
    s->p_geom = 1;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (s) s->acked_serial = serial;
}
static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xs_destroy, .get_toplevel = xs_toplevel, .get_popup = xs_popup,
    .set_window_geometry = xs_geometry, .ack_configure = xs_ack,
};
static void xs_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (s && s->xdg == r) s->xdg = NULL;
}

static void wm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wm_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Positioner *p = calloc(1, sizeof *p);
    struct wl_resource *res = p ? wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(p); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &positioner_impl, p, pos_gone);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    Surf *s = wl_resource_get_user_data(surface);
    /* one xdg_surface per wl_surface: a second one would keep pointing at the Surf after it is freed */
    if (!s || s->xdg) { wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "the wl_surface already has an xdg_surface"); return; }
    if (s->role == R_SUB || s->role == R_CURSOR || s->role == R_DNDICON) { wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "the wl_surface has another role"); return; }
    struct wl_resource *x = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    if (!x) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(x, &xdg_surface_impl, s, xs_gone);
    s->xdg = x;
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c; (void)r; (void)serial; }
static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_destroy, .create_positioner = wm_positioner, .get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};

static void bind_wm_base(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

/* ---- xdg-decoration: the app picks who draws the frame ----
 * We prefer the theme's frame (server side) and say so in the first configure, but the client's
 * choice wins: set_mode(CLIENT_SIDE) means the app draws its own bar and buttons (Chromium with
 * «Use system title bar and borders» off, GTK 4). unset_mode and SERVER_SIDE give the theme's frame.
 * A change after the window is mapped (the user flips Chromium's switch) goes to JS as «deco», and
 * the theme switches that window between its frame and the frameless presentation (sdk/wayland.js). */

static void set_csd(Surf *s, int csd) {
    csd = !!csd;
    if (s->csd == csd) return;
    s->csd = csd;
    if (s->mapped && (s->top || s->xw)) emit(s, "deco", "{\"csd\":%s}", csd ? "true" : "false");
}

/* What the compositor answers to a requested xdg-decoration mode: what the app asked for. */
static uint32_t deco_answer(uint32_t requested) {
    return requested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                                     : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

static void deco_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void deco_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    uint32_t m = deco_answer(mode);
    zxdg_toplevel_decoration_v1_send_configure(r, m);
    if (!s) return;
    set_csd(s, m == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    if (s->sent_configure) send_toplevel_configure(s);    /* the decoration configure needs an xdg_surface.configure */
}
static void deco_unset(struct wl_client *c, struct wl_resource *r) { deco_mode(c, r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = { .destroy = deco_destroy, .set_mode = deco_mode, .unset_mode = deco_unset };
static void deco_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s || s->deco != r) return;
    s->deco = NULL;
    /* protocol: without the object the app draws its own frame again (unless GTK 3 asked us via KDE's) */
    if (s->top) set_csd(s, !s->kde_ssd);
}
static void decm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void decm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *toplevel) {
    Surf *s = wl_resource_get_user_data(toplevel);
    if (s && s->deco) { wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, "the toplevel already has a decoration object"); return; }
    struct wl_resource *d = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &deco_impl, s, deco_gone);
    if (s) { s->deco = d; set_csd(s, 0); }
    zxdg_toplevel_decoration_v1_send_configure(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface decm_impl = { .destroy = decm_destroy, .get_toplevel_decoration = decm_get };
static void bind_decm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &decm_impl, NULL, NULL);
}

/* ---- org_kde_kwin_server_decoration (KDE): the one GTK 3 uses ----
 * GTK 3 does not speak xdg-decoration: if the compositor announces this protocol with «server» mode by
 * default, windows without their own bar (Firefox with the system title bar, GTK 3 apps
 * without a headerbar) ask for the theme's frame. It is a small protocol: the interface tables are
 * written here by hand (the wayland-scanner ones, without the plasma-wayland-protocols XML). */
enum { KDE_DECO_NONE = 0, KDE_DECO_CLIENT = 1, KDE_DECO_SERVER = 2 };
static const struct wl_interface kde_deco_interface;
static const struct wl_interface *kde_types[] = { NULL, &kde_deco_interface, &wl_surface_interface };
static const struct wl_message kde_mgr_requests[] = { { "create", "no", kde_types + 1 } };
static const struct wl_message kde_mgr_events[] = { { "default_mode", "u", kde_types } };
static const struct wl_interface kde_mgr_interface = {
    "org_kde_kwin_server_decoration_manager", 1, 1, kde_mgr_requests, 1, kde_mgr_events,
};
static const struct wl_message kde_deco_requests[] = { { "release", "", kde_types }, { "request_mode", "u", kde_types } };
static const struct wl_message kde_deco_events[] = { { "mode", "u", kde_types } };
static const struct wl_interface kde_deco_interface = {
    "org_kde_kwin_server_decoration", 1, 2, kde_deco_requests, 1, kde_deco_events,
};
struct kde_deco_impl {
    void (*release)(struct wl_client *, struct wl_resource *);
    void (*request_mode)(struct wl_client *, struct wl_resource *, uint32_t);
};
struct kde_mgr_impl { void (*create)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *); };

static void kde_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void kde_request_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    int ssd = mode == KDE_DECO_SERVER;
    wl_resource_post_event(r, 0, ssd ? KDE_DECO_SERVER : KDE_DECO_CLIENT);
    if (!s) return;
    s->kde_ssd = ssd;
    if (s->role == R_TOPLEVEL && !s->deco) set_csd(s, !ssd);   /* live too: Firefox's «Title Bar» switch */
}
static const struct kde_deco_impl kde_deco_impl = { kde_release, kde_request_mode };
static void kde_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (s && s->kde == r) s->kde = NULL;
}
static void kde_create(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    Surf *s = wl_resource_get_user_data(surface);
    struct wl_resource *d = wl_resource_create(c, &kde_deco_interface, wl_resource_get_version(r), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &kde_deco_impl, s, kde_gone);
    if (s && s->kde) wl_resource_set_user_data(s->kde, NULL);   /* no error in this protocol: the old one is detached */
    if (s) s->kde = d;
    wl_resource_post_event(d, 0, KDE_DECO_SERVER);
}
static const struct kde_mgr_impl kde_mgr_impl = { kde_create };
static void bind_kde_deco(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &kde_mgr_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &kde_mgr_impl, NULL, NULL);
    wl_resource_post_event(r, 0, KDE_DECO_SERVER);
}

/* ---- commit by role ---- */

static void geometry(Surf *s, int *w, int *h) {
    *w = s->has_geom && s->gw > 0 ? s->gw : s->lw;
    *h = s->has_geom && s->gh > 0 ? s->gh : s->lh;
}

/* JS learns about a window: title, app id, content size and whether it draws its own frame. */
static void emit_map(Surf *s, int w, int h) {
    char t[520], a[260];
    json_str(t, sizeof t, s->title);
    json_str(a, sizeof a, s->app_id);
    if (s->xw) {                    /* X11: WM_CLASS class, and its instance for the .desktop match */
        char alt[260];
        json_str(alt, sizeof alt, s->app_alt);
        emit(s, "map", "{\"title\":\"%s\",\"appId\":\"%s\",\"appAlt\":\"%s\",\"x11\":true,\"width\":%d,\"height\":%d,\"csd\":%s,\"dialog\":%s,\"fixed\":%s}",
             t, a, alt, w, h, s->csd ? "true" : "false", xwm_is_dialog(s->xw) ? "true" : "false", xwm_is_fixed(s->xw) ? "true" : "false");
    } else {
        int fixed = s->min_w > 0 && s->min_w == s->max_w && s->min_h > 0 && s->min_h == s->max_h;
        emit(s, "map", "{\"title\":\"%s\",\"appId\":\"%s\",\"width\":%d,\"height\":%d,\"csd\":%s,\"dialog\":%s,\"fixed\":%s,\"parent\":%d,\"modal\":%s}",
             t, a, w, h, s->csd ? "true" : "false", s->owner || s->dlg ? "true" : "false", fixed ? "true" : "false",
             s->owner ? s->owner->id : 0, s->modal ? "true" : "false");
    }
}

/* A new JS context (the theme was switched live): the windows that are still mapped are announced
 * again, with their maximized state, so they come back in the new theme instead of staying
 * connected but invisible. Their node belongs to the old tree: forgotten until the next paint. */
void wl_remap_all(void) {
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s || s->role != R_TOPLEVEL || !s->mapped) continue;
        int w, h;
        geometry(s, &w, &h);
        s->node = 0;
        emit_map(s, w, h);
        if (s->maximized) emit(s, "maximize", "{}");
    }
}

static void surface_commit_role(Surf *s) {
    if (s->role == R_TOPLEVEL && s->content && s->lw > 0) {   /* how the app renders for HiDPI: once per change */
        int ratio = s->w * 100 / s->lw;
        if (ratio != s->logged_ratio) {
            s->logged_ratio = ratio;
            fprintf(stderr, "wayland: %s: buffer %dx%d for %dx%d logical (%.2fx; buffer_scale %d, viewport %s, fractional %s, preferred %d%%)\n",
                    s->app_id[0] ? s->app_id : "?", s->w, s->h, s->lw, s->lh, ratio / 100.0, s->bscale, s->viewport ? "yes" : "no",
                    s->frac ? "yes" : "no", s->pref_scale);
        }
    }
    if (s->role == R_TOPLEVEL) {
        if (!s->sent_configure) { send_toplevel_configure(s); return; }     /* first commit: no buffer */
        if (!s->has_geom) { s->gx = s->gy = 0; }
        int w, h;
        geometry(s, &w, &h);
        if (s->content && !s->mapped) {
            s->mapped = 1;
            emit_map(s, w, h);
            s->conf_w = w;
            s->conf_h = h;
        } else if (!s->content && s->mapped) {
            s->mapped = 0;
            emit(s, "unmap", "{}");
        } else if (s->mapped && s->acked_serial == s->conf_serial && (w != s->conf_w || h != s->conf_h)) {
            /* a commit made before the app read our last configure is its old size on the way out, not a
             * resize of its own: taken as one, the window kept the size of a maximize just undone */
            emit(s, "geometry", "{\"width\":%d,\"height\":%d}", w, h);
            s->conf_w = w;
            s->conf_h = h;
        }
        damage_node(s);
    } else if (s->role == R_POPUP) {
        if (!s->sent_configure) {
            xdg_popup_send_configure(s->popup, s->pop_x, s->pop_y, s->conf_w, s->conf_h);
            xdg_surface_send_configure(s->xdg, wl_display_next_serial(dpy));
            s->sent_configure = 1;
            return;
        }
        if (!s->has_geom) { s->gx = s->gy = 0; }
        if (s->content && !s->mapped && npopups < 32) { s->mapped = 1; popups[npopups++] = s; }
        else if (!s->content && s->mapped) { s->mapped = 0; unmap_popup(s); }
        popup_box_damage(s);
    } else if (s->role == R_SUB) {
        Surf *top = s;
        while (top->parent) top = top->parent;
        if (top->role == R_POPUP) popup_box_damage(top); else damage_node(top);
    }
}

/* ---- drawing ----
 * CPU path: each surface's pixels are copied into the UI canvas.
 * GPU path: the UI leaves a transparent hole where each window goes and the surface is noted;
 * when presenting, their textures are drawn below the UI (layer 0). App menus and
 * fullscreen go on top (layer 1). This keeps the order between LordOS windows and
 * app windows, and app content does not go through the CPU. */

/* "Structural" clip of a node (overflow of its ancestors), without the frame's damage. */
static Rect node_clip(int node) {
    Rect r = { 0, 0, (int)ui.width, (int)ui.height };
    for (Node *n = node ? node_get(node) : NULL; n; n = n->parent) {
        if (!n->s.overflow_hidden) continue;
        int x0 = (int)floorf(n->ax), y0 = (int)floorf(n->ay), x1 = (int)ceilf(n->ax + n->w), y1 = (int)ceilf(n->ay + n->h);
        if (x0 > r.x0) r.x0 = x0;
        if (y0 > r.y0) r.y0 = y0;
        if (x1 < r.x1) r.x1 = x1;
        if (y1 < r.y1) r.y1 = y1;
    }
    return r;
}

static Rect rect_and(Rect a, Rect b) {
    Rect r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}

static void add_draw(int layer, Surf *s, float x, float y, Rect clip, float op) {
    if (ndraws[layer] < 128) draws[layer][ndraws[layer]++] = (Draw){ s->id, x, y, clip, op, 1, 1 };
}

/* Is the node (or an ancestor) a protected window? (screenshots, media/capture.c) */
static int node_protected(int node) {
    for (Node *n = node ? node_get(node) : NULL; n; n = n->parent) if (n->protect) return 1;
    return 0;
}

static Surf *toplevel_of(Surf *s) {
    while (s && s->parent) s = s->parent;
    return s;
}

static int holes_off;                   /* wl_paint_holes(0): only note it (the ui/paint.c layer already has the hole) */
void wl_paint_holes(int on) { holes_off = !on; }

/* The canvas is the display's physical frame (paint_view): logical → physical. */
static inline float PVX(float x) { return (x - paint_view.x) * paint_view.k + paint_view.dx; }
static inline float PVY(float y) { return (y - paint_view.y) * paint_view.k + paint_view.dy; }
static Rect phys_rect(Rect r) {
    return (Rect){ (int)floorf(PVX((float)r.x0)), (int)floorf(PVY((float)r.y0)), (int)ceilf(PVX((float)r.x1)), (int)ceilf(PVY((float)r.y1)) };
}

/* Source rect of a surface's buffer (the viewport crop is in surface coordinates: × buffer scale). */
static void source_rect(const Surf *s, float *sx, float *sy, float *sw, float *sh) {
    *sx = 0; *sy = 0; *sw = (float)s->w; *sh = (float)s->h;
    if (s->viewport && s->vsw > 0) {
        float b = (float)(s->bscale > 0 ? s->bscale : 1);
        *sx = s->vsx * b; *sy = s->vsy * b; *sw = s->vsw * b; *sh = s->vsh * b;
    }
}

/* GPU path, a window inside a transformed node (the Overview, the minimize effect): ui/paint.c rasterizes
 * the node and blends the raster at its place, so the app's quad and its hole can't be where layout says.
 * While the raster is painted, each surface is noted at the transformed box. Its alpha 0 in the raster
 * keeps what the subtree paints over it (the Overview's hover border), and blending leaves the canvas as
 * it was, so wl_tf_open_holes clears the same box in the canvas before the raster goes over it. Quad and
 * hole are rounded to the same physical pixels. A rotation or a transform inside another one has no quad:
 * the frame is drawn at its place with nothing in the content, never the window at its full size. */
#define TF_HOLES 32
static struct { int on, skip; float sx, sy, tx, ty, op; Rect outer; } tfv;
static struct { float x, y, w, h; Rect clip; } tf_holes[TF_HOLES];
static int ntf_holes;

void wl_tf_begin(int node, float sx, float sy, float tx, float ty, float op, int skip) {
    const Node *n = node_get(node);
    tfv.on = 1; tfv.skip = skip; tfv.sx = sx; tfv.sy = sy; tfv.tx = tx; tfv.ty = ty; tfv.op = op;
    tfv.outer = n && n->parent ? node_clip(n->parent->id) : (Rect){ -65536, -65536, 65536, 65536 };
    ntf_holes = 0;
}
void wl_tf_end(void) { tfv.on = 0; }
void wl_tf_nested(int d) { tfv.skip += d; }

static Rect tf_map(Rect r) {
    float x0 = r.x0 * tfv.sx + tfv.tx, y0 = r.y0 * tfv.sy + tfv.ty, x1 = r.x1 * tfv.sx + tfv.tx, y1 = r.y1 * tfv.sy + tfv.ty;
    return rect_and(tfv.outer, (Rect){ (int)floorf(fminf(x0, x1)), (int)floorf(fminf(y0, y1)), (int)ceilf(fmaxf(x0, x1)), (int)ceilf(fmaxf(y0, y1)) });
}

static Rect tf_pixels(float x, float y, float w, float h) {
    return (Rect){ (int)lroundf(PVX(x)), (int)lroundf(PVY(y)), (int)lroundf(PVX(x + w)), (int)lroundf(PVY(y + h)) };
}

void wl_tf_open_holes(Canvas *c) {
    Rect old = c->clip;
    for (int i = 0; i < ntf_holes; i++) {
        canvas_set_clip(c, rect_and(old, rect_and(phys_rect(tf_holes[i].clip), tf_pixels(tf_holes[i].x, tf_holes[i].y, tf_holes[i].w, tf_holes[i].h))));
        canvas_clear(c, 0);
    }
    canvas_set_clip(c, old);
    ntf_holes = 0;
}

/* CPU path: the buffer into its logical box (x, y, lw × lh) at the display's resolution. A client
 * that rendered at exactly that size (fractional-scale) is copied 1:1; others are scaled. */
static void blit_surface(Canvas *c, const Surf *s, float x, float y, float sxk, float syk, float op) {
    float sx, sy, sw, sh;
    source_rect(s, &sx, &sy, &sw, &sh);
    float dx = PVX(x), dy = PVY(y), dw = s->lw * paint_view.k * sxk, dh = s->lh * paint_view.k * syk;
    if (sx == 0 && sy == 0 && sw == (float)s->w && sh == (float)s->h && fabsf(dw - sw) < 0.5f && fabsf(dh - sh) < 0.5f)
        canvas_blit(c, s->px, s->w, s->h, (int)lroundf(dx), (int)lroundf(dy), 0, op, s->opaque);
    else canvas_blit_scaled(c, s->px, s->w, s->w, s->h, sx, sy, sw, sh, dx, dy, dw, dh, op, s->opaque);
}

/* A screenshot while the GPU composes: a window whose content is only a dmabuf texture (a GPU app,
 * for example a browser) has no pixels on the CPU, so it is read back for the capture. Without this
 * the capture showed the theme's frame and shadow with nothing inside. malloc'd, or NULL. */
static uint32_t *capture_readback(const Surf *s) {
    if (!paint_capture_mode || s->px || !s->dmabuf || !s->content) return NULL;
    const GpuTex *t = s->dmabuf->tex;
    int w = s->dmabuf->d.width, h = s->dmabuf->d.height;
    if (!gpu_tex_current(t) || w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (px && gpu_tex_read(t, px, w, h) == 0) return px;
    free(px);
    return NULL;
}

static void paint_tree(Canvas *c, Surf *s, float x, float y, float op, Rect clip) {
    uint32_t *tmp;
    if (gpu_active() && !paint_capture_mode) {          /* in a screenshot they are always copied */
        if (tfv.on) {
            if (s->content && !tfv.skip && ntf_holes < TF_HOLES && ndraws[0] < 128) {
                float X = x * tfv.sx + tfv.tx, Y = y * tfv.sy + tfv.ty, W = s->lw * tfv.sx, H = s->lh * tfv.sy;
                Rect m = tf_map(clip);
                add_draw(0, s, X, Y, m, op * tfv.op);
                draws[0][ndraws[0] - 1].sx = tfv.sx;
                draws[0][ndraws[0] - 1].sy = tfv.sy;
                tf_holes[ntf_holes].x = X; tf_holes[ntf_holes].y = Y; tf_holes[ntf_holes].w = W; tf_holes[ntf_holes].h = H;
                tf_holes[ntf_holes++].clip = m;
                Rect old = c->clip;                     /* in the raster */
                canvas_set_clip(c, rect_and(rect_and(old, phys_rect(clip)),
                                            (Rect){ (int)floorf(PVX(x)), (int)floorf(PVY(y)), (int)ceilf(PVX(x + s->lw)), (int)ceilf(PVY(y + s->lh)) }));
                canvas_clear(c, 0);
                canvas_set_clip(c, old);
            }
        } else if (s->content && holes_off) add_draw(0, s, x, y, clip, op);
        else if (s->content) {
            add_draw(0, s, x, y, clip, op);
            /* hole in the UI: what was painted below shows the window */
            Rect old = c->clip;
            Rect h = rect_and(rect_and(old, phys_rect(clip)),
                              (Rect){ (int)floorf(PVX(x)), (int)floorf(PVY(y)), (int)ceilf(PVX(x + s->lw)), (int)ceilf(PVY(y + s->lh)) });
            canvas_set_clip(c, h);
            canvas_clear(c, 0);
            canvas_set_clip(c, old);
        }
        s->hole_sig = hole_sig(s);
    } else if (s->px) blit_surface(c, s, x, y, 1, 1, op);
    else if ((tmp = capture_readback(s))) {
        s->px = tmp;
        blit_surface(c, s, x, y, 1, 1, op);
        s->px = NULL;
        free(tmp);
    }
    for (int i = 0; i < s->nkids; i++) paint_tree(c, s->kids[i], x + s->kids[i]->sx, y + s->kids[i]->sy, op, clip);
}

void wl_paint_begin(void) { ndraws[0] = ndraws[1] = 0; }

/* What is below a moving window (ui/paint.c) is painted once: its app windows (GPU
 * path) are saved and noted again on every frame. */
static Draw kept[128];
static int nkept;
void wl_paint_keep(void) {
    nkept = ndraws[0];
    memcpy(kept, draws[0], sizeof kept[0] * (size_t)nkept);
}
void wl_paint_kept(void) {
    for (int i = 0; i < nkept && ndraws[0] < 128; i++) draws[0][ndraws[0]++] = kept[i];
}

/* LORD_PAINT_VERIFY (main.c): its check repaint notes the app windows again; the frame keeps its list. */
static Draw verify_draws[2][128];
static int verify_n[2];
void wl_paint_save(void) {
    memcpy(verify_draws, draws, sizeof verify_draws);
    memcpy(verify_n, ndraws, sizeof verify_n);
}
void wl_paint_restore(void) {
    memcpy(draws, verify_draws, sizeof draws);
    memcpy(ndraws, verify_n, sizeof ndraws);
}

/* GPU drag (ui/paint.c): whether an app window noted so far for layer 0 is drawn into r (physical px of the
 * display being painted): its box as draw_surface gives it, one pixel more on each side for the
 * filtering, cut by its clip. A window that is gone is not drawn. */
int wl_draws_meet(Rect r) {
    for (int i = 0; i < ndraws[0]; i++) {
        const Draw *d = &draws[0][i];
        const Surf *s = find(d->sid);
        if (!s) continue;
        Rect b = rect_and(phys_rect(d->clip), (Rect){ (int)floorf(PVX(d->x)) - 1, (int)floorf(PVY(d->y)) - 1,
                                                      (int)ceilf(PVX(d->x + s->lw * d->sx)) + 1, (int)ceilf(PVY(d->y + s->lh * d->sy)) + 1 });
        if (b.x0 < r.x1 && r.x0 < b.x1 && b.y0 < r.y1 && r.y0 < b.y1) return 1;
    }
    return 0;
}

void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op) {
    Surf *s = find(id);
    if (!s || s->role != R_TOPLEVEL) return;
    s->node = node;
    int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
    /* the theme's frame decides the size: if it changed, the app is asked (except in fullscreen) */
    if (s->mapped && s != fs_surf && iw > 16 && ih > 16 && (iw != s->conf_w || ih != s->conf_h)) {
        s->conf_w = iw;
        s->conf_h = ih;
        send_toplevel_configure(s);
    }
    if (s->xw && s->mapped && s != fs_surf && !paint_capture_mode) xwm_place(s->xw, (int)lroundf(x), (int)lroundf(y));   /* X routes the pointer by it */
    if (s == fs_surf) return;                   /* presented by the compositor, on top of everything */
    Rect box = { (int)floorf(x), (int)floorf(y), (int)ceilf(x + w), (int)ceilf(y + h) };
    Rect clip = tfv.on ? box : rect_and(node_clip(node), box);  /* node_clip is the untransformed tree's */
    Rect old = canvas_push_clip(c, PVX(x), PVY(y), w * paint_view.k, h * paint_view.k);
    paint_tree(c, s, x - s->gx, y - s->gy, op, clip);
    canvas_set_clip(c, old);
}

static Surf *dnd_icon_at(float *x, float *y);
void wl_paint_popups(Canvas *c) {
    if (gpu_active() && !paint_capture_mode) return;    /* GPU path: they go in layer 1 */
    for (int i = 0; i < npopups; i++) {
        float x, y;
        if (!origin(popups[i], &x, &y)) continue;
        Surf *top = toplevel_of(popups[i]);
        if (paint_capture_mode && top && node_protected(top->node)) {   /* menu of a protected window */
            canvas_rrect(c, PVX(x), PVY(y), popups[i]->lw * paint_view.k, popups[i]->lh * paint_view.k, 0, 0xff000000u);
            continue;
        }
        paint_tree(c, popups[i], x, y, 1, (Rect){ -65536, -65536, 65536, 65536 });
    }
    float ix, iy;
    Surf *icon = dnd_icon_at(&ix, &iy);
    if (icon) paint_tree(c, icon, ix, iy, 1, (Rect){ -65536, -65536, 65536, 65536 });
}

/* Texture with a surface's current content (GPU path). */
static GpuTex *surface_tex(Surf *s) {
    if (s->dmabuf) {
        GpuTex *t = dm_tex(s->dmabuf);
        if (t || !s->px) return t;
        /* this context does not import it: the last good frame (px) is drawn instead */
    }
    if (s->px && (s->tex_dirty || !s->tex)) {
        s->tex = gpu_tex_pixels(s->tex, s->px, s->w, s->h, s->opaque);
        s->tex_dirty = 0;
    }
    return s->tex;
}

static void draw_surface(Surf *s, float x, float y, float dw, float dh, Rect clip, float op) {
    /* x, y, dw, dh and clip are logical; the GPU draws in the display's physical pixels (paint_view) */
    GpuTex *t = s->content ? surface_tex(s) : NULL;
    if (!t) return;
    float sx, sy, sw, sh;
    source_rect(s, &sx, &sy, &sw, &sh);
    gpu_draw(t, PVX(x), PVY(y), dw * paint_view.k, dh * paint_view.k, sx, sy, sw, sh, phys_rect(clip), op);
}

static void draw_tree(Surf *s, float x, float y, float scale_x, float scale_y, Rect clip) {
    draw_surface(s, x, y, s->lw * scale_x, s->lh * scale_y, clip, 1);
    for (int i = 0; i < s->nkids; i++)
        draw_tree(s->kids[i], x + s->kids[i]->sx * scale_x, y + s->kids[i]->sy * scale_y, scale_x, scale_y, clip);
}

/* The fullscreen window takes the whole output (if its size does not match, it is scaled). */
static void fs_box(float *x, float *y, float *sx, float *sy) {
    int w, h;
    geometry(fs_surf, &w, &h);
    float fx, fy, fw, fh;
    fs_rect(&fx, &fy, &fw, &fh);                /* its display */
    *sx = w > 0 ? fw / w : 1;
    *sy = h > 0 ? fh / h : 1;
    *x = fx - fs_surf->gx * *sx;
    *y = fy - fs_surf->gy * *sy;
}

int wl_gpu_layers_empty(void) { float x, y; return !ndraws[0] && !ndraws[1] && !fs_surf && !npopups && !dnd_icon_at(&x, &y); }

void wl_gpu_draw_layer(int layer) {
    Rect all = { -65536, -65536, 65536, 65536 };
    if (layer == 0) {
        for (int i = 0; i < ndraws[0]; i++) {
            Draw *d = &draws[0][i];
            Surf *s = find(d->sid);
            if (!s || !s->content) continue;
            if (d->sx == 1 && d->sy == 1) { draw_surface(s, d->x, d->y, (float)s->lw, (float)s->lh, d->clip, d->op); continue; }
            Rect p = tf_pixels(d->x, d->y, s->lw * d->sx, s->lh * d->sy);
            GpuTex *t = surface_tex(s);
            float sx, sy, sw, sh;
            source_rect(s, &sx, &sy, &sw, &sh);
            if (t) gpu_draw(t, (float)p.x0, (float)p.y0, (float)(p.x1 - p.x0), (float)(p.y1 - p.y0), sx, sy, sw, sh, phys_rect(d->clip), d->op);
        }
        return;
    }
    if (fs_surf && fs_surf->content) {
        float x, y, sx, sy;
        fs_box(&x, &y, &sx, &sy);
        draw_tree(fs_surf, x, y, sx, sy, all);
    }
    for (int i = 0; i < npopups; i++) {
        float x, y;
        if (origin(popups[i], &x, &y)) draw_tree(popups[i], x, y, 1, 1, all);
    }
    float ix, iy;
    Surf *icon = dnd_icon_at(&ix, &iy);
    if (icon) draw_tree(icon, ix, iy, 1, 1, all);
}

/* CPU path: fullscreen and its menus are copied over the already built frame. */
void wl_paint_fullscreen(Canvas *out) {
    if (paint_capture_mode && fs_surf && fs_surf->content && node_protected(fs_surf->node)) {
        canvas_set_clip(out, (Rect){ 0, 0, out->w, out->h });
        canvas_clear(out, 0x000000);                /* protected fullscreen: black in the capture */
        return;
    }
    if (!fs_surf || !fs_surf->content) return;
    float x, y, sx, sy, fx, fy, fw, fh;
    fs_box(&x, &y, &sx, &sy);
    fs_rect(&fx, &fy, &fw, &fh);
    /* only on the display it covers (a mirror shows the same rect) */
    double vx1 = paint_view.x + (out->w - paint_view.dx) / paint_view.k, vy1 = paint_view.y + (out->h - paint_view.dy) / paint_view.k;
    if (fx >= vx1 || fy >= vy1 || fx + fw <= paint_view.x || fy + fh <= paint_view.y) return;
    uint32_t *tmp = fs_surf->px ? NULL : capture_readback(fs_surf);     /* a GPU app in a screenshot */
    if (!fs_surf->px && !tmp) return;
    if (tmp) fs_surf->px = tmp;
    Rect oc = out->clip;
    canvas_set_clip(out, rect_and(oc, (Rect){ (int)floorf(PVX(fx)), (int)floorf(PVY(fy)), (int)ceilf(PVX(fx + fw)), (int)ceilf(PVY(fy + fh)) }));
    blit_surface(out, fs_surf, x, y, sx, sy, 1);
    canvas_set_clip(out, oc);
    if (tmp) { fs_surf->px = NULL; free(tmp); }
    for (int i = 0; i < npopups; i++) {
        float px, py;
        if (origin(popups[i], &px, &py)) paint_tree(out, popups[i], px, py, 1, (Rect){ -65536, -65536, 65536, 65536 });
    }
    float ix, iy;
    Surf *icon = dnd_icon_at(&ix, &iy);
    if (icon) paint_tree(out, icon, ix, iy, 1, (Rect){ -65536, -65536, 65536, 65536 });
}

int wl_fullscreen_active(void) { return fs_surf && fs_surf->content; }
int wl_wants_frame(void) { return need_frame; }

/* Direct scanout: the fullscreen window has a dmabuf the size of the output, there are no
 * menus on top and the cursor does not need to be drawn (cursor plane or hidden). */
static void scanout_done(void *data) {
    Dmabuf *d = data;
    d->scanned = 0;
    int current = 0;
    for (int i = 0; i < MAX_SURF && !current; i++) if (surfs[i] && surfs[i]->dmabuf == d) current = 1;
    if (d->res && !current) wl_buffer_send_release(d->res);
    dm_unref(d);
}

int wl_try_scanout(int cursor_free) {
    float ix, iy;
    if (!gpu_active() || !fs_surf || !fs_surf->dmabuf || npopups || fs_surf->nkids || !cursor_free || dnd_icon_at(&ix, &iy)) return 0;
    Dmabuf *d = fs_surf->dmabuf;
    int pw = 0, ph = 0, nused = 0;              /* one display, at its physical size */
    for (int i = 0; i < OUT_MAX; i++) if (oslots[i].used) { nused++; pw = oslots[i].o.pw; ph = oslots[i].o.ph; }
    if (nused != 1 || fs_surf->viewport || fs_surf->gx || fs_surf->gy || !gpu_can_scanout(&d->d, pw, ph)) return 0;
    d->refs++;
    d->scanned = 1;
    if (gpu_scanout(&d->d, scanout_done, d)) { d->scanned = 0; dm_unref(d); return 0; }
    return 1;
}

void wl_frame_done(unsigned t) {
    if (!dpy) return;
    need_frame = 0;
    update_surface_outputs();
    for (int i = 0; i < nrelease; i++) {
        Dmabuf *d = to_release[i];
        if (d->res && !d->scanned) wl_buffer_send_release(d->res);
        dm_unref(d);
    }
    nrelease = 0;
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s) continue;
        struct wl_resource *cb, *tmp;
        wl_resource_for_each_safe(cb, tmp, &s->frames) {
            wl_callback_send_done(cb, t);
            wl_resource_destroy(cb);
        }
        wl_list_insert_list(s->sent_feedback.prev, &s->feedback);
        wl_list_init(&s->feedback);
    }
    wl_display_flush_clients(dpy);
    if (keylag_enabled) keylag_flushed();
}

int wl_frame_unseen(unsigned t) {
    if (!dpy) return 0;
    int waiting = 0;
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s) continue;
        struct wl_resource *r, *tmp;
        wl_resource_for_each_safe(r, tmp, &s->frames) { wl_callback_send_done(r, t); wl_resource_destroy(r); waiting = 1; }
        wl_resource_for_each_safe(r, tmp, &s->feedback) { wp_presentation_feedback_send_discarded(r); wl_resource_destroy(r); waiting = 1; }
        wl_resource_for_each_safe(r, tmp, &s->sent_feedback) { wp_presentation_feedback_send_discarded(r); wl_resource_destroy(r); waiting = 1; }
    }
    need_frame = 0;
    if (waiting) wl_display_flush_clients(dpy);
    return waiting;
}

/* ---- input ---- */

static Surf *surface_at(Surf *s, float lx, float ly, float *ox, float *oy) {
    for (int i = s->nkids - 1; i >= 0; i--) {
        Surf *k = s->kids[i];
        Surf *hit = surface_at(k, lx - k->sx, ly - k->sy, ox, oy);
        if (hit) return hit;
    }
    if (s->content && lx >= 0 && ly >= 0 && lx < s->lw && ly < s->lh && (!s->has_input || wlrgn_has(&s->input, lx, ly))) {
        *ox = lx; *oy = ly;
        return s;
    }
    return NULL;
}

static void pointer_enter(Surf *s, float lx, float ly) {
    if (pointer_focus == s) return;
    if (!s || !pointer_focus || s->client != pointer_focus->client) { cursor_surf = NULL; cursor_mode = 0; cursor_gen++; }
    struct wl_resource *p;
    if (pointer_focus) {
        FOR_CLIENT(&pointers, pointer_focus->client, p) {
            wl_pointer_send_leave(p, wl_display_next_serial(dpy), pointer_focus->res);
            if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
        }
    }
    pointer_focus = s;
    if (s) {
        FOR_CLIENT(&pointers, s->client, p) {
            wl_pointer_send_enter(p, wl_display_next_serial(dpy), s->res, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
            if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
        }
    }
    constraint_update();
}

static void dismiss_popups(void) {
    for (int i = npopups - 1; i >= 0; i--) if (popups[i]->popup) xdg_popup_send_popup_done(popups[i]->popup);
}

/* The surface under the pointer, a button held or not: menus first, then fullscreen, then the window the UI's
 * hit test found. lx, ly: the point in that surface. */
static Surf *surface_under(float x, float y, Node *hit, float *lx, float *ly) {
    Surf *target = NULL;
    for (int i = npopups - 1; i >= 0 && !target; i--) {
        float ox, oy;
        if (origin(popups[i], &ox, &oy)) target = surface_at(popups[i], x - ox, y - oy, lx, ly);
    }
    if (!target && fs_surf && fs_surf->content) {
        float ox, oy;
        if (origin(fs_surf, &ox, &oy)) target = surface_at(fs_surf, x - ox, y - oy, lx, ly);
    } else if (!target && hit && hit->type == NODE_SURFACE) {
        Surf *s = find(hit->term);
        float ox, oy;
        if (s && origin(s, &ox, &oy)) target = surface_at(s, x - ox, y - oy, lx, ly);
    }
    return target;
}

/* ---- drag and drop (wl_data_device.start_drag) ----
 * A Linux app starts a drag with the serial of the press it is in, on the window that got it. From then on
 * the pointer goes to no app until every button is up: the surface under it gets enter, motion and leave on its
 * data device, with a new wl_data_offer each time it enters (the source's types and, from version 3, its
 * actions). Releasing over a surface whose offer accepted a type (and agreed an action, from version 3) drops
 * there; anywhere else, or Escape, cancels. The icon (role "dnd icon") is drawn under the pointer above
 * everything. A second start_drag while one runs is cancelled.
 *
 * LordOS's Files is a source too (lord.wayland.dragFiles, wl_drag_files): while its drag is over a Linux
 * window the compositor offers the paths as text/uri-list and text/plain and writes them itself, from the event
 * loop and without blocking (send_bytes). And a target: an app's drag of files (text/uri-list) released over
 * LordOS itself is read by the compositor and handed to JS, where Files copies them into the folder under the
 * pointer (dnd_take_files). Only the UI's handling of that release may claim it (wl_drop_claim, DropGate);
 * one nobody claimed there is cancelled at once, and a claimed list has DNDG_READ_MS to arrive. The drags of
 * an isolated LordOS app or a sandboxed (Flatpak) app never hand files to LordOS: they may name files the app
 * itself can't reach (dnd_contained). Xwayland's windows take no drop yet (no XDND bridge), and neither do
 * isolated LordOS apps (a drop there needs a grant of its own, docs/DESKTOP-PARITY.md). */

#define DND_COPY WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
#define DND_MOVE WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
#define DND_ASK WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK
#define DND_RECEIVES 16                 /* receive requests one offer may make */

/* src: the client source while this is its offer (Source.offer), else NULL; lord: the generation of LordOS's
 * paths it offers (0: none) */
struct DOffer {
    struct wl_resource *res;
    Source *src;
    unsigned lord;
    uint32_t dest, pref, action;        /* the target's set_actions, and the action agreed */
    int accepted, dropped, finished, receives;
};

static struct {
    int on;                             /* a drag is running */
    struct wl_client *client;           /* who started it (NULL: LordOS's Files) */
    Source *src;                        /* its source (NULL: LordOS's paths, or a drag inside one client) */
    int lord;
    Surf *icon, *focus;                 /* the icon, and the surface under the pointer that got enter */
    struct wl_resource *dev;            /* the data device that got enter */
    DOffer *offer;                      /* the offer made for focus */
    int over_lord;                      /* over LordOS itself, told to the source as a copy of its files */
    int contained;                      /* started by an isolated or sandboxed app: no files for LordOS */
} drag;
static int seat_held;                   /* buttons down, wherever they were pressed (a drag ends when none is) */
static int shift_down;                  /* Shift held: a Files drag into a Linux app may move (it copies otherwise) */
/* the press a start_drag may name: its serials (one per wl_pointer of that client) and the surface it went to */
static uint32_t press_serials[4];
static int npress;
static Surf *press_surf;
/* LordOS's paths (wl_drag_files): kept after the drop for the offer reading them */
static char *lord_uris, *lord_text;
static size_t lord_uris_len, lord_text_len;
static unsigned lord_gen;
static const char *const lord_mimes[] = { "text/uri-list", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" };
#define NLORD_MIMES (sizeof lord_mimes / sizeof *lord_mimes)

static const char *app_of_client(struct wl_client *cl);

/* An app's files dropped on LordOS (dnd_take_files): claimed at the release, read from its source, then its
 * local paths for JS. */
static struct {
    Source *src;                        /* the source dropped (then being read), while it lives */
    DropGate gate;                      /* who may claim it: only the release that dropped it (wl_drop_claim) */
    int fd;
    struct wl_event_source *ev, *timer; /* the list's pipe, and how long it may take (DNDG_READ_MS) */
    char *buf, *paths;                  /* what the app wrote; the paths, each ended by its NUL */
    size_t len, cap;
    int npaths;
} in_drop = { .fd = -1 };

/* The drag icon, if one is shown, and where (under the pointer, moved by its attach and offset). */
static Surf *dnd_icon_at(float *x, float *y) {
    if (!drag.icon || !drag.icon->content) return NULL;
    *x = last_x + (float)drag.icon->dnd_x;
    *y = last_y + (float)drag.icon->dnd_y;
    return drag.icon;
}
static void dnd_icon_damage(void) {
    float x, y;
    Surf *icon = dnd_icon_at(&x, &y);
    if (!icon) return;
    if (gpu_active()) { need_frame = 1; return; }          /* above the UI, like the menus */
    damage_add((Box){ x - 2, y - 2, x + icon->lw + 2, y + icon->lh + 2 });
    ui.dirty = 1;
}

static int dnd_offers(const DOffer *o, const char *mime) {
    if (!mime) return 0;
    if (o->src) {
        for (int i = 0; i < o->src->nmimes; i++) if (!strcmp(o->src->mimes[i], mime)) return 1;
        return 0;
    }
    if (o->lord && o->lord == lord_gen) for (size_t i = 0; i < NLORD_MIMES; i++) if (!strcmp(lord_mimes[i], mime)) return 1;
    return 0;
}

/* The action both sides take: the target's preferred one, else copy, move, ask in that order. A source without
 * set_actions (before version 3) copies. Files' own drag into a Linux app only copies: dropped on a file manager,
 * a move would take the file out of its folder without the person asking; with Shift held it may move. */
static uint32_t dnd_source_actions(void) {
    if (drag.lord) return shift_down ? DND_COPY | DND_MOVE : DND_COPY;
    return drag.src && drag.src->actions_set ? drag.src->actions : DND_COPY;
}
static void dnd_choose(DOffer *o) {
    uint32_t both = dnd_source_actions() & o->dest, want = drag.lord ? (shift_down ? DND_MOVE : DND_COPY) : 0, a = 0;
    if (o->pref & both) a = o->pref;
    else if (want & both) a = want;
    else if (both & DND_COPY) a = DND_COPY;
    else if (both & DND_MOVE) a = DND_MOVE;
    else if (both & DND_ASK) a = DND_ASK;
    if (a == o->action) return;
    o->action = a;
    if (wl_resource_get_version(o->res) >= 3) wl_data_offer_send_action(o->res, a);
    if (o->src && wl_resource_get_version(o->src->res) >= 3) wl_data_source_send_action(o->src->res, a);
}

static void dnd_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime) {
    (void)c; (void)serial;
    DOffer *o = wl_resource_get_user_data(r);
    if (!o || o != drag.offer || o->dropped) return;
    o->accepted = dnd_offers(o, mime);
    if (o->src) wl_data_source_send_target(o->src->res, o->accepted ? mime : NULL);
}
static void dnd_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd) {
    (void)c;
    DOffer *o = wl_resource_get_user_data(r);
    /* only the offer under the pointer, or the dropped one until it finishes, and only for a type it offered */
    if (!o || (o != drag.offer && !o->dropped) || o->finished || !dnd_offers(o, mime) || ++o->receives > DND_RECEIVES) { close(fd); return; }
    if (o->src) { wl_data_source_send_send(o->src->res, mime, fd); close(fd); return; }
    if (!strcmp(mime, "text/uri-list")) send_bytes(fd, lord_uris, lord_uris_len);
    else send_bytes(fd, lord_text, lord_text_len);
}
static void dnd_finish(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    DOffer *o = wl_resource_get_user_data(r);
    if (!o || o->finished) return;
    if (!o->dropped || !o->accepted || !o->action || o->action == DND_ASK) {
        wl_resource_post_error(r, WL_DATA_OFFER_ERROR_INVALID_FINISH, "finish without a drop, a type or an action");
        return;
    }
    o->finished = 1;
    if (o->src) {
        if (wl_resource_get_version(o->src->res) >= 3) wl_data_source_send_dnd_finished(o->src->res);
        o->src->offer = NULL;
        o->src = NULL;
    }
}
static void dnd_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions, uint32_t pref) {
    (void)c;
    if (actions & ~(uint32_t)DND_ALL) { wl_resource_post_error(r, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK, "invalid dnd actions 0x%x", actions); return; }
    if (pref && (!(pref & actions) || (pref & (pref - 1)))) { wl_resource_post_error(r, WL_DATA_OFFER_ERROR_INVALID_ACTION, "invalid preferred action 0x%x", pref); return; }
    DOffer *o = wl_resource_get_user_data(r);
    /* after the drop only an "ask" is settled this way */
    if (!o || o->finished || (o != drag.offer && !(o->dropped && o->action == DND_ASK))) return;
    o->dest = actions;
    o->pref = pref;
    dnd_choose(o);
}
static void dnd_offer_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_data_offer_interface dnd_offer_impl = {
    .accept = dnd_accept, .receive = dnd_receive, .destroy = dnd_offer_destroy, .finish = dnd_finish, .set_actions = dnd_set_actions,
};
static void dnd_offer_gone(struct wl_resource *r) {
    DOffer *o = wl_resource_get_user_data(r);
    if (!o) return;
    if (drag.offer == o) drag.offer = NULL;
    if (o->src) {
        /* dropped and never finished: an offer before version 3 can't finish (it is done), a newer one gave up */
        if (o->dropped && !o->finished && wl_resource_get_version(o->src->res) >= 3) {
            if (wl_resource_get_version(r) < 3) wl_data_source_send_dnd_finished(o->src->res);
            else wl_data_source_send_cancelled(o->src->res);
        }
        if (o->src->offer == o) o->src->offer = NULL;
    }
    free(o);
}

/* The surface under the pointer leaves the drag: leave, and its offer stops reading the source (unless it was
 * dropped there: then it reads until it finishes). */
static void dnd_leave(void) {
    if (drag.over_lord && drag.src) wl_data_source_send_target(drag.src->res, NULL);
    drag.over_lord = 0;
    if (drag.dev) wl_data_device_send_leave(drag.dev);
    DOffer *o = drag.offer;
    if (o && !o->dropped) {
        if (o->src && o->src->offer == o) o->src->offer = NULL;
        o->src = NULL;
        o->lord = 0;
        if (drag.src) wl_data_source_send_target(drag.src->res, NULL);
    }
    drag.focus = NULL;
    drag.dev = NULL;
    drag.offer = NULL;
}

/* The newest data device of a client (where the drag's events go). */
static struct wl_resource *dnd_device(struct wl_client *c) {
    struct wl_resource *d;
    FOR_CLIENT(&data_devices, c, d) return d;
    return NULL;
}

/* Who may take the drag: a Wayland app's surface; a drag without a source stays inside the client that started it. */
static int dnd_takes(Surf *t) {
    if (!t || t->client == xwl_client || app_of_client(t->client)) return 0;
    return drag.src || drag.lord || t->client == drag.client;
}

/* An app's drag that carries files and may copy them: LordOS itself takes it (Files, dnd_take_files). */
static int dnd_files_in(void) {
    if (!drag.src || drag.contained || !(dnd_source_actions() & DND_COPY)) return 0;
    for (int i = 0; i < drag.src->nmimes; i++) if (!strcmp(drag.src->mimes[i], "text/uri-list")) return 1;
    return 0;
}
static void dnd_over_lord(void) {
    if (drag.over_lord || !dnd_files_in()) return;
    drag.over_lord = 1;
    wl_data_source_send_target(drag.src->res, "text/uri-list");
    if (wl_resource_get_version(drag.src->res) >= 3) wl_data_source_send_action(drag.src->res, DND_COPY);
}

static void dnd_focus(Surf *t, float lx, float ly) {
    if (t != drag.focus && t && !dnd_takes(t)) t = NULL;
    if (t == drag.focus) {
        if (t && drag.dev) wl_data_device_send_motion(drag.dev, now_ms(), wl_fixed_from_double(lx), wl_fixed_from_double(ly));
        if (!t) dnd_over_lord();
        return;
    }
    dnd_leave();
    if (!t) { dnd_over_lord(); return; }
    drag.focus = t;
    if (!(drag.dev = dnd_device(t->client))) return;
    struct wl_resource *ores = NULL;
    if (drag.src || drag.lord) {
        DOffer *o = calloc(1, sizeof *o);
        ores = o ? wl_resource_create(t->client, &wl_data_offer_interface, wl_resource_get_version(drag.dev), 0) : NULL;
        if (!ores) { free(o); wl_client_post_no_memory(t->client); drag.dev = NULL; return; }
        o->res = ores;
        if (wl_resource_get_version(ores) < 3) o->dest = o->pref = DND_COPY;   /* before version 3 a drop copies */
        wl_resource_set_implementation(ores, &dnd_offer_impl, o, dnd_offer_gone);
        wl_data_device_send_data_offer(drag.dev, ores);
        if (drag.src) {
            o->src = drag.src;
            drag.src->offer = o;
            for (int i = 0; i < drag.src->nmimes; i++) wl_data_offer_send_offer(ores, drag.src->mimes[i]);
        } else {
            o->lord = lord_gen;
            for (size_t i = 0; i < NLORD_MIMES; i++) wl_data_offer_send_offer(ores, lord_mimes[i]);
        }
        if (wl_resource_get_version(ores) >= 3) wl_data_offer_send_source_actions(ores, dnd_source_actions());
        drag.offer = o;
    }
    wl_data_device_send_enter(drag.dev, wl_display_next_serial(dpy), t->res, wl_fixed_from_double(lx), wl_fixed_from_double(ly), ores);
    if (drag.offer) dnd_choose(drag.offer);     /* before version 3 the target copies; a newer one says so itself */
}

static void dnd_end(void) {
    dnd_icon_damage();
    memset(&drag, 0, sizeof drag);
}
static void dnd_cancel(void) {
    if (!drag.on) return;
    dnd_leave();
    if (drag.src) wl_data_source_send_cancelled(drag.src->res);
    dnd_end();
}

/* The app's files dropped on LordOS: Files claims the drop while the UI handles that release (wl_drop_claim);
 * only then is its list asked for and read (at most URIL_DRAG_MAX_BYTES, from the event loop, within DNDG_READ_MS),
 * and the local paths go to JS ("filesdrop" on window 0). A drop not claimed at its release, a list that takes
 * too long or has no local path, is cancelled to its source. */
static void in_drop_close(void) {
    if (in_drop.ev) wl_event_source_remove(in_drop.ev);
    if (in_drop.timer) wl_event_source_remove(in_drop.timer);
    if (in_drop.fd >= 0) close(in_drop.fd);
    free(in_drop.buf);
    in_drop.ev = in_drop.timer = NULL;
    in_drop.fd = -1;
    in_drop.buf = NULL;
    in_drop.len = in_drop.cap = 0;
}
/* the drop ends without files: its source (if it lives) is told cancelled */
static void in_drop_cancel(void) {
    if (in_drop.src) wl_data_source_send_cancelled(in_drop.src->res);
    in_drop.src = NULL;
    in_drop_close();
    dndg_reset(&in_drop.gate);
}
/* An isolated LordOS app or a sandboxed client: its drags may name files it can't reach, so they never hand
 * files to LordOS. Nothing known of the client counts as contained. */
static int dnd_contained(struct wl_client *c) {
    return !c || app_of_client(c) || client_sandboxed(c);
}
static int in_drop_ready(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    int full = 0;
    for (;;) {
        if (in_drop.len + 4096 > in_drop.cap) {
            size_t cap = in_drop.cap ? in_drop.cap * 2 : 8192;
            char *nb = cap <= URIL_DRAG_MAX_BYTES ? realloc(in_drop.buf, cap) : NULL;
            if (!nb) { full = 1; break; }   /* a longer list is refused whole: its last line may be cut short */
            in_drop.buf = nb;
            in_drop.cap = cap;
        }
        ssize_t n = read(fd, in_drop.buf + in_drop.len, in_drop.cap - in_drop.len);
        if (n > 0) { in_drop.len += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        break;                              /* end (or error): done */
    }
    /* the same reader as a file list on the clipboard (urilist.c): only this machine's files, good paths */
    free(in_drop.paths);
    size_t plen = 0;
    int cut = 0, count = 0;
    in_drop.paths = in_drop.len && !full ? uril_parse(in_drop.buf, in_drop.len, 0, &cut, &plen, &count) : NULL;
    in_drop.npaths = in_drop.paths && count <= URIL_MAX_PATHS ? count : 0;
    int ok = in_drop.gate.claimed && in_drop.npaths > 0;
    if (in_drop.src && ok && wl_resource_get_version(in_drop.src->res) >= 3) wl_data_source_send_dnd_finished(in_drop.src->res);
    else if (in_drop.src && !ok) wl_data_source_send_cancelled(in_drop.src->res);
    in_drop.src = NULL;
    in_drop_close();
    dndg_reset(&in_drop.gate);
    if (ok) emit_id(0, "filesdrop", "{}");
    else { free(in_drop.paths); in_drop.paths = NULL; in_drop.npaths = 0; }
    return 0;
}
static int in_drop_timeout(void *data) {
    (void)data;
    in_drop_cancel();                       /* the list never ended: nothing is copied */
    return 0;
}
/* The release dropped files on LordOS: claimable while the UI handles it (wl_drop_release_done right after). */
static void dnd_take_files(void) {
    Source *src = drag.src;
    in_drop_cancel();                       /* one still pending or being read gives way */
    if (drag.contained || dnd_contained(wl_resource_get_client(src->res))) { wl_data_source_send_cancelled(src->res); return; }
    in_drop.src = src;
    dndg_drop(&in_drop.gate);
}
int wl_drop_claim(void) {
    Source *src = in_drop.src;
    if (!src || dnd_contained(wl_resource_get_client(src->res)) || !dndg_claim(&in_drop.gate)) return 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK)) { in_drop_cancel(); return 0; }
    in_drop.fd = p[0];
    in_drop.ev = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, in_drop_ready, NULL);
    in_drop.timer = wl_event_loop_add_timer(loop, in_drop_timeout, NULL);
    if (!in_drop.ev || !in_drop.timer || wl_event_source_timer_update(in_drop.timer, DNDG_READ_MS) < 0) {
        close(p[1]);
        in_drop_cancel();
        return 0;
    }
    wl_data_source_send_send(src->res, "text/uri-list", p[1]);
    close(p[1]);
    if (wl_resource_get_version(src->res) >= 3) wl_data_source_send_dnd_drop_performed(src->res);
    return 1;
}
void wl_drop_release_done(void) {
    if (dpy && dndg_release_done(&in_drop.gate)) in_drop_cancel();    /* nobody took it at its release */
}
const char *wl_drop_paths(int *n) {
    *n = in_drop.paths ? in_drop.npaths : 0;
    return *n ? in_drop.paths : NULL;
}
void wl_drop_clear(void) {
    free(in_drop.paths);
    in_drop.paths = NULL;
    in_drop.npaths = 0;
}

static void dnd_drop(void) {
    DOffer *o = drag.offer;
    int ok = drag.focus && drag.dev && (o ? o->accepted && (wl_resource_get_version(o->res) < 3 || o->action) : !drag.src && !drag.lord);
    if (!ok && drag.over_lord && drag.src) {   /* files on LordOS itself */
        dnd_take_files();
        drag.over_lord = 0;
        dnd_end();
        return;
    }
    if (!ok) { dnd_cancel(); return; }
    wl_data_device_send_drop(drag.dev);
    if (o) o->dropped = 1;
    if (drag.src && wl_resource_get_version(drag.src->res) >= 3) wl_data_source_send_dnd_drop_performed(drag.src->res);
    dnd_leave();                        /* the session ends; the dropped offer still reads until it finishes */
    dnd_end();
}

/* Pointer input while a drag runs: no app gets it, the surface under it gets the drag. The last release drops. */
static int dnd_pointer(float x, float y, Node *hit, int nbuttons, const int *down) {
    float lx = 0, ly = 0;
    dnd_icon_damage();
    last_x = x;
    last_y = y;
    dnd_icon_damage();
    Surf *t = surface_under(x, y, hit, &lx, &ly);
    dnd_focus(t, lx, ly);
    int released = 0;
    for (int i = 0; i < nbuttons; i++) {
        buttons_held += down[i] ? 1 : -1;
        if (!down[i] && !seat_held) released = 1;
    }
    if (buttons_held < 0) buttons_held = 0;
    if (released) {
        dnd_drop();
        buttons_held = 0;
        npress = 0;
        press_surf = NULL;
        t = surface_under(x, y, hit, &lx, &ly);
        pointer_enter(t, lx, ly);       /* the pointer is the app's again */
    }
    return t ? 1 : 0;
}

static void dev_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src_res, struct wl_resource *origin_res,
                     struct wl_resource *icon_res, uint32_t serial) {
    Source *src = src_res ? wl_resource_get_user_data(src_res) : NULL;
    Surf *o = wl_resource_get_user_data(origin_res), *icon = icon_res ? wl_resource_get_user_data(icon_res) : NULL;
    if (icon && icon->role != R_NONE && icon->role != R_DNDICON) { wl_resource_post_error(r, WL_DATA_DEVICE_ERROR_ROLE, "the icon has another role"); return; }
    if (src && src->used) { wl_resource_post_error(r, WL_DATA_DEVICE_ERROR_USED_SOURCE, "the source was already used"); return; }
    if (src) src->used = 2;
    /* the press it names: this client's, current (a button still down), on the window that got it */
    int ok = !drag.on && o && o->client == c && buttons_held > 0 && press_surf && press_surf->client == c &&
             toplevel_of(o) == toplevel_of(press_surf), known = 0;
    for (int i = 0; ok && i < npress; i++) if (press_serials[i] == serial) known = 1;
    if (!ok || !known) {
        if (src) wl_data_source_send_cancelled(src->res);
        return;
    }
    if (icon) icon->role = R_DNDICON;
    Surf *under = pointer_focus;
    pointer_enter(NULL, 0, 0);          /* no app has the pointer while it drags */
    memset(&drag, 0, sizeof drag);
    drag.on = 1;
    drag.client = c;
    drag.src = src;
    drag.icon = icon;
    drag.contained = dnd_contained(c);
    float ox, oy;
    if (under && origin(under, &ox, &oy)) dnd_focus(under, last_x - ox, last_y - oy);
    dnd_icon_damage();
}

/* A source, a surface, a data device or a client that went away mid-drag. */
static void dnd_source_gone(Source *s) {
    if (s->offer) s->offer->src = NULL;
    if (in_drop.src == s) {             /* gone before its list was asked for or read: nothing to copy */
        in_drop.src = NULL;
        if (!in_drop.gate.claimed) in_drop_cancel();
    }
    if (drag.src == s) { drag.src = NULL; dnd_cancel(); }
}
static void dnd_surface_gone(Surf *s) {
    if (press_surf == s) { press_surf = NULL; npress = 0; }
    if (drag.icon == s) { dnd_icon_damage(); drag.icon = NULL; }
    if (drag.focus == s) dnd_leave();
}
static void dev_gone(struct wl_resource *r) {
    if (drag.dev == r) drag.dev = NULL;
    link_gone(r);
}
static void dnd_client_gone(struct wl_client *c) {
    if (drag.on && drag.client == c) dnd_cancel();
}

int wl_drag_files(const char *const *paths, int n) {
    if (!dpy || drag.on || seat_held <= 0) return 0;
    if (n < 1 || n > URIL_MAX_PATHS) return 0;
    /* the same encoder as Files' copy to the clipboard (urilist.c) */
    size_t pl = 0, un = 0, tn = 0;
    char *list = uril_join(paths, n, &pl);
    char *u = list ? uril_encode(list, pl, URIL_URI_LIST, 0, &un) : NULL, *t = u ? uril_encode(list, pl, URIL_PLAIN, 0, &tn) : NULL;
    free(list);
    if (!u || !t) { free(u); free(t); return 0; }
    free(lord_uris);
    free(lord_text);
    lord_uris = u;
    lord_text = t;
    lord_uris_len = un;
    lord_text_len = tn;
    lord_gen++;
    memset(&drag, 0, sizeof drag);
    drag.on = drag.lord = 1;
    pointer_enter(NULL, 0, 0);          /* the window under it was only hovered: now the drag goes to it */
    return 1;
}
void wl_drag_files_end(void) {
    if (drag.on && drag.lord) dnd_cancel();
}
int wl_drag_files_taken(void) {
    return drag.on && drag.lord && drag.offer && drag.offer->accepted && drag.offer->action ? (int)drag.offer->action : 0;
}

int wl_pointer(float x, float y, Node *hit, int moved, int nbuttons, const int *down, const int *button, int wheel) {
    if (!dpy) return 0;
    for (int i = 0; i < nbuttons; i++) seat_held += down[i] ? 1 : -1;
    if (seat_held < 0) seat_held = 0;
    if (drag.on) return dnd_pointer(x, y, hit, nbuttons, down);
    last_x = x;
    last_y = y;
    Surf *target = NULL;
    float lx = 0, ly = 0;
    if (buttons_held && pointer_focus) {        /* drag: stays on the same surface */
        float ox, oy;
        target = pointer_focus;
        if (origin(target, &ox, &oy)) { lx = x - ox; ly = y - oy; }
    } else {
        target = surface_under(x, y, hit, &lx, &ly);
    }
    for (int i = 0; i < nbuttons; i++)          /* click outside the menus: they close */
        if (down[i] && npopups && (!target || target->role != R_POPUP)) {
            Surf *t = target;
            while (t && t->role == R_SUB) t = t->parent;
            if (!t || t->role != R_POPUP) dismiss_popups();
        }
    pointer_enter(target, lx, ly);
    if (!target) return fs_surf && fs_surf->content ? 2 : 0;    /* a hole in a fullscreen window: not the UI below */
    /* a window under a modal dialog gets no presses and no wheel (releases still go: a drag ends);
     * the theme still sees the click and brings the dialog forward (sdk/dialogmodel.js) */
    Surf *owner = target;
    while (owner && owner->role == R_SUB) owner = owner->parent;
    int held = owner && owner->role == R_TOPLEVEL && modal_over(owner);
    if (held) wheel = 0;
    for (int i = 0; i < nbuttons; i++)          /* a new press: the one a start_drag may name */
        if (down[i] && !held) { npress = 0; press_surf = target; }
    unsigned t = now_ms();
    struct wl_resource *p;
    FOR_CLIENT(&pointers, target->client, p) {
        if (moved && !(constraint && constraint->active && constraint->lock))
            wl_pointer_send_motion(p, t, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
        for (int i = 0; i < nbuttons; i++) {
            if (held && down[i]) continue;
            uint32_t b = button[i] == 0 ? BTN_LEFT : button[i] == 1 ? BTN_MIDDLE : BTN_RIGHT;
            uint32_t serial = input_serial(target->client);
            if (down[i] && npress < (int)(sizeof press_serials / sizeof *press_serials)) press_serials[npress++] = serial;
            wl_pointer_send_button(p, serial, t, b, down[i] ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (wheel) wl_input_wheel(p, t, wheel);      /* axis + axis_source wheel + value120 (wl_input.c) */
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    for (int i = 0; i < nbuttons; i++) {
        buttons_held += down[i] ? 1 : -1;
        if (down[i]) { press_x = x; press_y = y; }
    }
    if (buttons_held < 0) buttons_held = 0;
    if (!buttons_held) { npress = 0; press_surf = NULL; }
    Surf *top = target;
    while (top && top->role == R_SUB) top = top->parent;
    if (fs_surf && fs_surf->content) return 2;  /* fullscreen: the UI does not see the pointer */
    return top && top->role == R_POPUP ? 2 : 1;
}

void wl_pointer_grab(int nbuttons, const int *down) {
    if (!dpy) return;
    for (int i = 0; i < nbuttons; i++) seat_held += down[i] ? 1 : -1;
    if (seat_held < 0) seat_held = 0;
    dnd_cancel();                       /* a window move took the pointer: no drop at its release */
    pointer_enter(NULL, 0, 0);
    for (int i = 0; i < nbuttons; i++) buttons_held += down[i] ? 1 : -1;
    if (buttons_held < 0) buttons_held = 0;
}

/* Pastes into the Linux app with the keyboard: a synthetic Ctrl+V only for that app (the LordOS
 * clipboard panel uses it after copying something again). Control is XKB core
 * modifier 2 in all keymaps. 1 if there was an app with the keyboard. */
int wl_has_key_focus(void) { return dpy && key_focus; }

/* The field's cursor rectangle is in surface coordinates, the same logical pixels the window is drawn
 * at (a buffer scale only changes how many pixels the app draws), so the window's origin is all it
 * takes. A rectangle outside the window is brought to its edge; without one, the window's top left. */
int wl_text_caret(float *x, float *y, float *w, float *h) {
    int r[4], in[4];
    float ox, oy;
    if (!dpy || !key_focus) return 0;
    int how = wl_textinput_rect(r);
    if (!how || !origin(key_focus, &ox, &oy)) return 0;
    if (how == 2) { r[0] = key_focus->gx; r[1] = key_focus->gy; r[2] = r[3] = 0; }
    tiu_rect_in(r, key_focus->lw, key_focus->lh, in);
    *x = ox + (float)in[0];
    *y = oy + (float)in[1];
    *w = (float)in[2];
    *h = (float)in[3];
    return 1;
}
int wl_focus_protected(void) {
    Surf *top = dpy ? toplevel_of(key_focus) : NULL;
    return top && node_protected(top->node);
}

static const char *app_of_client(struct wl_client *cl) {
    pid_t pid = 0;
    uid_t uid;
    gid_t gid;
    if (!cl) return NULL;
    wl_client_get_credentials(cl, &pid, &uid, &gid);
    return brokersrv_app_of_pid((int)pid);
}

/* Where a client's clipboard file list comes from (urilist.h URIL_FROM_*): an isolated LordOS app (the broker
 * knows its pid), a Flatpak app (its root holds /.flatpak-info, which is how xdg-desktop-portal tells one), or
 * an app outside a sandbox. The client's root is opened as a directory and only looked into, never following
 * a link out of it; a client whose root can't be looked at, now or when it connected, counts as sandboxed. */
static int client_from(struct wl_client *c) {
    if (!c) return URIL_FROM_UNKNOWN;
    if (app_of_client(c)) return URIL_FROM_ISOLATED;
    pid_t pid = client_pid(c);
    if (pid <= 0) return URIL_FROM_UNKNOWN;
    char p[48];
    snprintf(p, sizeof p, "/proc/%d/root", (int)pid);
    int root = open(p, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return URIL_FROM_UNKNOWN;
    int flatpak = dndg_root_sandboxed(root);
    close(root);
    if (flatpak) return URIL_FROM_FLATPAK;
    /* fail closed: an app only on positive evidence now (same root and namespaces as ours, dndg_pid_sandboxed)
     * and when it connected, while its pid was surely its own */
    return dndg_pid_sandboxed((int)pid) || client_sandboxed(c) ? URIL_FROM_UNKNOWN : URIL_FROM_APP;
}

const char *wl_focus_app(void) { return dpy && key_focus ? app_of_client(key_focus->client) : NULL; }
int wl_focus_pid(void) { return dpy && key_focus ? (int)client_pid(key_focus->client) : 0; }

const char *wl_node_app(int node) {
    if (!dpy || !node) return NULL;
    for (int i = 0; i < MAX_SURF; i++) if (surfs[i] && surfs[i]->node == node) return app_of_client(surfs[i]->client);
    return NULL;
}

/* A paste the person asked for, reaching an isolated LordOS app: it may read the clipboard once for it (brokerclip.h).
 * mods: the core modifiers the app sees with the key (bit 0 Shift, bit 2 Ctrl). */
static void note_paste(int code, uint32_t mods) {
    if (!key_focus) return;
    if (!((code == KEY_V && (mods & 1u << 2)) || (code == KEY_INSERT && (mods & 1u)))) return;
    const char *app = app_of_client(key_focus->client);
    if (app) brokersrv_paste_gesture(app);
}

int wl_send_paste(void) {
    if (!dpy || !key_focus) return 0;
    unsigned t = now_ms();
    uint32_t d, l, lk, g;
    kb_mods(&d, &l, &lk, &g);
    struct wl_resource *kb;
    note_paste(KEY_V, d | (1u << 2));
    FOR_CLIENT(&keyboards, key_focus->client, kb) {
        wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), d | (1u << 2), l, lk, g);
        wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, KEY_V, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t + 1, KEY_V, WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), d, l, lk, g);
    }
    mods_sent[0] = d; mods_sent[1] = l; mods_sent[2] = lk; mods_sent[3] = g;
    return 1;
}

static int focus_is_lord_app(void);
static int node_protected(int node);
static Surf *toplevel_of(Surf *s);

/* Key latency trace (keylag.h): only keys for a Linux app, never an isolated LordOS app or a
 * protected window. */
static int keylag_traceable(void) {
    Surf *top = toplevel_of(key_focus);
    return !focus_is_lord_app() && !(top && node_protected(top->node));
}

static void send_key_to_focus(int code, int value) {
    unsigned t = now_ms();
    if (keylag_enabled) keylag_sent_wl(keylag_traceable());
    uint32_t d, l, lk, g;
    kb_mods(&d, &l, &lk, &g);
    if (value == 1) note_paste(code, d);
    struct wl_resource *kb;
    FOR_CLIENT(&keyboards, key_focus->client, kb) {
        wl_keyboard_send_key(kb, input_serial(key_focus->client), t, (uint32_t)code,
                             value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        if (d != mods_sent[0] || l != mods_sent[1] || lk != mods_sent[2] || g != mods_sent[3])
            wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), d, l, lk, g);
    }
    mods_sent[0] = d; mods_sent[1] = l; mods_sent[2] = lk; mods_sent[3] = g;
}

/* The client with the keyboard is an isolated LordOS app (`lord-server --app`): its pid is the one
 * lord-server registered when it spawned it (brokersrv). A Linux app never matches. */
static int focus_is_lord_app(void) {
    pid_t pid = 0;
    uid_t uid;
    gid_t gid;
    if (!key_focus || !key_focus->client) return 0;
    wl_client_get_credentials(key_focus->client, &pid, &uid, &gid);
    return brokersrv_app_of_pid((int)pid) != NULL;
}

/* ❖ shortcuts that stay with LordOS even when an isolated app has the keyboard: lock (❖L, ❖⌃Q),
 * quit (❖Q), search and keyboard layout (❖Space, ❖⌥Space), clipboard history (❖V, ❖⇧V), emoji
 * (❖.), window switching (❖Tab, ❖`), screenshots (❖⇧3/4/5) and ❖Esc. */
static int system_meta_key(int code, const KeyInfo *k) {
    switch (code) {
    case KEY_L: case KEY_Q: case KEY_SPACE: case KEY_V: case KEY_DOT: case KEY_TAB: case KEY_GRAVE: case KEY_ESC:
        return 1;
    case KEY_3: case KEY_4: case KEY_5:
        return k->shift;
    default:
        return code >= KEY_F1 && code <= KEY_F12;
    }
}

/* Keys the shell keeps (keygrab.c) and the ❖ → Ctrl of Lord Flow. grabbed[code]: its press went to
 * the shell, so its release does too. remapped[code]: its press went to the app as Ctrl+key.
 * grab_hold: the modifier of a switcher in progress; until it is released every key is the shell's,
 * while modifiers still reach the app too, so it never keeps one stuck. */
#define KEY_SLOTS 256
static unsigned char grabbed[KEY_SLOTS], remapped[KEY_SLOTS];
static unsigned grab_hold;
static int focus_terminal;

void wl_focus_terminal(int on) { focus_terminal = !!on; }

static int is_modifier_key(int code) {
    return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
           code == KEY_LEFTALT || code == KEY_RIGHTALT || code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}

/* Like wl_send_paste, for one key held down: the client repeats it on its own while Ctrl stays set.
 * Super (core modifier 6, Mod4) is taken out so the app sees plain Ctrl. */
static void send_remapped(int code, int press) {
    unsigned t = now_ms();
    uint32_t d, l, lk, g;
    kb_mods(&d, &l, &lk, &g);
    d &= ~(1u << 6);
    uint32_t held = d | 1u << 2 | (focus_terminal ? 1u : 0);
    if (press) note_paste(code, held);
    struct wl_resource *kb;
    FOR_CLIENT(&keyboards, key_focus->client, kb) {
        if (press) wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), held, l, lk, g);
        wl_keyboard_send_key(kb, input_serial(key_focus->client), t, (uint32_t)code,
                             press ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        if (!press) wl_keyboard_send_modifiers(kb, wl_display_next_serial(dpy), d, l, lk, g);
    }
    mods_sent[0] = press ? held : d; mods_sent[1] = l; mods_sent[2] = lk; mods_sent[3] = g;
}

int wl_key(int code, int value, const KeyInfo *k) {
    if (!dpy) return 0;
    unsigned char *was_grabbed = code >= 0 && code < KEY_SLOTS ? &grabbed[code] : NULL;
    unsigned char *was_remapped = code >= 0 && code < KEY_SLOTS ? &remapped[code] : NULL;
    unsigned mods = (k->shift ? KG_SHIFT : 0u) | (k->ctrl ? KG_CTRL : 0u) | (k->alt ? KG_ALT : 0u) | (k->meta ? KG_META : 0u);
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { /* a Files drag over a Linux window: Shift moves */
        shift_down = value != 0;
        if (drag.on && drag.lord && drag.offer && wl_resource_get_version(drag.offer->res) >= 3)
            wl_data_offer_send_source_actions(drag.offer->res, dnd_source_actions());
        if (drag.on && drag.offer) dnd_choose(drag.offer);
    }
    if (drag.on && code == KEY_ESC) {                      /* Escape cancels a drag; LordOS still sees it for its own */
        int lord = drag.lord;
        if (value == 1) dnd_cancel();
        if (!lord) return 1;
    }
    if (was_grabbed && *was_grabbed) {
        if (!value) *was_grabbed = 0;
        return 0;
    }
    if (was_remapped && *was_remapped) {
        if (value) return 1;                    /* apps repeat on their own (repeat_info) */
        *was_remapped = 0;
        if (key_focus) send_remapped(code, 0);
        return 1;
    }
    if (grab_hold) {
        if (!(mods & grab_hold)) grab_hold = 0;
        if (is_modifier_key(code)) { if (key_focus && value != 2) send_key_to_focus(code, value); }
        else if (value && was_grabbed) *was_grabbed = 1;
        else if (!value && key_focus) send_key_to_focus(code, 0);   /* pressed before the switcher opened */
        return 0;
    }
    if (k->meta && code == KEY_ESC && !k->alt && (fs_surf || wl_pointer_locked())) {   /* ❖Esc exits; the shell learns ❖ was not alone */
        if (value != 1) return 1;
        if (wl_pointer_locked()) constraint_escape();
        if (fs_surf) leave_fullscreen(fs_surf);
        return 2;
    }
    if (fs_surf && fs_surf->content && key_focus != fs_surf) wl_set_focus(fs_surf->id);
    if (!key_focus) return 0;
    if (value == 1 && was_grabbed) {
        char letter = keygrab_letter(code, k->base);
        int how = keygrab_match(code, letter, mods);
        if (how == KG_HOLD && fs_surf && fs_surf->content) how = KG_PASS;   /* presented above the switcher */
        if (how != KG_PASS) {
            *was_grabbed = 1;
            if (how == KG_HOLD) grab_hold = mods & (KG_ALT | KG_META);
            return 0;
        }
        if (keygrab_is_remap(letter, mods) && !focus_is_lord_app()) {
            *was_remapped = 1;
            send_remapped(code, 1);
            return 2;
        }
    }
    int super = code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
    if (k->meta || super) {
        /* ❖ + key: LordOS shortcuts. An isolated LordOS app also gets the ones that are not the
         * system's (❖N, ❖C, ❖A, ❖W…), with the ❖ key itself so it knows the modifier, as an app in
         * the theme would; the theme still sees them (it tracks ❖ pressed alone). A Linux app only
         * gets the release of ❖, as before. */
        if (value != 2 && (focus_is_lord_app() ? super || !system_meta_key(code, k) : super && !value)) send_key_to_focus(code, value);
        return 0;
    }
    if (keygrab_system_key(code)) return 0;
    if (value == 2) return 1;                   /* apps repeat on their own (repeat_info) */
    send_key_to_focus(code, value);
    return 1;
}

/* ---- zwp_linux_dmabuf_v1 (v4): apps' GPU buffers ---- */

typedef struct { int fd[4]; uint32_t off[4], stride[4]; uint64_t mod; int set[4], used; } Params;

static void dmabuf_buffer_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface dmabuf_buffer_impl = { .destroy = dmabuf_buffer_destroy };
static void dmabuf_buffer_gone(struct wl_resource *r) {
    Dmabuf *d = wl_resource_get_user_data(r);
    if (!d) return;
    d->res = NULL;
    dm_unref(d);
}

static void params_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void params_gone(struct wl_resource *r) {
    Params *p = wl_resource_get_user_data(r);
    if (!p) return;
    for (int i = 0; i < 4; i++) if (p->set[i]) close(p->fd[i]);
    free(p);
}

static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t idx, uint32_t off, uint32_t stride,
                       uint32_t mod_hi, uint32_t mod_lo) {
    (void)c;
    Params *p = wl_resource_get_user_data(r);
    if (p->used) { close(fd); wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "already used"); return; }
    if (idx >= 4) { close(fd); wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "invalid plane"); return; }
    if (p->set[idx]) { close(fd); wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "repeated plane"); return; }
    p->fd[idx] = fd;
    p->off[idx] = off;
    p->stride[idx] = stride;
    p->mod = (uint64_t)mod_hi << 32 | mod_lo;
    p->set[idx] = 1;
}

/* Can we show it? On GPU, it must import as an EGLImage; on CPU, it must be linear and mappable. */
static int gpu_format_known(const GpuDmabuf *g);
static int dmabuf_usable(Dmabuf *d) {
    if (dm_tex(d)) return 1;
    /* no GPU, or a dmabuf EGL does not import (for example udmabuf with a driver that does not
     * accept it): if it is linear it is mapped and copied, as in the CPU path. Off the GPU, a
     * format the last GPU context imported is accepted too (an app may still be allocating with
     * the old feedback): it is held and imported once the GPU is back. */
    return dm_linear(d) || (!gpu_active() && gpu_format_known(&d->d));
}

static void params_create_common(struct wl_client *c, struct wl_resource *r, uint32_t buffer_id, int32_t w, int32_t h,
                                 uint32_t format, uint32_t flags) {
    Params *p = wl_resource_get_user_data(r);
    if (p->used) { wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "already used"); return; }
    p->used = 1;
    int n = 0;
    while (n < 4 && p->set[n]) n++;
    for (int i = n; i < 4; i++)
        if (p->set[i]) { wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "skipped planes"); return; }
    if (!n || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        wl_resource_post_error(r, n ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS : ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "invalid buffer");
        return;
    }
    Dmabuf *d = calloc(1, sizeof *d);
    if (!d) { wl_client_post_no_memory(c); return; }
    d->d = (GpuDmabuf){ .width = w, .height = h, .nplanes = n, .fourcc = format, .modifier = p->mod };
    for (int i = 0; i < 4; i++) d->d.fd[i] = -1;
    for (int i = 0; i < n; i++) {
        d->d.fd[i] = p->fd[i];
        d->d.offset[i] = p->off[i];
        d->d.stride[i] = p->stride[i];
        p->set[i] = 0;                          /* now they belong to the Dmabuf */
    }
    d->refs = 1;
    if (flags || !dmabuf_usable(d)) {
        dm_unref(d);
        if (buffer_id) wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "could not import");
        else zwp_linux_buffer_params_v1_send_failed(r);
        return;
    }
    struct wl_resource *b = wl_resource_create(c, &wl_buffer_interface, 1, buffer_id);
    if (!b) { dm_unref(d); wl_client_post_no_memory(c); return; }
    d->res = b;
    wl_resource_set_implementation(b, &dmabuf_buffer_impl, d, dmabuf_buffer_gone);
    if (!buffer_id) zwp_linux_buffer_params_v1_send_created(r, b);
}

static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    params_create_common(c, r, 0, w, h, format, flags);
}
static void params_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
    params_create_common(c, r, id, w, h, format, flags);
}
static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy, .add = params_add, .create = params_create, .create_immed = params_create_immed,
};

/* Advertised formats: those EGL imports (GPU) or the linear ones that can be mapped (CPU). */
static int dmabuf_formats(GpuFormat *out, int max) {
    int n = gpu_active() ? gpu_formats(out, max) : 0;
    if (n) return n;
    static const uint32_t f[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
    for (int i = 0; i < 2 && n + 1 < max; i++) {
        out[n++] = (GpuFormat){ f[i], DRM_FORMAT_MOD_LINEAR };
        out[n++] = (GpuFormat){ f[i], DRM_FORMAT_MOD_INVALID };
    }
    return n;
}

static void feedback_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = { .destroy = feedback_destroy };
static void feedback_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }

static int gpu_format_known(const GpuDmabuf *g) {
    for (int i = 0; i < ngpu_fmt; i++)
        if (gpu_fmt_cache[i].fourcc == g->fourcc && gpu_fmt_cache[i].modifier == g->modifier) return 1;
    return 0;
}

/* What the feedback says right now (formats and device), to know when it must be sent again. */
static uint64_t formats_sig(const GpuFormat *f, int n, dev_t dev) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)dev;
    for (int i = 0; i < n; i++) {
        h = (h ^ f[i].fourcc) * 1099511628211ull;
        h = (h ^ f[i].modifier) * 1099511628211ull;
    }
    return h;
}

static dev_t feedback_dev(void) {
    dev_t dev = drm_dev;
    if (gpu_active()) gpu_device(&dev);
    return dev;
}

static void send_feedback(struct wl_resource *fb) {
    GpuFormat f[256];
    int n = dmabuf_formats(f, 256);
    feedback_sig = formats_sig(f, n, feedback_dev());
    size_t size = (size_t)n * 16;
    int fd = memfd_create("lord-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { if (fd >= 0) close(fd); return; }
    uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return; }
    for (int i = 0; i < n; i++) {
        memcpy(m + i * 16, &f[i].fourcc, 4);
        memset(m + i * 16 + 4, 0, 4);
        memcpy(m + i * 16 + 8, &f[i].modifier, 8);
    }
    munmap(m, size);
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    zwp_linux_dmabuf_feedback_v1_send_format_table(fb, fd, (uint32_t)size);
    close(fd);
    dev_t dev = feedback_dev();
    struct wl_array devs, idx;
    wl_array_init(&devs);
    dev_t *dp = wl_array_add(&devs, sizeof dev);
    if (dp) *dp = dev;
    zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &devs);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &devs);
    wl_array_init(&idx);
    for (int i = 0; i < n; i++) { uint16_t *ip = wl_array_add(&idx, sizeof *ip); if (ip) *ip = (uint16_t)i; }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);
    wl_array_release(&devs);
    wl_array_release(&idx);
}

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void dmabuf_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    Params *p = calloc(1, sizeof *p);
    struct wl_resource *res = p ? wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(p); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &params_impl, p, params_gone);
}
static void dmabuf_get_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *fb = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(r), id);
    if (!fb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(fb, &feedback_impl, NULL, feedback_gone);
    wl_list_insert(&feedbacks, wl_resource_get_link(fb));
    send_feedback(fb);
}
static void dmabuf_get_surface_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    (void)surface;
    dmabuf_get_feedback(c, r, id);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy, .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_feedback, .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
    if (ver >= 4) return;                       /* v4: formats go through the feedback */
    GpuFormat f[256];
    int n = dmabuf_formats(f, 256);
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (f[j].fourcc == f[i].fourcc) seen = 1;
        if (ver < 3) { if (!seen) zwp_linux_dmabuf_v1_send_format(r, f[i].fourcc); }
        else zwp_linux_dmabuf_v1_send_modifier(r, f[i].fourcc, (uint32_t)(f[i].modifier >> 32), (uint32_t)f[i].modifier);
    }
}

void wl_set_drm_device(dev_t dev) { drm_dev = dev; }

/* ---- GPU context re-created (docs/GPU.md, "When the GPU context is re-created") ---- */

/* The GPU is going away (gpu_shutdown, its context still current): every window showing a dmabuf
 * keeps its last good frame as pixels for the CPU path, and textures are freed while they still can be. */
static void wl_gpu_lost(void) {
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s) continue;
        Dmabuf *d = s->dmabuf;
        if (d && gpu_tex_current(d->tex)) {
            int w = d->d.width, h = d->d.height;
            uint32_t *px = w <= 8192 && h <= 8192 ? malloc((size_t)w * (size_t)h * 4) : NULL;
            if (px && gpu_tex_read(d->tex, px, w, h) == 0) {
                free(s->px);
                s->px = px;
                s->w = w;
                s->h = h;
            } else {
                free(px);
                if (map_linear(s, d)) { free(s->px); s->px = NULL; }
            }
        }
        if (d) { gpu_tex_free(d->tex); d->tex = NULL; }
        gpu_tex_free(s->tex);
        s->tex = NULL;
        s->tex_dirty = 1;
    }
}

/* The switch is done (GPU → CPU or a new GPU context): held buffers are imported again, pixel
 * textures re-uploaded, everything is repainted and the dmabuf feedback is re-sent if the formats
 * or the device changed, so apps reallocate buffers the new path can use. */
static void wl_gpu_changed(void) {
    if (!dpy) return;
    if (gpu_active()) ngpu_fmt = gpu_formats(gpu_fmt_cache, 256);
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s) continue;
        if (s->dmabuf && dm_tex(s->dmabuf)) {
            const GpuDmabuf *g = &s->dmabuf->d;
            s->w = g->width;
            s->h = g->height;
            s->opaque = g->fourcc == DRM_FORMAT_XRGB8888 || g->fourcc == DRM_FORMAT_XBGR8888;
            s->content = 1;
            free(s->px);
            s->px = NULL;
            apply_viewport(s);
        }
        s->tex_dirty = 1;
        if (s->node) damage_node(s);
    }
    need_frame = 1;
    ui.dirty = 1;
    damage_add((Box){ 0, 0, ui.width, ui.height });
    GpuFormat f[256];
    int n = dmabuf_formats(f, 256);
    if (formats_sig(f, n, feedback_dev()) != feedback_sig) {
        struct wl_resource *fb;
        int sent = 0;
        wl_resource_for_each(fb, &feedbacks) { send_feedback(fb); sent++; }
        if (sent) fprintf(stderr, "wayland: dmabuf feedback sent again (%s formats) to %d object(s)\n", gpu_active() ? "GPU" : "CPU", sent);
    }
}

/* ---- wp_presentation: when each frame was seen ---- */

static void pres_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void feedback_link_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void pres_feedback(struct wl_client *c, struct wl_resource *r, struct wl_resource *surface, uint32_t id) {
    (void)r;
    Surf *s = wl_resource_get_user_data(surface);
    struct wl_resource *fb = wl_resource_create(c, &wp_presentation_feedback_interface, 1, id);
    if (!fb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(fb, NULL, NULL, feedback_link_gone);
    wl_list_insert(s->p_feedback.prev, wl_resource_get_link(fb));
}
static const struct wp_presentation_interface presentation_impl = { .destroy = pres_destroy, .feedback = pres_feedback };
static void bind_presentation(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_presentation_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &presentation_impl, NULL, NULL);
    wp_presentation_send_clock_id(r, CLOCK_MONOTONIC);
}

/* The flip finished (main.c, with the KMS time and vblank number). */
void wl_presented(uint64_t ns, unsigned seq, unsigned refresh_ns) {
    if (!dpy) return;
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s) continue;
        struct wl_resource *fb, *tmp;
        wl_resource_for_each_safe(fb, tmp, &s->sent_feedback) {
            struct wl_resource *o;
            for (int k = 0; k < OUT_MAX; k++)          /* the display it is on (the main one if none) */
                if (oslots[k].used && ((s->outmask >> k & 1) || (!s->outmask && k == out_main)))
                    FOR_CLIENT(&oslots[k].res, s->client, o) wp_presentation_feedback_send_sync_output(fb, o);
            uint64_t sec = ns / 1000000000ull;
            wp_presentation_feedback_send_presented(fb, (uint32_t)(sec >> 32), (uint32_t)sec, (uint32_t)(ns % 1000000000ull),
                                                    refresh_ns, 0, seq,
                                                    WP_PRESENTATION_FEEDBACK_KIND_VSYNC | WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK |
                                                    WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION);
            wl_resource_destroy(fb);
        }
    }
}

/* ---- wp_viewporter: crop and scale of a surface ---- */

static void vp_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void vp_gone(struct wl_resource *r) {
    Surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->viewport = NULL;
    s->p_vsw = -1;
    s->p_vdw = s->p_vdh = 0;
    s->p_vp = 1;
}
static void vp_source(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "no surface"); return; }
    if (w == wl_fixed_from_int(-1)) { s->p_vsw = -1; s->p_vp = 1; return; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid source rectangle"); return; }
    s->p_vsx = (float)wl_fixed_to_double(x);
    s->p_vsy = (float)wl_fixed_to_double(y);
    s->p_vsw = (float)wl_fixed_to_double(w);
    s->p_vsh = (float)wl_fixed_to_double(h);
    s->p_vp = 1;
}
static void vp_destination(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    (void)c;
    Surf *s = wl_resource_get_user_data(r);
    if (!s) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE, "no surface"); return; }
    if ((w == -1 && h == -1)) { s->p_vdw = s->p_vdh = 0; s->p_vp = 1; return; }
    if (w <= 0 || h <= 0) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid destination"); return; }
    s->p_vdw = w;
    s->p_vdh = h;
    s->p_vp = 1;
}
static const struct wp_viewport_interface viewport_impl = { .destroy = vp_destroy, .set_source = vp_source, .set_destination = vp_destination };
static void vpr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void vpr_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    Surf *s = wl_resource_get_user_data(surface);
    if (s->viewport) { wl_resource_post_error(r, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "already has a viewport"); return; }
    struct wl_resource *v = wl_resource_create(c, &wp_viewport_interface, 1, id);
    if (!v) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(v, &viewport_impl, s, vp_gone);
    s->viewport = v;
}
static const struct wp_viewporter_interface viewporter_impl = { .destroy = vpr_destroy, .get_viewport = vpr_get };
static void bind_viewporter(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_viewporter_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &viewporter_impl, NULL, NULL);
}

/* ---- zwp_relative_pointer_v1 and zwp_pointer_constraints_v1 (games with a captured mouse) ---- */

static void relp_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_relative_pointer_v1_interface relp_impl = { .destroy = relp_destroy };
static void relm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void relm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *pointer) {
    (void)pointer;
    struct wl_resource *rp = wl_resource_create(c, &zwp_relative_pointer_v1_interface, wl_resource_get_version(r), id);
    if (!rp) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(rp, &relp_impl, NULL, link_gone);
    wl_list_insert(&rel_pointers, wl_resource_get_link(rp));
}
static const struct zwp_relative_pointer_manager_v1_interface relm_impl = { .destroy = relm_destroy, .get_relative_pointer = relm_get };
static void bind_relm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_relative_pointer_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &relm_impl, NULL, NULL);
}

/* Relative motion (no acceleration or edges): main.c sends it with every motion. */
void wl_pointer_relative(double dx, double dy) {
    if (!dpy || !pointer_focus || (dx == 0 && dy == 0)) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t us = (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
    struct wl_resource *rp, *p;
    int sent = 0;
    FOR_CLIENT(&rel_pointers, pointer_focus->client, rp) {
        zwp_relative_pointer_v1_send_relative_motion(rp, (uint32_t)(us >> 32), (uint32_t)us, wl_fixed_from_double(dx),
                                                     wl_fixed_from_double(dy), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        sent = 1;
    }
    if (sent) FOR_CLIENT(&pointers, pointer_focus->client, p) if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
}

static void constraint_send(Constraint *k, int on) {
    if (!k->res) return;
    if (k->lock) { if (on) zwp_locked_pointer_v1_send_locked(k->res); else zwp_locked_pointer_v1_send_unlocked(k->res); }
    else { if (on) zwp_confined_pointer_v1_send_confined(k->res); else zwp_confined_pointer_v1_send_unconfined(k->res); }
}

/* Holds while the pointer is on its surface and the keyboard on its app (ptrlock.h). */
static void constraint_update(void) {
    Constraint *k = constraint;
    if (!k || !k->surf) return;
    Surf *top = pointer_focus;
    int on = top && (top == k->surf || top->parent == k->surf);
    int kb = key_focus && key_focus->client == k->surf->client;
    int what = ptrlock_step(&k->active, &k->spent, k->oneshot, on, kb);
    if (what == PTRLOCK_SAME) return;
    constraint_send(k, what == PTRLOCK_ON);
    cursor_gen++;
}

/* ❖Esc: the way out that doesn't depend on the app. */
static void constraint_escape(void) {
    Constraint *k = constraint;
    if (!k || ptrlock_escape(&k->active, &k->spent) == PTRLOCK_SAME) return;
    constraint_send(k, 0);
    cursor_gen++;
}

static void cons_gone(struct wl_resource *r) {
    Constraint *k = wl_resource_get_user_data(r);
    if (!k) return;
    if (constraint == k) constraint = NULL;
    cursor_gen++;
    free(k);
}
static void cons_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void cons_hint(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y) { (void)c; (void)r; (void)x; (void)y; }
static void cons_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *region) { (void)c; (void)r; (void)region; }
static const struct zwp_locked_pointer_v1_interface locked_impl = { .destroy = cons_destroy, .set_cursor_position_hint = cons_hint, .set_region = cons_region };
static const struct zwp_confined_pointer_v1_interface confined_impl = { .destroy = cons_destroy, .set_region = cons_region };

static void cons_new(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, uint32_t lifetime, int lock) {
    Surf *s = wl_resource_get_user_data(surface);
    if (constraint && constraint->surf == s) { wl_resource_post_error(r, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, "already constrained"); return; }
    Constraint *k = calloc(1, sizeof *k);
    struct wl_resource *res = k ? wl_resource_create(c, lock ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface, 1, id) : NULL;
    if (!res) { free(k); wl_client_post_no_memory(c); return; }
    k->res = res;
    k->surf = s;
    k->lock = lock;
    k->oneshot = lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT;
    if (lock) wl_resource_set_implementation(res, &locked_impl, k, cons_gone);
    else wl_resource_set_implementation(res, &confined_impl, k, cons_gone);
    if (constraint) { if (constraint->active) constraint_send(constraint, 0); constraint->surf = NULL; }
    constraint = k;
    constraint_update();
}
static void cons_lock(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *pointer,
                      struct wl_resource *region, uint32_t lifetime) {
    (void)pointer; (void)region;
    cons_new(c, r, id, surface, lifetime, 1);
}
static void cons_confine(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *pointer,
                         struct wl_resource *region, uint32_t lifetime) {
    (void)pointer; (void)region;
    cons_new(c, r, id, surface, lifetime, 0);
}
static void consm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_pointer_constraints_v1_interface consm_impl = { .destroy = consm_destroy, .lock_pointer = cons_lock, .confine_pointer = cons_confine };
static void bind_consm(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_pointer_constraints_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &consm_impl, NULL, NULL);
}

/* For main.c: pointer locked (does not move) or confined to the surface (clipped). */
int wl_pointer_locked(void) { return constraint && constraint->active && constraint->lock; }
void wl_pointer_confine(int *x, int *y) {
    if (!constraint || !constraint->active || constraint->lock || !constraint->surf) return;
    float ox, oy;
    Surf *s = constraint->surf;
    if (!origin(s, &ox, &oy)) return;
    int x0 = (int)ox, y0 = (int)oy, x1 = x0 + s->lw - 1, y1 = y0 + s->lh - 1;
    if (*x < x0) *x = x0;
    if (*y < y0) *y = y0;
    if (*x > x1) *x = x1;
    if (*y > y1) *y = y1;
}

/* Cursor requested by the app with the pointer: 0 = LordOS arrow, 1 = image, 2 = hidden. */
void wl_set_pointer_scale(int pct) { if (pct != pointer_scale) { pointer_scale = pct; cursor_gen++; } }

int wl_cursor(const uint32_t **px, int *w, int *h, int *hx, int *hy, unsigned *gen) {
    *gen = cursor_gen + (cursor_size_serial() << 20);      /* the pointer size redraws own shapes */
    if (wl_pointer_locked()) return 2;
    if (cursor_mode == 1 && cursor_surf && cursor_surf->px) {
        *px = cursor_surf->px;
        *w = cursor_surf->w;
        *h = cursor_surf->h;
        *hx = cursor_surf->cursor_hx;
        *hy = cursor_surf->cursor_hy;
        return 1;
    }
    if (cursor_mode == 3) {             /* cursor-shape-v1: an own shape */
        const CursorImage *ci = cursor_image_pct(cursor_from_wayland_shape(cursor_shape), pointer_scale);
        if (ci) { *px = ci->px; *w = ci->w; *h = ci->h; *hx = ci->hx; *hy = ci->hy; return 1; }
    }
    return cursor_mode == 2 ? 2 : 0;
}

/* ---- from JS ---- */

/* A text field of the app with the keyboard took or lost focus: the on-screen keyboard's hint. */
static void text_field_report(int on, int x, int y, int w, int h) {
    if (!key_focus) return;
    emit(key_focus, "textinput", "{\"on\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", on ? "true" : "false", x, y, w, h);
}

void wl_set_focus(int id) {
    if (!dpy) return;
    Surf *s = id ? find(id) : NULL;
    if (s && s->role != R_TOPLEVEL) s = NULL;
    if (s && s->parked) s = NULL;                      /* a window on a hidden desktop never has the keyboard */
    if (s && modal_over(s)) s = modal_over(s);         /* under a modal dialog: the keys are the dialog's */
    if (fs_surf && fs_surf->content) s = fs_surf;      /* in fullscreen the keyboard is its own */
    if (s == key_focus) return;
    struct wl_resource *kb;
    if (key_focus) {
        FOR_CLIENT(&keyboards, key_focus->client, kb) wl_keyboard_send_leave(kb, wl_display_next_serial(dpy), key_focus->res);
        key_focus->activated = 0;
        send_toplevel_configure(key_focus);
        dismiss_popups();
    }
    wl_textinput_focus(NULL, NULL);                    /* the field's "off" goes to the window that had it */
    key_focus = s;
    xact_focus_moved(s ? s->client : NULL);           /* tokens other apps asked for stop granting */
    if (s) wl_textinput_focus(s->client, s->res);
    if (s) {
        FOR_CLIENT(&keyboards, s->client, kb) send_enter_keyboard(kb, s);
        s->activated = 1;
        send_toplevel_configure(s);
        offer_to_focus();
        prim_offer_focus();
        clip_seen = term_clipboard_gen();
    }
    /* X11: the X focus follows (and with it whether X apps may read LordOS's clipboard) */
    xwm_focus(s && s->xw ? s->xw : NULL);
    constraint_update();                /* a pointer lock holds only with the keyboard (ptrlock.h) */
    wl_display_flush_clients(dpy);
}

void wl_close(int id) {
    Surf *s = find(id);
    if (s && s->top) xdg_toplevel_send_close(s->top);
    else if (s && s->xw) xwm_close(s->xw);
    if (dpy) wl_display_flush_clients(dpy);
}

/* The session is ending: every window is asked to close as the user would, and the apps get up to
 * ms to do it and leave. wl_stop() alone cuts their connection, which a browser records as a crash.
 * An app that asks something back ("Save changes?") or doesn't answer is cut by wl_stop() as before. */
void wl_close_all(int ms) {
    if (!dpy) return;
    struct wl_client *apps[MAX_SURF];
    int napps = 0;
    for (int i = 0; i < MAX_SURF; i++) {
        Surf *s = surfs[i];
        if (!s || !(s->xw || (s->top && s->mapped))) continue;
        if (s->top) xdg_toplevel_send_close(s->top);
        else xwm_close(s->xw);
        if (s->client != xwl_client) {
            int seen = 0;
            for (int k = 0; k < napps && !seen; k++) seen = apps[k] == s->client;
            if (!seen) apps[napps++] = s->client;
        }
    }
    wl_display_flush_clients(dpy);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        /* what is left: X11 windows, and the processes of the Wayland ones */
        int left = 0;
        for (int i = 0; i < MAX_SURF; i++) if (surfs[i] && surfs[i]->xw) left++;
        struct wl_client *c;
        wl_client_for_each(c, wl_display_get_client_list(dpy))
            for (int k = 0; k < napps; k++) if (apps[k] == c) left++;
        if (!left) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int spent = (int)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
        if (spent >= ms) break;
        struct pollfd p = { wl_fd(), POLLIN, 0 };
        poll(&p, 1, spent + 50 < ms ? 50 : ms - spent);
        wl_dispatch();
        wl_flush();
    }
}

/* Force Quit (the crown menu): SIGKILL to the process that owns this Wayland window, for an app that
 * stopped answering. Only a client of this session's user, never lord-server itself, never pid 1, and
 * never an X11 window (its client is Xwayland, which every X11 app shares: those are asked to close).
 * 0, or an error code for the interface. */
const char *wl_kill(int id) {
    Surf *s = find(id);
    if (!s || !s->top) return s && s->xw ? "app.forceQuitX11" : "app.notFound";
    pid_t pid = 0;
    uid_t uid = (uid_t)-1;
    gid_t gid;
    wl_client_get_credentials(wl_resource_get_client(s->top), &pid, &uid, &gid);
    if (pid <= 1 || pid == getpid() || uid != getuid()) return "app.forceQuitRefused";
    if (kill(pid, SIGKILL)) return "app.forceQuitFailed";
    fprintf(stderr, "wayland: force quit pid %d (window %d)\n", (int)pid, id);
    return NULL;
}

void wl_set_state(int id, int maximized, int fullscreen) {
    Surf *s = find(id);
    (void)fullscreen;                           /* fullscreen is handled by the compositor */
    if (!s || (!s->top && !s->xw) || s->maximized == maximized) return;
    s->maximized = maximized;
    send_toplevel_configure(s);
}

/* Minimized by the theme: an X11 window goes to the bottom of X's stack (it never catches the
 * pointer there) and is marked iconic. Wayland apps aren't told (xdg-shell has no such event).
 * Fullscreen is presented above everything, so it would stay on screen with the keyboard: it stops
 * here and comes back with the window, as on a hidden desktop (wl_set_parked). */
void wl_set_minimized(int id, int minimized) {
    Surf *s = find(id);
    if (!s) return;
    s->xmin = minimized;
    if (minimized && s == fs_surf) {
        leave_fullscreen(s);
        s->fs_resume = 1;
    } else if (!minimized && s->fs_resume && !s->parked) {
        s->fs_resume = 0;
        enter_fullscreen(s);
    }
    if (s->xw) xwm_set_state(s->xw, s->maximized, s->fullscreen, minimized);
}

/* On a hidden desktop (sdk/desktopmodel.js surfaceState; C knows nothing else about desktops). The window
 * has no node there, so it can't be painted or reached with the pointer; this takes what doesn't depend
 * on the node: the keyboard, and fullscreen, which is presented above everything and would keep both.
 * Fullscreen comes back with the window, and one the app asks for meanwhile waits for it. */
void wl_set_parked(int id, int parked) {
    Surf *s = find(id);
    if (!s || (!s->top && !s->xw) || s->parked == !!parked) return;
    s->parked = !!parked;
    if (parked) {
        int resume = s == fs_surf || s->fs_resume;      /* wl_set_minimized runs first and may have kept it */
        if (s == fs_surf) leave_fullscreen(s);
        s->fs_resume = resume;
        if (s == key_focus) wl_set_focus(0);
        for (int i = npopups - 1; i >= 0; i--)
            if (toplevel_of(popups[i]) == s && popups[i]->popup) xdg_popup_send_popup_done(popups[i]->popup);
    } else if (s->fs_resume && !s->xmin) {
        s->fs_resume = 0;
        enter_fullscreen(s);
    }
    if (dpy) wl_display_flush_clients(dpy);
}

/* The screen changed size (headless session, docs/SESSIONS.md): new mode for the apps
 * and the fullscreen one adapts. */
void wl_output_resized(void) {
    if (!dpy) return;
    int w = (int)ui.width, h = (int)ui.height;
    WlOutput o = { 0, 0, w, h, w, h, 60000, 100, w * 254 / 960, h * 254 / 960, "LORD-1", "LordOS display" };
    wl_set_outputs(&o, 1, 0);
}

/* The fullscreen window follows its display (resized, or unplugged: the main one). */
static void fs_follow_outputs(void) {
    if (!fs_surf) return;
    if (fs_slot < 0 || fs_slot >= OUT_MAX || !oslots[fs_slot].used) fs_slot = out_main;
    float fx, fy, fw, fh;
    fs_rect(&fx, &fy, &fw, &fh);
    if ((int)fw == fs_surf->conf_w && (int)fh == fs_surf->conf_h) return;
    fs_surf->conf_w = (int)fw;
    fs_surf->conf_h = (int)fh;
    send_toplevel_configure(fs_surf);
    damage_add((Box){ 0, 0, ui.width, ui.height });
    ui.dirty = 1;
}

int wl_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_SURF; i++) if (surfs[i] && surfs[i]->role == R_TOPLEVEL && surfs[i]->mapped) n++;
    return n;
}

/* ---- startup and loop ---- */

int wl_start(void) {
    if (!getenv("XDG_RUNTIME_DIR")) {
        char dir[64];
        snprintf(dir, sizeof dir, "/run/user/%d", (int)getuid());
        if (access(dir, W_OK)) { fprintf(stderr, "wayland: no XDG_RUNTIME_DIR\n"); return -1; }
        setenv("XDG_RUNTIME_DIR", dir, 1);
    }
    dpy = wl_display_create();
    if (!dpy) return -1;
    const char *name = wl_display_add_socket_auto(dpy);
    if (!name) { wl_display_destroy(dpy); dpy = NULL; fprintf(stderr, "wayland: could not create the socket\n"); return -1; }
    snprintf(socket_name, sizeof socket_name, "%s", name);
    loop = wl_display_get_event_loop(dpy);
    wl_list_init(&pointers);
    wl_list_init(&keyboards);
    wl_list_init(&data_devices);
    wl_list_init(&outputs);
    wl_display_init_shm(dpy);
    wl_global_create(dpy, &wl_compositor_interface, 6, NULL, bind_compositor);
    wl_global_create(dpy, &wl_subcompositor_interface, 1, NULL, bind_subcompositor);
    wl_global_create(dpy, &zxdg_output_manager_v1_interface, 3, NULL, bind_xdg_output_mgr);
    wl_global_create(dpy, &wp_fractional_scale_manager_v1_interface, 1, NULL, bind_frac_mgr);
    wl_global_create(dpy, &wl_seat_interface, 9, NULL, bind_seat);    /* 8: axis_value120, 9: relative_direction */
    wl_global_create(dpy, &wl_data_device_manager_interface, 3, NULL, bind_ddm);
    wl_list_init(&prim_devices);
    wl_list_init(&inhibitors);
    wl_global_create(dpy, &zwp_primary_selection_device_manager_v1_interface, 1, NULL, bind_pdm);   /* middle-click paste */
    wl_global_create(dpy, &xdg_wm_base_interface, 6, NULL, bind_wm_base);
    wl_global_create(dpy, &zxdg_decoration_manager_v1_interface, 1, NULL, bind_decm);
    wl_list_init(&exports);
    wl_list_init(&imports);
    wl_global_create(dpy, &zxdg_exporter_v2_interface, 1, NULL, bind_exporter);    /* a window as a handle (portal dialogs) */
    wl_global_create(dpy, &zxdg_importer_v2_interface, 1, NULL, bind_importer);
    wl_global_create(dpy, &xdg_wm_dialog_v1_interface, 1, NULL, bind_wm_dialog);   /* modal dialogs */
    wl_list_init(&rel_pointers);
    wl_list_init(&feedbacks);
    if (gpu_active()) ngpu_fmt = gpu_formats(gpu_fmt_cache, 256);
    gpu_set_hooks(wl_gpu_lost, wl_gpu_changed);
    wl_global_create(dpy, &zwp_linux_dmabuf_v1_interface, 4, NULL, bind_dmabuf);
    wl_global_create(dpy, &wp_presentation_interface, 1, NULL, bind_presentation);
    wl_global_create(dpy, &wp_viewporter_interface, 1, NULL, bind_viewporter);
    wl_global_create(dpy, &zwp_relative_pointer_manager_v1_interface, 1, NULL, bind_relm);
    wl_global_create(dpy, &zwp_pointer_constraints_v1_interface, 1, NULL, bind_consm);
    wl_global_create(dpy, &wp_cursor_shape_manager_v1_interface, 1, NULL, bind_csm);   /* cursors by name */
    wl_global_create(dpy, &zwp_idle_inhibit_manager_v1_interface, 1, NULL, bind_iim);  /* video keeps the screen on */
    wl_global_create(dpy, &xdg_activation_v1_interface, 1, NULL, bind_activation);   /* a window comes forward with a token */
    client_created.notify = on_client_created;
    wl_display_add_client_created_listener(dpy, &client_created);
    wl_global_create(dpy, &kde_mgr_interface, 1, NULL, bind_kde_deco);   /* theme frame for GTK 3 (Firefox) */
    wl_input_globals(dpy);              /* zwp_pointer_gestures_v1 (wl_input.c) */
    wl_textinput_globals(dpy, text_field_report);    /* zwp_text_input_v3 (wl_textinput.c) */
    /* inherited by apps opened from the LordOS terminal */
    setenv("WAYLAND_DISPLAY", socket_name, 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "LordOS", 1);
    /* Wayland first; x11 only for a GTK program that allows nothing else (gdk_set_allowed_backends:
     * VMware's vmware-user agent). With "wayland" alone GTK skips x11 and such a program can't open
     * any display (docs/WAYLAND.md, "VMware Tools") */
    setenv("GDK_BACKEND", "wayland,x11", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    /* GTK 4.18 doesn't pick its text-input-v3 module by itself here (no input-method framework running), and the
     * keys then reach the app raw: LordOS's input methods compose only through it. Not overwritten when the person
     * set another one (an IBus or Fcitx user) */
    setenv("GTK_IM_MODULE", "wayland", 0);
    clip_seen = term_clipboard_gen();
    fprintf(stderr, "wayland: %s/%s\n", getenv("XDG_RUNTIME_DIR"), socket_name);
    return 0;
}

void wl_stop(void) {
    if (!dpy) return;
    xwl_stop();
    wl_display_destroy_clients(dpy);
    wl_display_destroy(dpy);
    dpy = NULL;
}

int wl_fd(void) { return dpy ? wl_event_loop_get_fd(loop) : -1; }
const char *wl_socket(void) { return dpy ? socket_name : ""; }

void wl_dispatch(void) { if (dpy) wl_event_loop_dispatch(loop, 0); }

void wl_flush(void) {
    if (!dpy) return;
    /* LordOS copied something (terminal, text field): the focused app sees it as the selection */
    unsigned g = term_clipboard_gen();
    if (g != clip_seen) {
        clip_seen = g;
        clip_is_image = 0;
        if (selection) { wl_data_source_send_cancelled(selection->res); selection = NULL; }
        clip_offer_gen++;
        offer_to_focus();
    }
    xwm_tick();                         /* X11: clipboard changes, pending requests */
    wl_display_flush_clients(dpy);
    if (keylag_enabled) keylag_flushed();
}

/* ---- for wl_input.c (gestures, fine scrolling and touches) ---- */
struct wl_display *wl__display(void) { return dpy; }
struct wl_resource *wl__pointer_focus(void) { return dpy && pointer_focus ? pointer_focus->res : NULL; }
struct wl_list *wl__pointers(void) { return &pointers; }

/* Surface under a screen point (menus, fullscreen or the node's window). */
struct wl_resource *wl__surface_at(float x, float y, float *lx, float *ly) {
    if (!dpy) return NULL;
    Surf *target = NULL;
    for (int i = npopups - 1; i >= 0 && !target; i--) {
        float ox, oy;
        if (origin(popups[i], &ox, &oy)) target = surface_at(popups[i], x - ox, y - oy, lx, ly);
    }
    if (!target && fs_surf && fs_surf->content) {
        float ox, oy;
        if (origin(fs_surf, &ox, &oy)) target = surface_at(fs_surf, x - ox, y - oy, lx, ly);
    } else if (!target) {
        Node *hit = hit_test(ui.root, x, y);
        if (hit && hit->type == NODE_SURFACE) {
            Surf *s = find(hit->term);
            float ox, oy;
            if (s && origin(s, &ox, &oy)) target = surface_at(s, x - ox, y - oy, lx, ly);
        }
    }
    return target ? target->res : NULL;
}

/* ---- X11 windows (xwm.c): the role of Xwayland's surfaces ---- */

struct wl_client *wlx_client(void) { return xwl_client; }
void wlx_set_client(struct wl_client *c) { xwl_client = c; }

int wlx_attach(uint32_t res_id, XWin *w, const XWinInfo *in) {
    if (!dpy || !xwl_client) return 0;
    struct wl_resource *r = wl_client_get_object(xwl_client, res_id);
    if (!r || !wl_resource_instance_of(r, &wl_surface_interface, &surface_impl)) return 0;
    Surf *s = wl_resource_get_user_data(r);
    if (!s || s->role != R_NONE || s->xw) return 0;
    s->xw = w;
    s->sent_configure = 1;                                  /* no xdg configure: X sizes it */
    snprintf(s->title, sizeof s->title, "%s", in->title ? in->title : "");
    snprintf(s->app_id, sizeof s->app_id, "%s", in->app_id ? in->app_id : "");
    snprintf(s->app_alt, sizeof s->app_alt, "%s", in->app_alt ? in->app_alt : "");
    s->csd = in->csd;
    if (in->popup) { s->role = R_POPUP; s->xpop = 1; s->xx = in->x; s->xy = in->y; }
    else s->role = R_TOPLEVEL;
    if (s->content) surface_commit_role(s);                 /* it already had a buffer */
    return s->id;
}

void wlx_detach(int sid) {
    Surf *s = find(sid);
    if (!s) return;
    if (s->role == R_TOPLEVEL && s->mapped) emit(s, "unmap", "{}");
    if (s->role == R_POPUP) unmap_popup(s);
    if (key_focus == s) { key_focus = NULL; wl_textinput_gone(); }
    if (pointer_focus == s) { pointer_focus = NULL; buttons_held = 0; }
    if (fs_surf == s) { fs_surf = NULL; damage_add((Box){ 0, 0, ui.width, ui.height }); }
    if (s->node) damage_node(s);
    ui.dirty = 1;
    s->mapped = 0;
    s->xw = NULL;
    s->xpop = 0;
    s->role = R_NONE;
}

void wlx_set_title(int sid, const char *t) {
    Surf *s = find(sid);
    if (!s) return;
    snprintf(s->title, sizeof s->title, "%s", t);
    if (s->mapped && s->role == R_TOPLEVEL) { char e[520]; json_str(e, sizeof e, s->title); emit(s, "title", "{\"title\":\"%s\"}", e); }
}

void wlx_popup_moved(int sid, int x, int y) {
    Surf *s = find(sid);
    if (!s || !s->xpop || (s->xx == x && s->xy == y)) return;
    popup_box_damage(s);
    s->xx = x;
    s->xy = y;
    popup_box_damage(s);
}

void wlx_request(int sid, int what, int arg) {
    Surf *s = find(sid);
    if (!s || s->role != R_TOPLEVEL) return;
    switch (what) {
    case XREQ_MOVE: if (s->mapped) { buttons_held = 0; emit(s, "move", "{\"x\":%.0f,\"y\":%.0f}", press_x, press_y); } break;
    case XREQ_RESIZE: if (s->mapped) { buttons_held = 0; emit(s, "resize", "{\"edges\":%d,\"x\":%.0f,\"y\":%.0f}", arg, press_x, press_y); } break;
    case XREQ_MAXIMIZE: if (s->mapped) emit(s, "maximize", "{}"); break;
    case XREQ_UNMAXIMIZE: if (s->mapped) emit(s, "unmaximize", "{}"); break;
    case XREQ_MINIMIZE: if (s->mapped) emit(s, "minimize", "{}"); break;
    case XREQ_FULLSCREEN: enter_fullscreen(s); break;
    case XREQ_UNFULLSCREEN: leave_fullscreen(s); break;
    default: break;
    }
}

void wlx_fullscreen_rect(int *x, int *y, int *w, int *h) {
    float fx, fy, fw, fh;
    fs_rect(&fx, &fy, &fw, &fh);
    *x = (int)fx; *y = (int)fy; *w = (int)fw; *h = (int)fh;
}

/* What an X11 app copied: LordOS's clipboard (the next wl_flush offers it to the focused Wayland app
 * and cancels a Wayland app's selection). */
void wlx_clip_text(const char *t, size_t n, const char *origin) {
    term_set_clipboard(t, n);
    term_clipboard_mark(origin && origin[0] ? origin : "x11", 0);
}

void wlx_clip_png(const unsigned char *png, size_t n, const char *origin) {
    if (n < 8 || memcmp(png, "\x89PNG\r\n\x1a\n", 8)) return;
    if (selection) { wl_data_source_send_cancelled(selection->res); selection = NULL; }
    clip_offer_gen++;
    store_png(png, n);
    snprintf(clip_img_origin, sizeof clip_img_origin, "%s", origin && origin[0] ? origin : "x11");
    clip_img_sensitive = 0;
    offer_to_focus();
}

void wlx_primary_text(const char *t, size_t n) {
    if (prim_src) { zwp_primary_selection_source_v1_send_cancelled(prim_src->res); prim_src = NULL; }
    prim_req++;
    prim_store(t, n, key_focus ? client_pid(key_focus->client) : 0);     /* an X11 app's: Xwayland has the keyboard */
}

/* LordOS's own selection (a text field, the Terminal): the app that had it is told it no longer does. Empty or
 * over CLIP_MAX: nothing changes. */
int wl_set_primary(const char *t, size_t n) {
    if (!t || !n || n > CLIP_MAX) return -1;
    if (prim_text && prim_len == n && !memcmp(prim_text, t, n) && !prim_pid) return 0;   /* the same selection again */
    if (prim_src) { zwp_primary_selection_source_v1_send_cancelled(prim_src->res); prim_src = NULL; }
    prim_req++;
    prim_store(t, n, 0);
    if (dpy) wl_display_flush_clients(dpy);
    return 0;
}

const char *wl_primary(size_t *n) {
    *n = prim_text ? prim_len : 0;
    return prim_text ? prim_text : "";
}

void wl_primary_paste(int on) {
    if (prim_paste == !!on) return;
    prim_paste = !!on;
    prim_offer_focus();                 /* the app with the keyboard gets the selection back, or loses it */
    if (dpy) wl_display_flush_clients(dpy);
}
int wl_primary_paste_on(void) { return prim_paste; }

const char *wl_focus_primary(size_t *n) {
    if (!dpy || !key_focus || !prim_text || !prim_len || !prim_pid || client_pid(key_focus->client) != prim_pid) return NULL;
    *n = prim_len;
    return prim_text;
}

const char *wlx_primary(size_t *n, unsigned *gen) {
    *n = prim_paste && prim_text ? prim_len : 0;          /* middle-click paste off: X apps get none either */
    *gen = prim_gen;
    return prim_text && prim_paste ? prim_text : "";
}

int wl_idle_inhibited(void) {
    if (!inhibitors.next) return 0;     /* before wl_start */
    struct wl_resource *r;
    wl_resource_for_each(r, &inhibitors) {
        struct wl_resource *sr = wl_resource_get_user_data(r);
        for (int i = 0; i < MAX_SURF; i++) if (surfs[i] && surfs[i]->res == sr && surfs[i]->mapped) return 1;
    }
    return 0;
}
