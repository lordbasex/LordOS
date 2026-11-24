/* guest_fuzz.c — lordos-guest against hostile peers (docs/VIRTUALIZATION.md 7.1, 8).
 *
 *   build/guest-fuzz build/lordos-guest-asan [seed] [seconds per run]
 *
 * lordos-guest reads from five sides: the host (the agent's port: lord-vm's frames), the guest's own lord-server (apps
 * mode's window endpoint), the X display (x11clip.c speaks X11 itself), the system and session buses (dbusmin.c: the
 * performance profile's replies, the notifications it monitors) and local programs (the link and notification socket).
 * This test plays all five at once with seeded random input: well-formed frames of every kind built with vmagent.c's
 * writers and then mutated (bytes changed, cut, grown), frames of random kinds, raw random bytes, X events and replies
 * with random fields, D-Bus replies and Notify calls with random and mutated bodies, connections dropped and made again.
 *
 * The helper is built with ASan and UBSan and runs in a user namespace as its root (unshare -Ur), where it refuses to
 * open a shell, so no random byte can ever reach a real shell; without one the shell's open frame is never sent.
 * Checks: it never dies until told to end (SIGTERM, then it exits 0), it has no sanitizer report, and every frame it
 * sends the host is one the host's own parser takes (relayed windows, icons and app lists, notifications, links,
 * clipboard frames, requests): nothing a hostile peer sent passes on unchecked. Exits 1 on any failure. */
#include "../src/vm/vmagent.h"
#include "../../tools/guest/dbusmin.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int fails;
static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "ok    " : "FAILED");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }
static int chance(int pct) { return (int)below(100) < pct; }
static void fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }
/* text: mostly printable, now and then UTF-8, controls or bytes that aren't UTF-8 */
static size_t text(uint8_t *p, size_t cap) {
    size_t n = below((uint32_t)cap);
    for (size_t i = 0; i < n; i++) {
        uint32_t k = below(100);
        if (k < 80) p[i] = (uint8_t)(0x20 + below(95));
        else if (k < 88 && i + 2 <= n) { p[i] = 0xc3; p[++i] = (uint8_t)(0x80 + below(64)); }
        else if (k < 94) p[i] = (uint8_t)below(32);
        else p[i] = (uint8_t)rnd();
    }
    return n;
}
/* a payload's mutations: bytes changed, cut short or grown */
static size_t mutate(uint8_t *p, size_t n, size_t cap) {
    if (chance(55)) return n;
    if (n && chance(60)) { int k = 1 + (int)below(4); while (k--) p[below((uint32_t)n)] ^= (uint8_t)(1u << below(8)); }
    if (n && chance(20)) n = below((uint32_t)n);
    if (chance(15)) { size_t g = below(16); if (n + g <= cap) { fill(p + n, g); n += g; } }
    return n;
}

static char dir[64];
static int use_ns;                      /* the helper runs as root of a user namespace (no shell can open) */

/* ---- sockets ---- */
static void sun_of(struct sockaddr_un *a, const char *path) {
    size_t l = strlen(path);
    if (l >= sizeof a->sun_path) { fprintf(stderr, "guest-fuzz: socket path too long: %s\n", path); exit(1); }
    memcpy(a->sun_path, path, l + 1);
}
static int listen_at(const char *path) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    sun_of(&a, path);
    unlink(path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror(path); exit(1); }
    return s;
}
static int connect_to(const char *path) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    sun_of(&a, path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { close(s); return -1; }
    return s;
}
static void drain(void);
static void put(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    long long end = now_ms() + 200;
    while (n && fd >= 0) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w > 0) { b += w; n -= (size_t)w; continue; }
        /* the helper may be writing to another of its peers meanwhile: what it sends is read while this waits */
        if (w < 0 && (errno == EAGAIN || errno == EINTR) && now_ms() < end) { drain(); struct pollfd q = { fd, POLLOUT, 0 }; poll(&q, 1, 2); continue; }
        return;                             /* the helper isn't reading: dropped, as a flood would be */
    }
}
static void frame(int fd, uint16_t kind, const uint8_t *p, size_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, (uint32_t)n);
    if (chance(3)) h[below(AG_HEAD)] ^= (uint8_t)(1u << below(8));     /* a header bent too */
    put(fd, h, AG_HEAD);
    put(fd, p, n);
}

/* ---- the host's side: lord-vm's frames ---- */
static int port_l = -1, port = -1;
static uint8_t pin[1 << 22];
static size_t pin_n;
static uint32_t offer_gen = 1;
static int bad_out, frames_out;
static char bad_what[64];
static uint32_t guest_send_id;          /* a file the guest offered: accepted now and then */
static uint64_t guest_send_size;

static const char *const WORDS[] = { "clipboard", "clipx", "clipreq", "links", "notify", "send", "shell", "perf", "ask", "open", "place",
                                     "windows", "stopping", "extra", "gvt", "pointer", "noclip" };
