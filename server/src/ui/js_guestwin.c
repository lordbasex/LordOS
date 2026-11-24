/* js_guestwin.c — lord.guestwin: apps mode's guest side when LordOS itself runs inside a Machines guest
 * (docs/VIRTUALIZATION.md 7.2; the wire format is server/src/vm/vmagent.h's LAG_*).
 *
 * The host shows each window of the guest as its own LordOS window. Inside the guest, lordos-guest (tools/guest) holds
 * org.lordos.agent; it asks the guest's own lord-server for the windows through this endpoint, and relays both ways.
 *
 * The endpoint: an AF_UNIX stream socket, $XDG_RUNTIME_DIR/lordos-guestwin.sock, mode 0600, made only when this
 * lord-server runs inside a Machines guest (/dev/virtio-ports/org.lordos.agent exists) or LORD_GUESTWIN=1 (tests).
 * A peer is accepted only if SO_PEERCRED says it runs as this lord-server's own uid; one peer at a time (a new one
 * replaces the old). Frames as on org.lordos.agent (ag_frame_head / ag_frame_next), at most GW_IN_MAX bytes each way in.
 *   lord-server → peer   AGK_HELLO "lord-server 1 windows"; LAG_WINDOWS on every change (serial + 1); LAG_WINICON once
 *                        per window whose app has an icon file (PNG or SVG, at most 64 × 64); LAG_APPLIST (the
 *                        launcher's apps: name and launcher id)
 *   peer → lord-server   LAG_APPS {on} (the desktop hides its Dock, bars and wallpaper while on; off, or the peer gone,
 *                        brings them back); LAG_WINACT on a window id this side listed; LAG_LAUNCH of a token from the
 *                        list this side sent. Anything else, or anything that fails vmagent.c's parsers, is dropped.
 * What it can do is what the person can do with their own windows and launcher: focus, close (the app's own close
 * guard still asks), minimize, restore, move or resize a listed window, and open a listed app through the SDK's
 * launchApp (sdk/guestwin.js). It never runs a command, a path or an Exec line, and lists nothing but the windows'
 * titles, app names, places and icons.
 *
 *
 * The clipboard (docs/VIRTUALIZATION.md 6.8, "LordOS as a guest"): lord-server has no data-control protocol and its X
 * selection is offered only to a focused X window, so lordos-guest can't use wl-paste or xclip here. When both hellos
 * say "clipboard" (lordos-guest's: "lordos-guest <v> clipboard"), lord-server sends the clipboard's text as AGK_CLIP
 * after each change (never a secret: term_clipboard_sensitive) and takes the peer's AGK_CLIP (valid UTF-8, at most
 * AG_TEXT_MAX) as the clipboard's new text, marked as coming from "machines". Only a peer that holds the agent port itself
 * (it passes the port's descriptor with its hello, SCM_RIGHTS: a character device with the port's device number, open
 * for reading and writing; lord-server looks at it and closes it at once, never keeping the port open) gets or sets it: another program of the same person that connects here gets the windows as before, never the
 * clipboard. lord-vm and lord-server on the host decide whether it goes on (machine.json "clipboard", the gate).
 *
 * JS: lord.guestwin.active() → the endpoint exists; poll() → [{ type: 'apps', on } | { type: 'act', id, action, x, y,
 * w, h } | { type: 'launch', token }] (also accepts and reads the peer: sdk/guestwin.js calls it every 100 ms);
 * publish([{ id, title, app, x, y, w, h, focused, minimized, owner, pid, icon }]) (rects in framebuffer pixels;
 * icon: an absolute .png or .svg path or ''); apps([{ name, token }]); connected() → a peer is there. */
#include "js_guestwin.h"
#include "ui.h"
#include "../media/svgicon.h"
#include "../vm/vmagent.h"
#include "../term/term.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define GW_IN_MAX   4096            /* what the peer may send in one frame (LAG_APPS, LAG_WINACT, LAG_LAUNCH, a hello) */
#define GW_OUT_MAX  (4u << 20)      /* queued toward a peer that doesn't read: past this it's dropped */
#define GW_EV_MAX   64

