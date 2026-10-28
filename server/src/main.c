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
#include "admin.h"
#include "keylag.h"
#include "bench/lbinput.h"
#include "net/net.h"
#include "disk/disk.h"
#include "backup/backup.h"
#include "sound/snd.h"
#include "keyboard.h"
#include "ui/js_sys.h"
#include "canvas.h"
#include "par.h"
#include "input.h"
#include "kms.h"
#include "drmcard.h"
#include "term/term.h"
#include "ui/js.h"
#include "ui/ui.h"
#include "wl/wl.h"
#include "gpu/gpu.h"
#include "remote/mirror.h"
#include "remote/proto.h"
#include "tp/tp.h"
#include "tp/tp_route.h"
#include "wl/wl_input.h"
#include "wl/xwayland.h"
#include "media/media.h"
#include "app/app.h"
#include "app/apphost_bc.h"
#include "sec/broker.h"
#include "cursor.h"
#include "ui/grab.h"
#include "ui/js_wm.h"
#include "ui/portal.h"
#include "ui/js_logs.h"
#include "hw/hw.h"
#include "hw/modes.h"
#include "ui/js_hw.h"
#include "outputs.h"
#include "monitor/netrec.h"
#include "sec/codesig.h"
#include "sec/netconn.h"
#include "bench/lbframes.h"

#include <libgen.h>
#include <limits.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
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
    /* cursor: the app's (wl_cursor) and whether it goes on the KMS cursor plane (GPU path) */
    unsigned cursor_gen;
    int cursor_mode, cursor_hx, cursor_hy, hw_cursor;
    GpuTex *cursor_tex;
    int cursor_tex_w, cursor_tex_h;
    /* session without a monitor (lord-sessiond shows it on the monitor or the remote desktop): it is
     * drawn in memory, without DRM or evdev; greeter = the login screen */
    int headless, greeter;
    double last_render;
    /* GPU paused because someone is watching over the network: it comes back when nobody is left */
    int gpu_paused;
    /* pointer motion gathered until the next frame; buttons held (mask); cursor shape from the
     * node under the pointer (`cursor` style, docs/WINDOWS.md) */
    int motion_pending, held, shape, over_app;
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

/* ---- the displays ---- */

static int out_count(const Server *s) {
    int n = 0;
    for (int i = 0; i < OUT_MAX; i++) n += s->out[i].on;
    return n;
}
static Out *main_out(Server *s) { return &s->out[s->out[s->main_out].on ? s->main_out : 0]; }
/* page flips that complete before vblank (see the main loop): consecutive early flips seen, 0..8 */
static int flip_instant, flip_told;
static double pace_next;                /* the next frame's slot when frames are paced in software, ms */

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
/* the display under the pointer (-1 headless or none) */
static int out_at_pointer(const Server *s) {
    ORect r[OUT_MAX];
    int idx[OUT_MAX], n = out_rects(s, r, idx), k = out_at(r, n, s->cx, s->cy);
    return k >= 0 ? idx[k] : -1;
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

static void cursor_now(const Server *s, Cur *c, int pct) {
    c->mode = wl_cursor(&c->px, &c->w, &c->h, &c->hx, &c->hy, &c->gen);
    int grabbing = grab_active();
    if (c->mode && !grabbing && (s->over_app || wl_pointer_locked())) return;   /* the app chose its own (or hid it) */
    int shape = grabbing ? grab_cursor() : s->shape;
    if (shape <= CUR_DEFAULT) { c->mode = 0; return; }
    const CursorImage *ci = cursor_image_pct(shape, pct);
    if (!ci) { c->mode = 0; return; }
    *c = (Cur){ 1, ci->px, ci->w, ci->h, ci->hx, ci->hy, 0xf0000000u + (unsigned)shape * 8 + (unsigned)(pct / 25) };
}

/* To draw it here (CPU, cursor plane, texture): the arrow is also an image. */
static void cursor_local(const Server *s, Cur *c, int pct) {
    cursor_now(s, c, pct);
    if (c->mode != 0) return;
    const CursorImage *ci = cursor_image_pct(CUR_DEFAULT, pct);
    if (ci) *c = (Cur){ 1, ci->px, ci->w, ci->h, ci->hx, ci->hy, 0xf0000000u + CUR_DEFAULT * 8 + (unsigned)(pct / 25) };
}

/* The pointer in a display's physical pixels. */
static void pointer_phys(const Server *s, const Out *o, int *x, int *y) {
    double px, py;
    out_to_phys(&o->v, s->cx, s->cy, &px, &py);
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
static void dispatch_pointer(Server *s) {
    s->motion_pending = 0;
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
    if (grabbing && !ended) wl_pointer_grab(s->nbuttons, down);
    else over = wl_pointer(x, y, target, s->moved, s->nbuttons, down, button, s->wheel);
    if (over == 2) id = 0;                  /* sobre un menú de una app Linux: el árbol no lo ve */
    if (ended) js_wm_end(gnode, gedges, gdx, gdy, gdw, gdh);
    int shape = over == 2 ? CUR_DEFAULT : node_cursor(target);
    if (shape != s->shape || grabbing != grab_active() || (over != 0) != s->over_app) { s->shape = shape; s->cursor_dirty = 1; }
    s->over_app = over != 0;                /* el cursor de la app solo vale sobre sus superficies */
    if (s->moved && !grabbing) js_pointer("move", id, x, y, 0);
    for (int i = 0; i < s->nbuttons; i++)          /* sobre un menú, solo los "up" (terminan arrastres) */
        if (over != 2 || !s->buttons[i].down) js_pointer(s->buttons[i].down ? "down" : "up", id, x, y, s->buttons[i].button);
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
            fprintf(stderr, "drag: %ld frames in %.0f ms = %.1f FPS (%ld moves); per frame: work %.2f ms "
                            "(max %.2f; JS %.2f, layout %.2f, paint %.2f, present %.2f), max gap %.1f ms; "
                            "layer: %ld painted (%ld of them a part), %ld copied (%s)\n",
                    dstat.frames, span, 1000.0 * dstat.frames / span, dstat.moves, dstat.work / dstat.frames, dstat.work_max,
                    dstat.js / dstat.frames, dstat.layout / dstat.frames, (dstat.paint - dstat.layout) / dstat.frames,
                    dstat.present / dstat.frames, dstat.gap_max, lr, lp, lb, gpu_active() ? "GPU" : "CPU");
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
    if (display_capture_all() && out_count(s) > 1) return &s->cap;
    return main_out(s);
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
    double lx, ly;                          /* frame pixels of the feed → logical */
    out_to_logical(&feed_out(s)->v, fx, fy, &lx, &ly);
    int x = (int)lround(lx), y = (int)lround(ly);
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
    if (s->nbuttons < MAX_BUTTONS) s->buttons[s->nbuttons++] = (ButtonEvent){ down, button };
    remote_settle(s);
}

static void remote_wheel(int steps) {
    Server *s = remote_server;
    s->wheel += steps;
    remote_settle(s);
}

static void remote_key(int code, int value) {
    Server *s = remote_server;
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) s->ctrl = value != 0;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) s->alt = value != 0;
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
                         out_scale(o), o->d.mm_w, o->d.mm_h, "", "" };
        if (s->headless) { snprintf(w->name, sizeof w->name, "LORD-1"); snprintf(w->desc, sizeof w->desc, "LordOS display"); }
        else { snprintf(w->name, sizeof w->name, "%s", o->d.connector); snprintf(w->desc, sizeof w->desc, "%s", o->d.name); }
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