static void host_hello(void) {
    char h[256];
    int n = snprintf(h, sizeof h, "lordos-vm 12");
    for (int i = 0; i < 17; i++) if (chance(60)) n += snprintf(h + n, sizeof h - (size_t)n, " %s", WORDS[i]);
    uint8_t b[400];
    memcpy(b, h, (size_t)n);
    size_t m = mutate(b, (size_t)n, sizeof b);
    if (chance(5)) m = 200 + below(150);       /* too long for a hello */
    frame(port, AGK_HELLO, b, m);
}
static const uint8_t PNG1[] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0,
                                0x1f, 0x15, 0xc4, 0x89, 0, 0, 0, 13, 'I', 'D', 'A', 'T', 0x78, 0x9c, 0x63, 0, 1, 0, 0, 5, 0, 1, 0x0d, 0x0a,
                                0x2d, 0xb4, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82 };
static uint8_t big[1 << 21];
static void host_frame(void) {
    size_t n = 0;
    uint16_t kind;
    switch (below(18)) {
    case 0: host_hello(); return;
    case 1: kind = AGK_CLIP; n = text(big, chance(90) ? 400 : 70000); break;
    case 2: {
        kind = AGK_CLIPX;
        static uint8_t t[600], h[600];
        AgClip c = { 0 };
        if (chance(70)) { c.text = t; c.text_n = (uint32_t)text(t, sizeof t); }
        if (chance(50)) { c.html = h; c.html_n = (uint32_t)text(h, sizeof h); }
        if (chance(40)) { c.png = PNG1; c.png_n = sizeof PNG1; }
        ag_clipx_write(&c, big);
        n = ag_clipx_size(&c);
        break;
    }
    case 3: kind = AGK_CLIPOFFER; ag_offer_write(big, chance(80) ? ++offer_gen : (uint32_t)rnd(), (uint8_t)(chance(80) ? 1 + below(7) : rnd()), (uint8_t)below(3)); n = 6; break;
    case 4: {
        kind = AGK_CLIPDATA;
        uint8_t form = (uint8_t)(chance(85) ? 1 + below(3) : rnd()), st = (uint8_t)(chance(70) ? AGD_OK : below(6));
        ag_clipdata_head(big, chance(80) ? offer_gen : (uint32_t)rnd(), form, st);
        n = 6;
        if (form == AGF_PNG && chance(60)) { memcpy(big + 6, PNG1, sizeof PNG1); n += sizeof PNG1; }
        else n += text(big + 6, 2000);
        break;
    }
    case 5: kind = LAG_APPS; big[0] = (uint8_t)below(3); n = chance(90) ? 1 : below(4); break;
    case 6: {
        kind = LAG_WINACT;
        LagAct a = { (uint32_t)below(70), (uint8_t)below(8), (int32_t)below(4000) - 100, (int32_t)below(3000) - 100, (int32_t)below(3000), (int32_t)below(3000) };
        n = lag_act_write(&a, big, sizeof big);
        if (!n) { n = 21; fill(big, n); }
        break;
    }
    case 7: { kind = LAG_LAUNCH; char tok[64]; snprintf(tok, sizeof tok, "app%u.desktop", below(8)); n = lag_launch_write(tok, big, sizeof big); if (!n) n = text(big, 600); break; }
    case 8: kind = AGK_PERF; big[0] = (uint8_t)below(3); n = 1; break;
    case 9: {               /* the shell's frames (its open only as the namespace's root, where it's refused) */
        uint16_t k[] = { AGK_SHELL_OPEN, AGK_SHELL_DATA, AGK_SHELL_SIZE, AGK_SHELL_CLOSE, AGK_SHELL_ACK };
        kind = k[below(5)];
        if (kind == AGK_SHELL_OPEN && !use_ns) kind = AGK_SHELL_ACK;
        n = kind == AGK_SHELL_DATA ? 4 + text(big + 4, 300) : 8;
        uint32_t id = below(4);
        memcpy(big, &id, 4);
        if (kind != AGK_SHELL_DATA) fill(big + 4, 4);
        break;
    }
    case 10: { kind = AGK_SEND_ACCEPT; uint8_t sha[32]; fill(sha, 32); ag_send_accept_write(big, chance(70) ? guest_send_id : (uint32_t)rnd(), (uint8_t)below(4), chance(70) ? below((uint32_t)guest_send_size + 1) : rnd(), sha); n = AG_SEND_ACCEPT_BYTES; break; }
    case 11: kind = AGK_SEND_ACK; ag_send_pos_write(big, chance(70) ? guest_send_id : (uint32_t)rnd(), chance(70) ? below((uint32_t)guest_send_size + 1) : rnd()); n = 12; break;
    case 12: kind = AGK_SEND_DONE; ag_send_done_write(big, chance(70) ? guest_send_id : (uint32_t)rnd(), (uint8_t)below(6)); n = 5; break;
    case 13: kind = AGK_STOPPING; big[0] = (uint8_t)below(6); n = 1; break;
    case 14: kind = AGK_EXTRA; big[0] = (uint8_t)below(3); n = 1; break;
    default: {              /* any kind, random bytes */
        kind = (uint16_t)below(45);
        if (kind == AGK_SHELL_OPEN && !use_ns) kind = 0;
        n = chance(90) ? below(300) : below(sizeof big);
        fill(big, n);
        break;
    }
    }
    n = mutate(big, n, sizeof big);
    frame(port, kind, big, n);
}
/* raw bytes on the port now and then: never a shell's open frame unless the shell is refused */
static void host_raw(void) {
    uint8_t b[64];
    size_t n = 1 + below(sizeof b);
    fill(b, n);
    if (!use_ns) for (size_t i = 0; i + 1 < n; i++) if (b[i] == AGK_SHELL_OPEN || b[i + 1] == AGK_SHELL_OPEN) b[i] = b[i + 1] = 0;
    put(port, b, n);
}
/* what the helper sent the host: each frame one the host's parsers take */
static void bad(const char *what) { if (!bad_out++) snprintf(bad_what, sizeof bad_what, "%s", what); }
static void from_guest(void) {
    for (;;) {
        ssize_t r = recv(port, pin + pin_n, sizeof pin - pin_n, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close(port); port = -1; pin_n = 0; return; }
        if (r < 0) break;
        pin_n += (size_t)r;
        if (pin_n == sizeof pin) break;
    }
    for (;;) {
        AgFrame f;
        int used = ag_frame_next(pin, pin_n, &f);
        if (used < 0) { bad("a malformed frame"); pin_n = 0; return; }
        if (!used) return;
        frames_out++;
        LagWinList *wl = malloc(sizeof *wl);
        LagAppList *al = malloc(sizeof *al);
        AgClip c;
        uint32_t u32, g; uint16_t w, h; const uint8_t *px; uint8_t form; int on; unsigned d1, d2; uint64_t off; uint32_t dn;
        AgSendOffer o;
        switch (f.kind) {
        case AGK_HELLO: if (f.len >= 200 || !lag_text_ok(f.data, f.len)) bad("hello"); break;
        case AGK_CLIP: if (!ag_utf8_ok(f.data, f.len)) bad("clipboard text"); break;
        case AGK_CLIPX: if (ag_clipx_parse(f.data, f.len, &c)) bad("rich clipboard"); break;
        case AGK_CLIPREQ: if (ag_clipreq_parse(f.data, f.len, &g, &form)) bad("clipboard request"); break;
        case LAG_WINDOWS: if (lag_winlist_read(f.data, f.len, wl)) bad("windows"); break;
        case LAG_WINICON: if (lag_winicon_parse(f.data, f.len, &u32, &w, &h, &px)) bad("window icon"); break;
        case LAG_APPLIST: if (lag_applist_read(f.data, f.len, al)) bad("app list"); break;
        case AGK_NOTIFY: if (ag_note_parse(f.data, f.len, NULL, NULL, NULL, NULL)) bad("notification"); break;
        case AGK_LINK: if (!ag_link_ok(f.data, f.len)) bad("link"); break;
        case AGK_PERFSTATE: if (ag_perfstate_parse(f.data, f.len, &on, &d1, &d2)) bad("performance state"); break;
        case AGK_SEND_OFFER: if (ag_send_offer_parse(f.data, f.len, &o)) bad("file offer"); else { guest_send_id = o.id; guest_send_size = o.size; } break;
        case AGK_SEND_DATA: if (ag_send_data_parse(f.data, f.len, &u32, &off, &px, &dn)) bad("file data"); break;
        }
        free(wl); free(al);
        memmove(pin, pin + used, pin_n - (size_t)used);
        pin_n -= (size_t)used;
    }
}