static int tried, lfd = -1, cfd = -1;
static char sock_path[108];
static uint8_t in_buf[AG_HEAD + AG_TEXT_MAX];       /* AGK_CLIP may be AG_TEXT_MAX; every other kind GW_IN_MAX */
static int peer_clip;                   /* the peer said "clipboard" and holds the agent port */
static int peer_port = -1;              /* the descriptor the peer passed with its hello (SCM_RIGHTS), until it is looked at */
static unsigned clip_sent_gen;          /* the clipboard's generation last sent (or set from the peer) */
static size_t in_n;
static uint8_t *out_q;
static size_t out_n, out_off;
static LagWinList *wins;            /* the last list published */
static uint8_t *wins_bytes;         /* and its bytes (serial left 0): compared to know a change */
static size_t wins_len;
static uint32_t serial;
static LagAppList *apps;
static uint8_t *apps_bytes;
static size_t apps_len;
static int apps_on;
typedef struct { uint32_t id; char path[512]; Image *img; int sent; } GwIcon;
static GwIcon icons[LAG_WIN_MAX];
typedef struct { int type, on; LagAct a; char token[LAG_TOKEN_MAX + 1]; } GwEvent;
static GwEvent evq[GW_EV_MAX];
static int evn;

static void push_ev(const GwEvent *e) { if (evn < GW_EV_MAX) evq[evn++] = *e; }

static void flush(void) {
    while (cfd >= 0 && out_off < out_n) {
        ssize_t w = send(cfd, out_q + out_off, out_n - out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) { out_off += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) return;
        break;
    }
    out_n = out_off = 0;
}

static void send_frame(uint16_t kind, const uint8_t *p, size_t n) {
    if (cfd < 0) return;
    if (out_n - out_off + AG_HEAD + n > GW_OUT_MAX) return;       /* a peer that doesn't read loses frames, never memory */
    if (out_off) { memmove(out_q, out_q + out_off, out_n - out_off); out_n -= out_off; out_off = 0; }
    uint8_t *nb = realloc(out_q, out_n + AG_HEAD + n);
    if (!nb) return;
    out_q = nb;
    ag_frame_head(out_q + out_n, kind, 0, (uint32_t)n);
    if (n) memcpy(out_q + out_n + AG_HEAD, p, n);
    out_n += AG_HEAD + n;
    flush();
}

static void drop_peer(void) {
    if (cfd >= 0) close(cfd);
    cfd = -1;
    in_n = 0;
    peer_clip = 0;
    if (peer_port >= 0) { close(peer_port); peer_port = -1; }
    out_n = out_off = 0;
    if (apps_on) { GwEvent e = { .type = 1, .on = 0 }; push_ev(&e); apps_on = 0; }
}

static void send_windows(void) {
    if (!wins || !wins_bytes || wins_len < 6) return;
    uint8_t *b = malloc(wins_len);
    if (!b) return;
    memcpy(b, wins_bytes, wins_len);
    b[0] = (uint8_t)serial; b[1] = (uint8_t)(serial >> 8); b[2] = (uint8_t)(serial >> 16); b[3] = (uint8_t)(serial >> 24);
    send_frame(LAG_WINDOWS, b, wins_len);
    free(b);
}

/* each listed window's icon, once its picture is decoded: premultiplied ARGB → straight RGBA, at most 64 a side */
static void send_icons(void) {
    if (cfd < 0) return;
    static uint8_t rgba[LAG_ICON_MAX * LAG_ICON_MAX * 4], frame[8 + sizeof rgba];
    for (int i = 0; i < LAG_WIN_MAX; i++) {
        GwIcon *g = &icons[i];
        if (!g->id || !g->img || g->sent) continue;
        int w, h;
        const uint32_t *px = image_ready_px(g->img, &w, &h);
        if (!px) continue;
        int ow = w, oh = h;
        if (ow > LAG_ICON_MAX || oh > LAG_ICON_MAX) {
            if (ow >= oh) { oh = oh * LAG_ICON_MAX / ow; ow = LAG_ICON_MAX; } else { ow = ow * LAG_ICON_MAX / oh; oh = LAG_ICON_MAX; }
            if (ow < 1) ow = 1;
            if (oh < 1) oh = 1;
        }
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                uint32_t p = px[(size_t)(y * h / oh) * w + (size_t)(x * w / ow)];
                uint32_t a = p >> 24, r = p >> 16 & 255, gg = p >> 8 & 255, b = p & 255;
                if (a && a < 255) { r = r * 255 / a; gg = gg * 255 / a; b = b * 255 / a; }
                uint8_t *o = rgba + ((size_t)y * ow + x) * 4;
                o[0] = (uint8_t)(r > 255 ? 255 : r); o[1] = (uint8_t)(gg > 255 ? 255 : gg); o[2] = (uint8_t)(b > 255 ? 255 : b); o[3] = (uint8_t)a;
            }
        size_t n = lag_icon_write(g->id, ow, oh, rgba, frame, sizeof frame);
        if (n) send_frame(LAG_WINICON, frame, n);
        g->sent = 1;
    }
}

