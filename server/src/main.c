#include <dirent.h>
#include <malloc.h>
#include <sys/mman.h>
/*
 * lord-server — servidor gráfico de LordOS, escrito desde cero.
 *
 * Toma la placa de video (DRM/KMS) y el input (evdev) directamente del kernel, sin X11,
 * Wayland, libdrm ni libinput. La interfaz la define un tema React que corre en QuickJS
 * y construye un árbol de nodos nativos (ui/); el servidor hace layout, pinta y envía
 * los eventos de mouse/teclado de vuelta al tema.
 *
 * Arranca como root (sudo) solo para abrir pantalla, input y consola; después corre como
 * el usuario de la sesión conservando únicamente CAP_SYS_TTY_CONFIG.
 *
 * La terminal (term/) también es propia: emulador xterm, PTY y dibujo en C.
 *
 * Salir: Ctrl+Alt+Backspace, "Cerrar sesión" del tema, SIGINT/SIGTERM o --timeout <segundos>.
 * Con una señal o --timeout los shells de la Terminal quedan vivos (term/hold.c) y se
 * recuperan en el próximo arranque; las dos primeras formas los cierran.
 */
#include "media/mstudio.h"
#include "apps.h"
#include "admin.h"
#include "lordlog.h"
#include "keylag.h"
#include "bench/lbinput.h"
#ifdef LORD_TEST_HOOKS
#include "testhooks.h"           /* build/lord-server-test only */
#include "cast/castproto.h"
#endif
#include "media/media_int.h"     /* the capture paths' pictures (test_frame, Hands' screenshots) */
#include "net/net.h"
#include "disk/disk.h"
#include "backup/backup.h"
#include "cloud/cloud.h"
#include "ui/js_cloud.h"
#include "fsasync.h"
#include "bgsave.h"
#include "fswatch.h"
#include "sound/snd.h"
#include "ui/wave.h"
#include "sec/json.h"
#include "keyboard.h"
#include "ui/js_sys.h"
#include "canvas.h"
#include "par.h"
#include "wdog.h"
#include "lord_version.h"
#include "input.h"
#include "inputkind.h"
#include "softpace.h"
#include "kms.h"
#include "drmcard.h"
#include <sys/resource.h>
#include "term/term.h"
#include "ui/js.h"
#include "ui/js_theme.h"
#include "ui/js_input.h"
#include "ui/ui.h"
#include "wl/wl.h"
#include "wl/outpick.h"
#include "gpu/gpu.h"
#include "remote/mirror.h"
#include "remote/share.h"
#include "remote/sharewire.h"
#include "osk.h"
#include "sec/restrict.h"
#include "fw/rules.h"
#include "remote/proto.h"
#include "tp/tp.h"
#include "tp/tp_route.h"
#include "wl/wl_input.h"
#include "wl/wl_tablet.h"
#include "wl/wl_warp.h"
#include "tablet.h"
#include "wl/xwayland.h"
#include "wl/drmlease.h"
#include "vault/dbus.h"
#include "media/media.h"
#include "media/svgicon.h"
#include "web/webmgr.h"
#include "ui/webalive.h"
#include "app/app.h"
#include "app/apphost_bc.h"
#include "sec/broker.h"
#include "cursor.h"
#include "shake.h"
#include "pointerlock.h"
#include "ui/grab.h"
#include "ui/js_wm.h"
#include "ui/portal.h"
#include "cast/cast.h"
#include "ui/sni.h"
#include "ui/js_logs.h"
#include "hw/hw.h"
#include "hw/pkagent.h"
#include "hw/sleeplock.h"
#include "hw/logindlock.h"
#include "hw/modes.h"
#include "hw/uevent.h"
#include "ui/js_hw.h"
#include "ui/js_vm.h"
#include "outputs.h"
#include "monitor/netrec.h"
#include "sec/codesig.h"
#include "lpk.h"
#include "sec/pkgapp.h"
#include "sec/pkgstate.h"
#include "sec/sign.h"
#include "sec/netconn.h"
#include "bench/lbframes.h"
#include "fpipe.h"
#include "nofile.h"
#include "hands/hands.h"
#include "hands/handsimg.h"
#include "hands/handsmark.h"
#include "hands/handscur.h"
#include "ui/js_hands.h"

#include <ctype.h>
#include <libgen.h>
#include <limits.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern int paint_profile;

#define MAX_BUTTONS 16
#define LOGOUT_CLOSE_MS 3000             /* how long the apps get to close at the end of a session */

typedef struct { int down, button; } ButtonEvent;

/* Drag measurement (docs/WINDOWS.md): with a button held and the pointer moving, the frames
 * and the work of each one are counted (pointer JS + layout + paint + present). On
 * release a "drag: …" line is written to the session log. */
typedef struct {
    int held, active;
    double t0, last, js, work_js, work, work_max, gap_max, paint, present, layout;
    long frames, moves;
} DragStats;
static DragStats dstat;

/* The copy of what is below the dragged window during the drag (paint.c, docs/WINDOWS.md): built
 * whole, built in part, frames that used it where 001633b's rule painted everything, and why others
 * didn't. Appended to the "drag: …" line, and written on its own when LordBench's scripted move ends. */
static void below_stats_text(char *buf, size_t len) {
    BelowStats b;
    paint_below_stats(&b);
    snprintf(buf, len, "below: %ld built whole, %ld in part, %ld frames switched to it (not: %ld ancestor, %ld blur, "
                       "%ld transform, %ld unreached, %ld cost)", b.full, b.part, b.switched, b.refused[BR_ANCESTOR],
             b.refused[BR_BLUR], b.refused[BR_TRANSFORM], b.refused[BR_UNREACHED], b.refused[BR_COST]);
}

/* GPU drag, the dragged window composited on the GPU (docs/GPU.md): frames, the strips the CPU painted
 * around it, what was uploaded of the copy of what is below and of the layer, and why other frames that
 * used the copy were painted by the CPU. Appended to the "drag: …" lines. */
static void gdrag_stats_text(char *buf, size_t len) {
    GdragStats g;
    long bpx, lpx, quads;
    paint_gdrag_stats(&g);
    gpu_gdrag_stats(&bpx, &lpx, &quads);
    int k = snprintf(buf, len, "GPU drag: %ld of %ld frames (%ld quads, %ld of them cut around later nodes), strips %.2f Mpx, uploaded below %.2f Mpx, layer %.2f Mpx "
                               "(not: %ld layer, %ld clip, %ld ancestor, %ld app, %ld after, %ld post, %ld catching up",
                     g.frames, g.tried, quads, g.cut, g.strip_px / 1e6, bpx / 1e6, lpx / 1e6, g.refused[GDRAG_R_LAYER], g.refused[GDRAG_R_CLIP],
                     g.refused[GDRAG_R_ANCESTOR], g.refused[GDRAG_R_APP], g.refused[GDRAG_R_AFTER], g.refused[GDRAG_R_POST], g.refused[GDRAG_R_FEED]);
    /* the layer's refusals by cause (GDRAG_L_*) */
    for (int i = 0, first = 1; i < GDRAG_L_N && k > 0 && (size_t)k < len; i++)
        if (g.layer[i]) {
            int w = snprintf(buf + k, len - (size_t)k, "%s%s %ld", first ? "; layer: " : ", ", lbf_gdrag_l_name(i), g.layer[i]);
            if (w > 0) k += w;
            first = 0;
        }
    if (k > 0 && (size_t)k < len) snprintf(buf + k, len - (size_t)k, ")");
}
_Static_assert(GDRAG_R_N <= LBF_GDRAG_R && GDRAG_L_N <= LBF_GDRAG_L, "lbframes.c names every GDRAG_R_* and GDRAG_L_*");

/* One display (docs/DISPLAYS.md): its KMS head, where it sits in the global logical space, and the
 * frame the UI paints for it at its own resolution. out[0]'s head owns the card's fd; the main display
 * (menu bar, Dock) is s->main_out. Without a monitor (headless) out[0] is a frame in memory. */
typedef struct {
    Kms kms;
    int on;
    DisplayOut d;               /* its entry of the display plan (mode, rotation, scale) */
    OutView v;                  /* logical rect → physical frame */
    Canvas scene;               /* upright physical frame (pw × ph), without the cursor */
    uint32_t *frame;            /* rotated displays and the capture view: scene + fullscreen + cursor */
    Box pend;                   /* logical damage not painted on it yet (its flip was pending) */
    DamageList pl;              /* the same as separate rectangles (paint_damage_rects); pend is their box */
    int pframes;                /* how many frames' damage pend holds (more than one: a flip was pending) */
    int full;                   /* paint all of it next time */
    int cdirty, cur_on;         /* the cursor moved since it was presented; it was drawn on it */
    /* CPU path: each KMS buffer only gets what changed since it was last drawn into (what was
     * repainted in the last two frames and where the cursor was), not the whole frame */
    Rect stale[2], curs[2];
    /* the same as separate rectangles (physical): what paint_out painted this frame, and what each
     * buffer is missing; with several damaged rectangles only those are copied (0.8.2) */
    DamageList pstale[2];
    Box painted[DAMAGE_MAX];
    int npainted;
    int full_copy, was_fs;
    int gpu;                    /* its GPU output (gpu_frame), -1 = composed by the CPU */
    int wl_new;                 /* GPU path: a Linux app's window has new content not presented here yet */
    /* GPU drag (paint_root_gdrag, docs/GPU.md): this frame left the dragged window's rectangle to the GPU
     * (gdragtaken, gdrag); the last frame shown has it there, so the scene and the UI texture are stale in gdragw
     * (gdrag_on) until a frame paints it on the CPU; gdragcpu: the GPU could not draw it, the next frame paints
     * it on the CPU without trying GPU drag */
    int gdragtaken, gdrag_on, gdragcpu;
    Rect gdragw;
    GpuDrag gdrag;
    int pgdrag, pgdragwhy, pgdragsub;    /* what the frame painted last got from GPU drag (paint_gdrag_last), until it is submitted */
    /* the frame pipeline (fpipe.h, docs/GPU.md "The frame pipeline"): a frame painted into the scene while the
     * flip was pending (prep, an FPIPE_*), what it painted (prep_r; prep_whole: all of it) and what it was painted
     * against, checked before it is submitted (the scene's pixels, the GPU context and output, paint.c's GPU drag
     * epoch); its CPU time so far and its phases for LordBench (main display) */
    int prep, prep_whole, prep_gpu;
    int prep_js;                /* an FPIPE_EMPTY prepare ran the JS frame callbacks and they asked again (fpipe_hold_callbacks) */
    Rect prep_r;
    const uint32_t *prep_px;
    unsigned prep_gen, prep_epoch;
    double prep_ms;
    LbPhase pph;
} Out;

typedef struct {
    Out out[OUT_MAX];
    int main_out;               /* index of the main display */
    Out cap;                    /* "all displays" for the remote desktop and recording (capture view) */
    Input input;
    int W, H;                   /* the global logical space */
    int cx, cy, ctrl, alt;
    int moved, wheel, cursor_dirty;
    ButtonEvent buttons[MAX_BUTTONS];
    int nbuttons;
    int quit, debug;
    long frames, renders;
    double layout_total, paint_total;
    /* pointer: raw motion (relative-pointer) and last position (lock) */
    double rdx, rdy;
    int lx, ly;
    /* cursor: the app's (wl_cursor) and whether it goes on the KMS cursor plane (GPU path): hw_cursor_ok
     * when the planes work, hw_cursor while the cursor is on them (not when its image is larger) */
    unsigned cursor_gen;
    int cursor_mode, cursor_hx, cursor_hy, hw_cursor, hw_cursor_ok;
    GpuTex *cursor_tex;
    int cursor_tex_w, cursor_tex_h;
    /* session without a monitor (lord-sessiond shows it on the monitor or the remote desktop): it is
     * drawn in memory, without DRM or evdev; greeter = the login screen */
    int headless, greeter, shift, meta;
    double last_render;
    /* GPU paused because someone is watching over the network: it comes back when nobody is left */
    int gpu_paused;
    /* pointer motion gathered until the next frame; buttons held (mask); cursor shape from the
     * node under the pointer (`cursor` style, docs/WINDOWS.md) */
    int motion_pending, held, shape, over_app;
    int over_media;             /* source of the media node under the pointer (its own pointer, Machines) */
} Server;

#define K0(s) (&(s)->out[0].kms)

static Rect rect_join(Rect a, Rect b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Rect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
static Box box_join(Box a, Box b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Box){ fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1) };
}
static int box_empty(Box b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

/* The scene's GPU drag rectangle (still the GPU's) is painted again on the CPU in the next frame of the display */
static void gdrag_stale(Out *o) {
    float x0, y0, x1, y1;
    out_logical_box(&o->v, o->gdragw, &x0, &y0, &x1, &y1);
    if (box_empty(o->pend)) { o->pl.n = 0; o->pframes = 0; }
    o->pend = box_join(o->pend, (Box){ x0, y0, x1, y1 });
    o->pframes++;                       /* another frame's damage joins: paint_out settles the blur rule again */
    o->gdrag_on = 0;
}

/* The frame pipeline: a prepared frame that will not be submitted (the GPU, the displays or GPU drag's layer changed
 * under it). What it painted is in the scene but not on the screen, so it joins the next frame's damage (with
 * the rectangle it left to GPU drag, stale in the scene). */
static void prep_undo(Out *o) {
    if (o->prep == FPIPE_READY) {
        Rect r = o->prep_r;
        if (o->gdragtaken) r = rect_join(r, o->gdrag.w);
        if (o->prep_whole) o->full = 1;
        else if (r.x1 > r.x0 && r.y1 > r.y0) {
            float x0, y0, x1, y1;
            out_logical_box(&o->v, r, &x0, &y0, &x1, &y1);
            if (box_empty(o->pend)) { o->pl.n = 0; o->pframes = 0; }
            o->pend = box_join(o->pend, (Box){ x0, y0, x1, y1 });
            o->pframes++;               /* (as gdrag_stale) */
        }
        o->gdragtaken = 0;
    }
    o->prep = FPIPE_NONE;
    o->prep_js = 0;
}

/* ---- the displays ---- */

static int out_count(const Server *s) {
    int n = 0;
    for (int i = 0; i < OUT_MAX; i++) n += s->out[i].on;
    return n;
}
static Out *main_out(Server *s) { return &s->out[s->out[s->main_out].on ? s->main_out : 0]; }
/* page flips that complete before vblank (see the main loop): consecutive early flips seen, 0..8 */
static int flip_instant, flip_told;
static SoftPace pace_sw;                /* the next frame's slot when frames are paced in software (softpace.h) */

static FPipe fpipe;                     /* LORD_PIPELINE (fpipe.h), read once at startup */
static void prep_undo_all(Server *s) { for (int i = 0; i < OUT_MAX; i++) prep_undo(&s->out[i]); }

static int any_flip_pending(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].kms.flip_pending) return 1;
    return 0;
}
/* every display is waiting for its flip: nothing can be presented now */
static int all_busy(const Server *s) {
    if (s->headless) return 0;
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && !s->out[i].kms.flip_pending) return 0;
    return 1;
}
static int out_rects(const Server *s, ORect *r, int *idx) {
    int n = 0;
    for (int i = 0; i < OUT_MAX; i++)
        if (s->out[i].on) { const OutView *v = &s->out[i].v; idx[n] = i; r[n++] = (ORect){ v->x, v->y, v->w, v->h }; }
    return n;
}
/* where the pointer's image is drawn: at the pointer, or where a pointer lock's owner shows it (pointerlock.h: a
 * machine's guest pointer, inside its picture; the pointer itself stays put) */
/* Hands (docs/HANDS.md "The agent's cursor"): an agent's input goes through the pointer's own path, but the person's
 * pointer stays where they left it. The first agent move keeps the person's position; their pointer's image is drawn
 * there while the agent holds the pointer, and their next pointer input of their own (a move, a button, a wheel, a
 * remote viewer's) takes it back from there (hands_reclaim). */
static int hands_borrowed, hands_px, hands_py;
static void cursor_at(const Server *s, int *x, int *y) {
    if (!ptrlock_cursor(x, y)) {
        *x = hands_borrowed ? hands_px : s->cx;
        *y = hands_borrowed ? hands_py : s->cy;
    }
}
static void hands_reclaim(Server *s) {
    if (!hands_borrowed) return;
    hands_borrowed = 0;
    s->cx = hands_px;
    s->cy = hands_py;
    s->lx = s->cx;
    s->ly = s->cy;
    s->moved = 1;
    s->cursor_dirty = 1;
}
/* the display under the pointer's image (-1 headless or none) */
static int out_at_pointer(const Server *s) {
    ORect r[OUT_MAX];
    int cx, cy;
    cursor_at(s, &cx, &cy);
    int idx[OUT_MAX], n = out_rects(s, r, idx), k = out_at(r, n, cx, cy);
    return k >= 0 ? idx[k] : -1;
}
/* a pointer lock's node, where it is now (pointerlock.h) */
static int ptrlock_rect(int node, float r[4]) {
    const Node *n = node_get(node);
    if (!n) return -1;
    r[0] = n->ax; r[1] = n->ay; r[2] = n->w; r[3] = n->h;
    return 0;
}
/* The pointer never lands outside the displays (nor in the gap next to a shorter one). */
static void clamp_pointer(Server *s) {
    ORect r[OUT_MAX];
    int idx[OUT_MAX], n = s->headless ? 0 : out_rects(s, r, idx);
    if (n) out_clamp(r, n, &s->cx, &s->cy);
    if (s->cx < 0) s->cx = 0;
    if (s->cy < 0) s->cy = 0;
    if (s->cx >= s->W) s->cx = s->W - 1;
    if (s->cy >= s->H) s->cy = s->H - 1;
}
static int out_scale(const Out *o) { return o->d.scale ? o->d.scale : 100; }
static void view_of(const Out *o, int index) {
    paint_view = (PaintView){ (float)o->v.x, (float)o->v.y, (float)o->v.k, (float)o->v.dx, (float)o->v.dy, index };
}
static void view_reset(void) { paint_view = (PaintView){ 0, 0, 1, 0, 0, 0 }; }
/* The displays the UI is painted on, for the blurred backdrop cache (paint.c): a blurred node is left out of the
 * blur rule only when every display showing it keeps its blurred pixels */
static void paint_views_set(const Server *s) {
    paint_nviews = 0;
    for (int i = 0; i <= OUT_MAX && paint_nviews < PAINT_VIEWS_MAX; i++) {
        const Out *o = i < OUT_MAX ? &s->out[i] : &s->cap;
        if (!o->on) continue;
        paint_views[paint_nviews++] = (PaintViewBox){ { (float)o->v.x, (float)o->v.y, (float)(o->v.x + o->v.w), (float)(o->v.y + o->v.h) },
                                                      (float)o->v.x, (float)o->v.y, (float)o->v.k, (float)o->v.dx, (float)o->v.dy, i };
    }
}

/* "text" for JSON (display names come from EDID: printable ASCII, but escaped anyway). */
static void jquote(char *out, size_t n, const char *s) {
    size_t k = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[k++] = '"';
    for (; *s && k + 8 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[k++] = '\\'; out[k++] = (char)ch; }
        else if (ch < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", ch);
        else out[k++] = (char)ch;
    }
    out[k++] = '"';
    out[k] = 0;
}

/* Cursor that is shown: the Linux app's under the pointer, or a LordOS shape (cursor.c) at the
 * display's scale (pct). mode 0 = the LordOS arrow, 1 = image, 2 = hidden (like wl_cursor). */
typedef struct { int mode; const uint32_t *px; int w, h, hx, hy; unsigned gen; } Cur;

/* An own shape's generation: changes with the shape, the scale it's drawn at and the pointer size. */
static unsigned cursor_gen_of(int shape, int pct) {
    return 0xf0000000u + ((cursor_size_serial() & 0xfffu) << 12) + (unsigned)shape * 16 + (unsigned)(cursor_effective_pct(pct) / 25);
}

static void cursor_now(const Server *s, Cur *c, int pct) {
    /* a pointer lock with no pointer shown (pointerlock.h): hidden */
    if (ptrlock_hidden()) { *c = (Cur){ 2, NULL, 0, 0, 0, 0, 0xd0000000u }; return; }
    c->mode = wl_cursor(&c->px, &c->w, &c->h, &c->hx, &c->hy, &c->gen);
    int grabbing = grab_active();
    if (c->mode && !grabbing && (s->over_app || wl_pointer_locked())) return;   /* the app chose its own (or hid it) */
    /* over a machine's picture: the guest's pointer, at the size the picture is shown (media_node_cursor) */
    if (!grabbing && !s->over_app && s->over_media && s->shape <= CUR_DEFAULT) {
        Cur m = { 0 };
        m.mode = media_node_cursor(s->over_media, &m.px, &m.w, &m.h, &m.hx, &m.hy, &m.gen);
        if (m.mode) { *c = m; return; }
    }
    int shape = grabbing ? grab_cursor() : s->shape;
    if (shape <= CUR_DEFAULT) { c->mode = 0; return; }
    const CursorImage *ci = cursor_image_pct(shape, pct);
    if (!ci) { c->mode = 0; return; }
    *c = (Cur){ 1, ci->px, ci->w, ci->h, ci->hx, ci->hy, cursor_gen_of(shape, pct) };
}

/* To draw it here (CPU, cursor plane, texture): the arrow is also an image. */
static void cursor_local(const Server *s, Cur *c, int pct) {
    cursor_now(s, c, pct);
    if (c->mode != 0) return;
    const CursorImage *ci = cursor_image_pct(CUR_DEFAULT, pct);
    if (ci) *c = (Cur){ 1, ci->px, ci->w, ci->h, ci->hx, ci->hy, cursor_gen_of(CUR_DEFAULT, pct) };
}

/* The pointer in a display's physical pixels. */
static void pointer_phys(const Server *s, const Out *o, int *x, int *y) {
    double px, py;
    int cx, cy;
    cursor_at(s, &cx, &cy);
    out_to_phys(&o->v, cx, cy, &px, &py);
    *x = (int)lround(px);
    *y = (int)lround(py);
}

/* GPU path, one display: the cursor plane follows the pointer. */
static void hw_cursor_move(Server *s) {
    if (!s->hw_cursor) return;
    /* the plane of the display under the pointer shows it; the others hide theirs */
    int at = out_at_pointer(s);
    for (int i = 0; i < OUT_MAX; i++) {
        Out *o = &s->out[i];
        if (!o->on || o->gpu < 0) continue;
        if (i != at || s->cursor_mode == 2) { kms_cursor_show(&o->kms, 0); continue; }
        int x, y;
        pointer_phys(s, o, &x, &y);
        kms_cursor_show(&o->kms, 1);
        kms_cursor_move(&o->kms, x - s->cursor_hx, y - s->cursor_hy);
    }
}
static const char *const cursor_arrow[] = {
    "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX",
    "XooooooooX", "XoooooooooX", "XooooooooooX", "XooooooXXXXX", "XoooXooX",
    "XooX XooX", "XoX  XooX", "XX    XooX", "X     XooX", "       XX", NULL,
};

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Drops from root to the user who ran sudo (or --user), keeping only
 * CAP_SYS_TTY_CONFIG so the console can be put back in text mode on exit. */
static int drop_privileges(const char *user) {
    if (geteuid() != 0) return 0;
    if (!user) user = getenv("SUDO_USER");
    if (!user) {
        fprintf(stderr, "lord-server: warning, still running as root (use sudo or --user)\n");
        return 0;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw || pw->pw_uid == 0) { fprintf(stderr, "lord-server: invalid user '%s'\n", user); return -1; }
    if (prctl(PR_SET_KEEPCAPS, 1) || initgroups(pw->pw_name, pw->pw_gid) ||
        setgid(pw->pw_gid) || setuid(pw->pw_uid)) {
        perror("lord-server: dropping privileges");
        return -1;
    }
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct caps[2];
    memset(caps, 0, sizeof caps);
    caps[0].effective = caps[0].permitted = 1u << CAP_SYS_TTY_CONFIG;
    if (syscall(SYS_capset, &hdr, caps)) perror("lord-server: capset");
    prctl(PR_SET_KEEPCAPS, 0);
    if (setuid(0) == 0) { fprintf(stderr, "lord-server: could not drop privileges\n"); return -1; }
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    fprintf(stderr, "lord-server: running as %s (uid %d)\n", pw->pw_name, (int)pw->pw_uid);
    return 0;
}

static void update_cursor(Server *s);
static int gpu_start(Server *s);

/* Forma del cursor del nodo bajo el puntero: la del primero hacia arriba que tenga `cursor`. */
static int node_cursor(const Node *n) {
    for (; n; n = n->parent) if (n->s.cursor) return n->s.cursor;
    return CUR_DEFAULT;
}

static int out_scale(const Out *o);
static int out_at_pointer(const Server *s);

/* The window drag modifier's state came from the remote desktop or the test hooks (remote_key), not a keyboard here. */
static int mods_injected;
/* ❖ and Alt are tracked from every keyboard and a release can be lost (another console, a keyboard that sleeps or is
 * unplugged with the key down): before a press becomes a window drag, the kernel is asked whether one of this
 * computer's keyboards really holds the modifier (EVIOCGKEY), so a stale state never turns every click into a move.
 * What the remote desktop injected is taken as it is. */
static int drag_mod_really_held(Server *s) {
    if (mods_injected) return 1;
    int key1 = grab_mod() == GRAB_MOD_SUPER ? KEY_LEFTMETA : KEY_LEFTALT, key2 = grab_mod() == GRAB_MOD_SUPER ? KEY_RIGHTMETA : KEY_RIGHTALT;
    for (int i = 0; i < s->input.count; i++) {
        unsigned char keys[KEY_MAX / 8 + 1];
        memset(keys, 0, sizeof keys);
        if (s->input.dev[i].fd < 0 || ioctl(s->input.dev[i].fd, EVIOCGKEY(sizeof keys), keys) < 0) continue;
        if (((keys[key1 / 8] >> (key1 % 8)) & 1) || ((keys[key2 / 8] >> (key2 % 8)) & 1)) return 1;
    }
    if (grab_mod() == GRAB_MOD_SUPER) s->meta = 0; else s->alt = 0;
    return 0;
}
/* shake the pointer to find it (shake.h): every motion, from any device, feeds it */
static Shake pointer_shake;
static void dispatch_pointer(Server *s) {
    s->motion_pending = 0;
    {
        static int px, py, seen;
        if (s->moved && cursor_shake() && seen) shake_motion(&pointer_shake, now_ms(), s->cx - px, s->cy - py);
        px = s->cx; py = s->cy; seen = 1;
    }
    {   /* app cursors by name (cursor-shape-v1) at the scale of the display under the pointer */
        int oi = out_at_pointer(s);
        wl_set_pointer_scale(oi >= 0 ? out_scale(&s->out[oi]) : 100);
    }
    float x = (float)s->cx, y = (float)s->cy;
    /* arrastre de una ventana (grab.c): lo sigue C, sin JS ni la app Linux por cuadro */
    int grabbing = grab_active();
    if (grabbing && s->moved) grab_motion(x, y);
    for (int i = 0; i < s->nbuttons; i++) {
        if (s->buttons[i].down) s->held |= 1 << s->buttons[i].button;
        else s->held &= ~(1 << s->buttons[i].button);
    }
    grab_note_pointer(x, y, s->held);           /* antes del "down" en JS: ahí puede empezar un arrastre */
    int gnode = 0, gedges = 0;
    float gdx = 0, gdy = 0, gdw = 0, gdh = 0;
    int ended = grabbing && grab_finish(&gnode, &gedges, &gdx, &gdy, &gdw, &gdh);
    Node *target = hit_test(ui.root, x, y);
    int id = target ? target->id : 0;
    /* apps Wayland: reciben el puntero; el tema también (para enfocar y subir la ventana) */
    int down[MAX_BUTTONS], button[MAX_BUTTONS];
    for (int i = 0; i < s->nbuttons; i++) { down[i] = s->buttons[i].down; button[i] = s->buttons[i].button; }
    double t0 = now_ms();
    int over = 0;
    /* ❖ (or Alt, Settings → Appearance → Window options) + a press anywhere in a window: its move (left) or resize (right,
     * middle), offered to the shell before the app under the pointer or the theme's own handlers see it (grab.h,
     * sdk/moddrag.js, GitHub #219). Never while an app holds the pointer or the shortcuts (wl_moddrag_allowed) or
     * LordOS's own pointer lock runs. Once the shell took it, every button stays the drag's until all are released:
     * the app gets neither that press nor its release, and the theme only the moves and the releases. */
    static int mod_swallow;
    if (!grabbing && !mod_swallow && s->nbuttons && grab_mod_held(s->meta, s->alt, s->ctrl) && !ptrlock_active())
        for (int i = 0; i < s->nbuttons; i++) {
            if (!s->buttons[i].down || s->buttons[i].button > 2) continue;
            if (wl_moddrag_allowed(x, y, target) && drag_mod_really_held(s) && js_pointer_modgrab(id, x, y, s->buttons[i].button))
                mod_swallow = 1;
            break;
        }
    int swallowed = mod_swallow;
    if (mod_swallow && !s->held) mod_swallow = 0;
    if ((grabbing && !ended) || swallowed) wl_pointer_grab(s->nbuttons, down, button);
    else over = wl_pointer(x, y, target, s->moved, s->nbuttons, down, button, s->wheel);
    if (over == 2) id = 0;                  /* sobre un menú de una app Linux: el árbol no lo ve */
    if (ended) js_wm_end(gnode, gedges, gdx, gdy, gdw, gdh);
    int shape = over == 2 ? CUR_DEFAULT : node_cursor(target);
    if (shape != s->shape || grabbing != grab_active() || (over != 0) != s->over_app) { s->shape = shape; s->cursor_dirty = 1; }
    s->over_app = over != 0;                /* el cursor de la app solo vale sobre sus superficies */
    int om = over == 0 && target && target->type == NODE_MEDIA ? target->term : 0;
    if (om != s->over_media) { s->over_media = om; s->cursor_dirty = 1; }
    if (s->moved && !grabbing) js_pointer("move", id, x, y, 0);
    for (int i = 0; i < s->nbuttons; i++)          /* sobre un menú, solo los "up" (terminan arrastres) */
        if ((over != 2 && !swallowed) || !s->buttons[i].down) js_pointer(s->buttons[i].down ? "down" : "up", id, x, y, s->buttons[i].button);
    wl_drop_release_done();                 /* a Linux app's files dropped at this release: claimed by now, or cancelled */
    if (s->wheel && over != 2) js_pointer("wheel", id, x, y, s->wheel);
    if (dstat.held && s->moved) {
        if (!dstat.active) { dstat.active = 1; dstat.t0 = dstat.last = t0; }
        dstat.moves++;
        dstat.js += now_ms() - t0;
    }
    for (int i = 0; i < s->nbuttons; i++) {      /* máscara: dos dispositivos pueden repetir el mismo botón */
        if (s->buttons[i].down) dstat.held |= 1 << s->buttons[i].button;
        else dstat.held &= ~(1 << s->buttons[i].button);
    }
    if (!dstat.held) {
        if (dstat.active && dstat.frames > 1 && dstat.last > dstat.t0) {
            double span = dstat.last - dstat.t0;
            long lr = 0, lb = 0, lp = paint_layer_parts();
            paint_layer_stats(&lr, &lb);
            char bs[192], gs[512];
            below_stats_text(bs, sizeof bs);
            gdrag_stats_text(gs, sizeof gs);
            fprintf(stderr, "drag: %ld frames in %.0f ms = %.1f FPS (%ld moves); per frame: work %.2f ms "
                            "(max %.2f; JS %.2f, layout %.2f, paint %.2f, present %.2f), max gap %.1f ms; "
                            "layer: %ld painted (%ld of them a part), %ld copied (%s); %s; %s\n",
                    dstat.frames, span, 1000.0 * dstat.frames / span, dstat.moves, dstat.work / dstat.frames, dstat.work_max,
                    dstat.js / dstat.frames, dstat.layout / dstat.frames, (dstat.paint - dstat.layout) / dstat.frames,
                    dstat.present / dstat.frames, dstat.gap_max, lr, lp, lb, gpu_active() ? "GPU" : "CPU", bs, gs);
        }
        dstat = (DragStats){ 0 };
    }
    s->moved = s->wheel = s->nbuttons = 0;
}