/* ---- the guest's lord-server: apps mode's window endpoint ---- */
static int win_l = -1, win = -1;
static void win_frame(void) {
    size_t n = 0;
    uint16_t kind;
    switch (below(6)) {
    case 0: { kind = AGK_HELLO; const char *h = chance(50) ? "lord-server 1 clipboard" : "lord-server 1"; n = strlen(h); memcpy(big, h, n); break; }
    case 1: {
        kind = LAG_WINDOWS;
        static LagWinList l;
        l.serial = (uint32_t)rnd();
        l.n = (int)below(chance(90) ? 6 : LAG_WIN_MAX + 1);
        for (int i = 0; i < l.n; i++) {
            LagWinEntry *e = &l.w[i];
            e->id = below(70); e->flags = below(8); e->owner = below(70); e->pid = (uint32_t)rnd();
            e->x = (int32_t)below(4000) - 100; e->y = (int32_t)below(3000) - 100; e->w = 1 + (int32_t)below(3000); e->h = 1 + (int32_t)below(2000);
            size_t t = text((uint8_t *)e->title, 60); e->title[t] = 0;
            for (size_t k = 0; k < t; k++) if (!e->title[k]) e->title[k] = 'x';
            snprintf(e->app, sizeof e->app, "app%u", below(8));
        }
        n = lag_windows_write(&l, big, sizeof big);
        if (!n) { n = below(400); fill(big, n); }
        break;
    }
    case 2: {
        kind = LAG_WINICON;
        int w = 1 + (int)below(LAG_ICON_MAX + (chance(10) ? 4 : 0)), h = 1 + (int)below(LAG_ICON_MAX);
        static uint8_t px[4 * 70 * 70];
        fill(px, (size_t)w * h * 4 < sizeof px ? (size_t)w * h * 4 : sizeof px);
        n = lag_icon_write(below(70), w, h, px, big, sizeof big);
        if (!n) { n = below(400); fill(big, n); }
        break;
    }
    case 3: {
        kind = LAG_APPLIST;
        static LagAppList l;
        l.n = (int)below(chance(90) ? 10 : LAG_APPS_MAX + 1);
        for (int i = 0; i < l.n; i++) {
            size_t t = text((uint8_t *)l.a[i].name, 40); l.a[i].name[t] = 0;
            for (size_t k = 0; k < t; k++) if (!l.a[i].name[k]) l.a[i].name[k] = 'y';
            snprintf(l.a[i].token, sizeof l.a[i].token, "app%u.desktop", below(8));
        }
        n = lag_applist_write(&l, big, sizeof big);
        if (!n) { n = below(400); fill(big, n); }
        break;
    }
    case 4: kind = AGK_CLIP; n = text(big, 500); break;
    default: kind = (uint16_t)below(45); n = below(300); fill(big, n); break;
    }
    n = mutate(big, n, sizeof big);
    frame(win, kind, big, n);
}
static void win_read(void) {
    uint8_t b[65536];
    for (;;) {
        ssize_t r = recv(win, b, sizeof b, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close(win); win = -1; return; }
        if (r < 0) return;
    }
}