/* does process pid hold the agent port (a descriptor on the same character device)? 1 yes */
static int holds_port(int fd) {
    struct stat port, st;
    /* tests (LORD_GUESTWIN=1 only): LORD_GUESTWIN_PORT names another character device as the port */
    const char *force = getenv("LORD_GUESTWIN"), *alt = getenv("LORD_GUESTWIN_PORT");
    const char *path = force && !strcmp(force, "1") && alt && alt[0] == '/' ? alt : "/dev/virtio-ports/org.lordos.agent";
    if (fd < 0 || stat(path, &port) || !S_ISCHR(port.st_mode)) return 0;
    int fl = fcntl(fd, F_GETFL);
    return !fstat(fd, &st) && S_ISCHR(st.st_mode) && st.st_rdev == port.st_rdev && fl >= 0 && (fl & O_ACCMODE) == O_RDWR;
}

/* the clipboard's text to the peer when it changed (not a secret, not empty, at most AG_TEXT_MAX, valid UTF-8) */
static void send_clip(void) {
    if (cfd < 0 || !peer_clip) return;
    unsigned g = term_clipboard_gen();
    if (g == clip_sent_gen) return;
    clip_sent_gen = g;
    const char *t = term_clipboard();
    size_t n = t ? strlen(t) : 0;
    if (!n || n > AG_TEXT_MAX || term_clipboard_sensitive() || !ag_utf8_ok((const uint8_t *)t, n)) return;
    send_frame(AGK_CLIP, (const uint8_t *)t, n);
}

static void hello(void) {
    static const char me[] = "lord-server 1 windows clipboard";
    send_frame(AGK_HELLO, (const uint8_t *)me, sizeof me - 1);
    send_windows();
    if (apps_bytes) send_frame(LAG_APPLIST, apps_bytes, apps_len);
    for (int i = 0; i < LAG_WIN_MAX; i++) icons[i].sent = 0;
    send_icons();
}

static void setup(void) {
    tried = 1;
    const char *force = getenv("LORD_GUESTWIN");
    if (!(force && !strcmp(force, "1")) && access("/dev/virtio-ports/org.lordos.agent", F_OK)) return;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/' || snprintf(sock_path, sizeof sock_path, "%s/lordos-guestwin.sock", rt) >= (int)sizeof sock_path) return;
    struct stat st;
    if (!lstat(sock_path, &st)) { if (!S_ISSOCK(st.st_mode)) return; unlink(sock_path); }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, sock_path, strlen(sock_path) + 1);
    mode_t old = umask(0177);
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r || chmod(sock_path, 0600) || listen(s, 2)) { close(s); return; }
    lfd = s;
    fprintf(stderr, "guestwin: apps mode's endpoint is listening (%s)\n", sock_path);
}

static void take_peer(void) {
    for (;;) {
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) return;
        struct ucred cr;
        socklen_t cl = sizeof cr;
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != getuid()) { close(c); continue; }
        drop_peer();
        cfd = c;
        clip_sent_gen = term_clipboard_gen() - 1;     /* the current text goes once the peer says "clipboard" */
        hello();
    }
}

static int listed(uint32_t id) {
    if (!wins) return 0;
    for (int i = 0; i < wins->n; i++) if (wins->w[i].id == id) return 1;
    return 0;
}