/* ---- remote desktop: the browser's input, as if it were local (remote/mirror.c) ---- */

static Server *remote_server;

/* What the remote desktop and the screen recording see: the main display at its resolution, or
 * every display (Settings → Displays, "capture all": the capture view, the global space at 100 %). */
static Out *feed_out(Server *s) {
    if (s->headless) return &s->out[0];
    /* a running share may have asked for another display (SHARE_DISPLAY); one door at a time, so no 5673 viewer
     * sees this choice, only a recording made while the share runs would follow it */
    uint32_t want = share_display();
    if (want == SW_DISPLAY_ALL && out_count(s) > 1) return &s->cap;
    if (want && want != SW_DISPLAY_ALL)
        for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].kms.conn_id == want) return &s->out[i];
    if (display_capture_all() && out_count(s) > 1) return &s->cap;
    return main_out(s);
}

/* The displays the screen-sharing API offers (share.c), the "all displays" picture last, and the one shown now. A
 * session with no monitor has one, id 1. Sizes: logical, and the picture's pixels (what RP_PTR is in). */
static int share_displays_of(SwDisplay *d, int max, uint32_t *current) {
    Server *s = remote_server;
    const Out *feed = feed_out(s);
    int n = 0;
    *current = 0;
    for (int i = 0; i < OUT_MAX && n < max && n < SW_DISPLAYS_MAX; i++) {
        const Out *o = &s->out[i];
        if (!o->on) continue;
        SwDisplay *e = &d[n++];
        memset(e, 0, sizeof *e);
        e->id = s->headless ? 1 : o->kms.conn_id;
        e->x = o->v.x;
        e->y = o->v.y;
        e->w = o->v.w;
        e->h = o->v.h;
        e->pw = o->v.pw;
        e->ph = o->v.ph;
        e->flags = i == s->main_out || s->headless ? SW_DF_MAIN : 0;
        snprintf(e->name, sizeof e->name, "%s", s->headless ? "LordOS" : o->d.name[0] ? o->d.name : o->d.connector);
        if (o == feed) *current = e->id;
        if (s->headless) break;
    }
    if (!s->headless && n > 1 && n < max) {
        SwDisplay *e = &d[n++];
        memset(e, 0, sizeof *e);
        *e = (SwDisplay){ SW_DISPLAY_ALL, 0, 0, s->W, s->H, s->W, s->H, SW_DF_ALL, "" };
        if (feed == &s->cap) *current = SW_DISPLAY_ALL;
    }
    return n;
}

static void remote_settle(Server *s) {
    display_activity(now_ms());             /* idle timers (Settings → Battery) */
    clamp_pointer(s);
    wl_pointer_confine(&s->cx, &s->cy);
    s->lx = s->cx;
    s->ly = s->cy;
    if (s->hw_cursor) hw_cursor_move(s);
    else s->cursor_dirty = 1;
    dispatch_pointer(s);
}

static void remote_pointer(int fx, int fy) {
    Server *s = remote_server;
    hands_person_input();                   /* a viewer at the remote desktop is a person too (hands.h) */
    hands_reclaim(s);
    double lx, ly;                          /* frame pixels of the feed → logical */
    out_to_logical(&feed_out(s)->v, fx, fy, &lx, &ly);
    int x = (int)lround(lx), y = (int)lround(ly);
    /* a pointer lock (pointerlock.h): the viewer's pointer moves by what it moved since its last position */
    static int rx = -1, ry = -1;
    int had = rx >= 0;
    int ddx = x - rx, ddy = y - ry;
    rx = x; ry = y;
    if (ptrlock_active()) { if (had) ptrlock_motion(ddx, ddy); return; }
    if (x == s->cx && y == s->cy) return;
    s->rdx += x - s->cx;
    s->rdy += y - s->cy;
    s->cx = x;
    s->cy = y;
    s->moved = 1;
    wl_pointer_relative(s->rdx, s->rdy);
    s->rdx = s->rdy = 0;
    remote_settle(s);
}

static void remote_button(int button, int down) {
    Server *s = remote_server;
    hands_person_input();                   /* a viewer at the remote desktop is a person too (hands.h) */
    hands_reclaim(s);
    if (down) hands_person_press();
    if (ptrlock_button(button, down)) return;
    if (s->nbuttons < MAX_BUTTONS) s->buttons[s->nbuttons++] = (ButtonEvent){ down, button };
    remote_settle(s);
}

static void remote_wheel(int steps) {
    Server *s = remote_server;
    hands_person_input();                   /* a viewer at the remote desktop is a person too (hands.h) */
    hands_reclaim(s);
    if (ptrlock_wheel(steps, 0)) return;
    s->wheel += steps;
    remote_settle(s);
}

/* Is there a keyboard to type on (the on-screen keyboard's Automatic mode)? A viewer who typed on the
 * remote desktop has one too, until the session is back on the monitor. */
static int remote_typed;
static void keyboards_check(Server *s) {
    static int last = -1;
    int now = input_has_keyboard(&s->input) || remote_typed;
    int local = 0;                                         /* volume buttons alone are not a way to answer it */
    for (int i = 0; i < s->input.count; i++) if (!s->input.dev[i].syskeys) local = 1;
    pkagent_set_local(local);                              /* whether a PS/2 keyboard is real is unknown until it types */
    js_input_set_keyboard(now);
    if (now != last) { last = now; js_keyboards_event(now); }
}

static void remote_key(int code, int value) {
    Server *s = remote_server;
    hands_person_input();                   /* a viewer at the remote desktop is a person too (hands.h) */
    if (value == 1) hands_person_press();
    if (pkagent_blocks_injected(value)) return;           /* an authentication prompt takes only this computer's keys */
    if (value == 1 && code >= KEY_Q && code <= KEY_M && !remote_typed) { remote_typed = 1; keyboards_check(s); }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) s->ctrl = value != 0;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) s->alt = value != 0;
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) s->meta = value != 0;   /* ❖ + drag moves a window (grab.h) */
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA || code == KEY_LEFTALT || code == KEY_RIGHTALT) mods_injected = value != 0;
    js_key(code, value);
}
static void remote_clip(const char *text, unsigned len) { term_set_clipboard(text, len); }
static void remote_clients(const char *json) { js_remote_event(json); }
static void full_damage(Server *s) {
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on) { s->out[i].full = 1; s->out[i].full_copy = 2; }
    s->cap.full = 1;
    damage_add((Box){ 0, 0, (float)s->W, (float)s->H });
    ui.dirty = 1;
}
static void remote_refresh(void) { full_damage(remote_server); }

/* The displays for JS (lord.screen.displays) and the Wayland apps (one wl_output each). */
static void publish_displays(Server *s) {
    WlOutput wo[OUT_MAX];
    char json[4096];
    int n = 0, len = 0, main_i = 0;
    const DisplayPlan *p = display_current();
    len += snprintf(json + len, sizeof json - (size_t)len, "{\"mirror\":%s,\"displays\":[", !s->headless && p->mirror ? "true" : "false");
    for (int i = 0; i < OUT_MAX; i++) {
        const Out *o = &s->out[i];
        if (!o->on) continue;
        if (i == s->main_out) main_i = n;
        WlOutput *w = &wo[n];
        memset(w, 0, sizeof *w);
        *w = (WlOutput){ o->v.x, o->v.y, o->v.w, o->v.h, o->v.pw, o->v.ph, s->headless ? 60000 : mode_refresh_mhz(&o->kms.mode),
                         out_scale(o), o->d.mm_w, o->d.mm_h, "", "", i, { 0 } };
        if (s->headless) { snprintf(w->name, sizeof w->name, "LORD-1"); snprintf(w->desc, sizeof w->desc, "LordOS display"); }
        else { snprintf(w->name, sizeof w->name, "%s", o->d.connector); snprintf(w->desc, sizeof w->desc, "%s", o->d.name); }
        if (!s->headless) memcpy(w->chroma, o->d.chroma, sizeof w->chroma);
        if (len < (int)sizeof json - 300) {
            char name[140];
            jquote(name, sizeof name, s->headless ? "LordOS" : o->d.name);
            len += snprintf(json + len, sizeof json - (size_t)len, "%s{\"id\":%u,\"name\":%s,\"connector\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                            "\"pw\":%d,\"ph\":%d,\"scale\":%d,\"main\":%s}", n ? "," : "", s->headless ? 0 : o->kms.conn_id, name, w->name,
                            o->v.x, o->v.y, o->v.w, o->v.h, o->v.pw, o->v.ph, out_scale(o), i == s->main_out ? "true" : "false");
        }
        n++;
    }
    snprintf(json + len, sizeof json - (size_t)len, "]}");
    wl_set_outputs(wo, n, main_i);
    js_set_displays(json);
    share_displays_changed();               /* a share's list, and its display if that one left */
}

/* Another size of the global logical space: the arrangement changed, or the browser window's
 * size (headless). Headless, out[0] is that frame in memory. */
static void set_size(Server *s, int w, int h) {
    if (s->headless) {
        Out *o = &s->out[0];
        if (w != o->scene.w || h != o->scene.h || !o->scene.px) {
            uint32_t *scene = malloc((size_t)w * h * 4), *frame = calloc((size_t)w * h, 4);
            if (!scene || !frame) { free(scene); free(frame); return; }
            free(o->scene.px);
            free(o->frame);
            canvas_init(&o->scene, scene, w, h, w);
            o->frame = frame;
        }
        o->on = 1;
        o->v = out_view(0, 0, w, h, 100);
        o->d.scale = 100;
        o->full = 1;
        s->main_out = 0;
    }
    int changed = w != s->W || h != s->H;
    if (changed) fprintf(stderr, "lord-server: desktop %dx%d → %dx%d\n", s->W, s->H, w, h);
    s->W = w;
    s->H = h;
    ui.width = (float)w;
    ui.height = (float)h;
    clamp_pointer(s);
    s->lx = s->cx;
    s->ly = s->cy;
    publish_displays(s);
    js_screen_resized();
    full_damage(s);
}

/* Request from the browser: only if the session is not on the monitor (there the monitor rules). */
static void remote_resize(int w, int h) {
    Server *s = remote_server;
    w &= ~1;
    h &= ~1;
    if (!s->headless || w < 640 || h < 480 || w > 3840 || h > 2400) return;
    set_size(s, w, h);
}

/* ---- the screen-sharing API (remote/share.c): the same injection as the remote desktop, with its own checks ---- */

/* Text for SHARE_TEXT waits here and goes as on-screen keyboard taps, through the active layout: a character a key
 * types goes as that key; a Latin letter with a mark the layout has as a dead key (á on Spanish, ü on German) goes as
 * the dead key and then the letter, which the layout's compose joins; anything else (ñ on a US layout, €, emoji) goes as
 * an input method's finished text to the field that has the keyboard (js_commit_text), and is dropped where no field
 * takes text (an X11 app, a terminal). A Linux app's field with text input gets all of it as one commit
 * (js_text_input_app). The queue is checked at each step, so it stops the moment a prompt opens. */
static char share_text_buf[SW_TEXT_MAX];
static unsigned share_text_len, share_text_off;
static void share_no_taps(void) { share_text_len = share_text_off = 0; osk_clear(); }
static void share_text(const char *t, unsigned n) {
    if (share_text_len || n > sizeof share_text_buf) return;  /* one at a time */
    memcpy(share_text_buf, t, n);
    share_text_len = n;
    share_text_off = 0;
}
static int share_surface_up(void) {
    /* the overlay, modal and system layers (sdk/layers.js): root nodes with something in them */
    for (const Node *n = ui.root ? ui.root->first : NULL; n; n = n->next) {
        int z = (int)n->s.z_index;
        if ((z == 40 || z == 60 || z == 100) && n->first) return 1;
    }
    return 0;
}
/* A visitor doesn't reach the windows of the app that let them in: there a person accepts, sets a password or changes
 * what the visitor may do. Theirs to press, never the visitor's. */
static int share_is_holder(const char *app) {
    const char *id = share_holder();
    return app && id && !strncmp(id, "lpk:", 4) && !strcmp(id + 4, app);
}
static int share_key_protected(void) { return wl_focus_protected() || share_is_holder(wl_focus_app()) || (!wl_has_key_focus() && media_protected_count() > 0); }
static int share_point_protected(void) {
    Server *s = remote_server;
    for (const Node *n = hit_test(ui.root, (float)s->cx, (float)s->cy); n; n = n->parent) if (n->protect || share_is_holder(wl_node_app(n->id))) return 1;
    return 0;
}
/* The key and level of the active layout that types `label` (kb_label's text, "dead:´" for a dead key). */
static int share_key_of(const char *label, int *code, int *level) {
    char got[32];
    for (size_t i = 0; i < osk_label_count; i++)
        for (int lv = 0; lv < 3; lv++) {
            kb_label(osk_label_codes[i], lv, got, sizeof got);
            if (!strcmp(got, label)) { *code = osk_label_codes[i]; *level = lv; return 1; }
        }
    return 0;
}
static void share_text_pump(void) {
    if (!share_text_len) return;
    if (share_surface_up() || share_key_protected()) { share_no_taps(); return; }
    if (js_text_input_app()) {
        /* a Linux app's field with text input: all of it as one input method's commit, after what is still queued */
        char t[SW_TEXT_MAX + 1];
        unsigned n = share_text_len - share_text_off;
        memcpy(t, share_text_buf + share_text_off, n);
        t[n] = 0;
        js_input_flush();
        if (!pkagent_blocks_injected(1)) js_commit_text(t);
        share_text_len = share_text_off = 0;
        return;
    }
    if (!kb_labels_begin()) { share_no_taps(); return; }
    while (share_text_off < share_text_len) {
        uint32_t cp = 0;
        int l = sw_utf8_next(share_text_buf + share_text_off, share_text_len - share_text_off, &cp);
        if (l <= 0) { share_text_off = share_text_len; break; }
        char want[8] = "";
        memcpy(want, share_text_buf + share_text_off, (size_t)l);
        int code, lv, full = 0;
        if (share_key_of(want, &code, &lv)) full = !osk_push(code, lv);
        else {
            char base[8], dead[16];
            const char *mark;
            int dcode, dlv;
            if (sw_dead_split(cp, base, &mark) && (size_t)snprintf(dead, sizeof dead, "dead:%s", mark) < sizeof dead &&
                share_key_of(dead, &dcode, &dlv) && share_key_of(base, &code, &lv)) {
                if (osk_room() < 2) full = 1;   /* both or neither: a dead key alone would change the next letter */
                else { osk_push(dcode, dlv); osk_push(code, lv); }
            } else if (!pkagent_blocks_injected(1)) {
                /* no key types it: what came before goes first, then this as an input method's text */
                js_input_flush();
                js_commit_text(want);
            }
        }
        if (full) break;                /* the next pass of the loop */
        share_text_off += (unsigned)l;
    }
    kb_labels_end();
    if (share_text_off >= share_text_len) share_text_len = share_text_off = 0;
}
static int share_person(void) { return !remote_server->headless && !display_is_off(); }
static void share_activity(void) { display_activity(now_ms()); }
static int share_restricted(const char *id) { return restrict_blocks(id, FW_R_NO_SCREEN); }

/* ---- Hands: an AI agent's input and screenshots (hands/hands.h, docs/HANDS.md; approved by the maintainer 2026-11-26) ----
 * The agent's pointer, wheel and keys go through the same path as the remote desktop's (remote_settle, dispatch_pointer,
 * hit test, js_key, the Wayland apps), never /dev/uinput or evdev. hands.c decided the call may happen; these say where
 * a point or a key would land and refuse the places an agent never reaches. */

/* the system band (sdk/layers.js: system 100, agent 105, input 110, capture 120): HANDS_SYSTEM_Z, handsmark.h */
static int hands_node_place(const Node *n) {
    if (pkagent_blocks_injected(1)) return HANDS_WHY_PROMPT;   /* an authentication prompt takes only real keys and clicks */
    for (const Node *a = n; a; a = a->parent) {
        if (a->protect) return HANDS_WHY_PROTECTED;
        if (a->hands_deny) return HANDS_WHY_DENIED_NODE;
        if (a->parent == ui.root && a->s.z_index >= HANDS_SYSTEM_Z) return HANDS_WHY_SYSTEM;
    }
    return 0;
}
static int hands_place(double x, double y, int *node) {
    Node *t = ui.root ? hit_test(ui.root, (float)x, (float)y) : NULL;
    *node = t ? t->id : 0;
    if (!t) return pkagent_blocks_injected(1) ? HANDS_WHY_PROMPT : 0;
    return hands_node_place(t);
}
static int hands_node_place_id(int id) {
    const Node *n = id > 0 ? node_get(id) : NULL;
    return n ? hands_node_place(n) : HANDS_WHY_OUTSIDE;
}
static int hands_same_target(int hit, int ref) {
    if (hit <= 0 || ref <= 0) return 0;
    const Node *h = node_get(hit), *r = node_get(ref);
    if (!h || !r) return 0;
    for (const Node *a = h; a; a = a->parent) if (a == r) return 1;     /* the click lands inside the ref */
    for (const Node *a = r; a; a = a->parent) {                          /* or on what holds it, below its window */
        if (a == h) return 1;
        if (a->parent == ui.root) break;
    }
    return 0;
}
/* a system prompt or Screenshots' selection shows (they take the keyboard): an agent's key would go there */
static int hands_prompt_up(void) {
    for (const Node *n = ui.root ? ui.root->first : NULL; n; n = n->next) {
        int z = (int)n->s.z_index;
        if ((z == 100 || z == 120) && n->first) return 1;
    }
    return 0;
}
static int hands_keys_place(void) {
    if (pkagent_blocks_injected(1)) return HANDS_WHY_PROMPT;
    if (hands_prompt_up()) return HANDS_WHY_SYSTEM;
    if (wl_has_key_focus()) {
        if (wl_focus_protected()) return HANDS_WHY_PROTECTED;
        if (wl_textinput_secret()) return HANDS_WHY_PASSWORD;
    }
    return 0;
}
static void hands_size(int *w, int *h) { *w = remote_server->W; *h = remote_server->H; }
static int hands_pointer_locked(void) { return ptrlock_active() || wl_pointer_locked(); }
static void hands_move(double x, double y) {
    Server *s = remote_server;
    if (!hands_borrowed) { hands_borrowed = 1; hands_px = s->cx; hands_py = s->cy; }    /* the person's, kept */
    int ix = (int)lround(x), iy = (int)lround(y);
    if (ix != s->cx || iy != s->cy) {
        s->rdx += ix - s->cx;
        s->rdy += iy - s->cy;
        s->cx = ix;
        s->cy = iy;
        wl_pointer_relative(s->rdx, s->rdy);
        s->rdx = s->rdy = 0;
    }
    s->moved = 1;                           /* a hover over the same place still tells what is under it */
    remote_settle(s);
}
static void hands_button(int button, int down) {
    Server *s = remote_server;
    if (s->nbuttons < MAX_BUTTONS) s->buttons[s->nbuttons++] = (ButtonEvent){ down, button };
    remote_settle(s);
}
static void hands_wheel(int dx, int dy) {
    Server *s = remote_server;
    if (dy) { s->wheel += dy; remote_settle(s); }
    if (dx) { tp_route_wheel120(0, dx * 120); tp_route_wheel_flush(); }
}
/* one key, as the remote desktop types it: the modifiers' state follows (remote_key) */
static void hands_keyev(int code, int value) {
    Server *s = remote_server;
    if (pkagent_blocks_injected(value)) return;
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) s->ctrl = value != 0;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) s->alt = value != 0;
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) s->meta = value != 0;
    js_key(code, value);
}
/* a key with its level's modifiers (Shift, AltGr) and the chord's, pressed and released */
static void hands_press(int code, int level, unsigned mods) {
    static const struct { unsigned m; int code; } M[] = {
        { HANDS_M_CTRL, KEY_LEFTCTRL }, { HANDS_M_ALT, KEY_LEFTALT }, { HANDS_M_SUPER, KEY_LEFTMETA }, { HANDS_M_SHIFT, KEY_LEFTSHIFT },
    };
    if (level & OSK_LEVEL_SHIFT) mods |= HANDS_M_SHIFT;
    int altgr = (level & OSK_LEVEL_ALTGR) && kb_has_altgr();
    for (int i = 0; i < 4; i++) if (mods & M[i].m) hands_keyev(M[i].code, 1);
    if (altgr) hands_keyev(KEY_RIGHTALT, 1);
    hands_keyev(code, 1);
    hands_keyev(code, 0);
    if (altgr) hands_keyev(KEY_RIGHTALT, 0);
    for (int i = 3; i >= 0; i--) if (mods & M[i].m) hands_keyev(M[i].code, 0);
}
static int hands_key(const HandsChord *c) {
    int code = c->code, level = 0;
    if (!code) {
        if (!kb_labels_begin()) return -1;
        int ok = share_key_of(c->ch, &code, &level);
        kb_labels_end();
        if (!ok) return -1;
        if (c->mods & (HANDS_M_CTRL | HANDS_M_ALT | HANDS_M_SUPER)) level &= ~OSK_LEVEL_ALTGR;   /* ctrl+s: the key, not its symbol */
    }
    /* a paste, by the key that will be pressed (whatever the layout calls it): never while the clipboard holds a secret */
    unsigned pm = c->mods | ((level & OSK_LEVEL_SHIFT) ? HANDS_M_SHIFT : 0);
    if (term_clipboard_sensitive() && ((code == KEY_V && (pm & (HANDS_M_CTRL | HANDS_M_SUPER))) || (code == KEY_INSERT && (pm & HANDS_M_SHIFT)) ||
                                       code == KEY_PASTE || hands_paste_chord(c))) return -2;
    hands_press(code, level, c->mods);
    return 0;
}
/* text with no line breaks or tabs (hands.c sends those as keys and checks the field again): through the active layout
 * as share_text_pump does, all at once; a Linux app's field with text input gets it as one input method's commit */
static int hands_type(const char *t, size_t n) {
    if (!n) return 0;
    if (js_text_input_app()) {
        char *z = strndup(t, n);
        if (!z) return HANDS_WHY_NOT_EDITABLE;
        js_input_flush();
        js_commit_text(z);
        free(z);
        return 0;
    }
    int labels = kb_labels_begin();
    for (size_t off = 0; off < n;) {
        int w = hands_keys_place();
        if (w) { if (labels) kb_labels_end(); return w; }
        uint32_t cp = 0;
        int l = sw_utf8_next(t + off, n - off, &cp);
        if (l <= 0) break;
        char want[8] = "";
        memcpy(want, t + off, (size_t)l);
        int code, lv;
        char base[8], dead[16];
        const char *mark;
        int dcode, dlv;
        if (labels && share_key_of(want, &code, &lv)) hands_press(code, lv, 0);
        else if (labels && sw_dead_split(cp, base, &mark) && (size_t)snprintf(dead, sizeof dead, "dead:%s", mark) < sizeof dead &&
                 share_key_of(dead, &dcode, &dlv) && share_key_of(base, &code, &lv)) {
            hands_press(dcode, dlv, 0);
            hands_press(code, lv, 0);
        } else {
            js_input_flush();
            js_commit_text(want);           /* no key types it: an input method's finished text (dropped where none takes it) */
        }
        off += (size_t)l;
    }
    if (labels) kb_labels_end();
    return 0;
}
/* what a capture by an agent leaves out (hands/handsmark.c): the system band, handsDeny nodes, other apps' windows with
 * an allowlist, marked for the capture and put back; when they can't all be marked, there is no capture (fail closed) */
static int hands_marked[HANDS_MARK_MAX + HANDS_BLACK_MAX], hands_nmarked;
static Node *hands_node_get(int id) { return node_get(id); }
static uint8_t *hands_shot(int kind, uint32_t display, int node, double x, double y, double w, double h, int maxdim,
                           const int *black, int nblack, const int *also, int nalso, size_t *len, int *iw, int *ih, char *meta, size_t metan) {
    *len = 0;
    MediaSrc *m = NULL;
    double lx = 0, ly = 0, lw = remote_server->W, lh = remote_server->H;
    if (hands_mark_nodes(ui.root, hands_node_get, black, nblack, hands_marked, (int)(sizeof hands_marked / sizeof hands_marked[0]), &hands_nmarked)) {
        fprintf(stderr, "lord-server: Hands: a screenshot refused: not everything it must hide could be hidden\n");
        return NULL;
    }
    /* an agent's capture: the marks black, its own cursor left out, a window refused only when it is itself protected */
    paint_hands_capture = 1;
    paint_hands_cursor = handscur_root();
    if (kind != HANDS_SHOT_DISPLAY) {
        /* the part of the desktop it really shows (a window partly off the screen): what lordos-mcp maps points with */
        double W = remote_server->W, H = remote_server->H, x0 = fmax(0, floor(x)), y0 = fmax(0, floor(y));
        double x1 = fmin(W, floor(x) + floor(w)), y1 = fmin(H, floor(y) + floor(h));
        if (x1 - x0 < 1 || y1 - y0 < 1) { paint_hands_capture = 0; hands_unmark_nodes(hands_node_get, hands_marked, hands_nmarked); hands_nmarked = 0; return NULL; }
        x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    }
    if (kind == HANDS_SHOT_DISPLAY) {
        const DisplayPlan *plan = display_current();
        const DisplayOut *o = NULL;
        for (int i = 0; i < plan->n && !o; i++) if (display ? plan->o[i].conn == display : i == plan->main) o = &plan->o[i];
        if (display && !o) { paint_hands_capture = 0; hands_unmark_nodes(hands_node_get, hands_marked, hands_nmarked); hands_nmarked = 0; return NULL; }
        if (o) { lx = o->v.x; ly = o->v.y; lw = o->v.w; lh = o->v.h; }
        m = media_screenshot("hands", 0, 0, 0, 0, o ? o->conn : 0, 0, NULL, 0, NULL);
    } else if (kind == HANDS_SHOT_WINDOW) {
        lx = x; ly = y; lw = w; lh = h;
        m = media_screenshot("hands", (int)x, (int)y, (int)w, (int)h, 0, node, also, nalso < PAINT_ALSO_MAX ? nalso : PAINT_ALSO_MAX, NULL);
    } else {
        lx = x; ly = y; lw = w; lh = h;
        m = media_screenshot("hands", (int)x, (int)y, (int)w, (int)h, 0, 0, NULL, 0, NULL);
    }
    paint_hands_capture = 0;
    hands_unmark_nodes(hands_node_get, hands_marked, hands_nmarked);
    hands_nmarked = 0;
    if (!m || !m->px) { if (m) media_src_close(m); return NULL; }
    hands_fit(m->w, m->h, maxdim, kind == HANDS_SHOT_REGION, iw, ih);
    uint8_t *rgb = hands_scale_rgb(m->px, m->w, m->w, m->h, *iw, *ih);
    media_src_close(m);
    uint8_t *png = rgb ? hands_png(rgb, *iw, *ih, len) : NULL;
    free(rgb);
    snprintf(meta, metan, "{\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f,\"width\":%d,\"height\":%d}", lx, ly, lw, lh, *iw, *ih);
    return png;
}
static char *hands_shell(const char *op, const char *json) { return js_hands_call(op, json); }
static double hands_now(void) { return now_ms(); }
static int hands_clip_sensitive(void) { return term_clipboard_sensitive(); }
static void hands_cursor_ev(int what, double x, double y, double w, double h) { handscur_event(what, x, y, w, h, now_ms()); }
static int hands_pointer_at(double *x, double *y) {
    const Server *s = remote_server;
    *x = hands_borrowed ? hands_px : s->cx;
    *y = hands_borrowed ? hands_py : s->cy;
    return 1;
}
static const HandsHost hands_host_impl = {
    hands_shell, js_hands_event, hands_size, hands_pointer_locked, hands_place, hands_same_target, hands_node_place_id, hands_keys_place,
    hands_move, hands_button, hands_wheel, hands_key, hands_type, hands_shot, hands_now, hands_clip_sensitive,
    hands_cursor_ev, hands_pointer_at,
};