/* ---- the X display ---- */
static int x_l = -1, x = -1, x_ready;
static uint8_t xin[1 << 16];
static size_t xin_n;
static uint16_t xseq;
#define X_WID 0x400001u
static void x_setup(void) {
    /* the connection's setup reply: base 0x400000, mask 0x1fffff, vendor "mock", no pixmap formats, one screen */
    uint8_t r[8 + 76];
    memset(r, 0, sizeof r);
    r[0] = 1; r[2] = 11; r[6] = 76 / 4;
    uint8_t *s = r + 8;
    s[4] = 0; s[5] = 0; s[6] = 0x40; s[7] = 0;                  /* resource-id-base 0x400000 */
    s[8] = 0xff; s[9] = 0xff; s[10] = 0x1f; s[11] = 0;          /* mask 0x1fffff */
    s[16] = 4; s[17] = 0;                                       /* vendor length */
    s[18] = 0xff; s[19] = 0xff;                                 /* max request length */
    s[20] = 1; s[21] = 0;                                       /* screens, formats */
    memcpy(s + 32, "mock", 4);
    s[36] = 0x00; s[37] = 0x01;                                 /* root 0x100 */
    if (chance(10)) r[8 + below(76)] ^= (uint8_t)rnd();
    put(x, r, sizeof r);
}
/* the requests the helper made: InternAtom answered (atom 100 + its sequence), the rest need no answer */
static void x_requests(void) {
    for (;;) {
        ssize_t r = recv(x, xin + xin_n, sizeof xin - xin_n, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close(x); x = -1; xin_n = 0; x_ready = 0; return; }
        if (r < 0) break;
        xin_n += (size_t)r;
        if (xin_n == sizeof xin) break;
    }
    for (;;) {
        if (!x_ready) {
            if (xin_n < 12) return;
            size_t an = (size_t)(xin[6] | xin[7] << 8), dn = (size_t)(xin[8] | xin[9] << 8);
            size_t need = 12 + ((an + 3) & ~(size_t)3) + ((dn + 3) & ~(size_t)3);
            if (xin_n < need) return;
            memmove(xin, xin + need, xin_n - need); xin_n -= need;
            x_ready = 1; xseq = 0;
            x_setup();
            continue;
        }
        if (xin_n < 4) return;
        size_t len = (size_t)(xin[2] | xin[3] << 8) * 4;
        if (len < 4) { close(x); x = -1; xin_n = 0; x_ready = 0; return; }
        if (xin_n < len) return;
        xseq++;
        if (xin[0] == 16) {             /* InternAtom */
            uint8_t a[32] = { 1, 0 };
            a[2] = (uint8_t)xseq; a[3] = (uint8_t)(xseq >> 8);
            uint32_t atom = 100 + xseq;
            if (chance(3)) atom = 0;
            memcpy(a + 8, &atom, 4);
            if (chance(3)) { a[4] = (uint8_t)rnd(); }    /* a reply that says it is longer */
            put(x, a, 32);
        }
        memmove(xin, xin + len, xin_n - len); xin_n -= len;
    }
}
/* events and replies the display sends on its own */
static void x_event(void) {
    uint8_t e[32];
    fill(e, 32);
    uint32_t atom = 100 + 1 + below(16), wid = chance(80) ? X_WID : (uint32_t)rnd(), clip = 101;
    switch (below(6)) {
    case 0: case 1:         /* SelectionRequest */
        e[0] = 30;
        memcpy(e + 8, &wid, 4);
        memcpy(e + 16, chance(85) ? &clip : &atom, 4);
        memcpy(e + 20, &atom, 4);
        break;
    case 2: e[0] = 29; memcpy(e + 8, &wid, 4); memcpy(e + 12, &clip, 4); break;            /* SelectionClear */
    case 3: e[0] = 28; e[16] = 1; break;                                                  /* PropertyNotify, deleted */
    case 4: e[0] = 0; break;                                                              /* an error */
    case 5: e[0] = 1; e[4] = (uint8_t)below(4); e[5] = e[6] = e[7] = 0; break;           /* a reply nobody asked for */
    }
    put(x, e, 32);
    if (e[0] == 1) { size_t k = (size_t)e[4] * 4; uint8_t t[16]; fill(t, k); put(x, t, k); }
}