static void read_peer(void) {
    while (cfd >= 0) {
        /* recvmsg: lordos-guest passes the agent port's descriptor with its hello; any other descriptor is closed */
        union { struct cmsghdr c; char b[CMSG_SPACE(4 * sizeof(int))]; } u;
        struct iovec io = { in_buf + in_n, sizeof in_buf - in_n };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
        ssize_t r = recvmsg(cfd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        for (struct cmsghdr *c = r > 0 ? CMSG_FIRSTHDR(&mh) : NULL; c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < k; i++) {
                int pfd;
                memcpy(&pfd, CMSG_DATA(c) + i * sizeof(int), sizeof pfd);
                if (peer_port < 0 && !peer_clip) peer_port = pfd; else close(pfd);
            }
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { drop_peer(); return; }
        if (r < 0) return;
        in_n += (size_t)r;
        for (;;) {
            if (in_n >= 6) {
                uint32_t len = (uint32_t)in_buf[0] | (uint32_t)in_buf[1] << 8 | (uint32_t)in_buf[2] << 16 | (uint32_t)in_buf[3] << 24;
                uint16_t kind = (uint16_t)(in_buf[4] | in_buf[5] << 8);
                if (len > (kind == AGK_CLIP && peer_clip ? AG_TEXT_MAX : GW_IN_MAX)) { drop_peer(); return; }
            }
            AgFrame f;
            int k = ag_frame_next(in_buf, in_n, &f);
            if (k < 0) { drop_peer(); return; }
            if (!k) break;
            GwEvent e;
            memset(&e, 0, sizeof e);
            if (f.kind == AGK_HELLO && f.len < 200 && !peer_clip) {
                /* "lordos-guest <v> clipboard": the clipboard, only for the program that holds the agent port */
                int words = (ag_hello_caps(f.data, f.len) & AG_HELLO_CLIPBOARD) != 0;
                if (words && holds_port(peer_port)) { peer_clip = 1; send_clip(); }
                else if (words) fprintf(stderr, "guestwin: a peer asked for the clipboard without holding the agent port: refused\n");
                if (peer_port >= 0) { close(peer_port); peer_port = -1; }
            } else if (f.kind == AGK_CLIP && peer_clip && f.len && f.len <= AG_TEXT_MAX && ag_utf8_ok(f.data, f.len)) {
                /* the host's text, through lordos-guest: the clipboard's new text (and not sent back) */
                char *t = malloc(f.len + 1);
                if (t) {
                    memcpy(t, f.data, f.len);
                    t[f.len] = 0;
                    if (strcmp(t, term_clipboard() ? term_clipboard() : "")) { term_set_clipboard(t, f.len); term_clipboard_mark("machines", 0); }
                    clip_sent_gen = term_clipboard_gen();
                    free(t);
                }
            } else if (f.kind == LAG_APPS && f.len == 1 && f.data[0] <= 1) {
                e.type = 1; e.on = f.data[0];
                apps_on = e.on;
                push_ev(&e);
            } else if (f.kind == LAG_WINACT && !lag_winact_parse(f.data, f.len, &e.a) && listed(e.a.id)) {
                e.type = 2;
                push_ev(&e);
            } else if (f.kind == LAG_LAUNCH && !lag_launch_token(f.data, f.len, e.token, sizeof e.token) && apps && lag_applist_has(apps, e.token)) {
                e.type = 3;
                push_ev(&e);
            }
            memmove(in_buf, in_buf + k, in_n - (size_t)k);
            in_n -= (size_t)k;
        }
        if (in_n == sizeof in_buf) { drop_peer(); return; }
    }
}

/* JS text → what the wire allows: control characters become spaces, cut at max bytes on a character boundary */
static void clean_text(JSContext *c, JSValueConst o, const char *k, char *out, size_t max) {
    out[0] = 0;
    JSValue v = JS_GetPropertyStr(c, o, k);
    const char *s = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    JS_FreeValue(c, v);
    if (!s) return;
    size_t n = strlen(s), o2 = 0;
    for (size_t i = 0; i < n && o2 < max; i++) out[o2++] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? ' ' : s[i];
    out[o2] = 0;
    while (o2 && !lag_text_ok((const uint8_t *)out, o2)) out[--o2] = 0;      /* a character cut in half, or C1 controls */
    JS_FreeCString(c, s);
}
static int32_t num(JSContext *c, JSValueConst o, const char *k) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = 0;
    if (JS_ToFloat64(c, &d, v)) d = 0;
    JS_FreeValue(c, v);
    if (!(d == d)) d = 0;
    if (d > 1e9) d = 1e9;
    if (d < -1e9) d = -1e9;
    return (int32_t)(d < 0 ? d - 0.5 : d + 0.5);
}
static int flag(JSContext *c, JSValueConst o, const char *k) { JSValue v = JS_GetPropertyStr(c, o, k); int b = JS_ToBool(c, v) > 0; JS_FreeValue(c, v); return b; }