static void remote_greeter(const char *json) { js_greeter_event(json); }
static void remote_notice(const char *json) { js_remote_notice(json); }

/* ---- applying the display plan (Settings → Displays, hotplug; hw/display.c) ---- */

static int gpu_start(Server *s);

/* Does someone need the frame in memory, composed by the CPU? Recording the screen does. The remote
 * desktop doesn't any more (docs/REMOTE.md, "Video (WebRTC)"): with the GPU composing, the frame is
 * read back once (gpu_read_next) for every viewer. Only protected windows still need the CPU path
 * (they are painted black in a CPU copy of the scene, media/capture.c); LORD_REMOTE_GPU=0 forces it. */
static int remote_needs_cpu(void) {
    if (!mirror_watchers()) return 0;
    const char *e = getenv("LORD_REMOTE_GPU");
    return (e && !strcmp(e, "0")) || media_protected_count() > 0;
}
static int need_cpu(void) { return remote_needs_cpu() || media_needs_cpu(); }

/* The GPU composes one display, upright; several displays or a rotated one go by the CPU. */
static int gpu_allowed(const Server *s) {
    return !s->headless && out_count(s) >= 1 && !display_cpu_only() && !need_cpu();
}

/* The GPU is stopping: every display gets its dumb buffer back (CPU path). Called before gpu_shutdown(), so
 * no framebuffer it removes is still on screen. */
static void cpu_takeover(Server *s) {
    prep_undo_all(s);                           /* a frame prepared for the GPU is painted again */
    for (int i = 0; i < OUT_MAX; i++) {
        Out *o = &s->out[i];
        if (!o->on || o->gpu < 0) continue;
        kms_cursor_show(&o->kms, 0);
        kms_set_fb(&o->kms, o->kms.buf[o->kms.front].fb_id);
        o->kms.custom = 0;
        o->gpu = -1;
        o->full_copy = 2;
        o->wl_new = 0;
        if (o->gdrag_on) o->full = 1;               /* the scene's GPU drag rectangle is stale */
        o->gdrag_on = o->gdragtaken = 0;
    }
    s->hw_cursor = s->hw_cursor_ok = 0;
}

static void out_free(Out *o) {
    free(o->scene.px);
    free(o->frame);
    o->scene.px = NULL;
    o->frame = NULL;
}

/* (Re)size a display's frames for its view. */
static int out_frames(Out *o, int need_frame) {
    int w = o->v.pw, h = o->v.ph;
    if (w <= 0 || h <= 0) return -1;
    if (!o->scene.px || o->scene.w != w || o->scene.h != h) {
        uint32_t *px = calloc((size_t)w * h, 4);
        if (!px) return -1;
        free(o->scene.px);
        canvas_init(&o->scene, px, w, h, w);
        free(o->frame);
        o->frame = NULL;
    }
    if (need_frame && !o->frame) o->frame = calloc((size_t)w * h, 4);
    if (!need_frame) { free(o->frame); o->frame = NULL; }
    return need_frame && !o->frame ? -1 : 0;
}

static int same_mode(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b) {
    return a->hdisplay == b->hdisplay && a->vdisplay == b->vdisplay && a->clock == b->clock && a->htotal == b->htotal &&
           a->vtotal == b->vtotal && a->flags == b->flags;
}

/* Drop entry j from a plan (no CRTC for it, or its modeset failed) and close the arrangement again. */
static void plan_drop(DisplayPlan *p, int j) {
    memmove(&p->o[j], &p->o[j + 1], sizeof p->o[0] * (size_t)(p->n - j - 1));
    p->n--;
    if (p->main == j) p->main = 0;
    else if (p->main > j) p->main--;
    if (p->mirror || p->n <= 0) { if (p->n < 2) p->mirror = 0; return; }
    ORect r[OUT_MAX];
    for (int i = 0; i < p->n; i++) r[i] = (ORect){ p->o[i].v.x, p->o[i].v.y, p->o[i].v.w, p->o[i].v.h };
    if (p->main) { ORect t = r[0]; r[0] = r[p->main]; r[p->main] = t; }
    out_fix(r, p->n);
    if (p->main) { ORect t = r[0]; r[0] = r[p->main]; r[p->main] = t; }
    out_normalize(r, p->n, &p->bw, &p->bh);
    for (int i = 0; i < p->n; i++) { p->o[i].v.x = r[i].x; p->o[i].v.y = r[i].y; }
}

/* Between frames (no flip pending): open, move, modeset and close heads so the displays match
 * the plan. out[0] keeps the card's fd: if its display went away it takes another one. */
static void outputs_apply(Server *s) {
    paint_blur_cache_reset();            /* kept blurred pixels belong to the old displays */
    Kms *k0 = K0(s);
    if (s->headless || k0->fd < 0) return;
    if (!s->greeter) drmlease_card(k0->fd);    /* a VR headset plugged in or out: offered for lease, or its lease ends */
    KmsOutput ko[KMS_MAX_OUTPUTS];
    int n = kms_outputs(k0->fd, ko, KMS_MAX_OUTPUTS);
    DisplayPlan p;
    display_plan(ko, n, &p);
    kms_outputs_free(ko, n);
    /* nothing changed (a backlight uevent, the same choice again): keep everything as it is */
    const DisplayPlan *cur = display_current();
    int same = p.n > 0 && p.n == cur->n && p.mirror == cur->mirror && p.main == cur->main && out_count(s) == p.n;
    for (int j = 0; same && j < p.n; j++) {
        const DisplayOut *a = &p.o[j], *b = &cur->o[j];
        same = a->conn == b->conn && same_mode(&a->mode, &b->mode) && a->rot == b->rot && a->scale == b->scale &&
               !memcmp(&a->v, &b->v, sizeof a->v) && !strcmp(a->id, b->id);
    }
    if (same) { display_resync_done(1, &p); tp_set_rotation(display_touch_rotation()); return; }
    prep_undo_all(s);                           /* (outputs: views, frames and the GPU change below) */
    /* the dumb buffers go on screen before the GPU's framebuffers are removed: removing one still scanned
     * out makes the kernel try to turn its CRTC off, which amdgpu refuses ("atomic remove_fb failed") */
    int had_gpu = gpu_active();
    cpu_takeover(s);
    if (had_gpu) { gpu_tex_free(s->cursor_tex); s->cursor_tex = NULL; gpu_shutdown(); }
    kms_cursor_show(k0, 0);
    int ok = p.n > 0, head[OUT_MAX];
    for (int j = 0; j < OUT_MAX; j++) head[j] = -1;
    for (int j = 0; j < p.n; j++)
        for (int i = 0; i < OUT_MAX; i++)
            if (s->out[i].on && s->out[i].kms.conn_id == p.o[j].conn) head[j] = i;
    int has0 = 0;
    for (int j = 0; j < p.n; j++) has0 |= head[j] == 0;
    if (!has0 && p.n) {                     /* out[0]'s display is gone: it takes the main one (or another free one) */
        int j0 = head[p.main] < 0 ? p.main : -1;
        for (int j = 0; j < p.n && j0 < 0; j++) if (head[j] < 0) j0 = j;
        if (j0 >= 0) head[j0] = 0;
        else {                              /* every display already has a head: out[0] trades places */
            int i = head[p.main];
            kms_close(&s->out[i].kms);
            s->out[i].on = 0;
            out_free(&s->out[i]);
            head[p.main] = 0;
        }
    }
    /* heads whose display left the plan */
    for (int i = 1; i < OUT_MAX; i++) {
        if (!s->out[i].on) continue;
        int used = 0;
        for (int j = 0; j < p.n; j++) used |= head[j] == i;
        if (used) continue;
        fprintf(stderr, "displays: connector %u off\n", s->out[i].kms.conn_id);
        kms_close(&s->out[i].kms);
        out_free(&s->out[i]);
        memset(&s->out[i], 0, sizeof s->out[i]);
        s->out[i].kms.fd = -1;
    }
    /* modesets (out[0] first: another display may need the CRTC it frees) and new heads */
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < p.n; j++) {
            int i = head[j];
            if (pass == 0 ? i != 0 : i == 0) continue;
            if (i >= 0) {
                Kms *k = &s->out[i].kms;
                if (k->conn_id == p.o[j].conn && same_mode(&k->mode, &p.o[j].mode)) continue;
                if (kms_modeset(k, p.o[j].conn, &p.o[j].mode) == 0) { s->out[i].full_copy = 2; continue; }
                fprintf(stderr, "displays: modeset of connector %u failed\n", p.o[j].conn);
                ok = 0;
                if (i == 0) continue;       /* out[0] keeps its previous mode */
                kms_close(k);
                out_free(&s->out[i]);
                memset(&s->out[i], 0, sizeof s->out[i]);
                s->out[i].kms.fd = -1;
                head[j] = -1;
            } else {
                int slot = -1;
                for (int q = 1; q < OUT_MAX && slot < 0; q++) if (!s->out[q].on) slot = q;
                if (slot >= 0 && kms_head_open(&s->out[slot].kms, k0->fd, p.o[j].conn, &p.o[j].mode) == 0) {
                    s->out[slot].on = 1;
                    head[j] = slot;
                } else fprintf(stderr, "displays: no CRTC for connector %u (the card drives %d)\n", p.o[j].conn, out_count(s));
            }
        }
    for (int j = p.n - 1; j >= 0; j--) if (head[j] < 0) { memmove(&head[j], &head[j + 1], sizeof head[0] * (size_t)(OUT_MAX - j - 1)); plan_drop(&p, j); }
    /* out[0] that kept a mode the plan did not get: its entry follows the real mode */
    for (int j = 0; j < p.n; j++) {
        Out *o = &s->out[head[j]];
        o->on = 1;
        if (o->kms.conn_id != p.o[j].conn || !same_mode(&o->kms.mode, &p.o[j].mode)) {
            p.o[j].conn = o->kms.conn_id;
            p.o[j].mode = o->kms.mode;
        }
        o->d = p.o[j];
        o->v = p.o[j].v;
        if (out_frames(o, o->d.rot != 0)) { fprintf(stderr, "displays: out of memory for a %dx%d frame\n", o->v.pw, o->v.ph); ok = 0; }
        memset(o->stale, 0, sizeof o->stale);
        memset(o->pstale, 0, sizeof o->pstale);
        o->npainted = 0;
        memset(o->curs, 0, sizeof o->curs);
        o->full = 1;
        o->full_copy = 2;
        o->pend = (Box){ 0, 0, 0, 0 };
        o->pl.n = 0;
        if (j == p.main) s->main_out = head[j];
        fprintf(stderr, "displays: %s (%s) %dx%d at %d%%, logical %dx%d at (%d, %d)%s%s\n", o->d.connector, o->d.name, o->v.pw, o->v.ph,
                out_scale(o), o->v.w, o->v.h, o->v.x, o->v.y, j == p.main ? ", main" : "", p.mirror ? ", mirror" : "");
    }
    display_resync_done(ok, &p);
    tp_set_rotation(display_touch_rotation());   /* a touchscreen turns with its panel */
    /* the login screen opens its clock and users on this session's main display (lord-sessiond keeps
     * the connector for it, docs/DISPLAYS.md): only a change is sent */
    static char told_main[32];
    if (ok && !s->greeter && p.n > 1 && strcmp(told_main, p.o[p.main].connector)) {
        char j[96];
        snprintf(told_main, sizeof told_main, "%s", p.o[p.main].connector);
        snprintf(j, sizeof j, "{\"a\":\"display\",\"main\":\"%s\"}", told_main);
        mirror_session(j);
    }
    set_size(s, p.bw > 0 ? p.bw : (int)k0->width, p.bh > 0 ? p.bh : (int)k0->height);
    for (int i = 0; i < OUT_MAX; i++) s->out[i].gpu = -1;     /* the GPU stopped above */
    if (gpu_allowed(s) && !s->gpu_paused) gpu_start(s);
    s->cursor_dirty = 1;
}

/* ---- the monitor, lent by lord-sessiond (docs/SESSIONS.md) ----
 * fds[0] is the card (lord-sessiond already made it master), the rest keyboards and mice. The
 * session draws on the card like a plain lord-server: with GPU if there is one, direct scanout of
 * fullscreen and hardware cursor. When switching user, lord-sessiond takes the master away and
 * revokes the input (EVIOCREVOKE); before that it sends RP_UNDISPLAY. */
/* lord-sessiond: a keyboard, mouse or touchpad plugged in while the session is on the monitor */
static void remote_input_add(const int *fds, int n) {
    Server *s = remote_server;
    if (s->headless) { for (int i = 0; i < n; i++) close(fds[i]); return; }
    if (input_add_fds(&s->input, fds, n)) wl_input_devices_changed();
    keyboards_check(s);
}

/* A lent card with no monitor connected (a Raspberry Pi booted headless, a laptop lid closed on a
 * dock): the fds are kept and display_attach is tried again on the next DRM uevent, so plugging in the
 * monitor later shows the session without restarting anything. RP_UNDISPLAY drops them. */
#define PARK_MAX 40
static int parked_fd[PARK_MAX], parked_n, parked_flags;
static unsigned parked_seq;
static void parked_drop(void) {
    for (int i = 0; i < parked_n; i++) close(parked_fd[i]);
    parked_n = 0;
}

/* Boot splash hand-off: lord-sessiond lent the card with RP_DISPLAY_KEEP; RP_SHOWN goes back once
 * the first frame is on the screen (docs/SESSIONS.md, "Boot splash"). */
static int splash_wait;

static void display_detach(void);

static void display_attach(const int *fds, int n, int flags) {
    Server *s = remote_server;
    parked_drop();                      /* a new RP_DISPLAY replaces what was waiting */
    if (n < 1) return;
    /* lord-sessiond lends the monitor only after taking it back, so a session still on the screen
     * missed that: what it holds is stale (revoked input, no DRM master). Leave it and take the new one,
     * or the screen stays black. */
    if (!s->headless) {
        fprintf(stderr, "monitor: lent again while still shown; leaving the old one first\n");
        display_detach();
    }
    Out *o0 = &s->out[0];
    out_free(o0);
    memset(o0, 0, sizeof *o0);
    o0->gpu = -1;
    int card = dup(fds[0]);             /* kms_attach closes its fd when it fails */
    int keep = (flags & RP_DISPLAY_KEEP) != 0;
    uevent_set_card(uevent_card_of_fd(fds[0]));     /* only this card's drm uevents are display hotplugs (a parked one's too) */
    if (kms_attach(&o0->kms, fds[0], keep)) {
        o0->kms.fd = -1;
        if (card >= 0 && n <= PARK_MAX) {
            fprintf(stderr, "monitor: nothing connected to the card; waiting for a monitor (hotplug)\n");
            /* the boot splash waits for our first frame (RP_DISPLAY_KEEP), which can't come with nothing connected: told
             * now, so a headless boot (a Raspberry Pi with no monitor) doesn't wait for Plymouth's timeout (35 s) */
            if (flags & RP_DISPLAY_KEEP) mirror_shown();
            parked_fd[0] = card;
            for (int i = 1; i < n; i++) parked_fd[i] = fds[i];
            parked_n = n;
            parked_flags = flags;
            parked_seq = display_hotplug_seq();
        } else {
            fprintf(stderr, "monitor: could not use the card\n");
            if (card >= 0) close(card);
            for (int i = 1; i < n; i++) close(fds[i]);
        }
        set_size(s, s->W, s->H);            /* back to the frame in memory */
        return;
    }
    if (card >= 0) close(card);
    splash_wait = keep;
    if (input_open_fds(&s->input, fds + 1, n - 1)) fprintf(stderr, "monitor: no keyboard or mouse\n");
    wl_input_devices_changed();         /* is there a touch screen? (wl_seat touch) */
    s->headless = 0;
    s->gpu_paused = need_cpu();
    o0->on = 1;
    struct stat st;
    if (fstat(o0->kms.fd, &st) == 0) wl_set_drm_device(st.st_rdev);
    display_attached();                 /* Night Light, idle timers, the list in Settings; queues a resync */
    logindlock_shown();                 /* unlocked: logind's LockedHint goes back to false (hw/logindlock.c) */
    remote_typed = 0;
    keyboards_check(s);
    display_take_resync();
    outputs_apply(s);                   /* the other displays, saved modes, scales and arrangement */
    /* the pointer where it is (an absolute tablet does not report if it does not move) or in the middle of the main display */
    Out *m = main_out(s);
    s->cx = m->v.x + m->v.w / 2;
    s->cy = m->v.y + m->v.h / 2;
    for (int i = 0; i < s->input.count; i++) {
        const InputDevice *d = &s->input.dev[i];
        int rx = d->ax.maximum - d->ax.minimum, ry = d->ay.maximum - d->ay.minimum;
        if (!d->has_abs || rx <= 0 || ry <= 0) continue;
        s->cx = (int)((long)(d->ax.value - d->ax.minimum) * (s->W - 1) / rx);
        s->cy = (int)((long)(d->ay.value - d->ay.minimum) * (s->H - 1) / ry);
        break;
    }
    clamp_pointer(s);
    s->lx = s->cx;
    s->ly = s->cy;
    full_damage(s);
    fprintf(stderr, "monitor: the session is shown (%d display(s), %dx%d, %s)\n", out_count(s), s->W, s->H, gpu_active() ? "GPU" : "CPU");
}

static unsigned tab_emul;               /* DOM buttons a pen holds down as a mouse (graphics tablets, below) */
static void display_detach(void) {
    Server *s = remote_server;
    /* every DRM lease ends first, revoked while the card is still open (a lock, a user switch, a text console; under
     * lord-sessiond master is already gone and the lessee with it, docs/WAYLAND.md "DRM leases") */
    drmlease_card(-1);
    parked_drop();
    splash_wait = 0;
    sleeplock_monitor_taken();          /* a lock before sleep: the computer may sleep now (hw/sleeplock.c) */
    display_left();
    if (s->headless) return;
    prep_undo_all(s);                           /* a prepared frame is dropped with its display */
    gpu_tex_free(s->cursor_tex);
    s->cursor_tex = NULL;
    gpu_shutdown();
    for (int i = 0; i < OUT_MAX; i++) s->out[i].gpu = -1;
    input_close(&s->input);
    tab_emul = 0;                               /* a pen pressing as a mouse: let go with its device */
    kb_reset_state();                           /* a key held now (the ❖ of ❖L) is released where we can't see it */
    s->ctrl = s->alt = s->shift = s->meta = 0;  /* Ctrl and Alt of Ctrl+Alt+F2 are released on the text console */
    ptrlock_end(PTRLOCK_END_INPUT);             /* a pointer lock goes with the input (pointerlock.h) */
    for (int i = OUT_MAX - 1; i >= 0; i--) {        /* the second displays first: out[0] owns the fd */
        Out *o = &s->out[i];
        if (!o->on && i) continue;
        kms_close(&o->kms);
        out_free(o);
        memset(o, 0, sizeof *o);
        o->kms.fd = -1;
        o->gpu = -1;
    }
    s->hw_cursor = s->hw_cursor_ok = 0;
    s->gpu_paused = 0;
    s->headless = 1;
    display_detached();
    js_input_drop();
    set_size(s, s->W & ~1, s->H & ~1);  /* a frame in memory of the same size */
    fprintf(stderr, "monitor: the session leaves the screen\n");
}

/* Text consoles (Ctrl+Alt+F1…F12, docs/SESSIONS.md "Text consoles"). Under lord-sessiond the session only asks
 * ({"a":"vt"}): lord-sessiond locks it, takes the monitor back and switches. A standalone lord-server (startlordos
 * from a text console, as startx; or a developer's) owns its console in VT_PROCESS mode: the kernel asks before
 * switching away (SIGUSR1, read from vt_sfd), and it lets the card and the input go; back on its console (SIGUSR2) it
 * opens them again and draws everything. */
static int vt_tty = -1, vt_sfd = -1, vt_away;
static char vt_card[64];
static void vt_request(int n) {
    if (vt_tty < 0) {
        char j[48];
        snprintf(j, sizeof j, "{\"a\":\"vt\",\"n\":%d}", n);
        mirror_session(j);
        return;
    }
    struct vt_stat st;
    if (vt_away || (!ioctl(vt_tty, VT_GETSTATE, &st) && st.v_active == n)) return;
    if (ioctl(vt_tty, VT_ACTIVATE, n)) fprintf(stderr, "lord-server: text console %d: %s\n", n, strerror(errno));
}
static void display_attach(const int *fds, int n, int flags);
static void display_detach(void);
static void vt_signal(Server *s) {
    struct signalfd_siginfo si;
    while (read(vt_sfd, &si, sizeof si) == (ssize_t)sizeof si) {
        if (si.ssi_signo == SIGUSR1 && !vt_away) {
            fprintf(stderr, "lord-server: leaving for another text console\n");
            display_detach();               /* the card's master and the input go with their fds */
            ioctl(vt_tty, KDSETMODE, KD_TEXT);
            ioctl(vt_tty, VT_RELDISP, 1);
            vt_away = 1;
        } else if (si.ssi_signo == SIGUSR2 && vt_away) {
            ioctl(vt_tty, VT_RELDISP, VT_ACKACQ);
            ioctl(vt_tty, KDSETMODE, KD_GRAPHICS);
            vt_away = 0;
            int fd = open(vt_card, O_RDWR | O_CLOEXEC);   /* nobody is master now: this open is */
            if (fd < 0) { fprintf(stderr, "lord-server: back on the console, but %s: %s\n", vt_card, strerror(errno)); continue; }
            display_attach(&fd, 1, 0);
            if (input_open(&s->input)) fprintf(stderr, "input: no devices (permissions?)\n");
            wl_input_devices_changed();
            fprintf(stderr, "lord-server: back on its text console\n");
        }
    }
}

/* Ctrl and Alt as the kernel has them on one keyboard right now (EVIOCGKEY); 0 when it can't be asked */
/* the community themes' watchdog (community_watch, below): Ctrl+Alt+Shift+Backspace sets rescue */
static struct { double since; int healthy; volatile int rescue; char prev[1024]; } cw;

static int zap_held(int fd, int *ctrl, int *alt) {
    unsigned char keys[KEY_MAX / 8 + 1];
    memset(keys, 0, sizeof keys);
    if (fd < 0 || ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) return 0;
#define HELD(k) ((keys[(k) / 8] >> ((k) % 8)) & 1)
    *ctrl = HELD(KEY_LEFTCTRL) || HELD(KEY_RIGHTCTRL);
    *alt = HELD(KEY_LEFTALT) || HELD(KEY_RIGHTALT);
#undef HELD
    return 1;
}