/* ---- the buses: the system bus and the session bus ---- */
typedef struct { int fd, state, monitor; uint8_t in[1 << 16]; size_t n; uint32_t serial; } Bus;
static int sys_l = -1, ses_l = -1;
static Bus buses[4];
static size_t al8(size_t n) { return (n + 7) & ~(size_t)7; }
/* a reply (or an error) to serial with a signature and a body, little-endian */
static size_t db_reply(uint8_t *m, size_t cap, int type, uint32_t serial, uint32_t reply_to, const char *sig, const uint8_t *body, size_t bn) {
    if (cap < 128 + bn) return 0;
    memset(m, 0, 128 + bn);
    m[0] = 'l'; m[1] = (uint8_t)type; m[3] = 1;
    memcpy(m + 8, &serial, 4);
    size_t o = 16;
    m[o] = 5; m[o + 1] = 1; m[o + 2] = 'u'; memcpy(m + o + 4, &reply_to, 4); o += 8;
    if (type == DB_ERROR) {
        o = al8(o);
        const char *en = "org.freedesktop.DBus.Error.Failed";
        uint32_t l = (uint32_t)strlen(en);
        m[o] = 4; m[o + 1] = 1; m[o + 2] = 's'; memcpy(m + o + 4, &l, 4); memcpy(m + o + 8, en, l + 1); o += 8 + l + 1;
    }
    if (sig && *sig) {
        o = al8(o);
        size_t l = strlen(sig);
        m[o] = 8; m[o + 1] = 1; m[o + 2] = 'g'; m[o + 4] = (uint8_t)l; memcpy(m + o + 5, sig, l + 1); o += 5 + l + 1;
    }
    uint32_t fl = (uint32_t)(o - 16), bl = (uint32_t)bn;
    memcpy(m + 12, &fl, 4);
    memcpy(m + 4, &bl, 4);
    o = al8(o);
    memcpy(m + o, body, bn);
    return o + bn;
}
static void bus_answer(Bus *b, const DbMsg *q) {
    uint8_t body[512], m[2048];
    size_t bn = 0, n;
    const char *sig = "";
    int type = DB_RETURN;
    if (!strcmp(q->member, "BecomeMonitor")) b->monitor = 1;
    if (chance(2)) return;                                      /* never answered: the helper's wait runs out */
    if (!strcmp(q->member, "Hello")) {
        sig = "s"; uint32_t l = 4; memcpy(body, &l, 4); memcpy(body + 4, ":1.9", 5); bn = 9;
    } else if (strcmp(q->member, "BecomeMonitor")) {
        switch (below(5)) {
        case 0: { sig = "v"; const char *v = chance(50) ? "performance" : "balanced"; uint32_t l = (uint32_t)strlen(v);
                  body[0] = 1; body[1] = 's'; body[2] = 0; body[3] = 0; memcpy(body + 4, &l, 4); memcpy(body + 8, v, l + 1); bn = 8 + l + 1; break; }
        case 1: { sig = "v"; body[0] = 1; body[1] = 'b'; body[2] = 0; body[3] = 0; uint32_t v = below(2); memcpy(body + 4, &v, 4); bn = 8; break; }
        case 2: type = DB_ERROR; break;
        case 3: sig = chance(50) ? "v" : "s"; bn = below(64); fill(body, bn); break;
        default: break;
        }
    }
    n = db_reply(m, sizeof m, type, ++b->serial, q->serial, sig, body, bn);
    if (chance(15)) n = mutate(m, n, sizeof m);
    put(b->fd, m, n);
}
static void bus_notify(Bus *b) {
    uint8_t body[2048], m[4096];
    DbW w;
    dbw_init(&w, body, sizeof body);
    char s[4][300];
    for (int i = 0; i < 4; i++) { size_t t = text((uint8_t *)s[i], i == 3 ? 290 : 100); s[i][t] = 0; for (size_t k = 0; k < t; k++) if (!s[i][k]) s[i][k] = 'z'; }
    dbw_str(&w, s[0]);
    dbw_u32(&w, below(10));
    dbw_str(&w, "");
    dbw_str(&w, chance(80) ? s[1] : "<b>bold</b> &amp; &lt;x&gt;");
    dbw_str(&w, s[3]);
    const char *acts[] = { "default", "Open" };
    dbw_strv(&w, acts, (int)below(3));
    if (w.bad) return;
    size_t n = w.n;
    while (n % 4) body[n++] = 0;
    memset(body + n, 0, 4); n += 4;                             /* a{sv}: empty */
    while (n % 8) body[n++] = 0;
    int32_t ex = -1; memcpy(body + n, &ex, 4); n += 4;          /* i */
    size_t mn = db_call_build(m, sizeof m, ++b->serial, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", body, n);
    if (!mn) return;
    mn = mutate(m, mn, sizeof m);
    put(b->fd, m, mn);
}
static void bus_read(Bus *b) {
    for (;;) {
        ssize_t r = recv(b->fd, b->in + b->n, sizeof b->in - b->n, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close(b->fd); b->fd = -1; return; }
        if (r < 0) break;
        b->n += (size_t)r;
        if (b->n == sizeof b->in) { close(b->fd); b->fd = -1; return; }
    }
    for (;;) {
        if (b->state < 2) {
            uint8_t *e = memmem(b->in, b->n, "\r\n", 2);
            if (!e) return;
            size_t l = (size_t)(e - b->in) + 2;
            if (b->state == 0) {
                if (chance(5)) put(b->fd, "REJECTED EXTERNAL\r\n", 19);
                else put(b->fd, "OK 0123456789abcdef0123456789abcdef\r\n", 37);
            }
            b->state++;
            memmove(b->in, b->in + l, b->n - l); b->n -= l;
            continue;
        }
        long total = db_msg_total(b->in, b->n);
        if (total < 0) { close(b->fd); b->fd = -1; return; }
        if (!total || (size_t)total > b->n) return;
        DbMsg q;
        if (db_msg_parse(b->in, (size_t)total, &q) > 0 && q.type == DB_CALL) bus_answer(b, &q);
        memmove(b->in, b->in + total, b->n - (size_t)total); b->n -= (size_t)total;
    }
}

/* ---- local programs: the link and notification socket ---- */
static char lsock[300], sendfile_path[300];
static void local_client(void) {
    int c = connect_to(lsock);
    if (c < 0) return;
    uint8_t b[3000];
    size_t n;
    uint16_t kind;
    switch (below(4)) {
    case 0: { kind = AGK_LINK; const char *u = "https://example.org/a?b=c"; n = strlen(u); memcpy(b, u, n); if (chance(50)) n = text(b, 2100); break; }
    case 1: { kind = AGK_NOTIFY; uint8_t t[200], bo[700]; size_t tn = text(t, 130), bn = text(bo, 620); n = ag_note_write(t, tn, bo, bn, b, sizeof b); if (!n) n = text(b, 800); break; }
    case 2: kind = AGK_SEND_OFFER; n = strlen(sendfile_path); memcpy(b, sendfile_path, n); break;
    default: kind = (uint16_t)below(45); n = below(400); fill(b, n); break;
    }
    n = mutate(b, n, sizeof b);
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, (uint32_t)n);
    put(c, h, AG_HEAD);
    put(c, b, n);
    /* its answer isn't waited for: the connection is closed a few programs later */
    static int open_c[4] = { -1, -1, -1, -1 }, at;
    if (open_c[at] >= 0) close(open_c[at]);
    open_c[at] = c;
    at = (at + 1) % 4;
}