static JSValue f_active(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!tried) setup();
    return JS_NewBool(c, lfd >= 0);
}
static JSValue f_connected(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewBool(c, cfd >= 0); }

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!tried) setup();
    if (lfd >= 0) take_peer();
    read_peer();
    send_icons();
    send_clip();
    flush();
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < evn; i++) {
        JSValue o = JS_NewObject(c);
        const GwEvent *e = &evq[i];
        JS_SetPropertyStr(c, o, "type", JS_NewString(c, e->type == 1 ? "apps" : e->type == 2 ? "act" : "launch"));
        if (e->type == 1) JS_SetPropertyStr(c, o, "on", JS_NewBool(c, e->on));
        if (e->type == 2) {
            static const char *const A[] = { "", "focus", "close", "minimize", "restore", "move" };
            JS_SetPropertyStr(c, o, "id", JS_NewUint32(c, e->a.id));
            JS_SetPropertyStr(c, o, "action", JS_NewString(c, A[e->a.action]));
            JS_SetPropertyStr(c, o, "x", JS_NewInt32(c, e->a.x)); JS_SetPropertyStr(c, o, "y", JS_NewInt32(c, e->a.y));
            JS_SetPropertyStr(c, o, "w", JS_NewInt32(c, e->a.w)); JS_SetPropertyStr(c, o, "h", JS_NewInt32(c, e->a.h));
        }
        if (e->type == 3) JS_SetPropertyStr(c, o, "token", JS_NewString(c, e->token));
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    evn = 0;
    return arr;
}

/* an icon file the endpoint may decode: an absolute .png or .svg path, no ".." */
static Image *icon_image(const char *p) {
    size_t n = strlen(p);
    if (p[0] != '/' || n < 5 || strstr(p, "..")) return NULL;
    if (!strcmp(p + n - 4, ".png")) return image_get_thumb(p, LAG_ICON_MAX, "");
    if (!strcmp(p + n - 4, ".svg")) return svgicon_get(p, LAG_ICON_MAX, -1);
    return NULL;
}