static void tablet_input(Server *s, const InputDevice *d, const struct input_event *ev);
static void on_input(void *ud, const InputDevice *d, const struct input_event *ev) {
    Server *s = ud;
    if (ev->type == EV_KEY || ev->type == EV_REL || ev->type == EV_ABS) {
        display_input(now_ms());
        hands_person_input();               /* the person's own input wins: an agent waits (hands.h) */
        if (ev->type == EV_KEY && ev->value == 1) hands_person_press();   /* a real click or key: theirs again (not motion) */
        /* their pointer, back where they left it (after hands_person_input: an agent's drag let go where it was) */
        if (ev->type != EV_KEY || ev->code >= BTN_MISC) hands_reclaim(s);
    }
    if (s->debug && ev->type != EV_SYN)
        fprintf(stderr, "ev %s type=%u code=%u value=%d\n", d->name, ev->type, ev->code, ev->value);
    if (d->tab) { tablet_input(s, d, ev); return; }     /* a graphics tablet's pen or pad (tablet.c) */
    if (d->tp) {                        /* touchpad o pantalla táctil: driver propio (tp/tp.c) */
        tp_event(d->tp, ev);
        if (ev->type != EV_SYN || ev->code != SYN_REPORT) return;
    }
    switch (ev->type) {
    case EV_REL:
        if (ev->code == REL_X || ev->code == REL_Y) {
            /* Settings → Mouse & Trackpad: 0.4 to 2 times what the mouse sends, the fractions kept */
            static double part[2];
            double k = tp_config()->mouse_speed, f = k <= 0.5 ? 0.4 + 1.2 * k : 1 + 2 * (k - 0.5);
            int y = ev->code == REL_Y;
            double v = ev->value * f + part[y];
            int n = (int)v;
            part[y] = v - n;
            if (y) { s->cy += n; s->rdy += n; } else { s->cx += n; s->rdx += n; }
            s->moved = 1;
        }
        /* ruedas: la de alta resolución y la horizontal van como scroll fino (tp/tp_route.c, value120);
         * los pasos enteros de siempre, solo si el mouse no tiene la de alta resolución */
        /* a pointer lock with a wheel sink: whole steps straight to it (pointerlock.h) */
        if (ptrlock_wants_wheel()) {
            if (ev->code == REL_WHEEL) ptrlock_wheel(tp_config()->mouse_natural ? -ev->value : ev->value, 0);
            if (ev->code == REL_HWHEEL) ptrlock_wheel(0, ev->value);
            break;
        }
        if (ev->code == REL_WHEEL && !d->wheel_hires) s->wheel += tp_config()->mouse_natural ? -ev->value : ev->value;
        if (ev->code == REL_WHEEL_HI_RES) tp_route_wheel120(tp_config()->mouse_natural ? -ev->value : ev->value, 0);
        if (ev->code == REL_HWHEEL_HI_RES) tp_route_wheel120(0, ev->value);
        if (ev->code == REL_HWHEEL && !d->hwheel_hires) tp_route_wheel120(0, ev->value * 120);
        break;
    case EV_ABS: {
        const struct input_absinfo *a = ev->code == ABS_X ? &d->ax : ev->code == ABS_Y ? &d->ay : NULL;
        int range = a ? a->maximum - a->minimum : 0;
        if (range <= 0) break;
        if (out_count(s) == 1 && s->out[0].d.rot) {
            /* rotated screen: the tablet reports physical positions; both axes can change */
            static double nx, ny;
            if (ev->code == ABS_X) nx = (double)(ev->value - a->minimum) / range; else ny = (double)(ev->value - a->minimum) / range;
            double lx, ly;
            display_unrotate_point(s->out[0].d.rot, nx, ny, &lx, &ly);
            int x = (int)(lx * (s->W - 1)), y = (int)(ly * (s->H - 1));
            s->rdx += x - s->lx; s->rdy += y - s->ly;
            s->cx = x; s->cy = y;
            s->moved = 1;
            break;
        }
        int v = (int)((long)(ev->value - a->minimum) * (ev->code == ABS_X ? s->W - 1 : s->H - 1) / range);
        if (ev->code == ABS_X) { s->rdx += v - s->lx; s->cx = v; } else { s->rdy += v - s->ly; s->cy = v; }
        s->moved = 1;
        break;
    }
    case EV_KEY: {
        if (ev->value == 1 && ev->code < BTN_MISC) tp_key_activity(ev->code, now_ms());   /* ignorar el touchpad al escribir */
        if (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL) s->ctrl = ev->value != 0;
        if (ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT) s->alt = ev->value != 0;
        if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) s->shift = ev->value != 0;
        if (ev->code == KEY_LEFTMETA || ev->code == KEY_RIGHTMETA) s->meta = ev->value != 0;
        if (ev->code == KEY_LEFTMETA || ev->code == KEY_RIGHTMETA || ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT) mods_injected = 0;
        /* Ctrl+Alt+Backspace ends the session at once. The modifiers are tracked from every keyboard and a release can
         * be lost (another console, a keyboard that sleeps or reconnects), so the kernel is asked whether Ctrl and Alt
         * are really held on this keyboard: a stale state must never end a session on a lone Backspace. */
        /* Ctrl+Alt+Shift+Backspace: back to Flow, whatever theme runs (a community theme can't take this key: it is
         * handled here, before any JavaScript; community_watch does the switch) */
        if (ev->code == KEY_BACKSPACE && ev->value == 1 && s->ctrl && s->alt && s->shift && !s->greeter) {
            int ctrl = 0, alt = 0;
            if (zap_held(d->fd, &ctrl, &alt) && ctrl && alt) {
                fprintf(stderr, "lord-server: Ctrl+Alt+Shift+Backspace on %s: restoring Flow\n", d->name);
                cw.rescue = 1;
            }
            break;
        }
        if (ev->code == KEY_BACKSPACE && ev->value == 1 && s->ctrl && s->alt) {
            int ctrl = 0, alt = 0;
            if (zap_held(d->fd, &ctrl, &alt) && ctrl && alt) {
                fprintf(stderr, "lord-server: Ctrl+Alt+Backspace on %s: ending the session\n", d->name);
                s->quit = 1;
            } else {
                fprintf(stderr, "lord-server: Backspace on %s with Ctrl and Alt noted but not held: the session goes on\n", d->name);
                s->ctrl = ctrl; s->alt = alt;
            }
        }
        /* Hands' panic key, Ctrl+Alt+Esc: every agent stopped and Hands off for the session (hands.h), here on the
         * physical keyboard, before any JavaScript; nothing an agent injects passes through on_input */
        if (ev->code == KEY_ESC && ev->value == 1 && s->ctrl && s->alt && !s->shift) {
            int ctrl = 0, alt = 0;
            if (zap_held(d->fd, &ctrl, &alt) && ctrl && alt) {
                fprintf(stderr, "lord-server: Ctrl+Alt+Esc on %s: every AI agent stopped (Hands)\n", d->name);
                hands_panic();
                break;
            }
        }
        /* ends a share with no JavaScript in the way; only here, on the physical keyboard: what the API or
         * the remote desktop injects doesn't pass through on_input (docs/SHORTCUTS.md) */
        if (ev->code == KEY_ESC && ev->value == 1 && s->ctrl && s->alt && s->shift && share_user_stop()) break;
        /* Ctrl+Alt+F1…F12: a text console (taken here, so no app or theme gets the key) */
        if (ev->value == 1 && s->ctrl && s->alt && !s->shift && input_vt_key(ev->code)) { vt_request(input_vt_key(ev->code)); break; }
        /* a pointer lock (Machines' "Capture mouse") ends with ❖Esc or Ctrl+Alt, before any app sees the key (pointerlock.h) */
        if (ev->code < BTN_MISC && ptrlock_key(ev->code, ev->value, s->meta, s->ctrl, s->alt)) break;
        /* Buttons in the DOM's numbering: 0 left, 1 middle, 2 right, 3 back, 4 forward (inputkind.h, issue #159) */
        int button = input_dom_button(ev->code);
        /* Settings → Mouse & Trackpad, primary button on the right: a mouse's (not a touchpad's) */
        if ((button == 0 || button == 2) && !d->tp && d->fd >= 0 && tp_config()->mouse_left_handed) button = 2 - button;
        /* a pointer lock with a button sink takes them straight, with no hit test (pointerlock.h) */
        if (button >= 0 && ev->value != 2 && ptrlock_button(button, ev->value == 1)) break;
        if (button >= 0 && ev->value != 2 && s->nbuttons < MAX_BUTTONS)
            s->buttons[s->nbuttons++] = (ButtonEvent){ ev->value == 1, button };
        else if (ev->code < BTN_MISC) {
            if (pkagent_key(ev->code, ev->value)) break;   /* before JavaScript, input methods and apps */
            if (keylag_enabled) keylag_rx(ev->value, &ev->time);
            js_key(ev->code, ev->value);
            if (keylag_enabled) keylag_handled();
        }
        break;
    }
    case EV_SYN:
        if (s->moved && ptrlock_active()) {
            /* LordOS's own pointer lock (pointerlock.h): the motion goes to its owner and the pointer stays where it is */
            ptrlock_motion(s->rdx, s->rdy);
            s->rdx = s->rdy = 0;
            s->cx = s->lx; s->cy = s->ly;
            s->moved = 0;
        } else if (s->moved) {
            /* juegos: movimiento relativo crudo; con el puntero bloqueado el cursor no se mueve */
            wl_pointer_relative(s->rdx, s->rdy);
            s->rdx = s->rdy = 0;
            if (wl_pointer_locked()) { s->cx = s->lx; s->cy = s->ly; }
        }
        clamp_pointer(s);                   /* inside the displays (docs/DISPLAYS.md) */
        wl_pointer_confine(&s->cx, &s->cy);
        s->lx = s->cx;
        s->ly = s->cy;
        if (s->moved) {
            if (s->hw_cursor) hw_cursor_move(s);
            else s->cursor_dirty = 1;
        }
        /* el movimiento solo se junta: se manda una vez por cuadro (pointer_flush, al terminar el
         * page flip); los botones y la rueda, enseguida (con el movimiento que haya hasta acá) */
        if (s->nbuttons || s->wheel) {
            dispatch_pointer(s);
            if (gpu_active()) update_cursor(s);     /* la app pudo pedir otro cursor (u ocultarlo) */
        } else if (s->moved) s->motion_pending = 1;
        if (d->tp) tp_latency(d->tp, ev);       /* touchpad: latencia por gesto en el registro */
        tp_route_wheel_flush();                 /* rueda de alta resolución u horizontal */
        break;
    }
}

/* ---- graphics tablets (tablet.c; docs/WAYLAND.md, "Graphics tablets") ----
 * A pen goes to the app under it by tablet-v2 when that app takes it (wl/wl_tablet.c): the pointer image follows and
 * no pointer event is sent, and the theme still sees the tip's presses (to raise and focus the window). Anywhere
 * else it is a mouse: the tip is the left button, BTN_STYLUS the right, BTN_STYLUS2 the middle, BTN_STYLUS3 back.
 * A press as a mouse stays one until released (a stroke never moves into an app half-way), and so does one an app had
 * while a window move it began (xdg_toplevel.move) runs. Where on screen: a pen display (INPUT_PROP_DIRECT) covers the
 * main display, a tablet every display (as the mouse sees them), both turned with a rotated display. */
static void tablet_place(Server *s, const TabCaps *c, double nx, double ny, double *x, double *y) {
    Out *m = c->direct ? main_out(s) : out_count(s) == 1 ? &s->out[0] : NULL;
    if (m && m->d.rot) {
        double lx, ly;
        display_unrotate_point(m->d.rot, nx, ny, &lx, &ly);
        nx = lx; ny = ly;
    }
    if (c->direct) { *x = m->v.x + nx * (m->v.w - 1); *y = m->v.y + ny * (m->v.h - 1); }
    else { *x = nx * (s->W - 1); *y = ny * (s->H - 1); }
}
static void tablet_mouse_button(Server *s, int button, int down) {
    if (button < 0 || ((tab_emul >> button) & 1) == (unsigned)(down != 0)) return;
    if (down) tab_emul |= 1u << button; else tab_emul &= ~(1u << button);
    if (ptrlock_button(button, down)) return;
    if (s->nbuttons < MAX_BUTTONS) s->buttons[s->nbuttons++] = (ButtonEvent){ down, button };
}
static void tablet_pen(Server *s, const InputDevice *d, const TabFrame *f) {
    static const InputDevice none = { .fd = -1 };
    double x, y;
    tablet_place(s, tablet_caps(d->tab), f->x, f->y, &x, &y);
    int ix = (int)x, iy = (int)y;
    if (!tab_emul && !grab_active() && !ptrlock_active() && !wl_pointer_locked() && !wl__dragging()) {
        if (wl_tablet_tool(d->tab, f, (float)x, (float)y)) {
            s->cx = ix; s->cy = iy;
            clamp_pointer(s);
            s->lx = s->cx; s->ly = s->cy;
            if (s->hw_cursor) hw_cursor_move(s);
            s->cursor_dirty = 1;                /* the app's cursor for the tool, or LordOS's */
            s->over_app = 1;
            if (f->flags & (TF_DOWN | TF_UP)) {
                Node *hit = hit_test(ui.root, (float)s->cx, (float)s->cy);
                js_pointer(f->flags & TF_DOWN ? "down" : "up", hit ? hit->id : 0, (float)s->cx, (float)s->cy, 0);
            }
            return;
        }
    } else wl_tablet_tool_away(d->tab, f);
    if (f->flags & (TF_MOTION | TF_PROX_IN)) {
        s->rdx += ix - s->lx; s->rdy += iy - s->ly;
        s->cx = ix; s->cy = iy;
        s->moved = 1;
    }
    /* a press an app had, now a window move's (or a lock's): the left button stays down until the tip comes up */
    if (f->down && !(f->flags & TF_DOWN) && !(tab_emul & 1)) { tab_emul |= 1; s->held |= 1; }
    if (f->flags & (TF_DOWN | TF_UP)) tablet_mouse_button(s, 0, f->down);
    for (int i = 0; i < f->nbtn; i++) {
        unsigned c = f->btn[i].code;
        tablet_mouse_button(s, c == BTN_STYLUS ? 2 : c == BTN_STYLUS2 ? 1 : c == BTN_STYLUS3 ? 3 : -1, f->btn[i].down);
    }
    struct input_event syn = { .type = EV_SYN, .code = SYN_REPORT };
    on_input(s, &none, &syn);                   /* as a mouse's report */
}
static void tablet_input(Server *s, const InputDevice *d, const struct input_event *ev) {
    TabFrame f[2];
    TabPadFrame p;
    int n = tablet_event(d->tab, ev, f, &p);
    if (tablet_caps(d->tab)->kind == TAB_KIND_PAD) { if (n) wl_tablet_pad(d->tab, &p); return; }
    for (int i = 0; i < n; i++) tablet_pen(s, d, &f[i]);
}
/* A pen unplugged near the tablet: it leaves, and lets go of what it pressed (as an app's tool or as a mouse). */
static void tablet_gone(Server *s, const InputDevice *d) {
    TabFrame f;
    if (d->tab && tablet_caps(d->tab)->kind == TAB_KIND_PEN && tablet_leave(d->tab, &f)) tablet_pen(s, d, &f);
}

#ifdef LORD_TEST_HOOKS
/* the UI's repainted pixels, summed (render_ui) and taken by the test hooks' "painted" */
static long long test_painted_px;
static long long test_painted(void) { long long n = test_painted_px; test_painted_px = 0; return n; }
static void test_where(int *x, int *y) { cursor_at(remote_server, x, y); }
#endif

#ifdef LORD_TEST_HOOKS
/* build/lord-server-test only (-DLORD_TEST_HOOKS, testhooks.h; never in the installed lord-server): the conformance tool's fake graphics tablet, a pen and a pad made
 * with tablet.c's own tablet_new and fed evdev events through tablet_input, as a real device's are. */
static InputDevice test_tab[2] = { { .fd = -1 }, { .fd = -1 } };
static int test_tablet(int add) {
    for (int i = 0; i < 2; i++) {
        InputDevice *d = &test_tab[i];
        if (!add) {
            if (d->tab) { tablet_gone(remote_server, d); tablet_free(d->tab); d->tab = NULL; }
            continue;
        }
        if (d->tab) continue;
        TabCaps c = { 0 };
        c.kind = i ? TAB_KIND_PAD : TAB_KIND_PEN;
        snprintf(c.name, sizeof c.name, "LordOS test tablet %s", i ? "Pad" : "Pen");
        snprintf(c.phys, sizeof c.phys, "lordos-test-tablet");
        snprintf(c.path, sizeof c.path, "/dev/input/lordos-test-%s", i ? "pad" : "pen");     /* no such node: a name only */
        c.bus = BUS_USB;
        c.vendor = 0x056a;
        c.product = 0x0001;
        if (!i) {
            c.x = (struct input_absinfo){ .minimum = 0, .maximum = 10000, .resolution = 100 };
            c.y = (struct input_absinfo){ .minimum = 0, .maximum = 10000, .resolution = 100 };
            c.pressure = (struct input_absinfo){ .minimum = 0, .maximum = 1023 };
            c.distance = (struct input_absinfo){ .minimum = 0, .maximum = 63 };
            c.tilt_x = c.tilt_y = (struct input_absinfo){ .minimum = -64, .maximum = 63 };
            c.rotation = (struct input_absinfo){ .minimum = 0, .maximum = 359 };
            c.slider = (struct input_absinfo){ .minimum = -100, .maximum = 100 };
            c.caps = TAB_CAP_PRESSURE | TAB_CAP_DISTANCE | TAB_CAP_TILT | TAB_CAP_ROTATION | TAB_CAP_SLIDER;
            c.tools = TAB_TOOL_BIT(BTN_TOOL_PEN) | TAB_TOOL_BIT(BTN_TOOL_RUBBER) | TAB_TOOL_BIT(BTN_TOOL_BRUSH) |
                      TAB_TOOL_BIT(BTN_TOOL_PENCIL) | TAB_TOOL_BIT(BTN_TOOL_AIRBRUSH) | TAB_TOOL_BIT(BTN_TOOL_MOUSE);
            c.has_touch = 1;
            c.wheel = 1;                                    /* the mouse tool's wheel (REL_WHEEL) */
        } else {
            c.nbuttons = 4;
            for (int b = 0; b < 4; b++) c.button[b] = (unsigned short)(BTN_0 + b);
            c.nrings = 1; c.ring_code[0] = ABS_WHEEL; c.ring[0] = (struct input_absinfo){ .minimum = 0, .maximum = 71 };
            c.nstrips = 1; c.strip_code[0] = ABS_RX; c.strip[0] = (struct input_absinfo){ .minimum = 0, .maximum = 4096 };
            c.ndials = 1; c.dial_code[0] = REL_WHEEL;
        }
        d->tab = tablet_new(&c);
        if (!d->tab) return -1;
        tablet_announce(d->tab);
    }
    return 0;
}
static int test_tablet_event(int pad, unsigned type, unsigned code, int value) {
    InputDevice *d = &test_tab[pad ? 1 : 0];
    if (!d->tab) return -1;
    struct input_event ev = { .type = (unsigned short)type, .code = (unsigned short)code, .value = value };
    tablet_input(remote_server, d, &ev);
    return 0;
}
static void remote_pointer(int fx, int fy);
static void remote_button(int button, int down);
static void remote_wheel(int steps);
static void test_pointer(int x, int y) { remote_pointer(x, y); }
/* a modifier key only (testhooks.c checks the code), as the remote desktop types it: the window drag modifier's test */
static void test_key(int code, int value) { remote_key(code, value); }
/* "frame W [N]" (testhooks.h): the headless display's picture or one of the capture paths', as a PPM in the runtime folder */
static int test_frame_write(const char *which, const uint32_t *px, int w, int h, int stride) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    if (!rt || !px || w < 1 || h < 1 || snprintf(path, sizeof path, "%s/lordos-test-frame-%s.ppm", rt, which) >= (int)sizeof path) return -1;
    FILE *f = fopen(path, "wbe");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * stride + x];
            unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
            fwrite(rgb, 1, 3, f);
        }
    return fclose(f) ? -1 : 0;
}
static int test_frame(const char *which, long node) {
    Server *s = remote_server;
    if (!s || !s->headless) return -1;
    Out *o = &s->out[0];
    if (!strcmp(which, "output")) return o->scene.px ? test_frame_write(which, o->scene.px, o->scene.w, o->scene.h, o->scene.stride) : -1;
    if (!strcmp(which, "mirror")) {
        if (!o->scene.px) return -1;
        Canvas out = o->scene;
        const Canvas *seen = media_composed(&out, 1, NULL);
        int r = test_frame_write(which, seen->px, seen->w, seen->h, seen->stride);
        media_composed(&out, mirror_watchers() > 0, NULL);      /* back as the session had it */
        return r;
    }
    if (!strcmp(which, "screenshot")) {
        MediaSrc *m = media_screenshot("test", 0, 0, 0, 0, 0, 0, NULL, 0, NULL);
        int r = m ? test_frame_write(which, m->px, m->w, m->h, m->w) : -1;
        if (m) media_src_close(m);
        return r;
    }
    CastSource src = { 0 };
    if (!strcmp(which, "portal")) src.kind = CAST_SRC_MONITOR;
    else {
        const Node *n = node > 0 && node < (1L << 30) ? node_get((int)node) : NULL;
        if (!n) return -1;
        src.kind = CAST_SRC_WINDOW;
        src.node = (int)node;
        src.serial = n->serial;
    }
    int w = 0, h = 0;
    uint32_t *px = cast_still(&src, NULL, &w, &h);
    int r = px ? test_frame_write(which, px, w, h, w) : -1;
    free(px);
    return r;
}
#endif

/* pointer-warp-v1 (wl/wl_warp.c): the app in front moves the pointer on its window. No relative motion: a game sees
 * a jump, not a move. */
static int warp_move(float x, float y) {
    Server *s = remote_server;
    if (ptrlock_active() || grab_active() || tab_emul) return 0;
    s->cx = (int)(x + 0.5f);
    s->cy = (int)(y + 0.5f);
    clamp_pointer(s);
    wl_pointer_confine(&s->cx, &s->cy);
    s->lx = s->cx; s->ly = s->cy;
    s->moved = 1;
    s->motion_pending = 1;                      /* the app's motion goes with the next frame (dispatch_pointer) */
    if (s->hw_cursor) hw_cursor_move(s); else s->cursor_dirty = 1;
    return 1;
}

/* ---- touchpad y pantalla táctil (tp/): mueven el puntero de siempre y pasan por dispatch_pointer ---- */

static void tp_move(double dx, double dy, double rx, double ry) {
    static double fx, fy;               /* fracciones de píxel que quedan para el próximo */
    Server *s = remote_server;
    fx += dx;
    fy += dy;
    int ix = (int)fx, iy = (int)fy;
    fx -= ix;
    fy -= iy;
    s->cx += ix;
    s->cy += iy;
    s->rdx += rx;
    s->rdy += ry;
    s->moved = 1;
}
static void tp_warp(double x, double y) {
    Server *s = remote_server;
    int ix = (int)(x + 0.5), iy = (int)(y + 0.5);
    s->rdx += ix - s->cx;
    s->rdy += iy - s->cy;
    s->cx = ix;
    s->cy = iy;
    s->moved = 1;
}
static void tp_button(int button, int down) {
    Server *s = remote_server;
    if (ptrlock_button(button, down)) return;
    if (s->nbuttons < MAX_BUTTONS) s->buttons[s->nbuttons++] = (ButtonEvent){ down, button };
}
static void tp_position(double *x, double *y, int *w, int *h) {
    Server *s = remote_server;
    *x = s->cx;
    *y = s->cy;
    *w = s->W;
    *h = s->H;
    tp_set_screen(s->W, s->H);
}
/* salida de los temporizadores del driver (tocar, inercia): como un SYN de un dispositivo */
static void tp_flush(void) {
    static const InputDevice none = { .fd = -1 };
    struct input_event syn = { .type = EV_SYN, .code = SYN_REPORT };
    on_input(remote_server, &none, &syn);
}
static const TpHost tp_host = { tp_move, tp_warp, tp_button, tp_position, tp_flush };

/* escritorio remoto: scroll fino del trackpad del navegador y pellizco (ctrl+rueda) */
static void remote_scroll(int phase, double dx, double dy) { tp_route_scroll(phase, dx, dy, TPR_FINGER); }
static void remote_pinch(int phase, double scale, double rotation) { tp_route_pinch(phase, 2, 0, 0, scale, rotation); }

/* Layout and damage of the UI (once per frame, logical px). The damage goes to every display; each
 * one paints it when its flip is not pending (paint_out), at its own scale. */
/* render_frame: paint_ui skips the JS frame callbacks (hold); they ran in this render (ran) */
static int ui_js_hold, ui_js_ran;
static Box paint_ui(Server *s) {
    Box d = { 0, 0, 0, 0 };
    if (!ui_js_hold && js_wants_frame()) { js_frame(now_ms()); ui_js_ran = 1; }
    if (!ui.dirty) return d;
    ui.dirty = 0;
    double t0 = now_ms();
    layout_root(ui.root, (float)s->W, (float)s->H);
    double t1 = now_ms();
    if (paint_damage(ui.root)) d = ui.damage;
    ui.damage = (Box){ 0, 0, 0, 0 };
    if (!box_empty(d)) cast_damage(d.x0, d.y0, d.x1, d.y1);   /* a shared screen or window (cast/cast.c): nothing if none */
    /* the window being dragged: its layer and what is below belong to the display with most of it */
    int ln = paint_layer_node();
    Node *lnode = ln ? node_get(ln) : NULL;
    if (lnode && !s->headless) {
        ORect r[OUT_MAX];
        int idx[OUT_MAX], n = out_rects(s, r, idx);
        int b = out_best(r, n, lnode->prev_box.x0, lnode->prev_box.y0, lnode->prev_box.x1, lnode->prev_box.y1);
        paint_layer_view(b >= 0 ? idx[b] : 0);
    }
    if (!box_empty(d)) {
        Box rs[DAMAGE_MAX];
        int nr = paint_damage_rects(rs, DAMAGE_MAX);
#ifdef LORD_TEST_HOOKS
        for (int k = 0; k < nr; k++) test_painted_px += (long long)((rs[k].x1 - rs[k].x0) * (rs[k].y1 - rs[k].y0));
#endif
        for (int i = 0; i <= OUT_MAX; i++) {
            Out *o = i < OUT_MAX ? &s->out[i] : &s->cap;
            if (i < OUT_MAX && !o->on) continue;
            if (box_empty(o->pend)) { o->pl.n = 0; o->pframes = 0; }
            for (int k = 0; k < nr; k++) dlist_add(&o->pl, rs[k]);
            o->pend = box_join(o->pend, d);
            o->pframes++;
        }
    }
    ui.layout_ms = t1 - t0;
    js_stall_note("layout", "tree", ui.layout_ms);
    s->layout_total += ui.layout_ms;
    s->renders++;
    return d;
}

/* LORD_PAINT_VERIFY=N (checking the partial repaints, docs/LAYERS.md): every N-th frame painted on a
 * display, the whole tree is painted again into a scratch canvas with nothing kept from earlier frames
 * (paint_root_full: no drag layer, no copy of what is below it, no skipped node) and the pixels that
 * differ from the display's frame are counted on stderr. Off by default; it only reads the frame. The
 * terminals and Linux apps note where they were painted, so with N = 1 a terminal's change during a
 * drag always repaints the whole layer: use N > 1 to check the layer's partial repaints. On a frame
 * whose dragged window the GPU composites (GPU drag, skip) the scene is stale there and skip is left out:
 * LORD_GPU_VERIFY checks those pixels against the CPU's. */
static void paint_verify(Out *o, int index, Rect skip) {
    static int every = -1;
    static long frames[OUT_MAX];
    static uint32_t *px;
    static size_t cap;
    if (every < 0) {
        const char *e = getenv("LORD_PAINT_VERIFY");
        every = e ? atoi(e) : 0;
        if (every < 0) every = 0;
    }
    if (!every || index < 0 || index >= OUT_MAX || ++frames[index] % every) return;
    size_t n = (size_t)o->scene.w * o->scene.h;
    if (n > cap) {
        free(px);
        px = malloc(n * 4);
        cap = px ? n : 0;
        if (!px) return;
    }
    Canvas c;
    canvas_init(&c, px, o->scene.w, o->scene.h, o->scene.w);
    canvas_clear(&c, 0x000000);
    view_of(o, index);
    wl_paint_save();
    paint_root_full(&c, ui.root);
    wl_paint_restore();
    view_reset();
    long diff = 0;
    int worst = 0;
    for (int y = 0; y < o->scene.h; y++) {
        const uint32_t *a = o->scene.px + (size_t)y * o->scene.stride, *b = px + (size_t)y * o->scene.w;
        for (int x = 0; x < o->scene.w; x++) {
            if (a[x] == b[x] || (x >= skip.x0 && x < skip.x1 && y >= skip.y0 && y < skip.y1)) continue;
            diff++;
            for (int sh = 0; sh < 32; sh += 8) {
                int d = abs((int)((a[x] >> sh) & 255) - (int)((b[x] >> sh) & 255));
                if (d > worst) worst = d;
            }
        }
    }
    fprintf(stderr, "paint-verify: display %d frame %ld: %ld pixels differ from a full repaint (worst %d)%s%s\n",
            index, frames[index], diff, worst, paint_layer_node() ? ", drag layer" : "",
            skip.x1 > skip.x0 ? ", the GPU's window rectangle left out" : "");
}

/* Paints a display's pending damage into its frame at its scale. Returns the physical rect painted. */
static Rect paint_out(Server *s, Out *o, int index, int feed) {
    Rect r = { 0, 0, 0, 0 };
    Box b = o->pend;
    o->npainted = 0;
    if (o->full) b = (Box){ (float)o->v.x, (float)o->v.y, (float)(o->v.x + o->v.w), (float)(o->v.y + o->v.h) };
    int whole = o->full;
    o->full = 0;
    o->pend = (Box){ 0, 0, 0, 0 };
    o->gdragtaken = 0;
    if (box_empty(b) || !o->scene.px) return r;
    int joined = o->pframes > 1;
    /* GPU drag: the rectangle the GPU composed last frame is stale in the scene: painted again with this damage
     * (another frame's rectangle joined: the blur rule is settled again below) */
    if (o->gdrag_on && !whole) {
        float gx0, gy0, gx1, gy1;
        out_logical_box(&o->v, o->gdragw, &gx0, &gy0, &gx1, &gy1);
        b = box_join(b, (Box){ gx0, gy0, gx1, gy1 });
        joined = 1;
    }
    /* several damaged rectangles (paint.c, "Several damaged rectangles"): each one painted on its
     * own; r, what is presented, is the box around them (the scene keeps the pixels in between) */
    Box lb = o->pl.n ? o->pl.r[0] : (Box){ 0, 0, 0, 0 };
    for (int k = 1; k < o->pl.n; k++) lb = box_join(lb, o->pl.r[k]);
    int multi = !whole && o->pl.n > 1 && lb.x0 == b.x0 && lb.y0 == b.y0 && lb.x1 == b.x1 && lb.y1 == b.y1;
    DamageList l = { .n = 0 };
    if (multi) l = o->pl;
    else if (!whole) dlist_add(&l, b);
    /* several frames' damage (a pending flip): joined rectangles, or an older frame's after a
     * blurred node moved, may cut through a blurred node (paint_damage_settle) */
    if (!whole && joined) paint_damage_settle(&l);
    if (!whole && l.n) {
        b = l.r[0];
        for (int k = 1; k < l.n; k++) b = box_join(b, l.r[k]);
    }
    multi = !whole && l.n > 1;
    int nparts = multi ? l.n : 1;
    /* GPU drag (paint_root_gdrag): one box on a GPU display while a window is dragged */
    int gdrag = !whole && nparts == 1 && index < OUT_MAX && o->gpu >= 0 && gpu_active() && !(o->d.rot & 3) &&
             !wl_fullscreen_active() && !o->gdragcpu && paint_layer_node() && gpu_gdrag_ready(o->gpu);
    o->gdragcpu = 0;
    if (gdrag) {
        int gmw, gmh;
        gpu_gdrag_max(&gmw, &gmh);
        paint_gdrag_limit(gmw, gmh);
    }
    Box parts[DAMAGE_MAX];
    if (multi) memcpy(parts, l.r, sizeof(Box) * (size_t)nparts);
    else parts[0] = b;
    o->pl.n = 0;
    o->pframes = 0;
    double t0 = now_ms(), px = 0;
    view_of(o, index);
    paint_blur_cache_on = 1;                /* a display's scene: its blurred backdrops may be kept and reused */
    for (int k = 0; k < nparts; k++) {
        Box q = parts[k];
        Rect pr = whole ? (Rect){ 0, 0, o->scene.w, o->scene.h } : out_phys_rect(&o->v, q.x0, q.y0, q.x1, q.y1);
        if (pr.x1 <= pr.x0 || pr.y1 <= pr.y0) continue;
        float lx0, ly0, lx1, ly1;
        out_logical_box(&o->v, pr, &lx0, &ly0, &lx1, &ly1);
        ui.damage = (Box){ lx0, ly0, lx1, ly1 };      /* paint_root skips what this region does not need */
        canvas_set_clip(&o->scene, pr);
        if (gdrag) o->gdragtaken = paint_root_gdrag(&o->scene, ui.root, &o->gdrag);
        else paint_root_clear(&o->scene, ui.root, 0x000000);
        if (feed) media_scene_painted(&o->scene);     /* recording/remote desktop with protected windows */
        r = rect_join(r, pr);
        if (o->gdragtaken) {                             /* only the strips around the window were painted */
            for (int i = 0; i < o->gdrag.nstrip; i++) {
                Rect q = o->gdrag.strip[i];
                o->painted[o->npainted++] = (Box){ (float)q.x0, (float)q.y0, (float)q.x1, (float)q.y1 };
                px += (double)(q.x1 - q.x0) * (q.y1 - q.y0);
            }
            continue;
        }
        o->painted[o->npainted++] = (Box){ (float)pr.x0, (float)pr.y0, (float)pr.x1, (float)pr.y1 };
        px += (double)(pr.x1 - pr.x0) * (pr.y1 - pr.y0);
    }
    paint_blur_cache_on = 0;
    canvas_set_clip(&o->scene, (Rect){ 0, 0, o->scene.w, o->scene.h });
    ui.damage = (Box){ 0, 0, 0, 0 };
    view_reset();
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return (Rect){ 0, 0, 0, 0 };
    double ms = now_ms() - t0;
    js_stall_note("paint", "display", ms);
    ui.paint_ms += ms;
    s->paint_total += ms;
    ui.damage_px += px;
    if (s->debug || paint_profile)
        fprintf(stderr, "render %ld: display %d, layout %.2f ms, paint %.2f ms, repainted %.1f%% in %d rect%s\n", s->renders, index, ui.layout_ms, ms,
                100.0 * px / ((double)o->scene.w * o->scene.h), nparts, nparts == 1 ? "" : "s");
    paint_verify(o, index, o->gdragtaken ? o->gdrag.w : (Rect){ 0, 0, 0, 0 });
    return r;
}