/* reads what the helper sent each peer (and answers it as each peer does): never nested */
static void drain(void) {
    static int in;
    if (in) return;
    in = 1;
    if (port >= 0) from_guest();
    if (win >= 0) win_read();
    if (x >= 0) x_requests();
    for (int k = 0; k < 4; k++) if (buses[k].fd >= 0) bus_read(&buses[k]);
    in = 0;
}

static void write_file(const char *path, const char *s, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0 || write(fd, s, strlen(s)) < 0) { perror(path); exit(1); }
    close(fd);
}
/* what the stand-in xclip prints: the clipboard's types and contents, changed by the test */
static void clip_change(void) {
    char p[400];
    static const char *const T[] = { "UTF8_STRING\nTARGETS\n", "text/html\nUTF8_STRING\n", "image/png\n", "x-kde-passwordManagerHint\nUTF8_STRING\n", "" };
    snprintf(p, sizeof p, "%s/types", dir);
    write_file(p, T[below(5)], 0644);
    uint8_t b[2000];
    size_t n = text(b, sizeof b - 1);
    b[n] = 0;
    for (size_t i = 0; i < n; i++) if (!b[i]) b[i] = ' ';
    snprintf(p, sizeof p, "%s/clip", dir);
    write_file(p, (char *)b, 0644);
}