static JSValue f_publish(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (lfd < 0 || argc < 1 || !JS_IsArray(c, argv[0])) return JS_FALSE;
    static LagWinList l;
    static uint8_t buf[LAG_WINDOWS_BYTES];
    static char paths[LAG_WIN_MAX][512];
    memset(&l, 0, sizeof l);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    uint32_t len = 0;
    JS_ToUint32(c, &len, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < len && l.n < LAG_WIN_MAX; i++) {
        JSValue o = JS_GetPropertyUint32(c, argv[0], i);
        LagWinEntry *w = &l.w[l.n];
        memset(w, 0, sizeof *w);
        w->id = (uint32_t)num(c, o, "id");
        w->x = num(c, o, "x"); w->y = num(c, o, "y"); w->w = num(c, o, "w"); w->h = num(c, o, "h");
        w->flags = (flag(c, o, "focused") ? LAGW_FOCUSED : 0) | (flag(c, o, "minimized") ? LAGW_MINIMIZED : 0) | (flag(c, o, "topmost") ? LAGW_TOPMOST : 0);
        w->owner = (uint32_t)num(c, o, "owner");
        if (w->owner && w->owner != w->id) w->flags |= LAGW_OWNED; else w->owner = 0;
        w->pid = (uint32_t)num(c, o, "pid");
        clean_text(c, o, "title", w->title, LAG_TITLE_MAX);
        clean_text(c, o, "app", w->app, LAG_APPNAME_MAX);
        clean_text(c, o, "icon", paths[l.n], sizeof paths[0] - 1);
        JS_FreeValue(c, o);
        int dup = 0;
        for (int j = 0; j < l.n; j++) if (l.w[j].id == w->id) dup = 1;
        if (w->flags & LAGW_MINIMIZED) w->x = w->y = w->w = w->h = 0;
        if (!w->id || dup || (!(w->flags & LAGW_MINIMIZED) && !lag_rect_ok(w->x, w->y, w->w, w->h))) continue;
        l.n++;
    }
    /* an owner that isn't listed: plain window */
    for (int i = 0; i < l.n; i++) {
        int found = 0;
        for (int j = 0; j < l.n; j++) if (l.w[j].id == l.w[i].owner) found = 1;
        if (!found) { l.w[i].owner = 0; l.w[i].flags &= ~(uint32_t)LAGW_OWNED; }
    }
    size_t n = lag_windows_write(&l, buf, sizeof buf);
    if (!n) return JS_FALSE;
    /* icons: kept for the ids still listed with the same file */
    for (int k = 0; k < LAG_WIN_MAX; k++) {
        GwIcon *g = &icons[k];
        if (!g->id) continue;
        int keep = 0;
        for (int i = 0; i < l.n; i++) if (l.w[i].id == g->id && !strcmp(paths[i], g->path)) keep = 1;
        if (!keep) { if (g->img) image_release(g->img); memset(g, 0, sizeof *g); }
    }
    for (int i = 0; i < l.n; i++) {
        if (!paths[i][0]) continue;
        int have = 0, free_k = -1;
        for (int k = 0; k < LAG_WIN_MAX; k++) { if (icons[k].id == l.w[i].id) have = 1; else if (!icons[k].id && free_k < 0) free_k = k; }
        if (have || free_k < 0) continue;
        Image *img = icon_image(paths[i]);
        if (!img) continue;
        icons[free_k].id = l.w[i].id;
        icons[free_k].img = img;
        icons[free_k].sent = 0;
        snprintf(icons[free_k].path, sizeof icons[free_k].path, "%s", paths[i]);
    }
    if (!wins && !(wins = malloc(sizeof *wins))) return JS_FALSE;
    int changed = n != wins_len || !wins_bytes || memcmp(buf + 4, wins_bytes + 4, n - 4);
    if (changed) {
        uint8_t *nb = realloc(wins_bytes, n);
        if (!nb) return JS_FALSE;
        wins_bytes = nb;
        memcpy(wins_bytes, buf, n);
        wins_len = n;
        *wins = l;
        serial++;
        send_windows();
    }
    send_icons();
    return JS_NewBool(c, changed);
}

static JSValue f_apps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (lfd < 0 || argc < 1 || !JS_IsArray(c, argv[0])) return JS_FALSE;
    static LagAppList l;
    static uint8_t buf[LAG_APPLIST_BYTES];
    memset(&l, 0, sizeof l);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    uint32_t len = 0;
    JS_ToUint32(c, &len, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < len && l.n < LAG_APPS_MAX; i++) {
        JSValue o = JS_GetPropertyUint32(c, argv[0], i);
        LagAppEntry *a = &l.a[l.n];
        clean_text(c, o, "name", a->name, LAG_APPNAME_MAX);
        clean_text(c, o, "token", a->token, LAG_TOKEN_MAX);
        JS_FreeValue(c, o);
        if (a->name[0] && a->token[0] && strlen(a->token) == strspn(a->token, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:")) l.n++;
    }
    size_t n = lag_applist_write(&l, buf, sizeof buf);
    if (!n) return JS_FALSE;
    if (apps_bytes && n == apps_len && !memcmp(buf, apps_bytes, n)) return JS_FALSE;
    if (!apps && !(apps = malloc(sizeof *apps))) return JS_FALSE;
    uint8_t *nb = realloc(apps_bytes, n);
    if (!nb) return JS_FALSE;
    apps_bytes = nb;
    memcpy(apps_bytes, buf, n);
    apps_len = n;
    *apps = l;
    send_frame(LAG_APPLIST, apps_bytes, apps_len);
    return JS_TRUE;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("active", 0, f_active), JS_CFUNC_DEF("connected", 0, f_connected), JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("publish", 1, f_publish), JS_CFUNC_DEF("apps", 1, f_apps),
};

void js_guestwin_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "guestwin", o);
}