/* The cursor rect on a display (physical), empty if the pointer is not on it. */
static Rect cursor_rect(const Server *s, const Out *o, const Cur *k) {
    if (k->mode != 1) return (Rect){ 0, 0, 0, 0 };
    int x, y;
    pointer_phys(s, o, &x, &y);
    Rect r = { x - k->hx, y - k->hy, x - k->hx + k->w, y - k->hy + k->h };
    if (r.x1 <= 0 || r.y1 <= 0 || r.x0 >= o->v.pw || r.y0 >= o->v.ph) return (Rect){ 0, 0, 0, 0 };
    /* in a gap next to it the pointer belongs to another display */
    ORect me = { o->v.x, o->v.y, o->v.w, o->v.h };
    int cx, cy;
    cursor_at(s, &cx, &cy);
    if (out_at(&me, 1, cx, cy) < 0 && !(o->v.dx || o->v.dy)) return (Rect){ 0, 0, 0, 0 };
    return r;
}

/* The arrow of LordOS in ARGB (for the cursor plane or a texture). */
static void arrow_argb(uint32_t *px, int w, int h) {
    memset(px, 0, (size_t)w * h * 4);
    Canvas c;
    canvas_init(&c, px, w, h, w);
    canvas_bitmap(&c, 0, 0, cursor_arrow, 0xff000000u, 0xffffffffu);
}

/* GPU path cursor: it changes when the app asks for another (or hides it, or the arrow is back). */
static void update_cursor(Server *s) {
    Cur c;
    cursor_local(s, &c, out_scale(&s->out[0]));
    const uint32_t *px = c.px;
    int w = c.w, h = c.h, hx = c.hx, hy = c.hy, mode = c.mode;
    unsigned gen = c.gen;
    if (gen == s->cursor_gen && mode == s->cursor_mode && s->cursor_tex) return;
    s->cursor_gen = gen;
    s->cursor_mode = mode;
    static uint32_t arrow[64 * 64];
    if (mode != 1 || w > 256 || h > 256) { arrow_argb(arrow, 64, 64); px = arrow; w = h = 64; hx = hy = 0; }
    s->cursor_hx = hx;
    s->cursor_hy = hy;
    /* an image larger than a display's cursor plane (a 300 % display's 96 px pointer on a 64 px plane) is
     * drawn in the composition instead; hidden, it stays where it was */
    if (s->hw_cursor_ok && mode != 2) {
        int fits = 1;
        for (int i = 0; i < OUT_MAX; i++)
            if (s->out[i].on && s->out[i].gpu >= 0 && !kms_cursor_fits(&s->out[i].kms, w, h)) fits = 0;
        if (fits != s->hw_cursor) {
            if (!fits) {
                for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].gpu >= 0) kms_cursor_show(&s->out[i].kms, 0);
                static int said;
                if (said++ < 3)
                    fprintf(stderr, "gpu: a %dx%d pointer is larger than the cursor plane (%ux%u): drawn in the composition\n", w, h,
                            K0(s)->cursor_w, K0(s)->cursor_h);
            }
            s->hw_cursor = fits;
            s->cursor_dirty = 1;                /* the next frame draws it, or leaves it out */
        }
    }
    if (s->hw_cursor) {
        if (mode != 2)
            for (int i = 0; i < OUT_MAX; i++)
                if (s->out[i].on && s->out[i].gpu >= 0) kms_cursor_image(&s->out[i].kms, px, w, h, hx, hy);
        hw_cursor_move(s);
    }
    s->cursor_tex = gpu_tex_pixels(s->cursor_tex, px, w, h, 0);
    s->cursor_tex_w = w;
    s->cursor_tex_h = h;
}

/* GPU (Mesa) on the session's card; without a usable one, the CPU path goes on. */
static int gpu_start(Server *s) {
    if (!gpu_allowed(s) || gpu_init(K0(s))) return -1;
    prep_undo_all(s);                           /* frames prepared for the CPU path (full_damage below) */
    static uint32_t arrow[64 * 64];
    arrow_argb(arrow, 64, 64);
    for (int i = 0; i < OUT_MAX; i++) s->out[i].gpu = -1;
    s->out[0].gpu = 0;
    for (int i = 1; i < OUT_MAX; i++)           /* the other displays: their own surfaces on the same context */
        if (s->out[i].on) s->out[i].gpu = gpu_add_output(&s->out[i].kms);
    s->hw_cursor = getenv("LORD_SW_CURSOR") == NULL;
    for (int i = 0; i < OUT_MAX && s->hw_cursor; i++)
        if (s->out[i].on && s->out[i].gpu >= 0 && kms_cursor_image(&s->out[i].kms, arrow, 64, 64, 0, 0)) s->hw_cursor = 0;
    s->hw_cursor_ok = s->hw_cursor;
    hw_cursor_move(s);
    s->cursor_gen = 0;
    s->cursor_mode = -1;
    if (s->hw_cursor)       /* the plane's size is the driver's (kms.c cursor_cap): a report shows which one it took */
        fprintf(stderr, "gpu: cursor by hardware (cursor plane, %ux%u)\n", K0(s)->cursor_w, K0(s)->cursor_h);
    else fprintf(stderr, "gpu: cursor drawn in the composition\n");
    full_damage(s);
    return 0;
}

static Server *gpu_server;
static Out *gpu_out;                    /* the display gpu_frame is drawing */
static void gpu_layer(int layer) {
    wl_gpu_draw_layer(layer);
    Server *s = gpu_server;
    Out *o = gpu_out;
    if (layer == 1 && o && !s->hw_cursor && s->cursor_mode != 2 && s->cursor_tex) {     /* cursor without its own plane */
        int x, y;
        pointer_phys(s, o, &x, &y);
        gpu_draw(s->cursor_tex, (float)(x - s->cursor_hx), (float)(y - s->cursor_hy), (float)s->cursor_tex_w,
                 (float)s->cursor_tex_h, 0, 0, (float)s->cursor_tex_w, (float)s->cursor_tex_h, (Rect){ 0, 0, o->v.pw, o->v.ph }, 1);
    }
}

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* LordBench's frame phases (bench/lbframes.h): the poll() return before this frame and whether it handled
 * the main display's flip; render_frame fills the main display's part of fph */
static uint64_t loop_wake;
static int loop_wake_flip;
static LbPhase fph;
static int fph_main;
/* the frame pipeline: when render() started (a prepared frame's "before render" ends there), and whether this
 * render() submitted a prepared frame of the main display (its work and phases were recorded there) */
static uint64_t render_r0;
static int call_piped;

static int render_frame(Server *s);
static void gdrag_feed_now(Server *s, LbPhase *ph);
static void render(Server *s) {
    int phases = lbf_recording();
    gpu_fence_on(phases);                       /* the GPU's finish time of each frame, only while recording */
    uint64_t r0 = phases ? mono_ns() : 0;
    double t0 = now_ms();
    fph = (LbPhase){ .gdrag = -1, .gdrag_why = -1, .gdrag_sub = -1 };
    fph_main = 0;
    render_r0 = r0;
    call_piped = 0;
    int shown = render_frame(s);                /* 0: it only prepared the next frame (the frame pipeline) */
    double t1 = now_ms();
    uint64_t r1 = phases ? mono_ns() : 0;
    /* LordBench's compositor measurement (bench/lbframes.c); a prepared frame's goes at its submission */
    if (shown && !call_piped) lbf_frame(t1 - t0);
    gdrag_feed_now(s, &fph);                       /* after the frame's time: its flip is queued already */
    if (phases && fph_main) {
        fph.wake = loop_wake;
        fph.from_flip = loop_wake_flip;
        fph.render0 = r0;
        fph.built = r1;
        lbf_phase_built(&fph);
        if (s->headless) lbf_phase_shown(fph.submit, 0);
    }
    if (dstat.active && shown) {
        double js = dstat.js - dstat.work_js;
        double work = (t1 - t0) + js;
        dstat.work_js = dstat.js;
        dstat.frames++;
        dstat.work += work;
        dstat.paint += ui.layout_ms + ui.paint_ms;
        dstat.layout += ui.layout_ms;
        dstat.present += (t1 - t0) - (ui.layout_ms + ui.paint_ms);
        if (work > dstat.work_max) dstat.work_max = work;
        if (t1 - dstat.last > dstat.gap_max && dstat.frames > 1) dstat.gap_max = t1 - dstat.last;
        dstat.last = t1;
    }
}

/* Frame statistics per display count and scale (docs/DISPLAYS.md, "Measurements"): with
 * LORD_FRAME_STATS=1 one line every 300 frames with the average work per frame. */
static struct { long n; double work, paint, present; } fstat_;
static void frame_stats(Server *s, double work, double paint) {
    static int on = -1;
    if (on < 0) on = getenv("LORD_FRAME_STATS") != NULL;
    if (!on) return;
    fstat_.n++;
    fstat_.work += work;
    fstat_.paint += paint;
    fstat_.present += work - paint;
    if (fstat_.n < 300) return;
    char scales[64] = "";
    int len = 0;
    for (int i = 0; i < OUT_MAX; i++)
        if (s->out[i].on) len += snprintf(scales + len, sizeof scales - (size_t)len, "%s%dx%d@%d%%", len ? "+" : "", s->out[i].v.pw, s->out[i].v.ph, out_scale(&s->out[i]));
    fprintf(stderr, "frames: %d display(s) %s: %ld frames, per frame %.2f ms (layout+paint %.2f, present %.2f), %s\n", out_count(s), scales,
            fstat_.n, fstat_.work / fstat_.n, fstat_.paint / fstat_.n, fstat_.present / fstat_.n, gpu_active() ? "GPU" : "CPU");
    memset(&fstat_, 0, sizeof fstat_);
}

/* CPU path: what changed goes into the display's KMS buffer, then the cursor, then the flip. */
static void present_cpu(Server *s, Out *o, int index, Rect r, int feed) {
    KmsBuffer *b = kms_back(&o->kms);
    Canvas out, phys;
    /* rotated (Settings → Displays): composed upright in o->frame, copied rotated into the card's buffer */
    int rot = o->d.rot & 3, xf = rot != 0 && o->frame;
    int W = o->v.pw, H = o->v.ph;
    canvas_init(&phys, b->map, (int)o->kms.width, (int)o->kms.height, (int)(b->pitch / 4));
    if (xf) canvas_init(&out, o->frame, W, H, W);
    else { out = phys; out.w = W < phys.w ? W : phys.w; out.h = H < phys.h ? H : phys.h; canvas_set_clip(&out, (Rect){ 0, 0, out.w, out.h }); }
    int bi = o->kms.front ^ 1, fs = wl_fullscreen_active();
    if (fs != o->was_fs) { o->was_fs = fs; o->full_copy = 2; }
    Rect need = { 0, 0, W, H };             /* what this buffer gets now (also tinted) */
    int tinted = 0;                         /* the copied rectangles were tinted one by one */
    if (o->full_copy > 0 || fs) {
        canvas_copy(&out, &o->scene);
        if (o->full_copy > 0) o->full_copy--;
    } else {
        need = rect_join(rect_join(o->stale[bi], r), o->curs[bi]);
        if (need.x0 < 0) need.x0 = 0;
        if (need.y0 < 0) need.y0 = 0;
        if (need.x1 > out.w) need.x1 = out.w;
        if (need.y1 > out.h) need.y1 = out.h;
        /* several damaged rectangles: only they go into the buffer (with the moving dots spread over
         * the Monitor's map, the box around them was most of the screen) */
        DamageList cp = o->pstale[bi];
        for (int i = 0; i < o->npainted; i++) dlist_add(&cp, o->painted[i]);
        Rect cu = o->curs[bi];
        if (cu.x1 > cu.x0 && cu.y1 > cu.y0) dlist_add(&cp, (Box){ (float)cu.x0, (float)cu.y0, (float)cu.x1, (float)cu.y1 });
        Box cb = cp.n ? cp.r[0] : (Box){ 0, 0, 0, 0 };
        for (int i = 1; i < cp.n; i++) cb = box_join(cb, cp.r[i]);
        Rect cbr = { (int)cb.x0, (int)cb.y0, (int)cb.x1, (int)cb.y1 };
        Rect nj = rect_join(rect_join(o->stale[bi], r), o->curs[bi]);
        if (paint_multi_damage && !xf && cp.n > 1 && cbr.x0 == nj.x0 && cbr.y0 == nj.y0 && cbr.x1 == nj.x1 && cbr.y1 == nj.y1) {
            for (int i = 0; i < cp.n; i++) {
                Rect q = { (int)cp.r[i].x0, (int)cp.r[i].y0, (int)cp.r[i].x1, (int)cp.r[i].y1 };
                if (q.x0 < 0) q.x0 = 0;
                if (q.y0 < 0) q.y0 = 0;
                if (q.x1 > out.w) q.x1 = out.w;
                if (q.y1 > out.h) q.y1 = out.h;
                if (q.x1 <= q.x0 || q.y1 <= q.y0) continue;
                for (int y = q.y0; y < q.y1; y++)
                    memcpy(out.px + (size_t)y * out.stride + q.x0, o->scene.px + (size_t)y * o->scene.stride + q.x0, (size_t)(q.x1 - q.x0) * 4);
                display_tint(&out, q);      /* Night Light, on what was copied */
            }
            tinted = 1;
        } else
            for (int y = need.y0; y < need.y1; y++)
                memcpy(out.px + (size_t)y * out.stride + need.x0, o->scene.px + (size_t)y * o->scene.stride + need.x0, (size_t)(need.x1 - need.x0) * 4);
    }
    o->stale[bi] = (Rect){ 0, 0, 0, 0 };
    o->stale[bi ^ 1] = rect_join(o->stale[bi ^ 1], r);
    o->pstale[bi].n = 0;
    for (int i = 0; i < o->npainted; i++) dlist_add(&o->pstale[bi ^ 1], o->painted[i]);
    o->curs[bi] = (Rect){ 0, 0, 0, 0 };
    view_of(o, index);
    wl_paint_fullscreen(&out);
    view_reset();
    Cur k;
    cursor_now(s, &k, out_scale(o));
    int px_, py_;
    pointer_phys(s, o, &px_, &py_);
    if (feed) {
        /* remote desktop (without the cursor) and recording: with protected windows as the protected fill */
        MediaCursor mc = { px_, py_, k.mode, k.px, k.w, k.h, k.hx, k.hy };
        view_of(o, index);
        const Canvas *seen = media_composed(&out, mirror_watchers() > 0, &mc);
        view_reset();
        mirror_composed_on_gpu(0);        /* this frame was painted on the CPU */
        /* only what went into this buffer can differ from the last frame (not with a fullscreen app
         * painted over it, nor when the frame is the copy with protected windows as the protected fill) */
        int dmg[4] = { need.x0, need.y0, need.x1, need.y1 };
        mirror_frame(seen->px, seen->stride, W, H, seen == &out && !wl_fullscreen_active() ? dmg : NULL);
        mirror_cursor(px_, py_, k.mode, k.px, k.w, k.h, k.hx, k.hy, k.gen);
    }
    cursor_local(s, &k, out_scale(o));      /* the arrow is also an image (cursor.c) */
    Rect cr = cursor_rect(s, o, &k);
    o->cur_on = cr.x1 > cr.x0;
    if (!xf) {
        if (!tinted) display_tint(&out, need);   /* Night Light without a gamma LUT (software) */
        if (o->cur_on) {
            canvas_blit(&out, k.px, k.w, k.h, cr.x0, cr.y0, 0, 1, 0);
            o->curs[bi] = cr;
        }
    } else {
        if (need.x1 > need.x0 && need.y1 > need.y0) {
            display_rotate_copy(&out, need, &phys, rot);
            display_tint(&phys, display_rotate_rect(need, W, H, rot));
        }
        /* the cursor is composited apart (o->frame stays clean) and copied rotated */
        static uint32_t tmp[160 * 160];
        Rect cc = cr;
        if (cc.x0 < 0) cc.x0 = 0;
        if (cc.y0 < 0) cc.y0 = 0;
        if (cc.x1 > W) cc.x1 = W;
        if (cc.y1 > H) cc.y1 = H;
        int cw = cc.x1 - cc.x0, ch = cc.y1 - cc.y0;
        if (o->cur_on && cw > 0 && ch > 0 && cw <= 160 && ch <= 160) {
            for (int y = 0; y < ch; y++) memcpy(tmp + (size_t)y * cw, out.px + (size_t)(cc.y0 + y) * out.stride + cc.x0, (size_t)cw * 4);
            Canvas t;
            canvas_init(&t, tmp, cw, ch, cw);
            canvas_blit(&t, k.px, k.w, k.h, cr.x0 - cc.x0, cr.y0 - cc.y0, 0, 1, 0);
            display_rotate_copy_at(tmp, cw, cc.x0, cc.y0, cc, W, H, &phys, rot);
            display_tint(&phys, display_rotate_rect(cc, W, H, rot));
            o->curs[bi] = cc;
        }
    }
    o->cdirty = 0;
    kms_flip(&o->kms);
}

/* Memory frames (headless session, the capture view): scene + fullscreen, then the remote desktop. */
static void present_memory(Server *s, Out *o, int index, int feed, Rect drawn) {
    if (!o->frame) return;
    /* Without a fullscreen app the frame is the scene itself: read it there, no copy. A fullscreen app
     * paints over a copy (o->frame). Right after one, the whole screen counts as changed. */
    static int fs_before;
    int fs = wl_fullscreen_active(), whole = fs || fs_before;
    fs_before = fs;
    Canvas out;
    if (fs) {
        canvas_init(&out, o->frame, o->scene.w, o->scene.h, o->scene.w);
        canvas_copy(&out, &o->scene);
    } else out = o->scene;
    if (drawn.x0 < 0) drawn.x0 = 0;
    if (drawn.y0 < 0) drawn.y0 = 0;
    if (drawn.x1 > out.w) drawn.x1 = out.w;
    if (drawn.y1 > out.h) drawn.y1 = out.h;
    view_of(o, index);
    wl_paint_fullscreen(&out);
    Cur k;
    cursor_now(s, &k, out_scale(o));
    int x, y;
    pointer_phys(s, o, &x, &y);
    if (feed) {
        /* what the browser sees and what is recorded: with the protected windows as the protected fill */
        MediaCursor mc = { x, y, k.mode, k.px, k.w, k.h, k.hx, k.hy };
        const Canvas *seen = media_composed(&out, mirror_watchers() > 0, &mc);
        mirror_composed_on_gpu(0);        /* this frame was painted on the CPU */
        int dmg[4] = { drawn.x0, drawn.y0, drawn.x1, drawn.y1 };
        mirror_frame(seen->px, seen->stride, o->scene.w, o->scene.h, seen == &out && !whole ? dmg : NULL);
        mirror_cursor(x, y, k.mode, k.px, k.w, k.h, k.hx, k.hy, k.gen);
    }
    view_reset();
    o->cdirty = 0;
}

/* Does this display have something to show? (damage, a moved cursor on it, a fullscreen app) */
static int out_needs(const Server *s, const Out *o) {
    if (!o->on) return 0;
    if (o->full || !box_empty(o->pend) || o->wl_new) return 1;
    if (!o->cdirty) return 0;
    if (o->cur_on) return 1;
    ORect me = { o->v.x, o->v.y, o->v.w, o->v.h };
    int cx, cy;
    cursor_at(s, &cx, &cy);
    return out_at(&me, 1, cx, cy) >= 0 || o->v.dx || o->v.dy;
}
static int outs_have_work(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++) if (!s->out[i].kms.flip_pending && out_needs(s, &s->out[i])) return 1;
    return 0;
}

/* paint_gdrag_feed's way to the GPU: ctx is the display's GPU output */
static int gdrag_feed_up(void *ctx, int which, const uint32_t *px, int w, int h, Rect r) {
    return gpu_gdrag_feed(*(const int *)ctx, which, px, w, h, r);
}
/* the displays whose GPU drag textures catch up after this frame: after its flips are queued and its time is
 * taken, while the CPU would wait for the vblank anyway (docs/GPU.md, "Uploads without a stall") */
static int feed_idx[OUT_MAX], nfeed;
static void gdrag_feed_now(Server *s, LbPhase *ph) {
    for (int k = 0; k < nfeed; k++) {
        Out *o = &s->out[feed_idx[k]];
        if (!o->on || o->gpu < 0 || !gpu_gdrag_ready(o->gpu) || paint_gdrag_backlog(feed_idx[k]) <= 0) continue;
        double f0 = now_ms();
        long sent = paint_gdrag_feed(feed_idx[k], GDRAG_FEED_PX, gdrag_feed_up, &o->gpu);
        if (o == main_out(s)) { ph->feed_ms = now_ms() - f0; ph->feed_px = sent; }
    }
    nfeed = 0;
}

/* the frame pipeline (fpipe.h): it applies to this display now. One display with real page flips: without a
 * monitor there is no flip, a driver paced in software waits for its slot rather than the flip, and with
 * several displays paint.c's drag layer and GPU drag state are shared between them (each frame is built and
 * submitted at once there, as before). */
static int pipe_on(const Server *s, const Out *o) {
    return fpipe_engaged(&fpipe) && !s->headless && flip_instant < 3 && o->on && out_count(s) == 1;
}
/* pointer input and LordBench's scripted step wait for this display's flip (fpipe_blocked) */
static int out_blocked(const Server *s, const Out *o) { return fpipe_blocked(pipe_on(s, o), o->kms.flip_pending, o->prep); }
static int any_blocked(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && out_blocked(s, &s->out[i])) return 1;
    return 0;
}
/* the next frame's CPU work that can be done ahead: UI damage, layout or a JS frame callback waiting */
static int ahead_work(const Out *o) { return ui.dirty || js_wants_frame() || o->full || !box_empty(o->pend); }
static int can_prepare(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++) {
        const Out *o = &s->out[i];
        if (o->on && fpipe_step(pipe_on(s, o), o->kms.flip_pending, o->prep, 0, ahead_work(o)) == FPIPE_PREPARE) return 1;
    }
    return 0;
}
/* a prepared frame whose display's flip is done: it is submitted now */
static int prep_ready(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++)
        if (s->out[i].on && fpipe_step(1, s->out[i].kms.flip_pending, s->out[i].prep, 0, 0) == FPIPE_SUBMIT) return 1;
    return 0;
}
/* the prepared frame can be submitted as it was painted: the same scene, GPU context and output, and with
 * GPU drag the same layer buffers and textures' bookkeeping (paint_gdrag_epoch) */
static int prep_valid(const Out *o) {
    if (o->prep_px != o->scene.px || o->prep_gen != gpu_generation() || o->prep_gpu != (gpu_active() ? o->gpu : -1)) return 0;
    return !o->gdragtaken || (o->prep_epoch == paint_gdrag_epoch() && gpu_gdrag_ready(o->gpu));
}

static int par_on, par_light;           /* the paint threads (render_frame) */

/* Recording the screen, or a remote viewer with protected windows: the screen must be in memory (CPU path).
 * 1 if the GPU was stopped now (every display is painted whole next). */
static int cpu_switch(Server *s) {
    if (!gpu_active() || !need_cpu()) return 0;
    fprintf(stderr, "remote: composing by CPU (%s)\n", media_needs_cpu() ? "screen recording" : "protected windows on screen");
    cpu_takeover(s);                            /* first: the GPU's framebuffers leave the screen (outputs_apply) */
    gpu_tex_free(s->cursor_tex);
    s->cursor_tex = NULL;
    gpu_shutdown();
    s->gpu_paused = 1;
    full_damage(s);
    return 1;
}
static void cursor_mark(Server *s) {
    if (!s->cursor_dirty) return;
    for (int i = 0; i < OUT_MAX; i++) s->out[i].cdirty = 1;
    s->cap.cdirty = 1;
    s->cursor_dirty = 0;
}

/* The CPU part of a display's frame: its damage painted into the scene (paint_out), and what GPU drag did. */
static Rect out_paint(Server *s, Out *o, int i, const Out *feed) {
    /* GPU drag with no UI damage: the GPU draws the window again from its textures, if they are still this
     * display's; otherwise its rectangle is painted by the CPU now */
    if (o->gdrag_on && box_empty(o->pend) && !o->full && !gpu_gdrag_holds(o->gpu)) gdrag_stale(o);
    Rect r = paint_out(s, o, i, o == feed);
    o->pgdragwhy = o->pgdragsub = -1;
    o->pgdrag = r.x1 > r.x0 ? paint_gdrag_last(&o->pgdragwhy, &o->pgdragsub) : -1;
    return r;
}

/* The rest of the frame: what paint_out painted (r) and the layers go to the display (GPU: upload, draw, swap
 * and page flip; CPU: copied into its KMS buffer, then the flip). ph: the main display's phases, else NULL. */
static void out_present(Server *s, Out *o, int i, Rect r, const Out *feed, LbPhase *ph) {
    int gdragwhy = o->pgdragwhy, gdragsub = o->pgdragsub, gdraglast = o->pgdrag;
    if (ph) { ph->gdrag = gdraglast; ph->gdrag_why = gdragwhy; ph->gdrag_sub = gdragsub; }
    if (gpu_active() && o->gpu >= 0) {
        update_cursor(s);
        gpu_server = s;
        gpu_out = o;
        view_of(o, i);
        /* the remote desktop watches: the composed frame is read back once for every viewer */
        static uint32_t *rb;
        static size_t rb_n;
        /* never while a protected window is on screen: only a frame painted in capture mode may leave (media_composed);
         * cpu_switch moves the session to the CPU at the next frame, and until then the viewers get no frame */
        int readback = o == feed && mirror_watchers() > 0 && !media_protected_count();
        if (readback) {
            size_t need = (size_t)o->scene.w * o->scene.h;
            if (need > rb_n) { free(rb); rb = malloc(need * 4); rb_n = rb ? need : 0; }
            if (rb) gpu_read_next(rb, o->scene.w, o->scene.h);
            else readback = 0;
        }
        /* compatible fullscreen (one display): straight to the card; otherwise composed (always
         * composed while the remote desktop reads it back) */
        /* GPU drag: the strips around the window, and the window from its textures (gpu_frame_ex); a frame
         * with nothing painted while the last one had it draws it again (its rectangle is stale) */
        /* what paint_out painted: each damaged rectangle on its own (paint.c, "Several damaged rectangles") */
        Rect up[DAMAGE_MAX] = { r };
        int nup = 1, gdragdrawn = 0;
        if (!o->gdragtaken && o->npainted > 1 && r.x1 > r.x0) {
            nup = 0;
            for (int k = 0; k < o->npainted && nup < DAMAGE_MAX; k++)
                up[nup++] = (Rect){ (int)o->painted[k].x0, (int)o->painted[k].y0, (int)o->painted[k].x1, (int)o->painted[k].y1 };
        }
        GpuDrag again;
        const GpuDrag *gdrag = NULL;
        if (o->gdragtaken) {
            gdrag = &o->gdrag;
            nup = o->gdrag.nstrip;
            memcpy(up, o->gdrag.strip, sizeof o->gdrag.strip);
        } else if (o->gdrag_on && r.x1 <= r.x0) {
            again = o->gdrag;
            again.lpx = again.bpx = NULL;
            again.nl = again.nb = again.lfull = again.bfull = 0;
            again.ref = NULL;
            gdrag = &again;
        }
        int scanned = !readback && out_count(s) == 1 && wl_try_scanout(s->hw_cursor || s->cursor_mode == 2);
        if (!scanned) gpu_scissor_quiet(o->gpu, wl_gpu_layers_empty() && (s->hw_cursor || s->cursor_mode == 2 || !s->cursor_tex));
        int res = scanned ? 0 : gpu_frame_ex(o->gpu, &o->scene, up, nup, gdrag, &gdragdrawn, gpu_layer);
        int failed = res == -1, unshown = res == -2;
        if (o->gdragtaken && gdragdrawn) paint_gdrag_done();    /* the textures have what paint.c had */
        if (ph) {
            /* a frame scanned out straight has no times of its own (gpu_frame_times keeps the last
             * composed frame's) and no fence (one left from before would give an old GPU time) */
            GpuFrameTimes ft = { 0 };
            if (!scanned) gpu_frame_times(o->gpu, &ft);
            else if (lbf_recording()) { uint64_t old; gpu_fence_time(o->gpu, &old); }
            ph->up_ms = ft.up_ms;
            ph->up_px = ft.up_px;
            ph->gdrag_ms = ft.gdrag_ms;
            ph->gdrag_px = ft.gdrag_px;
            ph->gdrag_full = ft.gdrag_full;
            ph->swap_ms = ft.swap_ms;
            ph->submit_ms = ft.submit_ms;
            ph->unshown = unshown;
        }
        /* GPU drag's textures catch up after the frame (render, gdrag_feed_now): only when this frame
         * would have been GPU drag's but for what they lack (paint.c, paint_gdrag_feed); sent while GPU drag is
         * refused for another reason, most of it would be sent again before it is used */
        if (!failed && !gdragdrawn && gdraglast == 0 && gdragwhy == GDRAG_R_FEED && nfeed < OUT_MAX) feed_idx[nfeed++] = i;
        if (unshown) {
            /* the quad could not be drawn: nothing was shown (the display keeps the frame before,
             * which was right); W joins the damage and the next frame paints it on the CPU */
            o->gdragw = gdrag->w;
            gdrag_stale(o);
            o->gdragcpu = 1;
        } else if (gdrag && !gdragdrawn && !failed) {
            /* shown without the quad (direct scanout, or GPU drag turned off): painted next frame */
            o->gdragw = gdrag->w;
            gdrag_stale(o);
        } else {
            o->gdrag_on = gdragdrawn;
            if (gdragdrawn) o->gdragw = gdrag->w;
        }
        o->gdragtaken = 0;
        if (readback && !res && gpu_read_done()) {
            static int said;
            if (!said++) fprintf(stderr, "remote: composing on the GPU; frames read back for the remote desktop\n");
            Cur k;
            cursor_now(s, &k, out_scale(o));
            int cx, cy;
            pointer_phys(s, o, &cx, &cy);
            mirror_composed_on_gpu(1);
            mirror_frame(rb, o->scene.w, o->scene.w, o->scene.h, NULL);
            mirror_cursor(cx, cy, k.mode, k.px, k.w, k.h, k.hx, k.hy, k.gen);
        }
        view_reset();
        gpu_out = NULL;
        if (!unshown) {                        /* else they are presented with the next frame */
            o->cdirty = 0;
            o->wl_new = 0;
        }
        if (failed) {
            fprintf(stderr, "gpu: a frame failed; going on by CPU\n");
            cpu_takeover(s);
            gpu_shutdown();
            full_damage(s);
        }
    } else present_cpu(s, o, i, r, o == feed);
    if (ph) {
        ph->prev_flip = o->kms.flip_ns;
        ph->submit = o->kms.flip_pending ? o->kms.flip_submit_ns : 0;
        ph->period = o->kms.mode.vrefresh ? 1000000000u / o->kms.mode.vrefresh : 16666667u;
    }
}