static int run(const char *helper, int idx, int seconds, int windows_first) {
    int f0 = fails;
    char p[400], run_dir[100], home[100], bin[100], portp[300], winp[300], xp[300], sysp[300], sesp[300], errpath[300];
    snprintf(run_dir, sizeof run_dir, "%s/run%d", dir, idx); mkdir(run_dir, 0700);
    snprintf(home, sizeof home, "%s/home", dir); mkdir(home, 0700);
    snprintf(bin, sizeof bin, "%s/bin", dir); mkdir(bin, 0755);
    snprintf(p, sizeof p, "%s/xclip", bin);
    char script[1200];
    snprintf(script, sizeof script, "#!/bin/sh\ncase \"$*\" in *-o*) case \"$*\" in *TARGETS*) cat '%s/types';; *) cat '%s/clip';; esac;; *) cat >/dev/null;; esac\n", dir, dir);
    write_file(p, script, 0755);
    clip_change();
    snprintf(sendfile_path, sizeof sendfile_path, "%s/tosend.txt", dir);
    write_file(sendfile_path, "a file the guest sends to the host\n", 0644);
    snprintf(portp, sizeof portp, "%s/port.sock", run_dir);
    snprintf(winp, sizeof winp, "%s/lordos-guestwin.sock", run_dir);
    snprintf(xp, sizeof xp, "%s/X99", run_dir);
    snprintf(sysp, sizeof sysp, "%s/system_bus", run_dir);
    snprintf(sesp, sizeof sesp, "%s/bus", run_dir);
    snprintf(lsock, sizeof lsock, "%s/lordos-guest.sock", run_dir);
    snprintf(errpath, sizeof errpath, "build/guest-fuzz-%d.err", idx);
    port_l = listen_at(portp);
    x_l = listen_at(xp);
    sys_l = listen_at(sysp);
    ses_l = listen_at(sesp);
    win_l = windows_first ? listen_at(winp) : -1;
    printf("run %d: %d s, %s, the guest's lord-server %s\n", idx, seconds, use_ns ? "as a user namespace's root" : "as this user (no shell frames)",
           windows_first ? "there from the start" : "coming later");
    pid_t pid = fork();
    if (pid == 0) {
        int e = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (e >= 0) { dup2(e, 2); dup2(e, 1); }
        char disp[400];
        snprintf(disp, sizeof disp, "%s%s", idx == 2 ? "" : "unix:", xp);   /* both forms of a socket's path */
        clearenv();
        setenv("PATH", bin, 1);
        setenv("HOME", home, 1);
        setenv("XDG_RUNTIME_DIR", run_dir, 1);
        setenv("DISPLAY", disp, 1);
        setenv("XDG_CURRENT_DESKTOP", idx == 2 ? "GNOME" : "Mock", 1);     /* GNOME: its animations over the session bus too */
        setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", 1);
        setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1", 1);
        setenv("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1", 1);
        if (use_ns) {
            char *args[] = { "/usr/bin/unshare", "-Ur", (char *)helper, "--port", portp, "--windows", winp, "--system-bus", sysp, "--session-bus", sesp, NULL };
            execv(args[0], args);
        } else {
            char *args[] = { (char *)helper, "--port", portp, "--windows", winp, "--system-bus", sysp, "--session-bus", sesp, NULL };
            execv(helper, args);
        }
        _exit(127);
    }
    long long end = now_ms() + seconds * 1000LL, win_at = now_ms() + 1500, next_local = 0;
    int hellos = 0, conns = 0, xconns = 0, wconns = 0, bconns = 0, locals = 0, sent = 0, died = 0;
    for (int i = 0; i < 4; i++) buses[i].fd = -1;
    while (now_ms() < end) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { died = 1; break; }
        if (win_l < 0 && now_ms() >= win_at) win_l = listen_at(winp);
        int c;
        if (port < 0 && (c = accept4(port_l, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) { port = c; pin_n = 0; conns++; host_hello(); hellos++; }
        if ((c = accept4(x_l, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) { if (x >= 0) close(x); x = c; xin_n = 0; x_ready = 0; xconns++; }
        if (win_l >= 0 && (c = accept4(win_l, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) { if (win >= 0) close(win); win = c; wconns++; }
        for (int l = 0; l < 2; l++)
            if ((c = accept4(l ? ses_l : sys_l, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
                int k = 0;
                while (k < 4 && buses[k].fd >= 0) k++;
                if (k == 4) close(c);
                else { memset(&buses[k], 0, sizeof buses[k]); buses[k].fd = c; bconns++; }
            }
        if (port >= 0) {
            from_guest();
            for (int k = 0; k < 4 && port >= 0; k++) { if (chance(90)) host_frame(); else host_raw(); sent++; }
            if (port >= 0 && chance(1)) { close(port); port = -1; }      /* lord-vm went away; the helper comes back */
        }
        if (x >= 0) { x_requests(); if (x >= 0 && x_ready && chance(30)) x_event(); }
        if (win >= 0) { win_read(); if (win >= 0) win_frame(); if (win >= 0 && chance(1)) { close(win); win = -1; } }
        for (int k = 0; k < 4; k++) if (buses[k].fd >= 0) { bus_read(&buses[k]); if (buses[k].fd >= 0 && buses[k].monitor && chance(30)) bus_notify(&buses[k]); }
        if (now_ms() >= next_local) { local_client(); locals++; next_local = now_ms() + 10; }
        if (chance(2)) clip_change();
        struct pollfd pf[1] = { { port, POLLIN, 0 } };
        poll(pf, 1, 1);
    }
    if (!died && waitpid(pid, NULL, WNOHANG) == pid) died = 1;
    check(!died, "it lived through %d frames from the host (%d connections), %d X connections, %d window endpoint connections, %d bus connections, %d local programs",
          sent, conns, xconns, wconns, bconns, locals);
    check(conns > 0 && (xconns > 0 || windows_first) && wconns > 0 && bconns > 0, "every side was reached%s", windows_first ? " (no X display on a LordOS desktop)" : "");
    check(!bad_out, "all %d frames it sent the host are ones the host's parsers take%s%s", frames_out, bad_out ? ": not " : "", bad_out ? bad_what : "");
    int st = 0;
    if (!died) {
        kill(pid, SIGTERM);
        long long t = now_ms() + 30000;     /* it puts the performance profile back first: a bus call may take 3 s */
        pid_t w = 0;
        while (now_ms() < t && (w = waitpid(pid, &st, WNOHANG)) == 0) { if (port >= 0) from_guest(); struct pollfd q = { -1, 0, 0 }; poll(&q, 0, 10); }
        if (w != pid) {
            char cmd[200];       /* where it hangs, for the log */
            snprintf(cmd, sizeof cmd, "cat /proc/%d/syscall >&2; gdb -batch -ex bt -p %d >&2 2>/dev/null", (int)pid, (int)pid);
            if (system(cmd)) {}
            kill(pid, SIGKILL); waitpid(pid, &st, 0);
        }
        check(w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "SIGTERM: it ends with 0 (%s %d)", WIFEXITED(st) ? "status" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    }
    FILE *f = fopen(errpath, "r");
    int report = 0;
    char line[512];
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "Sanitizer") || strstr(line, "runtime error")) { if (!report) fprintf(stderr, "%s", line); report = 1; }
    if (f) fclose(f);
    check(!report, "no ASan or UBSan report (%s)", errpath);
    if (port >= 0) close(port);
    if (x >= 0) close(x);
    if (win >= 0) close(win);
    for (int k = 0; k < 4; k++) if (buses[k].fd >= 0) close(buses[k].fd);
    close(port_l); close(x_l); close(sys_l); close(ses_l); if (win_l >= 0) close(win_l);
    port = x = win = port_l = x_l = sys_l = ses_l = win_l = -1;
    bad_out = frames_out = 0;
    (void)hellos;
    return fails > f0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: guest-fuzz build/lordos-guest-asan [seed] [seconds per run]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9e57f022;
    int secs = argc > 3 ? atoi(argv[3]) : 8;
    rs = seed ? seed : 1;
    char cwd[200];
    if (!getcwd(cwd, sizeof cwd)) return 1;
    /* short socket paths: a Unix socket's path has 108 bytes */
    snprintf(dir, sizeof dir, "/tmp/lordos-guest-fuzz-%d", (int)getpid());
    if (mkdir(dir, 0700)) { perror(dir); return 1; }
    use_ns = system("unshare -Ur true >/dev/null 2>&1") == 0;
    printf("lordos-guest against hostile peers (seed 0x%" PRIx64 "):\n", seed);
    run(argv[1], 0, secs, 0);
    run(argv[1], 1, secs, 1);
    run(argv[1], 2, secs, 0);
    char rm[300];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    if (system(rm)) {}
    printf("%s\n", fails ? "guest-fuzz: FAILED" : "guest-fuzz: all passed");
    return fails ? 1 : 0;
}