/* The GPU stopped: every display gets its dumb buffer back (CPU path). */
static void cpu_takeover(Server *s) {
    for (int i = 0; i < OUT_MAX; i++) {
        Out *o = &s->out[i];
        if (!o->on || o->gpu < 0) continue;
        kms_cursor_show(&o->kms, 0);
        kms_set_fb(&o->kms, o->kms.buf[o->kms.front].fb_id);
        o->kms.custom = 0;
        o->gpu = -1;
        o->full_copy = 2;
        o->wl_new = 0;
    }
    s->hw_cursor = 0;
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
    Kms *k0 = K0(s);
    if (s->headless || k0->fd < 0) return;
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
    if (same) { display_resync_done(1, &p); return; }
    if (gpu_active()) { gpu_tex_free(s->cursor_tex); s->cursor_tex = NULL; gpu_shutdown(); }
    cpu_takeover(s);
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

static void display_attach(const int *fds, int n, int flags) {
    Server *s = remote_server;
    parked_drop();                      /* a new RP_DISPLAY replaces what was waiting */
    if (!s->headless || n < 1) { for (int i = 0; i < n; i++) close(fds[i]); return; }
    Out *o0 = &s->out[0];
    out_free(o0);
    memset(o0, 0, sizeof *o0);
    o0->gpu = -1;
    int card = dup(fds[0]);             /* kms_attach closes its fd when it fails */
    int keep = (flags & RP_DISPLAY_KEEP) != 0;
    if (kms_attach(&o0->kms, fds[0], keep)) {
        o0->kms.fd = -1;
        if (card >= 0 && n <= PARK_MAX) {
            fprintf(stderr, "monitor: nothing connected to the card; waiting for a monitor (hotplug)\n");
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

static void display_detach(void) {
    Server *s = remote_server;
    parked_drop();
    splash_wait = 0;
    if (s->headless) return;
    gpu_tex_free(s->cursor_tex);
    s->cursor_tex = NULL;
    gpu_shutdown();
    for (int i = 0; i < OUT_MAX; i++) s->out[i].gpu = -1;
    input_close(&s->input);
    for (int i = OUT_MAX - 1; i >= 0; i--) {        /* the second displays first: out[0] owns the fd */
        Out *o = &s->out[i];
        if (!o->on && i) continue;
        kms_close(&o->kms);
        out_free(o);
        memset(o, 0, sizeof *o);
        o->kms.fd = -1;
        o->gpu = -1;
    }
    s->hw_cursor = 0;
    s->gpu_paused = 0;
    s->headless = 1;
    display_detached();
    set_size(s, s->W & ~1, s->H & ~1);  /* a frame in memory of the same size */
    fprintf(stderr, "monitor: the session leaves the screen\n");
}

static void on_input(void *ud, const InputDevice *d, const struct input_event *ev) {
    Server *s = ud;
    if (ev->type == EV_KEY || ev->type == EV_REL || ev->type == EV_ABS) display_activity(now_ms());
    if (s->debug && ev->type != EV_SYN)
        fprintf(stderr, "ev %s type=%u code=%u value=%d\n", d->name, ev->type, ev->code, ev->value);
    if (d->tp) {                        /* touchpad o pantalla táctil: driver propio (tp/tp.c) */
        tp_event(d->tp, ev);
        if (ev->type != EV_SYN || ev->code != SYN_REPORT) return;
    }
    switch (ev->type) {
    case EV_REL:
        if (ev->code == REL_X) { s->cx += ev->value; s->rdx += ev->value; s->moved = 1; }
        if (ev->code == REL_Y) { s->cy += ev->value; s->rdy += ev->value; s->moved = 1; }
        /* ruedas: la de alta resolución y la horizontal van como scroll fino (tp/tp_route.c, value120);
         * los pasos enteros de siempre, solo si el mouse no tiene la de alta resolución */
        if (ev->code == REL_WHEEL && !d->wheel_hires) s->wheel += ev->value;
        if (ev->code == REL_WHEEL_HI_RES) tp_route_wheel120(ev->value, 0);
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
        if (ev->code == KEY_BACKSPACE && ev->value == 1 && s->ctrl && s->alt) s->quit = 1;
        /* Botones con la numeración del DOM: 0 izquierdo, 1 medio, 2 derecho. */
        int button = ev->code == BTN_LEFT ? 0 : ev->code == BTN_MIDDLE ? 1 : ev->code == BTN_RIGHT ? 2 : -1;
        if (button >= 0 && ev->value != 2 && s->nbuttons < MAX_BUTTONS)
            s->buttons[s->nbuttons++] = (ButtonEvent){ ev->value == 1, button };
        else if (ev->code < BTN_MISC) {
            if (keylag_enabled) keylag_rx(ev->value, &ev->time);
            js_key(ev->code, ev->value);
            if (keylag_enabled) keylag_handled();
        }
        break;
    }
    case EV_SYN:
        if (s->moved) {
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
static Box paint_ui(Server *s) {
    Box d = { 0, 0, 0, 0 };
    if (js_wants_frame()) js_frame(now_ms());
    if (!ui.dirty) return d;
    ui.dirty = 0;
    double t0 = now_ms();
    layout_root(ui.root, (float)s->W, (float)s->H);
    double t1 = now_ms();
    if (paint_damage(ui.root)) d = ui.damage;
    ui.damage = (Box){ 0, 0, 0, 0 };
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
    s->layout_total += ui.layout_ms;
    s->renders++;
    return d;
}

/* LORD_PAINT_VERIFY=N (checking the partial repaints, docs/LAYERS.md): every N-th frame painted on a
 * display, the whole tree is painted again into a scratch canvas with nothing kept from earlier frames
 * (paint_root_full: no drag layer, no copy of what is below it, no skipped node) and the pixels that
 * differ from the display's frame are counted on stderr. Off by default; it only reads the frame. The
 * terminals and Linux apps note where they were painted, so with N = 1 a terminal's change during a
 * drag always repaints the whole layer: use N > 1 to check the layer's partial repaints. */
static void paint_verify(Out *o, int index) {
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
            if (a[x] == b[x]) continue;
            diff++;
            for (int sh = 0; sh < 32; sh += 8) {
                int d = abs((int)((a[x] >> sh) & 255) - (int)((b[x] >> sh) & 255));
                if (d > worst) worst = d;
            }
        }
    }
    fprintf(stderr, "paint-verify: display %d frame %ld: %ld pixels differ from a full repaint (worst %d)%s\n",
            index, frames[index], diff, worst, paint_layer_node() ? ", drag layer" : "");
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
    if (box_empty(b) || !o->scene.px) return r;
    /* several damaged rectangles (paint.c, "Several damaged rectangles"): each one painted on its
     * own; r, what is presented, is the box around them (the scene keeps the pixels in between) */
    Box lb = o->pl.n ? o->pl.r[0] : (Box){ 0, 0, 0, 0 };
    for (int k = 1; k < o->pl.n; k++) lb = box_join(lb, o->pl.r[k]);
    int multi = !whole && o->pl.n > 1 && !gpu_active() && lb.x0 == b.x0 && lb.y0 == b.y0 && lb.x1 == b.x1 && lb.y1 == b.y1;
    DamageList l = { .n = 0 };
    if (multi) l = o->pl;
    else if (!whole) dlist_add(&l, b);
    /* several frames' damage (a pending flip): joined rectangles, or an older frame's after a
     * blurred node moved, may cut through a blurred node (paint_damage_settle) */
    if (!whole && o->pframes > 1) paint_damage_settle(&l);
    if (!whole && l.n) {
        b = l.r[0];
        for (int k = 1; k < l.n; k++) b = box_join(b, l.r[k]);
    }
    multi = !whole && !gpu_active() && l.n > 1;
    int nparts = multi ? l.n : 1;
    Box parts[DAMAGE_MAX];
    if (multi) memcpy(parts, l.r, sizeof(Box) * (size_t)nparts);
    else parts[0] = b;
    o->pl.n = 0;
    o->pframes = 0;
    double t0 = now_ms(), px = 0;
    view_of(o, index);
    for (int k = 0; k < nparts; k++) {
        Box q = parts[k];
        Rect pr = whole ? (Rect){ 0, 0, o->scene.w, o->scene.h } : out_phys_rect(&o->v, q.x0, q.y0, q.x1, q.y1);
        if (pr.x1 <= pr.x0 || pr.y1 <= pr.y0) continue;
        float lx0, ly0, lx1, ly1;
        out_logical_box(&o->v, pr, &lx0, &ly0, &lx1, &ly1);
        ui.damage = (Box){ lx0, ly0, lx1, ly1 };      /* paint_root skips what this region does not need */
        canvas_set_clip(&o->scene, pr);
        paint_root_clear(&o->scene, ui.root, 0x000000);
        if (feed) media_scene_painted(&o->scene);     /* recording/remote desktop with protected windows */
        r = rect_join(r, pr);
        o->painted[o->npainted++] = (Box){ (float)pr.x0, (float)pr.y0, (float)pr.x1, (float)pr.y1 };
        px += (double)(pr.x1 - pr.x0) * (pr.y1 - pr.y0);
    }
    canvas_set_clip(&o->scene, (Rect){ 0, 0, o->scene.w, o->scene.h });
    ui.damage = (Box){ 0, 0, 0, 0 };
    view_reset();
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return (Rect){ 0, 0, 0, 0 };
    double ms = now_ms() - t0;
    ui.paint_ms += ms;
    s->paint_total += ms;
    ui.damage_px += px;
    if (s->debug || paint_profile)
        fprintf(stderr, "render %ld: display %d, layout %.2f ms, paint %.2f ms, repainted %.1f%% in %d rect%s\n", s->renders, index, ui.layout_ms, ms,
                100.0 * px / ((double)o->scene.w * o->scene.h), nparts, nparts == 1 ? "" : "s");
    paint_verify(o, index);
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
    if (out_at(&me, 1, s->cx, s->cy) < 0 && !(o->v.dx || o->v.dy)) return (Rect){ 0, 0, 0, 0 };
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
    if (mode != 1 || w > 64 || h > 64) { arrow_argb(arrow, 64, 64); px = arrow; w = h = 64; hx = hy = 0; }
    s->cursor_hx = hx;
    s->cursor_hy = hy;
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
    static uint32_t arrow[64 * 64];
    arrow_argb(arrow, 64, 64);
    for (int i = 0; i < OUT_MAX; i++) s->out[i].gpu = -1;
    s->out[0].gpu = 0;
    for (int i = 1; i < OUT_MAX; i++)           /* the other displays: their own surfaces on the same context */
        if (s->out[i].on) s->out[i].gpu = gpu_add_output(&s->out[i].kms);
    s->hw_cursor = getenv("LORD_SW_CURSOR") == NULL;
    for (int i = 0; i < OUT_MAX && s->hw_cursor; i++)
        if (s->out[i].on && s->out[i].gpu >= 0 && kms_cursor_image(&s->out[i].kms, arrow, 64, 64, 0, 0)) s->hw_cursor = 0;
    hw_cursor_move(s);
    s->cursor_gen = 0;
    s->cursor_mode = -1;
    fprintf(stderr, "gpu: cursor %s\n", s->hw_cursor ? "by hardware (cursor plane)" : "drawn in the composition");
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

static void render_frame(Server *s);
static void render(Server *s) {
    double t0 = now_ms();
    render_frame(s);
    lbf_frame(now_ms() - t0);                   /* LordBench's compositor measurement (bench/lbframes.c) */
    if (dstat.active) {
        double t1 = now_ms(), js = dstat.js - dstat.work_js;
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
        /* remote desktop (without the cursor) and recording: with protected windows in black */
        MediaCursor mc = { px_, py_, k.mode, k.px, k.w, k.h, k.hx, k.hy };
        view_of(o, index);
        const Canvas *seen = media_composed(&out, mirror_watchers() > 0, &mc);
        view_reset();
        mirror_composed_on_gpu(0);        /* this frame was painted on the CPU */
        /* only what went into this buffer can differ from the last frame (not with a fullscreen app
         * painted over it, nor when the frame is the copy with protected windows in black) */
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
        /* what the browser sees and what is recorded: with the protected windows in black */
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
    return out_at(&me, 1, s->cx, s->cy) >= 0 || o->v.dx || o->v.dy;
}
static int outs_have_work(const Server *s) {
    for (int i = 0; i < OUT_MAX; i++) if (!s->out[i].kms.flip_pending && out_needs(s, &s->out[i])) return 1;
    return 0;
}

static void render_frame(Server *s) {
    double t0 = now_ms();
    ui.layout_ms = ui.paint_ms = 0;
    ui.damage_px = 0;
    if (keylag_enabled) keylag_render();
    /* GPU path: a Linux app's new frame is only a texture (wl.c damage_node), not UI damage. Every
     * GPU display must still present it: before 0.8.1 nothing was flipped until some other part of
     * the screen changed (the clock, the cursor...), so typing in Chromium or Firefox showed up to a
     * second late and Chromium logged "buggy presentation feedback" (docs/TROUBLESHOOTING.md). */
    if (wl_wants_frame() && gpu_active())
        for (int i = 0; i < OUT_MAX; i++) if (s->out[i].on && s->out[i].gpu >= 0) s->out[i].wl_new = 1;
    paint_ui(s);
    s->last_render = now_ms();
    if (s->cursor_dirty) {
        for (int i = 0; i < OUT_MAX; i++) s->out[i].cdirty = 1;
        s->cap.cdirty = 1;
        s->cursor_dirty = 0;
    }
    if (gpu_active() && need_cpu()) {     /* recording the screen, or a remote viewer with protected windows */
        /* the screen must be in memory (CPU path) */
        fprintf(stderr, "remote: composing by CPU (%s)\n", media_needs_cpu() ? "screen recording" : "protected windows on screen");
        gpu_tex_free(s->cursor_tex);
        s->cursor_tex = NULL;
        gpu_shutdown();
        s->gpu_paused = 1;
        cpu_takeover(s);
        full_damage(s);
        paint_ui(s);
    }
    Out *feed = feed_out(s), *mo = main_out(s);
    int main_shown = 0;                 /* the main display got this frame */
    if (s->headless) {
        Out *o = &s->out[0];
        Rect drawn = paint_out(s, o, 0, 1);
        present_memory(s, o, 0, 1, drawn);
    } else {
        for (int i = 0; i < OUT_MAX; i++) {
            Out *o = &s->out[i];
            /* a fullscreen app's new frame (render runs when something wants a frame) goes to every display */
            if (!o->on || o->kms.flip_pending || !(out_needs(s, o) || wl_fullscreen_active())) continue;
            Rect r = paint_out(s, o, i, o == feed);
            if (gpu_active() && o->gpu >= 0) {      /* (the loop above skips what has nothing new) */
                update_cursor(s);
                gpu_server = s;
                gpu_out = o;
                view_of(o, i);
                /* the remote desktop watches: the composed frame is read back once for every viewer */
                static uint32_t *rb;
                static size_t rb_n;
                int readback = o == feed && mirror_watchers() > 0;
                if (readback) {
                    size_t need = (size_t)o->scene.w * o->scene.h;
                    if (need > rb_n) { free(rb); rb = malloc(need * 4); rb_n = rb ? need : 0; }
                    if (rb) gpu_read_next(rb, o->scene.w, o->scene.h);
                    else readback = 0;
                }
                /* compatible fullscreen (one display): straight to the card; otherwise composed (always
                 * composed while the remote desktop reads it back) */
                int failed = !(!readback && out_count(s) == 1 && wl_try_scanout(s->hw_cursor || s->cursor_mode == 2)) && gpu_frame(o->gpu, &o->scene, r, gpu_layer);
                if (readback && !failed && gpu_read_done()) {
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
                o->cdirty = 0;
                o->wl_new = 0;
                if (failed) {
                    fprintf(stderr, "gpu: a frame failed; going on by CPU\n");
                    gpu_shutdown();
                    cpu_takeover(s);
                    full_damage(s);
                }
            } else present_cpu(s, o, i, r, o == feed);
            if (o == mo) main_shown = 1;
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
    lbf_built(s->headless || main_shown);      /* LordBench: the synthetic pointer's events are in this frame */
    s->frames++;
    /* The pool only while frames are heavy: it costs CPU to wake (a light frame painted on one thread
     * spends less), so it turns on when painting takes more than 70 % of a frame at the target rate (the
     * monitor's refresh, or the fastest remote viewer's without a monitor) and off after 60 light frames
     * in a row (with the pool a heavy frame paints fast, so not at once: it would flap).
     * A 1080p video under blurred windows: 45 → 57 frames a second on VM 116. */
    static int par_on, light;
    Out *pm = main_out(s);
    int hz = s->headless ? (mirror_fps() > 60 ? mirror_fps() : 60) : pm && pm->kms.mode.vrefresh ? (int)pm->kms.mode.vrefresh : 60;
    double frame_ms = 1000.0 / hz;
    if (!par_on && ui.paint_ms > frame_ms * 0.7 && par_threads() > 1) { par_on = 1; light = 0; canvas_parallel(par_rows); }
    else if (par_on) {
        light = ui.paint_ms < frame_ms * 0.25 ? light + 1 : 0;
        if (light >= 60) { par_on = 0; canvas_parallel(NULL); }
    }
    wl_frame_done((unsigned)now_ms());
    if (s->headless) {                          /* no flip without a monitor: the frame is out now */
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        lbf_presented((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec, (uint32_t)(1000000000u / (unsigned)hz));
    }
    /* No flip event will come (a modeset instead of a page flip: the first frame, or a driver
     * without page flip): the frame is on screen already, so its presentation feedback goes now.
     * Otherwise the apps' feedback would wait for some later flip. */
    if (main_shown && !s->headless && !mo->kms.flip_pending) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        wl_presented(ns, mo->kms.flip_seq, mo->kms.mode.vrefresh ? 1000000000u / mo->kms.mode.vrefresh : 16666667u);
        lbf_presented(ns, mo->kms.mode.vrefresh ? 1000000000u / mo->kms.mode.vrefresh : 16666667u);
        if (keylag_enabled) keylag_presented(ns);
    }
    frame_stats(s, now_ms() - t0, ui.layout_ms + ui.paint_ms);
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
    if (r != JS_REFUSED && codesig_state() != CODE_INVALID) return -1;
    if (r == JS_REFUSED) snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", theme);   /* lord.themes.use won't go back to it */
    if (!greeter && codesig_state() != CODE_INVALID) {
        char root[PATH_MAX], tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", theme);
        char *d = dirname(tmp);                     /* .../themes/<id> */
        snprintf(root, sizeof root, "%s", d);
        snprintf(tmp, sizeof tmp, "%s", root);
        snprintf(root, sizeof root, "%s", dirname(tmp));   /* .../themes */
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

/* 0 if the requested theme is running; -1 if it failed and `current` (the last one that loaded) was put
 * back: the caller keeps `current` then, so a second failed switch never falls back to a refused theme */
static int switch_theme(const char *current) {
    char path[sizeof ui.theme_request];
    snprintf(path, sizeof path, "%s", ui.theme_request);
    ui.theme_request[0] = 0;
    fprintf(stderr, "lord-server: cambiando a tema %s\n", path);
    js_shutdown();
    ui_reset();
    term_detach_all();
    if (js_init(path)) {
        fprintf(stderr, "lord-server: the theme failed, going back to %s\n", current);
        snprintf(ui.theme_failed, sizeof ui.theme_failed, "%s", path);
        js_shutdown();
        ui_reset();
        if (js_init(current)) fprintf(stderr, "lord-server: %s did not load again either\n", current);
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

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--theme bundle.js] [--fonts dir] [--shell dir] [--user name] [--card /dev/dri/cardN]\n"
                    "          [--headless WIDTHxHEIGHT [--greeter]] [--timeout s] [--debug] [--profile]\n", argv0);
}

int main(int argc, char **argv) {
    struct sigaction pipe_sa = { .sa_handler = on_sigpipe };
    sigemptyset(&pipe_sa.sa_mask);
    sigaction(SIGPIPE, &pipe_sa, NULL);
    /* guardián de una sesión de terminal (ver term/hold.c): no abre pantalla ni input */
    if (argc > 1 && !strcmp(argv[1], "--term-holder")) return hold_main(argc, argv);
    /* (0.8.5) a component's network connector (sec/netconn.h): only creates sockets in its scope */
    if (argc > 1 && !strcmp(argv[1], "--net-connector")) return netconn_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--check-code")) return check_code(argc, argv);
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
    static Server s;
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
        else if (!strcmp(argv[i], "--debug")) s.debug = 1;
        else if (!strcmp(argv[i], "--profile")) paint_profile = 1;
        else { usage(argv[0]); return 2; }
    }

    if (s.greeter && !s.headless) { usage(argv[0]); return 2; }
    js_sys_set_greeter(s.greeter);
    if (text_init(fonts)) return 1;
    {   /* assets/ = la carpeta de las fuentes, un nivel arriba */
        char a[1024];
        snprintf(a, sizeof a, "%s/..", fonts);
        js_sys_set_assets(a);
    }
    term_init(fonts, shell);
    kb_set("latam", "");         /* la UI elige la del usuario al cargar sus ajustes */
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

    int vt = -1;
    if (!s.headless) {
        if (input_open(&s.input)) fprintf(stderr, "input: no devices (permissions?)\n");
        /* The text console stops drawing over us. */
        vt = open("/dev/tty0", O_RDWR | O_CLOEXEC);
        if (vt >= 0 && ioctl(vt, KDSETMODE, KD_GRAPHICS)) { close(vt); vt = -1; }
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
    if (ok && s.headless && getenv("HOME")) {
        /* no terminal: the session log goes to the home folder */
        char log[1024];
        snprintf(log, sizeof log, "%s/.cache", getenv("HOME"));
        mkdir(log, 0700);
        snprintf(log, sizeof log, "%s/.cache/lordos", getenv("HOME"));
        mkdir(log, 0700);
        snprintf(log, sizeof log, "%s/.cache/lordos/%s", getenv("HOME"), s.greeter ? "greeter.log" : "session.log");
        if (!freopen(log, "w", stderr)) { /* carries on without a log */ }
        setvbuf(stderr, NULL, _IOLBF, 0);
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
    publish_displays(&s);                   /* one wl_output per display */
    if (ok && net_open()) fprintf(stderr, "lord-server: no network notifications (rtnetlink)\n");
    /* Backups: a thread running as the user, nice 19 and idle I/O (backup/backup.c) */
    if (ok && !s.greeter && getenv("HOME") && session_user) backup_start(getenv("HOME"), session_user);
    if (ok && snd_start()) fprintf(stderr, "lord-server: no PipeWire for now (sound); it retries on its own\n");
    if (ok && !s.greeter) portal_start();   /* dark mode, accent and buttons for GTK/GNOME/Qt (ui/portal.c) */
    if (ok && !s.greeter) term_restore();   /* shells still alive from the previous start */
    netrec_init(ok && !s.greeter && session_user != NULL);   /* the Monitor's traffic history (monitor/netrec.c) */
    if (getenv("LORD_DAMAGE") && !strcmp(getenv("LORD_DAMAGE"), "multi")) paint_multi_damage = 1;   /* several damaged rectangles (paint.c; off by default) */
    remote_server = &s;
    tp_route_init(&tp_host);
    static const MirrorInput mirror_in = { remote_pointer, remote_button, remote_wheel, remote_key, remote_clip, remote_clients,
                                           remote_refresh, remote_resize, remote_greeter, display_attach, display_detach,
                                           remote_scroll, remote_pinch, remote_input_add, remote_notice };
    if (ok && mirror_start(s.W, s.H, &mirror_in)) fprintf(stderr, "lord-server: no remote desktop (socket)\n");
    char current[sizeof ui.theme_request];
    if (ok) ok = start_theme(theme, s.greeter, current, sizeof current) == 0;
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
    keylag_init(s.greeter);                 /* key latency trace, only with LORD_INPUT_DEBUG=1 (keylag.h) */
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

    double start = now_ms();
    while (!s.quit && !ui.quit_requested) {
        if (ui.theme_request[0]) {
            char next[sizeof current];
            snprintf(next, sizeof next, "%s", ui.theme_request);
            if (!switch_theme(current)) snprintf(current, sizeof current, "%s", next);
        }
        /* El movimiento del puntero juntado: una vez por cuadro, cuando terminó el page flip. Así un
         * mouse de 1000 Hz no llama a JS (ni a las apps Linux) mil veces por segundo. */
        if (s.motion_pending && !main_out(&s)->kms.flip_pending) {
            dispatch_pointer(&s);
            if (gpu_active()) update_cursor(&s);
        }
        /* LordBench's scene moves its window one step per frame, as a drag does (ui/grab.c) */
        if (grab_scripted() && !any_flip_pending(&s)) grab_script_tick(now_ms());
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
        if (wl_idle_inhibited()) display_activity(now_ms());   /* a video or a presentation keeps the screen on */
        display_tick(now_ms());             /* screen off, lock and sleep after N minutes (src/hw) */
        double hwd = display_timeout(now_ms());
        if (hwd >= 0 && (delay < 0 || hwd < delay)) delay = hwd;
        /* Settings → Displays or a hotplug: displays, modes, scales, arrangement (between frames) */
        if (!s.headless && !any_flip_pending(&s) && display_take_resync()) outputs_apply(&s);
        netrec_tick();                      /* traffic history: every 5 s while on (monitor/netrec.c) */
        double nrd = netrec_timeout();
        if (nrd >= 0 && (delay < 0 || nrd < delay)) delay = nrd;
        double tpd = tp_timeout(now_ms());
        if (tpd >= 0 && (delay < 0 || tpd < delay)) delay = tpd;
        mirror_tick();                      /* remote desktop video: lossless tiles once still, the encoder */
        double mrd = mirror_timeout();
        if (mrd >= 0 && (delay < 0 || mrd < delay)) delay = mrd;
        /* sin monitor y sin nadie mirando no se dibuja (al conectarse alguien llega un cuadro entero) */
        int unseen = s.headless && !mirror_watchers() && !media_needs_cpu();   /* recording the screen: yes */
        if (!s.headless && display_is_off()) unseen = 1;     /* screen off after idle (DPMS): nothing to draw */
        /* nothing is drawn: the Linux apps waiting for a frame get it at most once a second (wl.c) */
        if (unseen) {
            static double unseen_tick;
            double since = now_ms() - unseen_tick;
            if (since >= 1000) { wl_frame_unseen((unsigned)now_ms()); unseen_tick = now_ms(); since = 0; }
            if (wl_wants_frame() || ui.dirty) {
                double left = 1000 - since;
                if (delay < 0 || left < delay) delay = left;
            }
        }
        int wants = !unseen && (ui.dirty || s.cursor_dirty || js_wants_frame() || wl_wants_frame() || media_wants_frame() || outs_have_work(&s));
        /* no monitor, no vblank: 60 frames a second, or as fast as the fastest remote viewer's screen
         * (120, 144 Hz…) */
        int hz = mirror_fps() > 60 ? mirror_fps() : 60;
        /* a driver that completes page flips at once instead of on vblank (vmwgfx, some virtual cards)
         * is paced to the mode's refresh, or the compositor and every app draw hundreds of frames a second */
        int soft = !s.headless && flip_instant >= 3;
        if (soft) hz = main_out(&s)->kms.mode.vrefresh > 0 ? main_out(&s)->kms.mode.vrefresh : 60;
        /* frames start on a fixed grid of 1000/hz ms (a software vblank): waiting in whole milliseconds
         * then never adds up, so 60 Hz stays 60 frames a second, not 57 */
        double pace = 0, period = 1000.0 / hz, tnow = now_ms();
        if (s.headless || soft) {
            if (!pace_next || tnow - pace_next > period) pace_next = tnow;     /* idle, or fell behind */
            pace = pace_next - tnow;
        }
        if (wants && !all_busy(&s) && pace <= 0) {
            render(&s);
            wants = 0;
            if (s.headless || soft) pace_next += period;
        }
        if (wants && pace > 0 && (delay < 0 || pace < delay)) delay = pace;

        int wait = delay < 0 ? -1 : (int)ceil(delay);
        if ((wants && !all_busy(&s) && pace <= 0) || ui.theme_request[0] || ui.quit_requested) wait = 0;
        if (timeout_ms > 0) {
            double left = start + timeout_ms - now_ms();
            if (left <= 0) break;
            if (wait < 0 || wait > left) wait = (int)left + 1;
        }

        wl_flush();
        mirror_clipboard(term_clipboard(), term_clipboard_gen());
        struct pollfd pfd[128 + INPUT_MAX_DEVICES + TERM_MAX];     /* +32 para los brokers de apps aisladas, +40 medios */
        int term_ids[TERM_MAX];
        int n = 0;
        pfd[n++] = (struct pollfd){ K0(&s)->fd, POLLIN, 0 };
        pfd[n++] = (struct pollfd){ sfd, POLLIN, 0 };
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
        int sbase = snd_fd() >= 0 ? n : -1;
        if (sbase >= 0) pfd[n++] = (struct pollfd){ snd_fd(), POLLIN, 0 };
        int pbase = portal_fd() >= 0 ? n : -1;
        if (pbase >= 0) pfd[n++] = (struct pollfd){ portal_fd(), (short)(POLLIN | (portal_wants_write() ? POLLOUT : 0)), 0 };
        int lgbase = logs_fd() >= 0 ? n : -1;                    /* app Registros: consultas e inotify */
        if (lgbase >= 0) pfd[n++] = (struct pollfd){ logs_fd(), POLLIN, 0 };
        int lbibase = lbi_fd() >= 0 ? n : -1;                    /* LordBench's synthetic pointer, like a device */
        if (lbibase >= 0) pfd[n++] = (struct pollfd){ lbi_fd(), POLLIN, 0 };
        int mbase = n, nm = mirror_pollfds(pfd + n, 12);
        n += nm;
        int brkbase = n, nbrk = brokersrv_pollfds(pfd + n, 32);   /* apps .lpk aisladas (broker) */
        n += nbrk;
        int abr = n, nabr = brokersrv_audio_pollfds(pfd + n, 16);  /* micrófono de las apps aisladas (PipeWire) */
        n += nabr;
        int medbase = n, nmed = media_pollfds(pfd + n, 40);      /* ayudantes lord-media y sus streams de PipeWire */
        n += nmed;
        int hwbase = n, nhw = hw_pollfds(pfd + n, 4);            /* uevents and the system bus (src/hw) */
        n += nhw;
        if (poll(pfd, n, wait) < 0 && errno != EINTR) { perror("poll"); break; }
        if (pfd[0].revents & POLLIN) {
            Kms *heads[OUT_MAX], *mk = &main_out(&s)->kms;
            int nh = 0, was = mk->flip_pending;
            for (int i = 0; i < OUT_MAX; i++) if (s.out[i].on && s.out[i].kms.fd >= 0) heads[nh++] = &s.out[i].kms;
            if (nh) kms_dispatch_heads(heads, nh);
            /* wp_presentation follows the main display's vblank */
            if (was && !mk->flip_pending) {
                /* a flip waits for vblank, half a period on average; one that completes within an eighth
                 * of it, again and again, comes from a driver that doesn't wait (a virtual card) */
                uint64_t period = mk->mode.vrefresh ? 1000000000ull / mk->mode.vrefresh : 16666667ull;
                if (mk->flip_submit_ns && mk->flip_ns >= mk->flip_submit_ns) {
                    if (mk->flip_ns - mk->flip_submit_ns < period / 8) { if (flip_instant < 8) flip_instant++; }
                    else if (flip_instant > 0) flip_instant--;
                    if (flip_instant == 3 && !flip_told++) fprintf(stderr, "kms: page flips complete before vblank: pacing to %d Hz\n", mk->mode.vrefresh ? mk->mode.vrefresh : 60);
                }
                wl_presented(mk->flip_ns, mk->flip_seq, mk->mode.vrefresh ? 1000000000u / mk->mode.vrefresh : 16666667u);
                lbf_presented(mk->flip_ns, mk->mode.vrefresh ? 1000000000u / mk->mode.vrefresh : 16666667u);
                if (keylag_enabled) keylag_presented(mk->flip_ns);
            }
        }
        if (pfd[1].revents & POLLIN) break;
        int lost[INPUT_MAX_DEVICES], nlost = 0;
        for (int i = 0; i < s.input.count; i++) {
            if (pfd[2 + i].revents & POLLIN) input_dispatch(&s.input, i, on_input, &s);
            if (pfd[2 + i].revents & (POLLERR | POLLHUP | POLLNVAL)) lost[nlost++] = i;
        }
        /* un dispositivo que dejó de servir (desenchufado, o revocado al cambiar de sesión): se cierra
         * solo ese. Dejar el monitor lo decide únicamente lord-sessiond (RP_UNDISPLAY), nunca un error */
        for (int k = nlost - 1; k >= 0; k--) input_remove(&s.input, lost[k]);
        if (nlost) wl_input_devices_changed();
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
        if (bkbase >= 0 && pfd[bkbase].revents) backup_dispatch(js_backup_event);
        if (lgbase >= 0 && pfd[lgbase].revents) logs_dispatch(js_logs_event);
        if (lbibase >= 0 && pfd[lbibase].revents) lbi_drain();  /* taken once per frame by grab_script_tick */
        if (nm) mirror_dispatch(pfd + mbase, nm);
        if (nbrk) brokersrv_dispatch(pfd + brkbase, nbrk);
        if (nabr) brokersrv_audio_dispatch(pfd + abr, nabr);
        if (nmed) media_dispatch(pfd + medbase, nmed);
        if (nhw) hw_dispatch(pfd + hwbase, nhw);
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
        wl_events(js_wl_event);
        js_clipboard_poll();
        term_events(js_term_event);
    }

    if (s.renders)
        fprintf(stderr, "lord-server: %ld frames, %ld UI renders, layout %.2f ms, paint %.2f ms (average)\n",
                s.frames, s.renders, s.layout_total / s.renders, s.paint_total / s.renders);
    input_close(&s.input);
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
    if (vt >= 0) { ioctl(vt, KDSETMODE, KD_TEXT); close(vt); }
    for (int i = 0; i < OUT_MAX; i++) out_free(&s.out[i]);
    out_free(&s.cap);
    return 0;
}