static double out_period_ms(const Out *o) { return o->kms.mode.vrefresh ? 1000.0 / o->kms.mode.vrefresh : 1000.0 / 60; }
/* a main display's frame for the pipeline's auto mode (fpipe_note); with -d its changes are logged */
static void pipe_note(const Server *s, double ms, const Out *o) {
    int was = fpipe_engaged(&fpipe);
    fpipe_note(&fpipe, now_ms(), ms, out_period_ms(o));
    if (s->debug && was != fpipe_engaged(&fpipe))
        fprintf(stderr, "frames: pipeline %s (a frame took %.1f ms of %.1f)\n", was ? "off" : "on", ms, out_period_ms(o));
}

/* The frame pipeline: the next frame painted into the scene while this display's flip is pending. Its CPU
 * time starts at ui0 (after any submission in this render: the JS frame callbacks, layout and paint). */
static void out_prepare(Server *s, Out *o, int i, const Out *feed, int is_main, double ui0, uint64_t ui0_ns) {
    double paint0 = ui.paint_ms;
    int whole = o->full;
    Rect r = out_paint(s, o, i, feed);
    /* nothing painted: the frame is built after the flip as without the pipeline, and nothing more is tried
     * before it (input waits for it as before, and a JS frame callback without damage does not spin) */
    if (r.x1 <= r.x0 || r.y1 <= r.y0) {
        o->prep = FPIPE_EMPTY;
        o->prep_js = ui_js_ran && js_wants_frame();     /* (fpipe_hold_callbacks) */
        return;
    }
    o->prep = FPIPE_READY;
    o->prep_r = r;
    o->prep_whole = whole;
    o->prep_px = o->scene.px;
    o->prep_gen = gpu_generation();
    o->prep_gpu = gpu_active() ? o->gpu : -1;
    o->prep_epoch = paint_gdrag_epoch();
    o->prep_ms = now_ms() - ui0;
    if (!is_main) return;
    lbf_painted();                              /* the synthetic pointer's events so far are in it */
    o->pph = (LbPhase){ .wake = loop_wake, .from_flip = loop_wake_flip, .pre_end = render_r0, .render0 = ui0_ns,
                        .built = mono_ns(), .paint_ms = ui.layout_ms + (ui.paint_ms - paint0), .par = par_on, .piped = 1 };
}

/* The frame pipeline: the prepared frame goes to the display (only upload, GPU and flip were left). */
static void out_submit(Server *s, Out *o, int i, const Out *feed, int is_main) {
    uint64_t s0 = lbf_recording() ? mono_ns() : 0;
    double t0 = now_ms();
    o->prep = FPIPE_NONE;
    o->prep_js = 0;
    LbPhase *ph = is_main ? &o->pph : NULL;
    out_present(s, o, i, o->prep_r, feed, ph);
    if (!is_main) return;
    double ms = o->prep_ms + (now_ms() - t0);
    lbf_submitted();                            /* its events end at its presentation */
    lbf_frame(ms);                              /* its work: the prepare and the submission */
    pipe_note(s, ms, o);
    ph->swake = loop_wake;
    ph->sfrom_flip = loop_wake_flip;
    ph->sub0 = s0;
    ph->sub1 = s0 ? mono_ns() : 0;
    call_piped = 1;
}

/* Returns 0 when it only painted the next frame ahead (every display was waiting for its flip). */
static int render_frame(Server *s) {
    double t0 = now_ms();
    int prep_only = !s->headless && all_busy(s);
    ui.layout_ms = ui.paint_ms = 0;
    ui.damage_px = 0;
    if (keylag_enabled) keylag_render();
    /* the latch: a Linux app's held content updates that may go in this frame (fifo-v1, commit-timing-v1; wl.c) */
    if (!prep_only) {
        unsigned lk = 0;
        if (s->headless) lk = OUTPICK_ALL;
        else for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && !s->out[i].kms.flip_pending) lk |= 1u << i;
        wl_latch(lk, s->headless || flip_instant >= 3);
    }
    /* GPU path: a Linux app's new frame is only a texture (wl.c damage_node), not UI damage. Every
     * GPU display must still present it: before 0.8.1 nothing was flipped until some other part of
     * the screen changed (the clock, the cursor...), so typing in Chromium or Firefox showed up to a
     * second late and Chromium logged "buggy presentation feedback" (docs/TROUBLESHOOTING.md). */
    /* Only the displays the window is on (0.8.9.5): a video on one display made every display compose again. */
    unsigned wlk = wl_new_content();
    if (wlk && gpu_active())
        for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].gpu >= 0 && (wlk >> i & 1)) s->out[i].wl_new = 1;
    /* a machine's new frame in the texture its window draws (media_src_present): only the displays it is on */
    float mb[32][4];
    int nmb = media_present_take(mb, 32);
    if (nmb && gpu_active())
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            if (!o->on || o->gpu < 0) continue;
            for (int b = 0; b < nmb; b++)
                if (mb[b][0] < o->v.x + o->v.w && mb[b][2] > o->v.x && mb[b][1] < o->v.y + o->v.h && mb[b][3] > o->v.y) { o->wl_new = 1; break; }
        }
    Out *feed = feed_out(s), *mo = main_out(s);
    int main_shown = 0, main_built = 0;     /* the main display got this frame; built here (not prepared before) */
    int done[OUT_MAX] = { 0 };              /* submitted or built in this render */
    /* The frame pipeline: the prepared frames are submitted first, before the JS frame callbacks and the
     * layout of the next one; GPU drag's textures catch up before that one is painted. One that can no longer be
     * submitted as painted is undone and built below like any frame. */
    int ready = !s->headless && prep_ready(s);
    if (ready) {
        cursor_mark(s);
        cpu_switch(s);
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            if (!o->on || o->kms.flip_pending || o->prep != FPIPE_READY) continue;
            if (!prep_valid(o)) { prep_undo(o); continue; }
            out_submit(s, o, i, feed, o == mo);
            done[i] = 1;
            if (o == mo) main_shown = 1;
        }
        if (call_piped) {
            gdrag_feed_now(s, &mo->pph);
            lbf_phase_built(&mo->pph);
        }
    }
    double ui0 = now_ms();
    uint64_t ui0_ns = lbf_recording() ? mono_ns() : 0;
    /* the JS frame callbacks already ran for the frame built now, in a prepare that painted nothing during
     * the flip (fpipe_hold_callbacks): not again; the ones they asked for run with the next frame */
    ui_js_hold = ui_js_ran = 0;
    if (!prep_only)
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            if (!o->on || o->kms.flip_pending) continue;
            if (fpipe_hold_callbacks(o->prep, o->prep_js)) ui_js_hold = 1;
            o->prep_js = 0;
        }
    paint_views_set(s);
    paint_ui(s);
    s->last_render = now_ms();
    cursor_mark(s);
    /* (not while every display waits for its flip: stopping the GPU sets the dumb buffer with a modeset, and a
     * pending flip's buffer must stay; a frame prepared meanwhile is undone at the flip by cpu_takeover) */
    if (!ready && !prep_only && cpu_switch(s)) paint_ui(s);
    ui_js_hold = 0;
    if (s->headless) {
        Out *o = &s->out[0];
        Rect drawn = paint_out(s, o, 0, 1);
        if (drawn.x1 > drawn.x0) fph.gdrag = paint_gdrag_last(&fph.gdrag_why, &fph.gdrag_sub);
        present_memory(s, o, 0, 1, drawn);
        fph_main = 1;                           /* presented at the end of this frame (below) */
    } else {
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            /* a fullscreen app's new frame (render runs when something wants a frame) goes to every display */
            if (!o->on || o->kms.flip_pending || done[i] || !(out_needs(s, o) || wl_fullscreen_active())) continue;
            prep_undo(o);                       /* (an attempt that painted nothing during the flip) */
            Rect r = out_paint(s, o, i, feed);
            out_present(s, o, i, r, feed, o == mo ? &fph : NULL);
            done[i] = 1;
            if (o == mo) {
                main_shown = main_built = 1;
                fph_main = 1;
                pipe_note(s, now_ms() - t0, o);
            }
        }
        /* the frame pipeline: the next frame of a display waiting for its flip is painted now. Right after
         * a submission or a frame built here only when there is damage already; called for it alone, it is
         * tried once per flip even without (a JS frame callback may bring it) */
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            if (!o->on) continue;
            int work = prep_only || o->full || !box_empty(o->pend);
            /* (not with GPU drag's catch-up still queued by a frame built here: it runs after render_frame, and must
             * never run between a frame's paint and its upload) */
            if (nfeed) continue;
            if (fpipe_step(pipe_on(s, o), o->kms.flip_pending, o->prep, 0, work) == FPIPE_PREPARE)
                out_prepare(s, o, i, feed, o == mo, ui0, ui0_ns);
        }
        /* every display in one frame for the remote desktop and recording (capture view) */
        Out *c = &s->cap;
        if (feed == c && (mirror_watchers() || media_needs_cpu())) {
            if (!c->on || c->v.w != s->W || c->v.h != s->H) {
                c->on = 1;
                c->v = out_view(0, 0, s->W, s->H, 100);
                c->d.scale = 100;
                c->full = out_frames(c, 1) == 0;
            }
            if (c->scene.px && (c->full || !box_empty(c->pend) || c->cdirty || wl_fullscreen_active())) {
                Rect drawn = paint_out(s, c, OUT_MAX, 1);
                present_memory(s, c, OUT_MAX, 1, drawn);
            }
        } else if (c->on) { out_free(c); c->on = 0; c->pend = (Box){ 0, 0, 0, 0 }; }
    }
    lbf_built(s->headless || main_built);      /* LordBench: the synthetic pointer's events are in this frame */
    if (!prep_only) s->frames++;
    /* The pool only while frames are heavy: it costs CPU to wake (a light frame painted on one thread
     * spends less), so it turns on when painting takes more than 70 % of a frame at the target rate (the
     * monitor's refresh, or the fastest remote viewer's without a monitor) and off after 60 light frames
     * in a row (with the pool a heavy frame paints fast, so not at once: it would flap).
     * A 1080p video under blurred windows: 45 → 57 frames a second on VM 116. */
    Out *pm = main_out(s);
    int hz = s->headless ? (mirror_fps() > 60 ? mirror_fps() : 60) : pm && pm->kms.mode.vrefresh ? (int)pm->kms.mode.vrefresh : 60;
    double frame_ms = 1000.0 / hz;
    fph.paint_ms = ui.layout_ms + ui.paint_ms;
    fph.par = par_on;
    /* (0.8.6) The whole frame counts too, not only its paint: what has to fit before the vblank is the
     * frame (layout, paint, the upload, the GPU's own part), and on the Pi 5 under an all-core load the
     * paint alone (about 7.5 ms) stayed under 70 % while the frame (about 13 ms) missed every vblank. On
     * when the frame takes more than 60 % of the period; light only with both paint and frame short. */
    double built = now_ms() - t0;
    if (!par_on && (ui.paint_ms > frame_ms * 0.7 || built > frame_ms * 0.6) && par_threads() > 1) { par_on = 1; par_light = 0; canvas_parallel(par_rows); }
    else if (par_on) {
        par_light = ui.paint_ms < frame_ms * 0.25 && built < frame_ms * 0.45 ? par_light + 1 : 0;
        if (par_light >= 60) { par_on = 0; canvas_parallel(NULL); }
    }
    /* the Linux apps' frame callbacks go with a frame submitted, not with one only painted ahead: each window's with its
     * own display's, a display that just got a frame or has none waiting for a flip (wl.c, outpick.h) */
    if (!prep_only) {
        unsigned keys = 0;
        if (s->headless) keys = OUTPICK_ALL;
        else for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && (done[i] || !s->out[i].kms.flip_pending)) keys |= 1u << i;
        wl_frame_done((unsigned)now_ms(), keys);
    }
    if (s->headless) {                          /* no flip without a monitor: the frame is out now */
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        lbf_presented(ns, (uint32_t)(1000000000u / (unsigned)hz));
        /* its phases: "queued" and "shown" now, the vblanks on the pacing grid of 1000/hz ms (render) */
        static uint64_t headless_prev;
        fph.prev_flip = headless_prev;
        fph.submit = ns;
        fph.period = (uint32_t)(1000000000u / (unsigned)hz);
        headless_prev = ns;
        wl_presented(-1, 0, ns, 0, (unsigned)(1000000000u / (unsigned)hz));   /* the apps' presentation feedback too */
    }
    /* No flip event will come (a modeset instead of a page flip: the first frame, or a driver
     * without page flip): the frame is on screen already, so its presentation feedback goes now.
     * Otherwise the apps' feedback would wait for some later flip. */
    if (!s->headless)                           /* (each display: its windows' feedback, wl_presented) */
        for (int i = 0; i < OUT_MAX; i++) {
            const Out *o = &s->out[i];
            if (!done[i] || !o->on || o->kms.flip_pending || o == mo) continue;
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            wl_presented(i, s->main_out, (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec, o->kms.flip_seq,
                         o->kms.mode.vrefresh ? 1000000000u / o->kms.mode.vrefresh : 16666667u);
        }
    if (main_shown && !s->headless && !mo->kms.flip_pending) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        wl_presented(s->main_out, s->main_out, ns, mo->kms.flip_seq, mo->kms.mode.vrefresh ? 1000000000u / mo->kms.mode.vrefresh : 16666667u);
        lbf_presented(ns, mo->kms.mode.vrefresh ? 1000000000u / mo->kms.mode.vrefresh : 16666667u);
        if (keylag_enabled) keylag_presented(ns);
    }
    if (!prep_only) frame_stats(s, now_ms() - t0, ui.layout_ms + ui.paint_ms);
    return !prep_only;
}

/* The theme the person chose (settings.json "theme"), next to the one asked for: a session starts with it instead of
 * loading the default theme only for its startShell to switch (0.8.8.10: one whole theme less evaluated at every
 * login, ~1.2 s and its memory). Read as the session user, after dropping privileges; the id is checked as
 * lord.themes.use checks it, and an unknown or missing one keeps the theme asked for. */
static void chosen_theme(const char *asked, char *out, size_t n) {
    snprintf(out, n, "%s", asked);
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) return;
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/.config/lordos/settings.json", pw->pw_dir) >= sizeof path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *text = malloc(4u << 20);
    size_t len = text ? fread(text, 1, (4u << 20) - 1, f) : 0;
    fclose(f);
    if (!text) return;
    text[len] = 0;
    JDoc doc;
    char err[64];
    if (!json_parse(text, len, &doc, err, sizeof err)) {
        const char *id = json_str(doc.root, "theme");
        int ok = id && *id && strlen(id) <= 64;
        for (const char *c = id; ok && *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') ok = 0;
        char a[PATH_MAX], b[PATH_MAX * 2];
        snprintf(a, sizeof a, "%s", asked);
        char *dir = dirname(a);                     /* .../themes/<id> */
        char root[PATH_MAX];
        snprintf(root, sizeof root, "%s", dir);
        snprintf(a, sizeof a, "%s", root);
        snprintf(root, sizeof root, "%s", dirname(a));   /* .../themes */
        size_t bl = ok ? (size_t)snprintf(b, sizeof b, "%s/%s/theme.bundle.js", root, id) : 0;
        if (ok && bl < n && !access(b, R_OK)) memcpy(out, b, bl + 1);
        /* (0.8.9.6.2) a community theme the person installed from a file (js_theme.c): unless it already ended too many
         * sessions in a row (then it is marked failed and the session starts in the theme asked for) */
        else if (ok && !jst_bundle_path(id, b, sizeof b) && strlen(b) < n && !access(b, R_OK) && !jst_session_starting(id))
            memcpy(out, b, strlen(b) + 1);
        json_free(&doc);
    }
    free(text);
}

/* Cambio de tema pedido desde JS (lord.themes.use): se descarta el contexto JS y el árbol. */
/* Loads the first theme that passes the verified-code check (sec/codesig.c): the one asked for,
 * then the default and the classic theme next to it; if none can be loaded because the system code
 * was modified, the built-in recovery screen (never a blank screen). The login screen goes straight
 * to the recovery screen. `used` gets the bundle that is running. 0 if something is on screen. */
static int start_theme(const char *theme, int greeter, char *used, size_t n) {
    snprintf(used, n, "%s", theme);
    int r = js_init(theme);
    if (r == 0) return 0;
    char cid[32];
    int community = jst_is_user_bundle(theme, cid, sizeof cid);
    if (community) {                    /* a community theme that didn't start: marked failed, the session goes on in ours */
        const char *why = js_init_error();
        snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", theme);
        snprintf(ui.theme_failed_why, sizeof ui.theme_failed_why, "%s", why[0] ? why : "the theme's code did not load (see the log)");
        ui.theme_failed_code = 0;
        jst_mark_failed(cid, ui.theme_failed_why);
    }
    if (!community && r != JS_REFUSED && codesig_state() != CODE_INVALID) return -1;
    if (r == JS_REFUSED) {              /* lord.themes.use won't go back to it */
        snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", theme);
        snprintf(ui.theme_failed_why, sizeof ui.theme_failed_why, "refused by the verified-code check (sec/codesig.c)");
        ui.theme_failed_code = 1;
        ui.theme_failed_gen = codesig_generation();
    }
    if (!greeter && codesig_state() != CODE_INVALID) {
        char root[PATH_MAX], tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", theme);
        char *d = dirname(tmp);                     /* .../themes/<id> */
        snprintf(root, sizeof root, "%s", d);
        snprintf(tmp, sizeof tmp, "%s", root);
        snprintf(root, sizeof root, "%s", dirname(tmp));   /* .../themes */
        if (jst_system_themes()[0]) snprintf(root, sizeof root, "%s", jst_system_themes());
        static const char *const fallback[] = { "default", "classic" };
        for (size_t i = 0; i < sizeof fallback / sizeof fallback[0]; i++) {
            char alt[sizeof ui.theme_request];
            if ((size_t)snprintf(alt, sizeof alt, "%.*s/%s/theme.bundle.js", (int)(sizeof alt - 32), root, fallback[i]) >= sizeof alt) continue;
            if (!strcmp(alt, theme) || access(alt, R_OK)) continue;
            js_shutdown();
            ui_reset();
            fprintf(stderr, "lord-server: theme %s was not loaded, trying %s\n", theme, alt);
            if (js_init(alt) == 0) { snprintf(used, n, "%s", alt); return 0; }
        }
    }
    js_shutdown();
    ui_reset();
    return js_init_recovery(greeter);
}

/* lord-server --check-apps: what lord.sign.status says of each installed app, as text, for lordos-issue and a person at a
 * terminal: a home app against its saved original, a packaged app against its signature. It changes nothing. */
static int check_apps(void) {
    int bad = 0;
    struct dirent *e;
    DIR *d = opendir(pkgapp_root());
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        PkgManifest m;
        LpkSigInfo si;
        char err[160] = "";
        memset(&si, 0, sizeof si);
        int r = pkgapp_verify(e->d_name, &m, &si, NULL, err, sizeof err);
        printf("packaged %s: %s", e->d_name, r ? "REFUSED " : "signature ok");
        if (r) { printf("%s", err); bad = 1; } else printf(" (%s, %s)", sig_trust_name(si.trust), si.signer);
        putchar('\n');
    }
    if (d) closedir(d);
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    d = home && (size_t)snprintf(dir, sizeof dir, "%s/.local/share/lordos/apps", home) < sizeof dir ? opendir(dir) : NULL;
    static PkgStatus st;
    while (d && (e = readdir(d))) {
        if (!lpk_valid_id(e->d_name) || pkg_status(e->d_name, &st)) continue;
        printf("home %s %s: ", e->d_name, st.version);
        if (st.error[0]) { printf("ORIGINAL DOESN'T VERIFY %s", st.error); bad = 1; }
        else if (!st.has_original) printf("no saved original");
        else if (st.modified) { printf("MODIFIED, %d file(s) (%s)", st.nchanged, sig_trust_name(st.sig.trust)); bad = 1; }
        else printf("as installed (%s, %s)", sig_trust_name(st.sig.trust), st.sig.signer);
        putchar('\n');
    }
    if (d) closedir(d);
    return bad;
}

/* lord-server --check-code [root]: checks the signed manifest and every file it lists, the way a
 * session would load them (docs/SIGNING.md, "Verified system code"). Used by tools/iso/mkdeb.sh
 * and for troubleshooting. Exit 0 when everything matches; 1 otherwise; with --require-signed, a
 * development (unsigned) manifest also fails. */
static int check_code(int argc, char **argv) {
    const char *root = NULL;
    int need_signed = codesig_release_build();
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--require-signed")) need_signed = 1;
        else root = argv[i];
    }
    char exe[PATH_MAX], def[PATH_MAX];
    if (!root) {                                    /* next to this binary: <root>/bin/lord-server */
        ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (k <= 0) return 1;
        exe[k] = 0;
        char *bin = dirname(exe);                   /* <root>/bin (dirname writes into exe) */
        snprintf(def, sizeof def, "%s/..", bin);
        root = def;
    }
    CodeState st = codesig_init_root(root, codesig_key(), need_signed);
    const CodeManifest *m = codesig_manifest();
    printf("root: %s\nstate: %s%s%s\nbuild: %s\n", codesig_root(), codesig_state_name(st), codesig_reason()[0] ? " — " : "",
           codesig_reason(), codesig_release_build() ? "release" : "development");
    int bad = st == CODE_INVALID || !m;
    for (int i = 0; m && i < m->nfiles; i++) {
        char path[PATH_MAX * 2], why[160];
        snprintf(path, sizeof path, "%s/%s", codesig_root(), m->files[i].path);
        size_t len;
        char *b = codesig_load(path, CODE_FILE_MAX, &len, why, sizeof why);
        printf("%-8s %s%s%s\n", b ? "ok" : "FAILED", m->files[i].path, b ? "" : ": ", b ? "" : why);
        if (!b) bad = 1;
        free(b);
    }
    printf("%s: %d file(s), %.1f ms\n", bad ? "FAILED" : "ok", m ? m->nfiles : 0, codesig_ms());
    return bad ? 1 : 0;
}

/* ---- the community themes' watchdog (js_theme.c; docs/THEMES-GUIDE.md, "Community themes") ----
 * A community theme that throws at its start, draws nothing within COMMUNITY_FIRST_MS, has its tree die (React's
 * uncaught error), or stalls the desktop until the main-loop watchdog stops it is taken off: it is marked failed in its
 * install record (with why, for Settings → Appearance's Report), and the theme that ran before it comes back (Lord
 * Flow when there was none). One that ends COMMUNITY_CRASHES sessions in a row is not started again (chosen_theme).
 * Ctrl+Alt+Shift+Backspace always restores Flow, whatever theme runs (on_input, before any JavaScript). */
#define COMMUNITY_FIRST_MS 6000
#define COMMUNITY_HEALTHY_MS 60000

static void default_bundle(char *out, size_t n) {
    snprintf(out, n, "%s/default/theme.bundle.js", jst_system_themes());
}

static void community_watch_start(const char *current, double now) {
    cw.since = now;
    cw.healthy = 0;
    default_bundle(cw.prev, sizeof cw.prev);
    (void)current;
}

static void community_watch_switched(const char *before, const char *now_running, double now) {
    cw.since = now;
    cw.healthy = 0;
    char id[32];
    /* the theme to come back to: the one before, unless it is the failed one */
    if (strcmp(before, ui.theme_failed)) snprintf(cw.prev, sizeof cw.prev, "%s", before);
    (void)now_running;
    (void)id;
}

static void community_back(const char *current, const char *why, int persist) {
    char id[32];
    snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", current);
    snprintf(ui.theme_failed_why, sizeof ui.theme_failed_why, "%s", why);
    ui.theme_failed_code = 0;
    if (persist && jst_is_user_bundle(current, id, sizeof id)) jst_mark_failed(id, why);
    char back[sizeof ui.theme_request];
    snprintf(back, sizeof back, "%s", cw.prev);
    if (!back[0] || !strcmp(back, current) || access(back, R_OK)) default_bundle(back, sizeof back);
    fprintf(stderr, "lord-server: theme %s taken off (%s): back to %s\n", current, why, back);
    snprintf(ui.theme_request, sizeof ui.theme_request, "%s", back);
}

static void community_watch(const char *current, double now) {
    if (cw.rescue) {                    /* Ctrl+Alt+Shift+Backspace: Flow, whatever runs */
        cw.rescue = 0;
        char def[sizeof ui.theme_request];
        default_bundle(def, sizeof def);
        if (strcmp(current, def) && !access(def, R_OK)) {
            snprintf(cw.prev, sizeof cw.prev, "%s", def);
            community_back(current, "restored with Ctrl+Alt+Shift+Backspace", 1);
        }
        return;
    }
    if (!js_community_theme() || ui.theme_request[0]) return;
    char id[32];
    if (!jst_is_user_bundle(current, id, sizeof id)) return;
    const char *why = jst_failure();
    if (!why && !jst_started() && now - cw.since > COMMUNITY_FIRST_MS) why = "it drew nothing within 6 seconds";
    if (!why && jst_stopped()) why = "it stalled the desktop and the watchdog stopped it";
    if (why) { community_back(current, why, 1); return; }
    if (!cw.healthy && now - cw.since > COMMUNITY_HEALTHY_MS) { cw.healthy = 1; jst_session_healthy(id); }
}

static unsigned renders_done;          /* frames drawn: a theme switch waits for two (its overlay on screen) */

/* 0 if the requested theme is running; -1 if it failed and `current` (the last one that loaded) was put
 * back: the caller keeps `current` then, so a second failed switch never falls back to a refused theme */
static int switch_theme(const char *current) {
    paint_blur_cache_reset();            /* the new theme's nodes may reuse the old ids */
    char path[sizeof ui.theme_request];
    snprintf(path, sizeof path, "%s", ui.theme_request);
    ui.theme_request[0] = 0;
    fprintf(stderr, "lord-server: cambiando a tema %s\n", path);
    share_shell_gone();                 /* the indicator goes with the old shell */
    cast_end_all();                     /* and with it what Linux apps were shown (the ScreenCast portal) */
    js_shutdown();
    bgsave_flush(NULL);                 /* the new theme reads settings.json and the clipboard's stores (bgsave.c) */
    ui_reset();
    term_detach_all();
    /* the bytes lord.themes.use verified when it was asked (js_sys.c f_themes_use): a refused theme never gets here */
    int r = js_init_prepared(path);
    if (r) {
        fprintf(stderr, "lord-server: the theme failed, going back to %s\n", current);
        snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", path);
        const char *why = js_init_error();     /* kept before the old theme's own start can overwrite it */
        snprintf(ui.theme_failed_why, sizeof ui.theme_failed_why, "%s", r == JS_REFUSED ? "refused by the verified-code check (sec/codesig.c)"
                 : why[0] ? why : "the theme's code did not load (see the log)");
        ui.theme_failed_code = r == JS_REFUSED;
        ui.theme_failed_gen = codesig_generation();
        char cid[32];
        if (jst_is_user_bundle(path, cid, sizeof cid)) jst_mark_failed(cid, ui.theme_failed_why);   /* kept for Report */
        js_shutdown();
        ui_reset();
        /* the theme that was running, from the bytes verified when it loaded: not its file again, which a package
         * update may have replaced under the session (js.c js_init_previous) */
        if (js_init_previous(current)) {
            /* it did not start again either (it can only be read again from disk when it was never kept). Never no
             * JavaScript at all: the main loop's next js_tick would call into a freed context (the 2026-11-22 13:41
             * crash, JS_GetGlobalObject(NULL)). The recovery screen instead, as at start (start_theme). */
            fprintf(stderr, "lord-server: %s did not load again either: the recovery screen\n", current);
            js_shutdown();
            ui_reset();
            if (js_init_recovery(0)) fprintf(stderr, "lord-server: the recovery screen did not load either\n");
        }
        wl_remap_all();
        return -1;
    }
    wl_remap_all();             /* Linux app windows stay connected: they come back in the new theme */
    return 0;
}

/* ---- Settings → Displays and the other hardware pages (src/hw, docs/SETTINGS.md) ---- */

static Server *hw_server;
static Kms *hw_kms(void) { Server *s = hw_server; return s && !s->headless && K0(s)->fd >= 0 ? K0(s) : NULL; }
static int hw_heads(Kms **out, int max) {
    Server *s = hw_server;
    int n = 0;
    if (!hw_kms()) return 0;
    for (int i = 0; i < OUT_MAX && n < max; i++) if (s->out[i].on && s->out[i].kms.fd >= 0) out[n++] = &s->out[i].kms;
    return n;
}
static void hw_redraw(void) { if (hw_server) full_damage(hw_server); }
static const DisplayHost hw_host = { hw_kms, hw_redraw, js_hw_event, hw_heads };

const char *lord_composition(void) {
    Server *s = hw_server;
    if (!s) return "";
    if (gpu_active()) return "gpu";
    if (s->headless) return "headless";
    if (out_count(s) < 1) return "no-display";
    if (display_cpu_only()) return "rotated";
    if (need_cpu()) return "capture";
    return "gpu-off";
}

/* A client that hangs up mid-write (remote desktop, Wayland, a helper) must not kill the
 * session: writes get EPIPE instead. Caught, not SIG_IGN, so exec'd children (shells, Linux
 * apps) get the default SIGPIPE back, as execve resets caught signals. */
static void on_sigpipe(int sig) { (void)sig; }

#ifdef LORD_TEST_HOOKS
#define USAGE_HEADLESS "[--headless WIDTHxHEIGHT [--greeter | --test-hooks | --hands-qa]]"
#else
#define USAGE_HEADLESS "[--headless WIDTHxHEIGHT [--greeter | --hands-qa]]"
#endif
static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--theme bundle.js] [--fonts dir] [--shell dir] [--user name] [--card /dev/dri/cardN]\n"
                    "          " USAGE_HEADLESS " [--timeout s] [--debug] [--profile]\n", argv0);
}

/* SIGINT or SIGTERM that reached a worker thread: to the main loop's signalfd */
static pthread_t loop_thread;
static void forward_stop(int sig) { pthread_kill(loop_thread, sig); }

/* (0.8.9.1) one memory report: the process (status, huge pages), glibc's heap (mallinfo2: in use against free
 * but kept), the QuickJS runtime, the image cache, the per-display scenes. Measured right after a malloc_trim. */
static void mem_report(Server *s) {
    malloc_trim(0);
    long rss = 0, anon = 0, file = 0, thp = 0;
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "VmRSS: %ld", &rss); sscanf(line, "RssAnon: %ld", &anon); sscanf(line, "RssFile: %ld", &file);
        }
        fclose(f);
    }
    if ((f = fopen("/proc/self/smaps_rollup", "r"))) {
        while (fgets(line, sizeof line, f)) sscanf(line, "AnonHugePages: %ld", &thp);
        fclose(f);
    }
    struct mallinfo2 mi = mallinfo2();
    int ic = 0, idle = 0;
    size_t ib = 0;
    image_cache_stats(&ic, &idle, &ib);
    long scenes = 0;
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].scene.px) scenes += (long)s->out[i].scene.w * s->out[i].scene.h * 4;
    fprintf(stderr, "memory: rss=%ldK anon=%ldK file=%ldK thp=%ldK | malloc arena=%zuK inuse=%zuK free=%zuK mmap=%zuK(%zu) "
            "top=%zuK | images=%d idle=%d %zuK | scenes=%ldK\n", rss, anon, file, thp, mi.arena >> 10, mi.uordblks >> 10,
            mi.fordblks >> 10, mi.hblkhd >> 10, mi.hblks, mi.keepcost >> 10, ic, idle, ib >> 10, scenes >> 10);
    int tn = 0, nb = 0, nt = 0;
    long tl = 0;
    size_t tc = 0, tr = 0, bb = 0, tb = 0, gn = 0, gb = 0, lb = 0;
    term_mem_stats(&tn, &tl, &tc, &tr);
    paint_mem_stats(&nb, &bb, &nt, &tb, &lb);
    text_mem_stats(&gn, &gb);
    fprintf(stderr, "memory: terminals=%d lines=%ld cells=%zuK replay=%zuK | blur=%d %zuK | transform=%d %zuK | layer=%zuK | glyphs=%zu %zuK\n",
            tn, tl, tc >> 10, tr >> 10, nb, bb >> 10, nt, tb >> 10, lb >> 10, gn, gb >> 10);
    long frames = 0;
    for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].frame) frames += (long)s->out[i].scene.w * s->out[i].scene.h * 4;
    fprintf(stderr, "memory: nodes=%d %zuK (Node %zu B) | frames=%ldK\n", node_count(), (size_t)node_count() * sizeof(Node) >> 10,
            sizeof(Node), frames >> 10);
    /* glibc's arenas one by one (threads get their own): heap number and its current size */
    char *mx = NULL;
    size_t mxn = 0;
    FILE *ms = open_memstream(&mx, &mxn);
    if (ms) {
        malloc_info(0, ms);
        fclose(ms);
        char ar[512];
        size_t ao = 0;
        int nr = -1;
        ar[0] = 0;
        for (char *q = mx; q && *q && ao + 40 < sizeof ar; q = strchr(q + 1, '<')) {
            long long v;
            if (sscanf(q, "<heap nr=\"%d\"", &nr) == 1) continue;
            if (nr >= 0 && sscanf(q, "<system type=\"current\" size=\"%lld\"", &v) == 1) {
                ao += (size_t)snprintf(ar + ao, sizeof ar - ao, " #%d=%lldK", nr, v >> 10);
                nr = -1;
            }
        }
        fprintf(stderr, "memory: arenas:%s\n", ar);
        free(mx);
    }
    char td[1024];
    paint_tf_detail(td, sizeof td);
    if (td[0]) fprintf(stderr, "memory: transform rasters:%s\n", td);
    char js[512];
    js_mem_line(js, sizeof js);
    fprintf(stderr, "memory: %s\n", js);
}

/* lord-server's static arrays (.bss, 31 MB) are sized for the worst case and mostly untouched. With
 * transparent huge pages set to "always", khugepaged turned each 2 MB region with any page in use into a
 * whole huge page, and about 16 MB of them stayed resident for ~1 MB of data (0.8.9.1, heaptrack and
 * /proc/<pid>/smaps). Only this range opts out; the heap keeps huge pages. */
static void bss_no_huge_pages(void) {
    extern char __bss_start[], _end[];
    uintptr_t a = (uintptr_t)__bss_start, b = (uintptr_t)_end, pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f || pg == 0 || pg == (uintptr_t)-1) { if (f) fclose(f); return; }
    char l[512];
    while (fgets(l, sizeof l, f)) {             /* the anonymous mappings the range covers (there may be a hole) */
        unsigned long s0, e0, ino;
        char perm[8];
        unsigned off, dmaj, dmin;
        if (sscanf(l, "%lx-%lx %7s %x %x:%x %lu", &s0, &e0, perm, &off, &dmaj, &dmin, &ino) != 7 || ino) continue;
        uintptr_t x = s0 > a ? s0 : a, y = e0 < b ? e0 : b;
        x = (x + pg - 1) & ~(pg - 1);
        y &= ~(pg - 1);
        if (y > x) madvise((void *)x, y - x, MADV_NOHUGEPAGE);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    bss_no_huge_pages();
    wdog_crash_hooks(LORD_VERSION " (" LORD_BUILD ")");     /* a crash leaves its C stack in the log (wdog.h) */
    struct sigaction pipe_sa = { .sa_handler = on_sigpipe };
    sigemptyset(&pipe_sa.sa_mask);
    sigaction(SIGPIPE, &pipe_sa, NULL);
    /* guardián de una sesión de terminal (ver term/hold.c): no abre pantalla ni input */
    if (argc > 1 && !strcmp(argv[1], "--term-holder")) return hold_main(argc, argv);
    /* a restricted Linux app's launch, re-executed clean to build its view (apps.c, apps_launch_view_main) */
    if (argc > 1 && !strcmp(argv[1], "--launch-view")) return apps_launch_view_main();
    /* (0.8.5) a component's network connector (sec/netconn.h): only creates sockets in its scope */
    if (argc > 1 && !strcmp(argv[1], "--net-connector")) return netconn_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--check-code")) return check_code(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--check-apps")) return check_apps();
    /* a packaged app's companion (app/companion.c): checks the signed package, then becomes it, in its sandbox */
    if (argc > 1 && !strcmp(argv[1], "--companion")) return companion_main(argc, argv);
    /* «modo app»: lord-server --app <id> corre una app .lpk como cliente Wayland aislado
     * (app/appclient.c). El id ya viene validado del propio lord-server (no de JS). */
    if (argc >= 3 && !strcmp(argv[1], "--app")) {
        const char *fonts = "../assets/fonts", *host = "../assets/app-host.bundle.js";
        for (int i = 3; i + 1 < argc; i++) {
            if (!strcmp(argv[i], "--fonts")) fonts = argv[++i];
            else if (!strcmp(argv[i], "--host")) host = argv[++i];
        }
        return app_main(argv[2], host, fonts);
    }
    const char *card = NULL;               /* sin --card: la que tiene pantalla (drmcard.h) */
    const char *theme = "../themes/default/theme.bundle.js";
    const char *fonts = "../assets/fonts";
    const char *shell = "../assets/shell";     /* integración de la terminal con bash */
    const char *user = NULL;
    double timeout_ms = 0;
#ifdef LORD_TEST_HOOKS
    int test_hooks = 0;
#endif
    static Server s;
    int hands_qa_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout_ms = atof(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(argv[i], "--fonts") && i + 1 < argc) fonts = argv[++i];
        else if (!strcmp(argv[i], "--shell") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) {
            /* sesión sin monitor para el escritorio remoto (la lanza lord-sessiond) */
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 640 || h < 480 || w > 3840 || h > 2400) { usage(argv[0]); return 2; }
            s.headless = 1;
            s.W = w & ~1;
            s.H = h & ~1;
        }
        else if (!strcmp(argv[i], "--greeter")) s.greeter = 1;      /* pantalla de ingreso (lord-sessiond) */
        /* Hands' QA desktop (docs/HANDS.md): a private headless session an agent drives with no prompt; lordos-mcp
         * --headless starts it with a private bus and a demo home. Never with a monitor or on the login screen. */
        else if (!strcmp(argv[i], "--hands-qa")) hands_qa_flag = 1;
#ifdef LORD_TEST_HOOKS
        else if (!strcmp(argv[i], "--test-hooks")) test_hooks = 1;  /* the conformance tool's socket (testhooks.h) */
#endif
        else if (!strcmp(argv[i], "--debug")) s.debug = 1;
        else if (!strcmp(argv[i], "--profile")) paint_profile = 1;
        else { usage(argv[0]); return 2; }
    }

    if (s.greeter && !s.headless) { usage(argv[0]); return 2; }
    if (hands_qa_flag && (!s.headless || s.greeter)) { usage(argv[0]); return 2; }
#ifdef LORD_TEST_HOOKS
    if (test_hooks && (!s.headless || s.greeter)) { usage(argv[0]); return 2; }   /* never on a monitor or the login screen */
#endif
    /* a test's session (--timeout, the test hooks) never drives LordOS Studio's loopback: /dev/video50 is the whole
     * computer's, and the person's own session may be feeding it to a video call */
    if (timeout_ms > 0) mstudio_hub_off();
#ifdef LORD_TEST_HOOKS
    if (test_hooks) mstudio_hub_off();
#endif
    js_sys_set_greeter(s.greeter);
    display_set_greeter(s.greeter);     /* #242: the login screen's scale (LORD_MAIN_SCALE) */
    wl_set_greeter(s.greeter);          /* the login screen never lets an app take the shortcuts */
    if (text_init(fonts)) return 1;
    {   /* assets/ = la carpeta de las fuentes, un nivel arriba */
        char a[1024];
        snprintf(a, sizeof a, "%s/..", fonts);
        js_sys_set_assets(a);
    }
    term_init(fonts, shell);
    {   /* the computer's layout until the UI applies the user's (sdk/keyboard.js) */
        char l[128], v[128];
        kb_system_from(KB_SYSTEM_FILE, l, sizeof l, v, sizeof v);
        if (kb_set(l, v)) kb_set("us", "");
        /* Num Lock on, as the firmware of most desktops leaves it: with it off the keypad moves the cursor and
         * looks dead. Not on a laptop, whose keypad is often printed on the letter keys (U I O = 4 5 6) */
        if (!input_portable_computer()) kb_set_numlock(1);
    }
    for (int i = 0; i < OUT_MAX; i++) { s.out[i].kms.fd = -1; s.out[i].gpu = -1; }
    s.cap.kms.fd = -1;
    s.cap.gpu = -1;
    if (s.headless) {
        int w = s.W, h = s.H;
        s.W = s.H = 0;
        set_size(&s, w, h);                 /* out[0] = the frame in memory */
    }
    else {
        char picked[64];
        if (!card) { drm_pick_card(picked, sizeof picked); card = picked; }
        kms_set_picker(display_pick);       /* the display saved in Settings (displays.json) */
        if (kms_open(K0(&s), card)) return 1;
        uevent_set_card(uevent_card_of_fd(K0(&s)->fd));    /* only this card's drm uevents are display hotplugs */
        /* one display until the plan (after dropping root: displays.json is the user's) opens the others */
        s.W = (int)K0(&s)->width;
        s.H = (int)K0(&s)->height;
        s.out[0].on = 1;
        s.out[0].v = out_view(0, 0, s.W, s.H, 100);
        s.out[0].d.scale = 100;
        out_frames(&s.out[0], 0);
    }
    ui.width = (float)s.W;
    ui.height = (float)s.H;
    s.cx = s.lx = s.W / 2;
    s.cy = s.ly = s.H / 2;
    ptrlock_init(ptrlock_rect);
    wl_tablet_init();                           /* before the devices open: tablets are announced to the apps */

    int vt = -1;
    if (!s.headless) {
        if (input_open(&s.input)) fprintf(stderr, "input: no devices (permissions?)\n");
        /* The text console stops drawing over us: our own (startlordos runs on the console it was typed on, which the
         * person owns), else the one in front (root) */
        const char *own = isatty(STDIN_FILENO) ? ttyname(STDIN_FILENO) : NULL;
        vt = own && !strncmp(own, "/dev/tty", 8) && own[8] >= '1' && own[8] <= '9' ? open(own, O_RDWR | O_CLOEXEC | O_NOCTTY) : -1;
        if (vt < 0) vt = open("/dev/tty0", O_RDWR | O_CLOEXEC);
        if (vt >= 0 && ioctl(vt, KDSETMODE, KD_GRAPHICS)) { close(vt); vt = -1; }
        snprintf(vt_card, sizeof vt_card, "%s", card);
    }

    /* Privileged helper (a closed set of actions: region, time zone, language packs). The
     * login screen has none: what needs root is done by lord-sessiond. */
    const char *session_user = s.greeter ? NULL : user ? user : getenv("SUDO_USER");
    if (session_user && admin_start(session_user)) fprintf(stderr, "lord-server: no administration helper\n");
    /* LordOS networking: with lord-sessiond the network belongs to the system (lord-netd brings it
     * up at boot) and net-restore only confirms; without it (a standalone lord-server) this
     * session's helper brings it up (see net_act_restore and docs/NETWORK.md) */
    else if (session_user) admin_send("net-restore", "", 0);
    int ok = drop_privileges(user) == 0;
    /* the log folder (lordlog.h): /var/log/lordos/users/<user>, which lord-sessiond made at login, or
     * ~/.cache/lordos; worked out once, here, before anything forks a child that opens a log */
    if (ok) lordlog_dir();
    /* the open files an app's buffers need (nofile.h, GitHub #251): the soft limit raised towards the hard one; every
     * child forked from here on gets the usual one back */
    if (ok) {
        rlim_t nf0 = 0, nf1 = 0;
        if (nofile_raise(&nf0, &nf1)) fprintf(stderr, "lord-server: the open files' limit stays at %llu\n", (unsigned long long)nf0);
        else if (nf1 != nf0) fprintf(stderr, "lord-server: open files allowed: %llu (was %llu)\n", (unsigned long long)nf1, (unsigned long long)nf0);
    }
    if (ok && s.headless && getenv("HOME")) {
        /* no terminal: stderr is stamped into compositor.log and shell.log there, the previous
         * session's kept as .prev (a small lord-log child; it outlives a crash, so nothing written is lost) */
        if (lordlog_session_start()) fprintf(stderr, "lord-server: no session log\n");
    }
    /* the CPU path's pixel loops on a pool (par.h): blur, blits, gradients, the remote desktop's
     * comparison and copies; same pixels as one thread. Its threads sleep between jobs, and jobs only
     * run inside a frame on this thread, so the processes this server forks never inherit a busy one.
     * Here, before this server starts any process or thread: it takes the real-time priority
     * lord-sessiond allows and lowers that limit to 0, so nothing started later inherits it. */
    par_init(0);
    /* GPU (Mesa) after dropping root: if there is no usable one, the CPU path goes on */
    /* Settings' hardware pages: displays, uevents, Bluetooth, power profiles (src/hw) */
    kms_set_picker(display_pick);
    hw_server = &s;
    /* the login screen too, without the bus: its DRM uevents bring a monitor plugged in after boot
     * (a card parked by display_attach) and the other displays */
    if (ok) hw_start(&hw_host, !s.greeter);
    if (ok && !s.headless) { display_take_resync(); outputs_apply(&s); }   /* every display, the GPU if it can */
    {
        struct stat st;
        if (K0(&s)->fd >= 0 && fstat(K0(&s)->fd, &st) == 0) wl_set_drm_device(st.st_rdev);
    }
    if (ok && wl_start()) fprintf(stderr, "lord-server: no Wayland (Linux apps will not be able to open)\n");
    /* X11 apps: an X socket now, Xwayland only when one connects (wl/xwayland.c); not on the login screen */
    else if (ok && !s.greeter && session_user) xwl_init();
    if (ok && !s.headless && !s.greeter) drmlease_card(K0(&s)->fd);   /* a VR headset already plugged in (wl/drmlease.c) */
    /* the session bus's address for every app the session starts (a terminal, the launcher, session
     * restore): without it Google Chrome has no bus, so no LordOS notifications, tray or portal */
    if (ok && !s.greeter && dbus_session_export()) fprintf(stderr, "lord-server: DBUS_SESSION_BUS_ADDRESS=%s\n", getenv("DBUS_SESSION_BUS_ADDRESS"));
    publish_displays(&s);                   /* one wl_output per display */
    if (ok && net_open()) fprintf(stderr, "lord-server: no network notifications (rtnetlink)\n");
    /* Backups: a thread running as the user, nice 19 and idle I/O (backup/backup.c) */
    if (ok && !s.greeter && getenv("HOME") && session_user) backup_start(getenv("HOME"), session_user);
    /* Cloud: rclone rcd on a Unix socket, started when there is an account (cloud/cloud.c, docs/CLOUD.md) */
    if (ok && !s.greeter && getenv("HOME") && session_user && js_cloud_start(getenv("HOME"))) fprintf(stderr, "lord-server: no Cloud\n");
    if (ok && snd_start()) fprintf(stderr, "lord-server: no PipeWire for now (sound); it retries on its own\n");
    wave_source(snd_spectrum_now());            /* <SoundWave> nodes draw the sound client's spectrum */
    if (ok && !s.greeter) portal_start();   /* dark mode, accent and buttons for GTK/GNOME/Qt (ui/portal.c) */
    if (ok && !s.greeter) sni_start();      /* the tray icons of Linux apps (ui/sni.c) */
    if (ok && !s.greeter) term_restore();   /* shells still alive from the previous start */
    netrec_init(ok && !s.greeter && session_user != NULL);   /* the Monitor's traffic history (monitor/netrec.c) */
    if (getenv("LORD_DAMAGE") && !strcmp(getenv("LORD_DAMAGE"), "one")) paint_multi_damage = 0;    /* one box per frame, as before 0.8.8.10 (paint.c) */
    remote_server = &s;
    hands_init(&hands_host_impl);           /* Hands (hands/hands.h): off until the person turns it on, or --hands-qa */
    if (hands_qa_flag) { hands_qa(1); js_sys_set_demo_identity(1); fprintf(stderr, "lord-server: Hands QA desktop: an agent may use this private session (docs/HANDS.md)\n"); }
    tp_route_init(&tp_host);
    wl_warp_set_host(warp_move);                /* pointer-warp-v1 (wl/wl_warp.c) */
    static const MirrorInput mirror_in = { remote_pointer, remote_button, remote_wheel, remote_key, remote_clip, remote_clients,
                                           remote_refresh, remote_resize, remote_greeter, display_attach, display_detach,
                                           remote_scroll, remote_pinch, remote_input_add, remote_notice };
    if (ok && mirror_start(s.W, s.H, &mirror_in)) fprintf(stderr, "lord-server: no remote desktop (socket)\n");
    /* never for the login screen: that session has no share socket at all */
    static const ShareHost share_host = { remote_pointer, remote_button, remote_wheel, remote_scroll, remote_key, remote_clip, remote_resize, share_activity,
                                          share_text, js_share_event, share_no_taps, share_person, share_surface_up, share_key_protected,
                                          share_point_protected, share_restricted, share_displays_of, remote_refresh };
    if (ok && !s.greeter && share_start(&share_host)) fprintf(stderr, "lord-server: no screen-sharing API (socket)\n");
#ifdef LORD_TEST_HOOKS
    static const TestHooks test_host = { test_pointer, remote_button, remote_wheel, test_tablet, test_tablet_event, wl_test_xwl, drmlease_fake_card, test_key,
                                         test_frame, test_painted, test_where };
    if (ok && test_hooks && testhooks_start(&test_host)) fprintf(stderr, "lord-server: no test hooks socket\n");
#endif
    char current[sizeof ui.theme_request];
    svgicon_enable();                       /* SVG icons drawn by lord-media (media/svgicon.c); never in app mode */
    char first[sizeof ui.theme_request];
    {   /* the system's themes folder: the one holding the theme asked for (community themes live in the person's) */
        char a[PATH_MAX], r[PATH_MAX];
        if (realpath(theme, r)) {
            snprintf(a, sizeof a, "%s", r);
            char *d = dirname(a);
            snprintf(r, sizeof r, "%s", d);
            snprintf(a, sizeof a, "%s", dirname(r));
            jst_set_system_themes(a);
        }
    }
    if (s.greeter) snprintf(first, sizeof first, "%s", theme);
    else chosen_theme(theme, first, sizeof first);
    if (ok) ok = start_theme(first, s.greeter, current, sizeof current) == 0;
    if (ok && !s.greeter) {                  /* the app host as bytecode for isolated apps (app/apphost_bc.c) */
        char host[1200];
        snprintf(host, sizeof host, "%s/app-host.bundle.js", js_sys_assets());
        apphost_bc_start(host);              /* reads it through codesig_load and compiles those bytes */
    }
    if (!ok) {
        input_close(&s.input);
        if (!s.headless) kms_close(K0(&s));
        if (vt >= 0) { ioctl(vt, KDSETMODE, KD_TEXT); close(vt); }
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    /* the worker threads started before this don't block them, and the default action there would
     * end the process without leaving the loop: the end of a session must always run */
    loop_thread = pthread_self();
    struct sigaction stop_sa = { .sa_handler = forward_stop, .sa_flags = SA_RESTART };
    sigemptyset(&stop_sa.sa_mask);
    sigaction(SIGINT, &stop_sa, NULL);
    sigaction(SIGTERM, &stop_sa, NULL);
    keylag_init(s.greeter);                 /* key latency trace, only with LORD_INPUT_DEBUG=1 (keylag.h) */
    fpipe_init(&fpipe, getenv("LORD_PIPELINE"));
    fprintf(stderr, "lord-server: frame pipeline %s (LORD_PIPELINE: unset or 0 off, 1 while frames are heavy, 2 always)\n",
            fpipe.mode == FPIPE_OFF ? "off" : fpipe.mode == FPIPE_ALWAYS ? "always" : "while frames are heavy");
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    /* text consoles, standalone: VT_PROCESS on our console once the loop can answer (vt_signal) */
    if (vt >= 0) {
        sigset_t vm;
        sigemptyset(&vm);
        sigaddset(&vm, SIGUSR1);
        sigaddset(&vm, SIGUSR2);
        sigprocmask(SIG_BLOCK, &vm, NULL);
        struct sigaction fw = { .sa_handler = forward_stop, .sa_flags = SA_RESTART };   /* a worker thread passes it on */
        sigemptyset(&fw.sa_mask);
        sigaction(SIGUSR1, &fw, NULL);
        sigaction(SIGUSR2, &fw, NULL);
        vt_sfd = signalfd(-1, &vm, SFD_CLOEXEC | SFD_NONBLOCK);
        struct vt_mode m = { .mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2 };
        if (vt_sfd >= 0 && !ioctl(vt, VT_SETMODE, &m)) vt_tty = vt;
        else fprintf(stderr, "lord-server: no text consoles (VT_PROCESS): %s\n", strerror(errno));
    }

    double start = now_ms();
    /* the watchdog (wdog.h): a stall of this loop never leaves the desktop frozen at real time */
    wdog_start();
    wdog_busy(mono_ns());
    community_watch_start(current, now_ms());
    while (!s.quit && !ui.quit_requested) {
        if (!s.greeter) community_watch(current, now_ms());
        if (ui.theme_request[0]) {
            /* two frames first (or 300 ms when none is drawn): the shell's "changing the desktop style" overlay (sdk
             * themeswitch.jsx) gets on screen, and stays there while the new theme loads behind it */
            static double asked;
            static unsigned asked_renders;
            if (!asked) { asked = now_ms(); asked_renders = renders_done; }
            if (renders_done - asked_renders >= 2 || now_ms() - asked >= 300) {
                asked = 0;
                char next[sizeof current];
                snprintf(next, sizeof next, "%s", ui.theme_request);
                wdog_expect(1);                 /* a theme loads for a second or more by design */
                char before[sizeof current];
                snprintf(before, sizeof before, "%s", current);
                if (!switch_theme(current)) snprintf(current, sizeof current, "%s", next);
                wdog_expect(0);
                if (strcmp(before, current)) community_watch_switched(before, current, now_ms());
            }
        }
        {   /* a pointer lock started, ended, or its owner moved the pointer image it shows (pointerlock.h) */
            static unsigned plk;
            int px, py;
            /* checked each time round: the node may have moved, or gone (then the lock ends here) */
            if (ptrlock_active() && ptrlock_where(&px, &py) && (px != s.cx || py != s.cy)) {
                s.cx = px; s.cy = py;
                clamp_pointer(&s);
                s.lx = s.cx; s.ly = s.cy;
                s.moved = 1;
                s.motion_pending = 1;
                if (s.hw_cursor) hw_cursor_move(&s);
            }
            if (ptrlock_serial() != plk) {
                plk = ptrlock_serial();
                s.cursor_dirty = 1;
                if (gpu_active()) update_cursor(&s);
                if (s.hw_cursor) hw_cursor_move(&s);     /* the shown image moved, or the lock ended */
            }
            js_pointer_lock_flush();        /* JavaScript's lock: its motion, once a frame (js_input.c) */
        }
        /* El movimiento del puntero juntado: una vez por cuadro, cuando terminó el page flip. Así un
         * mouse de 1000 Hz no llama a JS (ni a las apps Linux) mil veces por segundo. */
        if (s.motion_pending && !out_blocked(&s, main_out(&s))) {
            dispatch_pointer(&s);
            if (gpu_active()) update_cursor(&s);
        }
        /* LordBench's scene moves its window one step per frame, as a drag does (ui/grab.c) */
        if (grab_scripted() && !any_blocked(&s)) grab_script_tick(now_ms());
        static int scripted;                    /* its end: the layer's counters, as a drag's line has them */
        if (scripted && !grab_scripted()) {
            long lr = 0, lb = 0, lp = paint_layer_parts();
            paint_layer_stats(&lr, &lb);
            char bs[192], gs[512], ps[1536];
            below_stats_text(bs, sizeof bs);
            gdrag_stats_text(gs, sizeof gs);
            LbPhaseStats ph;
            lbf_drag_end(&ph);
            lbf_phases_text(&ph, ps, sizeof ps);
            fprintf(stderr, "drag (scripted): layer: %ld painted (%ld of them a part), %ld copied (%s); %s; %s; %s\n",
                    lr, lp, lb, gpu_active() ? "GPU" : "CPU", bs, gs, ps);
        }
        if (!scripted && grab_scripted()) {     /* its start: what came before is not counted */
            long lr, lb;
            BelowStats b;
            GdragStats g;
            paint_layer_stats(&lr, &lb);
            paint_layer_parts();
            paint_below_stats(&b);
            paint_gdrag_stats(&g);
            long q[3];
            gpu_gdrag_stats(&q[0], &q[1], &q[2]);
            lbf_drag_begin();                   /* its frames' phases (bench/lbframes.h) */
        }
        scripted = grab_scripted();
        /* Timers del tema (React agenda su trabajo con setTimeout/setImmediate). */
        double delay = -1, tick0 = now_ms();
        for (int i = 0; i < 64; i++) {
            delay = js_tick(now_ms());
            if (delay != 0) break;
        }
        if (dstat.active) dstat.js += now_ms() - tick0;     /* React agenda su trabajo en timers */
        /* Terminales: parpadeo del cursor y región que cambió con la salida de los shells. */
        double now = now_ms();
        term_tick(now);
        if (term_damage()) ui.dirty = 1;
        double td = term_timeout(now);
        if (td >= 0 && (delay < 0 || td < delay)) delay = td;
        tp_tick(now);                       /* touchpad: tocar, arrastrar, inercia (tp/tp.c) */
        {   /* Hands' human mode: a glide, a drag, a scroll's steps, typing; the agent cursor's ripple and fade (handscur.h) */
            hands_tick();
            handscur_keep(hands_acting());
            handscur_tick(now_ms());
            double hd = hands_timeout(), cd = handscur_timeout(now_ms());
            if (hd >= 0 && (delay < 0 || hd < delay)) delay = hd;
            if (cd >= 0 && (delay < 0 || cd < delay)) delay = cd;
        }
        if (ptrlock_waiting() && (delay < 0 || delay > 50)) delay = 50;     /* a pointer lock whose node went away */
        /* shake to find it: the cursor grows and shrinks back (a new size serial redraws it below) */
        cursor_set_boost(cursor_shake() ? shake_level(&pointer_shake, now) : 0);
        double shd0 = cursor_shake() ? shake_timeout(&pointer_shake, now) : -1;
        if (shd0 >= 0 && (delay < 0 || shd0 < delay)) delay = shd0;
        static unsigned cursor_size_seen;   /* Settings → Mouse & Trackpad: pointer size */
        if (cursor_size_serial() != cursor_size_seen) {
            cursor_size_seen = cursor_size_serial();
            s.cursor_dirty = 1;
            if (gpu_active()) update_cursor(&s);
        }
        /* a video or a presentation keeps the screen on: a Wayland surface's inhibitor or a portal hold (ui/portal.c) */
        if (wl_idle_inhibited() || portal_inhibited()) display_activity(now_ms());
        {   /* apps the person let know they're away (ext-idle-notify; inhibitors count as activity above) */
            double idd = wl_idle_tick(display_last_activity(), now_ms());
            if (idd >= 0 && (delay < 0 || idd < delay)) delay = idd;
        }
        display_tick(now_ms());             /* screen off, lock and sleep after N minutes (src/hw) */
        double hwd = display_timeout(now_ms());
        if (hwd >= 0 && (delay < 0 || hwd < delay)) delay = hwd;
        double bwd = hw_timeout(now_ms());  /* the battery's percentage, read again every 30 s (src/hw) */
        if (bwd >= 0 && (delay < 0 || bwd < delay)) delay = bwd;
        /* Settings → Displays or a hotplug: displays, modes, scales, arrangement (between frames) */
        if (!s.headless && !any_flip_pending(&s) && display_take_resync()) outputs_apply(&s);
        netrec_tick();                      /* traffic history: every 5 s while on (monitor/netrec.c) */
        double nrd = netrec_timeout();
        if (nrd >= 0 && (delay < 0 || nrd < delay)) delay = nrd;
        double tpd = tp_timeout(now_ms());
        if (tpd >= 0 && (delay < 0 || tpd < delay)) delay = tpd;
        share_tick();                       /* the API: its limits, the grant still holding */
        wl_fullscreen_allow(!share_holder());
        share_text_pump();
        if (share_text_len) delay = 0;
        double shd = share_timeout();
        if (shd >= 0 && (delay < 0 || shd < delay)) delay = shd;
        mirror_tick();                      /* remote desktop video: lossless tiles once still, the encoder */
        double mrd = mirror_timeout();
        if (mrd >= 0 && (delay < 0 || mrd < delay)) delay = mrd;
        media_reap();                       /* a stopped lord-media: not left a zombie */
        webmgr_reap();                      /* nor a lord-web that ended */
        webalive_tick(now_ms());            /* and one that stopped answering is started again (ui/webalive.h) */
        double wad = webalive_timeout(now_ms());
        if (wad >= 0 && (delay < 0 || wad < delay)) delay = wad;
        double zd = media_reap_timeout();
        if (zd >= 0 && (delay < 0 || zd < delay)) delay = zd;
        /* sin monitor y sin nadie mirando no se dibuja (al conectarse alguien llega un cuadro entero) */
        /* LORD_HEADLESS_DRAW=1 (tests/perf_headless.sh): headless and nobody watching, it still lays out and paints */
        static int headless_draw = -1;
        if (headless_draw < 0) headless_draw = getenv("LORD_HEADLESS_DRAW") != NULL || hands_qa_mode();   /* an agent's QA desktop: laid out and painted */
        int unseen = s.headless && !headless_draw && !mirror_watchers() && !media_needs_cpu();   /* recording the screen: yes */
        if (!s.headless && display_is_off()) unseen = 1;     /* screen off after idle (DPMS): nothing to draw */
        js_sys_unseen = unseen;                     /* the desk widgets' data feeds rest too (lord.system.seen) */
        /* moving wallpapers (media/mambient.c): which views show, their pause and stop, the loop cache's next frame */
        media_ambient_tick(unseen);
        {
            double ad = media_ambient_due();
            if (ad >= 0 && (delay < 0 || ad < delay)) delay = ad;
        }
        /* the minimized windows and those on a hidden desktop: their frame callbacks once a second (wl/framegate.h) */
        if (!unseen) {
            double hd = wl_frame_hidden((unsigned)now_ms());
            if (hd >= 0 && (delay < 0 || hd < delay)) delay = hd;
        }
        {   /* a Linux window's new title waiting for its frame slot (wl.c set_title) */
            double td = wl_title_wait();
            if (td >= 0 && (delay < 0 || td < delay)) delay = td;
        }
        /* nothing is drawn: the Linux apps waiting for a frame get it at most once a second (wl.c) */
        if (unseen) {
            /* a frame prepared before the screen went off would be shown stale when it comes back */
            prep_undo_all(&s);
            static double unseen_tick;
            double since = now_ms() - unseen_tick;
            if (since >= 1000) { wl_frame_unseen((unsigned)now_ms()); unseen_tick = now_ms(); since = 0; }
            if (wl_wants_frame() || ui.dirty) {
                double left = 1000 - since;
                if (delay < 0 || left < delay) delay = left;
            }
        }
        js_input_flush();                   /* the on-screen keyboard's taps (js_input.c), as physical keys */
        /* the frame pipeline (fpipe.h): a prepared frame whose flip is done is submitted, and the next one may
         * be painted while every display waits for its flip */
        {   /* a machine's guest changed its pointer (or the scale it's drawn at) while the host's is over it */
            static unsigned mcur;
            if (media_cursor_serial() != mcur) { mcur = media_cursor_serial(); if (s.over_media) s.cursor_dirty = 1; }
        }
        int ahead = !unseen && can_prepare(&s);
        int wants = !unseen && (ui.dirty || s.cursor_dirty || js_wants_frame() || wl_wants_frame() || media_wants_frame() || media_present_pending() ||
                                outs_have_work(&s) || prep_ready(&s) || ahead);
        /* no monitor, no vblank: 60 frames a second, or as fast as the fastest remote viewer's screen
         * (120, 144 Hz…) */
        int hz = mirror_fps() > 60 ? mirror_fps() : 60;
        /* a driver that completes page flips at once instead of on vblank (vmwgfx, some virtual cards)
         * is paced to the mode's refresh, or the compositor and every app draw hundreds of frames a second */
        int soft = !s.headless && flip_instant >= 3;
        if (soft) hz = main_out(&s)->kms.mode.vrefresh > 0 ? main_out(&s)->kms.mode.vrefresh : 60;
        /* frames start on a fixed grid of 1000/hz ms (a software vblank, softpace.h): a late one starts the grid
         * again, and the next frame waits half a refresh after one shown late (painted slowly) */
        double pace = 0, period = 1000.0 / hz, tnow = now_ms();
        if (s.headless || soft) pace = softpace_wait(&pace_sw, tnow, period);
        if (wants && (!all_busy(&s) || ahead) && pace <= 0) {
            render(&s);
            renders_done++;
            js_vm_painted();                /* the machines' frames it showed: lord-vmdisplay may send the next ones */
            wants = 0;
            if (s.headless || soft) { softpace_started(&pace_sw, period); softpace_shown(&pace_sw, now_ms(), period); }
        }
        if (wants && pace > 0 && (delay < 0 || pace < delay)) delay = pace;
        /* sharing a screen or a window with a Linux app (the ScreenCast portal): its frames, after this render's damage */
        cast_pointer(s.cx, s.cy);
        cast_tick();
        double cad = cast_timeout();
        if (cad >= 0 && (delay < 0 || cad < delay)) delay = cad;
        double fwd = fswatch_timeout(now_ms());        /* lord.fs.watch: a coalesced call waiting for its 150 ms */
        if (fwd >= 0 && (delay < 0 || fwd < delay)) delay = fwd;

        {   /* a Linux app's content updates held for a refresh or a time (fifo-v1, commit-timing-v1, wl.c): after the
             * render, whose frame may have let some apply that now want the next one */
            double twd = wl_timing_wait(unseen);
            if (twd == 0 && all_busy(&s)) twd = -1;     /* (due now, every display waiting for its flip: the flip wakes it) */
            if (twd >= 0 && (delay < 0 || twd < delay)) delay = twd;
        }
        int wait = delay < 0 ? -1 : (int)ceil(delay);
        if ((wants && (!all_busy(&s) || ahead) && pace <= 0) || ui.quit_requested) wait = 0;
        if (ui.theme_request[0] && (wait < 0 || wait > 16)) wait = 16;     /* a theme switch waits for its overlay's frames */
        if (timeout_ms > 0) {
            double left = start + timeout_ms - now_ms();
            if (left <= 0) break;
            if (wait < 0 || wait > left) wait = (int)left + 1;
        }

        wl_flush();
        mirror_clipboard(term_clipboard(), term_clipboard_gen());
        share_clipboard(term_clipboard(), term_clipboard_gen(), term_clipboard_sensitive());
        struct pollfd pfd[214 + INPUT_MAX_DEVICES + TERM_MAX];     /* +32 brokers of isolated apps, +40 media, +26 machines (their pictures, the state's eventfd, the library's watch, 8 lord-vm pidfds), +40 Spawn helpers, +5 test hooks (lord-server-test) */
        int term_ids[TERM_MAX];
        int n = 0;
        pfd[n++] = (struct pollfd){ K0(&s)->fd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ sfd, POLLIN, 0 };
        int vt_i = n;
        pfd[n++] = (struct pollfd){ vt_sfd, POLLIN, 0 };
        for (int i = 0; i < s.input.count; i++) pfd[n++] = (struct pollfd){ s.input.dev[i].fd, POLLIN, 0 };
        int tbase = n, nterm = term_pollfds(pfd + n, TERM_MAX, term_ids);
        n += nterm;
        int abase = admin_fd() >= 0 ? n : -1;
        if (abase >= 0) pfd[n++] = (struct pollfd){ admin_fd(), POLLIN, 0 };
        int wbase = wl_fd() >= 0 ? n : -1;
        if (wbase >= 0) pfd[n++] = (struct pollfd){ wl_fd(), POLLIN, 0 };
        int nbase = net_fd() >= 0 ? n : -1;
        if (nbase >= 0) pfd[n++] = (struct pollfd){ net_fd(), POLLIN, 0 };
        int dkbase = disk_watch_fd() >= 0 ? n : -1;          /* Disks app open: mounts and udev */
        if (dkbase >= 0) pfd[n++] = (struct pollfd){ disk_watch_fd(), POLLIN, 0 };
        int bkbase = backup_fd() >= 0 ? n : -1;
        if (bkbase >= 0) pfd[n++] = (struct pollfd){ backup_fd(), POLLIN, 0 };
        int clbase = cloud_fd() >= 0 ? n : -1;
        if (clbase >= 0) pfd[n++] = (struct pollfd){ cloud_fd(), POLLIN, 0 };
        int fabase = fsasync_fd() >= 0 ? n : -1;                 /* lord.fs.*Async answers (Files on slow mounts) */
        if (fabase >= 0) pfd[n++] = (struct pollfd){ fsasync_fd(), POLLIN, 0 };
        int fwbase = fswatch_fd() >= 0 ? n : -1;                 /* lord.fs.watch's inotify (fswatch.c) */
        if (fwbase >= 0) pfd[n++] = (struct pollfd){ fswatch_fd(), POLLIN, 0 };
        int imbase = image_async_fd() >= 0 ? n : -1;              /* thumbnails decoded off this thread (ui/image.c) */
        if (imbase >= 0) pfd[n++] = (struct pollfd){ image_async_fd(), POLLIN, 0 };
        int sbase = snd_fd() >= 0 ? n : -1;
        if (sbase >= 0) pfd[n++] = (struct pollfd){ snd_fd(), POLLIN, 0 };
        int pbase = portal_fd() >= 0 ? n : -1;
        if (pbase >= 0) pfd[n++] = (struct pollfd){ portal_fd(), (short)(POLLIN | (portal_wants_write() ? POLLOUT : 0)), 0 };
        int snbase = sni_fd() >= 0 ? n : -1;
        if (snbase >= 0) pfd[n++] = (struct pollfd){ sni_fd(), (short)(POLLIN | (sni_wants_write() ? POLLOUT : 0)), 0 };
        int lgbase = logs_fd() >= 0 ? n : -1;                    /* app Registros: consultas e inotify */
        if (lgbase >= 0) pfd[n++] = (struct pollfd){ logs_fd(), POLLIN, 0 };
        int lbibase = lbi_fd() >= 0 ? n : -1;                    /* LordBench's synthetic pointer, like a device */
        if (lbibase >= 0) pfd[n++] = (struct pollfd){ lbi_fd(), POLLIN, 0 };
#ifdef LORD_TEST_HOOKS
        int thbase = n, nth = testhooks_pollfds(pfd + n, 5);     /* build/lord-server-test's --test-hooks only (testhooks.h) */
        n += nth;
#endif
        int mbase = n, nm = mirror_pollfds(pfd + n, 12);
        n += nm;
        int shbase = n, nsh = share_pollfds(pfd + n, 8);
        n += nsh;
        int cabase = n, nca = cast_pollfds(pfd + n, 4);        /* lord-screencast, one per shared session */
        n += nca;
        int brkbase = n, nbrk = brokersrv_pollfds(pfd + n, 32);   /* apps .lpk aisladas (broker) */
        n += nbrk;
        int abr = n, nabr = brokersrv_audio_pollfds(pfd + n, 16);  /* micrófono de las apps aisladas (PipeWire) */
        n += nabr;
        int medbase = n, nmed = media_pollfds(pfd + n, 40);      /* ayudantes lord-media y sus streams de PipeWire */
        n += nmed;
        int hwbase = n, nhw = hw_pollfds(pfd + n, 48);   /* 16 fixed + up to 32 Spawn helpers */           /* uevents, the system bus and the session bus clients (src/hw) */
        n += nhw;
        int vmbase = n, nvm = js_vm_pollfds(pfd + n, 26);        /* Machines: each picture's lord-vmdisplay, the state for the shell (ui/js_vm.c) */
        n += nvm;
        wdog_idle();
        int ready = poll(pfd, n, wait);
        if (ready < 0 && errno != EINTR) { perror("poll"); break; }
        loop_wake = mono_ns();                  /* LordBench's frame phases: the next frame starts from here */
        wdog_busy(loop_wake);
        loop_wake_flip = 0;
        if (pfd[0].revents & POLLIN) {
            Kms *heads[OUT_MAX], *mk = &main_out(&s)->kms;
            int nh = 0, was = mk->flip_pending, waswho[OUT_MAX];
            for (int i = 0; i < OUT_MAX; i++) {
                waswho[i] = s.out[i].on && s.out[i].kms.flip_pending;
                if (s.out[i].on && s.out[i].kms.fd >= 0) heads[nh++] = &s.out[i].kms;
            }
            if (nh) kms_dispatch_heads(heads, nh);
            /* the other displays' flips: the presentation feedback of the windows on them (the main one's below) */
            for (int i = 0; i < OUT_MAX; i++) {
                const Kms *k = &s.out[i].kms;
                if (!waswho[i] || k->flip_pending || k == mk) continue;
                wl_presented(i, s.main_out, k->flip_ns, k->flip_seq, k->mode.vrefresh ? 1000000000u / k->mode.vrefresh : 16666667u);
            }
            /* wp_presentation follows the main display's vblank */
            if (was && !mk->flip_pending) {
                /* a flip waits for vblank, half a period on average; one that completes within an eighth
                 * of it, again and again, comes from a driver that doesn't wait (a virtual card) */
                uint64_t period = mk->mode.vrefresh ? 1000000000ull / mk->mode.vrefresh : 16666667ull;
                if (!mk->flip_async && mk->flip_submit_ns && mk->flip_ns >= mk->flip_submit_ns) {   /* (a torn flip: tearing-control) */
                    if (mk->flip_ns - mk->flip_submit_ns < period / 8) { if (flip_instant < 8) flip_instant++; }
                    else if (flip_instant > 0) flip_instant--;
                    if (flip_instant == 3 && !flip_told++) fprintf(stderr, "kms: page flips complete before vblank: pacing to %d Hz\n", mk->mode.vrefresh ? mk->mode.vrefresh : 60);
                }
                wl_presented(s.main_out, s.main_out, mk->flip_ns, mk->flip_seq, mk->mode.vrefresh ? 1000000000u / mk->mode.vrefresh : 16666667u);
                lbf_presented(mk->flip_ns, mk->mode.vrefresh ? 1000000000u / mk->mode.vrefresh : 16666667u);
                if (keylag_enabled) keylag_presented(mk->flip_ns);
                if (lbf_recording()) {
                    uint64_t done = 0;
                    if (gpu_active() && main_out(&s)->gpu >= 0) gpu_fence_time(main_out(&s)->gpu, &done);
                    lbf_phase_shown(mk->flip_ns, done);
                    loop_wake_flip = 1;
                }
            }
        }
        if (pfd[1].revents & POLLIN) {                 /* SIGINT or SIGTERM: who sent it, for the log */
            struct signalfd_siginfo si;
            if (read(pfd[1].fd, &si, sizeof si) == (ssize_t)sizeof si)
                fprintf(stderr, "lord-server: %s from pid %u (uid %u): ending the session\n",
                        si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT", si.ssi_pid, si.ssi_uid);
            break;
        }
        int vt_moved = vt_sfd >= 0 && (pfd[vt_i].revents & POLLIN);
        if (vt_moved) vt_signal(&s);           /* the input list changed: this pass's poll entries are stale */
        /* the watchdog's other half (wdog.h): a display whose page flip never completes leaves the desktop
         * frozen on it while this loop runs on; said once per flip, from fields this loop already keeps */
        if (!s.headless) {
            static uint64_t flip_told[OUT_MAX];
            for (int i = 0; i < OUT_MAX; i++) {
                const Kms *k = &s.out[i].kms;
                if (!s.out[i].on || !k->flip_pending) { flip_told[i] = 0; continue; }
                if (flip_told[i] != k->flip_submit_ns && k->flip_submit_ns && loop_wake > k->flip_submit_ns + 2000000000ull) {
                    flip_told[i] = k->flip_submit_ns;
                    fprintf(stderr, "lord-server: watchdog: %s: its page flip has not completed for %.1f s\n",
                            s.out[i].d.connector, (loop_wake - k->flip_submit_ns) / 1e9);
                }
            }
        }
        int lost[INPUT_MAX_DEVICES], nlost = 0;
        for (int i = 0; !vt_moved && i < s.input.count; i++) {
            if (pfd[3 + i].revents & POLLIN) input_dispatch(&s.input, i, on_input, &s);
            if (pfd[3 + i].revents & (POLLERR | POLLHUP | POLLNVAL)) lost[nlost++] = i;
        }
        /* un dispositivo que dejó de servir (desenchufado, o revocado al cambiar de sesión): se cierra
         * solo ese. Dejar el monitor lo decide únicamente lord-sessiond (RP_UNDISPLAY), nunca un error */
        for (int k = nlost - 1; k >= 0; k--) { tablet_gone(&s, &s.input.dev[lost[k]]); input_remove(&s.input, lost[k]); }
        if (nlost) wl_input_devices_changed();
        if (s.input.count || nlost) keyboards_check(&s);
        input_set_leds(&s.input, kb_leds());   /* Caps Lock and Num Lock lights follow the keyboard state */
        /* la GPU vuelve cuando ya nadie mira desde la red */
        if (s.gpu_paused && !s.headless && !any_flip_pending(&s) && gpu_allowed(&s)) {
            s.gpu_paused = 0;
            gpu_start(&s);
        }
        for (int i = 0; i < nterm; i++)
            if (pfd[tbase + i].revents) term_dispatch(term_ids[i], pfd[tbase + i].revents);
        if (abase >= 0 && pfd[abase].revents) admin_dispatch(js_admin_event);
        if (wbase >= 0 && pfd[wbase].revents) wl_dispatch();
        if (nbase >= 0 && pfd[nbase].revents) net_dispatch(js_net_event);
        if (dkbase >= 0 && pfd[dkbase].revents && disk_watch_dispatch()) js_disk_event("change");
        if (sbase >= 0 && pfd[sbase].revents) snd_dispatch();
        if (pbase >= 0 && pfd[pbase].revents) portal_dispatch();
        if (snbase >= 0 && pfd[snbase].revents) sni_dispatch();
        if (bkbase >= 0 && pfd[bkbase].revents) backup_dispatch(js_backup_event);
        if (clbase >= 0 && pfd[clbase].revents) js_cloud_dispatch();
        if (fabase >= 0 && pfd[fabase].revents) js_fs_dispatch();
        if (fwbase >= 0 && pfd[fwbase].revents) fswatch_read(now_ms());
        js_fswatch_dispatch(now_ms());                  /* the watches whose coalesced call is due */
        if (imbase >= 0 && pfd[imbase].revents) image_async_dispatch();
        if (lgbase >= 0 && pfd[lgbase].revents) logs_dispatch(js_logs_event);
        if (lbibase >= 0 && pfd[lbibase].revents) lbi_drain();  /* taken once per frame by grab_script_tick */
#ifdef LORD_TEST_HOOKS
        if (nth) testhooks_dispatch(pfd + thbase, nth);
#endif
        if (nm) mirror_dispatch(pfd + mbase, nm);
        if (nsh) share_dispatch(pfd + shbase, nsh);
        if (nca) cast_dispatch(pfd + cabase, nca);
        if (nbrk) brokersrv_dispatch(pfd + brkbase, nbrk);
        if (nabr) brokersrv_audio_dispatch(pfd + abr, nabr);
        if (nmed) media_dispatch(pfd + medbase, nmed);
        if (nhw) hw_dispatch(pfd + hwbase, nhw);
        if (nvm) js_vm_dispatch(pfd + vmbase, nvm);           /* a machine's frame: damaged now, painted next */
        if (parked_n && s.headless && display_hotplug_seq() != parked_seq) {   /* a monitor may have appeared */
            int f[PARK_MAX], fn = parked_n;
            memcpy(f, parked_fd, sizeof(int) * (size_t)fn);
            parked_n = 0;
            display_attach(f, fn, parked_flags);
        }
        if (splash_wait && !s.headless && main_out(&s)->kms.presented) {
            splash_wait = 0;
            mirror_shown();                 /* lord-sessiond: the boot splash can go */
        }
        hw_events();
        media_events(js_media_event);
        brokersrv_events(js_broker_event);        /* avisos de permiso e indicadores → tema */
        brokersrv_tick();                         /* teclado y reintentos hacia las apps aisladas */
        snd_events(js_sound_event);
        wave_tick();                              /* <SoundWave>: only its box, only when the bands moved */
        wl_events(js_wl_event);
        js_clipboard_poll();
        /* glibc keeps what was freed inside the heap for later, so a peak (a big window, an app opened and closed, the
         * theme loading) stays in the RSS for the rest of the session. When a wait ended with nothing to do (the
         * desktop at rest), at most once a minute, the free pages go back to the system: a few ms, never mid-frame. */
        /* a memory report in compositor.log (docs/perf/MEMORY-HEAPTRACK.md): 10 minutes after the start, when the
         * session has settled, then every 6 hours; one report is a malloc_trim and a few reads of /proc */
        static uint64_t mem_next;
        if (!mem_next) mem_next = loop_wake + 600ull * 1000000000ull;
        if (loop_wake >= mem_next) {
            mem_report(&s);
            mem_next = loop_wake + 6ull * 3600 * 1000000000ull;
        }
        static uint64_t trimmed_at;
        if (ready == 0 && loop_wake - trimmed_at > 60ull * 1000000000ull) {
            paint_release_idle();
            malloc_trim(0);
            trimmed_at = loop_wake;
        }
        term_events(js_term_event);
    }

    /* a logout on the monitor: lord-sessiond takes it back now (a black frame on every display, the login screen up),
     * so closing the apps and unmounting the cloud folders below happens off the screen instead of behind a frozen
     * desktop for seconds */
    wdog_stop();                          /* closing the apps below may take seconds by design */
    if (ui.quit_requested) fprintf(stderr, "lord-server: the theme asked to log out\n");
    {   /* the session ended as the person asked, from outside a community theme's reach: lord-sessiond's SIGTERM (log
         * out, shut down, restart, switch user), the physical Ctrl+Alt+Backspace, the end of a --timeout, or LordOS's own
         * code asking to log out (a confirmation the theme can't answer): never a crash of the theme, however short. A
         * lord.quit the theme's own code made (ui.quit_by_theme), a lord-server that died, a JS fatal or the watchdog
         * keep the count (main.c community_watch, chosen_theme) */
        char cid[32];
        if (!s.greeter && !ui.quit_by_theme && jst_is_user_bundle(current, cid, sizeof cid)) jst_session_ended(cid);
    }
    if (ui.quit_requested && !s.headless) mirror_session("{\"a\":\"logout\"}");
    js_before_exit();                     /* what is open goes to settings.json, marked clean */
    bgsave_flush(NULL);                   /* every save still on bgsave.c's worker is on the disk before the session ends */
    wl_close_all(LOGOUT_CLOSE_MS);        /* the apps close as if the user did it, before wl_stop() */
    cloud_stop();                         /* B11: every cloud folder unmounted (lazily if busy), then rclone quits */
    if (s.renders)
        fprintf(stderr, "lord-server: %ld frames, %ld UI renders, layout %.2f ms, paint %.2f ms (average)\n",
                s.frames, s.renders, s.layout_total / s.renders, s.paint_total / s.renders);
    input_close(&s.input);
    cast_end_all();
    share_stop();
#ifdef LORD_TEST_HOOKS
    testhooks_stop();
#endif
    mirror_stop();
    gpu_tex_free(s.cursor_tex);
    gpu_shutdown();
    for (int i = OUT_MAX - 1; i >= 0; i--) if (!s.headless && (s.out[i].on || i == 0)) kms_close(&s.out[i].kms);   /* out[0] (the fd) last */
    /* Cerrar sesión (tema o Ctrl+Alt+Backspace) termina los shells; una señal o un reinicio
     * del servidor los deja vivos en sus guardianes para recuperarlos al volver. */
    term_shutdown(!(ui.quit_requested || s.quit));
    netrec_shutdown();
    wl_stop();
    snd_stop();
    js_shutdown();
    bgsave_flush(NULL);
    if (vt_tty >= 0) { struct vt_mode m = { .mode = VT_AUTO }; ioctl(vt_tty, VT_SETMODE, &m); }
    if (vt >= 0) { ioctl(vt, KDSETMODE, KD_TEXT); close(vt); }
    for (int i = 0; i < OUT_MAX; i++) out_free(&s.out[i]);
    out_free(&s.cap);
    return 0;
}
