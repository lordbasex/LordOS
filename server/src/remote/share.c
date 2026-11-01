/* share.c — the public screen-sharing API, in lord-server (docs/SCREEN-SHARING.md).
 *
 * A separate door: $XDG_RUNTIME_DIR/lordos/share.sock (0600), with its own table and parser (sharewire.c).
 * It shares with remote.sock only the composed frame, the session's encoder and the media and input formats.
 * Everything here runs on the main loop, so one request is decided before the next is read.
 *
 * It fails closed:
 *  - who connects comes from the peer's root-owned scope, through a pidfd so a dead pid can't be reused;
 *    a process in no app scope is turned away;
 *  - a start needs a stored grant, or a person to ask; never a default;
 *  - no frame leaves until the shell says the indicator is on screen;
 *  - the session's lock state lives here and is set synchronously, in the call that locks, before the next
 *    frame is composed: frames, sound, the cursor and the clipboard stop, what is still queued is thrown away,
 *    and input is dropped. It clears only when lord-sessiond lends the monitor back, which needs the password;
 *  - the login screen's lord-server never calls share_start, so there is no socket to connect to. */
#include "share.h"
#include "sharegrant.h"
#include "sharewire.h"
#include "mirror.h"
#include "proto.h"
#include "../sec/appid.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

#define MAX_CONN 4
#define IN_MAX (SW_MAX_MSG + 64 * 1024)
#define OUT_MAX (8u << 20)              /* a client this far behind is cut */
#define VIDEO_LIMIT (3u << 20)          /* this far behind it skips to the next keyframe, as a 5673 viewer */
#define INDICATOR_MS 2000.0             /* no indicator within this: no_indicator */
#define PROMPT_MS 60000.0
#define RECHECK_MS 1000.0               /* the grant is read again this often: revoking ends a share within it */
#define PTR_RATE 1000.0                 /* a second */
#define KEY_RATE 200.0
#define OTHER_RATE 20.0                 /* resize, rate, text, clipboard, awake */

typedef enum { C_IDLE, C_ASKING, C_BADGE, C_ACTIVE } CState;

typedef struct {
    int fd;
    uint8_t *in, *out;
    size_t in_len, out_len, out_off, out_cap;
    size_t head;                        /* where the message being sent starts (head <= out_off) */
    CState st;
    int pidfd;
    char id[SG_ID_MAX], exe[SG_EXE_MAX];
    int flags, granted, codec, hz, hear, awake, need_key, cur_x, cur_y, cur_mode;
    unsigned kbps, clip_gen, token;
    double since;
    SwKeyLimit keys;
    SwBucket b_ptr, b_key, b_other;
    unsigned char held[0x300 / 8];
    int buttons;
    long frames, dropped;
} Conn;

static int lfd = -1;
static char sock_path[256];
static Conn cn[MAX_CONN];
static Conn *holder;
static ShareHost h;
static int session;                     /* 0, SW_LOCKED or SW_AWAY */
static unsigned next_token = 1;
static SgStore store;
static char store_path[96];
static struct timespec store_mtime;
static double recheck_at;
static const uint32_t *last_shape;
static int shape_w, shape_h, shape_hx, shape_hy;
static int vw, vh;
static unsigned clip_gen_now;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- output ---- */

static void put(Conn *c, const void *p, size_t n) {
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
}

/* Whole or not at all: the stream never holds half a message. */
static void msg(Conn *c, int type, const void *a, size_t an, const void *b, size_t bn) {
    size_t need = 5 + an + bn;
    if (c->out_len - c->out_off > OUT_MAX) return;
    if (c->out_len + need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 65536;
        while (cap < c->out_len + need) cap *= 2;
        uint8_t *nb = realloc(c->out, cap);
        if (!nb) return;
        c->out = nb;
        c->out_cap = cap;
    }
    uint8_t hd[5];
    uint32_t len = (uint32_t)(1 + an + bn);
    memcpy(hd, &len, 4);
    hd[4] = (uint8_t)type;
    put(c, hd, 5);
    if (an) put(c, a, an);
    if (bn) put(c, b, bn);
}

/* Follows the message boundary, so what is queued can be thrown away without cutting one in half. */
static void advance_head(Conn *c) {
    while (c->out_len - c->head >= 4) {
        uint32_t l;
        memcpy(&l, c->out + c->head, 4);
        if (c->head + 4 + l > c->out_off) break;
        c->head += 4 + (size_t)l;
    }
}

static void flush(Conn *c) {
    while (c->out_off < c->out_len) {
        ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        c->out_off += (size_t)w;
    }
    advance_head(c);
    if (c->out_off == c->out_len) c->out_off = c->out_len = c->head = 0;
    else if (c->head > (1u << 20)) {
        memmove(c->out, c->out + c->head, c->out_len - c->head);
        c->out_len -= c->head;
        c->out_off -= c->head;
        c->head = 0;
    }
}

/* What hasn't left yet doesn't leave: only the rest of the message already on its way. */
static void discard_queued(Conn *c) {
    advance_head(c);
    if (c->head >= c->out_off) c->out_len = c->out_off = c->head = 0;
    else {
        uint32_t l;
        memcpy(&l, c->out + c->head, 4);
        c->out_len = c->head + 4 + (size_t)l;
    }
}

static void say(Conn *c, int type, int reason) {
    uint8_t r = (uint8_t)reason;
    msg(c, type, &r, 1, NULL, 0);
    flush(c);
}

/* ---- identity ---- */

static pid_t pid_of_pidfd(int fd) {
    char p[64], line[128];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    pid_t pid = -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "Pid: %d", &pid) == 1) break;
    fclose(f);
    return pid;
}

/* Who is at the other end, now: its scope and program. Asked at connection and again at each start. */
static int identify(Conn *c) {
    pid_t pid = pid_of_pidfd(c->pidfd);
    char id[SG_ID_MAX], lnk[64], exe[SG_EXE_MAX];
    if (pid <= 0 || appid_of_pid((unsigned)pid, id, sizeof id) != 0 || !sg_id_valid(id)) return -1;
    snprintf(lnk, sizeof lnk, "/proc/%d/exe", (int)pid);
    ssize_t l = readlink(lnk, exe, sizeof exe - 1);
    if (l <= 0) return -1;
    exe[l] = 0;
    /* read after the pid: still the same process, or the pid above may have been someone else's */
    if (pid_of_pidfd(c->pidfd) != pid) return -1;
    if (!sg_exe_valid(exe)) exe[0] = 0;
    if (c->id[0] && strcmp(c->id, id)) return -1;      /* a connection doesn't change app */
    snprintf(c->id, sizeof c->id, "%s", id);
    snprintf(c->exe, sizeof c->exe, "%s", exe);
    return 0;
}

/* ---- the end of a share ---- */

static void release_input(Conn *c) {
    for (int i = 0; i < (int)sizeof c->held * 8; i++)
        if (c->held[i / 8] & (1u << (i % 8))) h.key(i, 0);
    memset(c->held, 0, sizeof c->held);
    for (int b = 0; b < 3; b++) if (c->buttons & (1 << b)) h.button(b, 0);
    c->buttons = 0;
    if (h.no_taps) h.no_taps();
}

static int want_input(const Conn *c) { return (c->flags & (SW_F_INPUT | SW_F_CLIPBOARD)) != 0; }

static void shell_event(const char *t, const Conn *c) {
    if (!h.event) return;
    char id[SG_ID_MAX * 6 + 3], exe[SG_EXE_MAX * 6 + 3], j[sizeof id + sizeof exe + 128];
    if (c) {
        json_quote(id, sizeof id, c->id);
        json_quote(exe, sizeof exe, c->exe);
        int ask_s = sg_get(&store, c->id, c->exe, SG_SCREEN) == SG_ASK, ask_i = want_input(c) && sg_get(&store, c->id, c->exe, SG_INPUT) == SG_ASK;
        snprintf(j, sizeof j, "{\"t\":\"%s\",\"token\":%u,\"app\":%s,\"exe\":%s,\"input\":%s,\"ask\":[%s%s%s]}", t, c->token, id, exe,
                 want_input(c) ? "true" : "false", ask_s ? "\"screen-share\"" : "", ask_s && ask_i ? "," : "", ask_i ? "\"remote-input\"" : "");
    }
    else snprintf(j, sizeof j, "{\"t\":\"%s\"}", t);
    h.event(j);
}

/* Ends what c holds. Synchronous: when this returns the session's frames no longer reach it, its queue is gone
 * if the picture must not leave (a lock), and the other door is free. */
static void end_share(Conn *c, int reason, int discard) {
    if (c->st == C_IDLE) return;
    CState was = c->st;
    int ran = was == C_ACTIVE;
    c->st = C_IDLE;
    if (holder == c) holder = NULL;
    if (ran || was == C_BADGE) release_input(c);
    if (discard) discard_queued(c);
    say(c, ran || was == C_BADGE ? SHARE_END : SHARE_REFUSED, reason);
    fprintf(stderr, "share: %s %s (%s), %ld frames, %ld inputs dropped\n", c->id, ran ? "ended" : "refused", sw_reason_name(reason), c->frames, c->dropped);
    c->granted = c->hear = c->awake = c->need_key = 0;
    c->frames = c->dropped = 0;
    shell_event("end", NULL);
    if (ran) mirror_share_changed();    /* the encoder stops; a 5673 sign-in may come in */
}

static void drop_conn(Conn *c) {
    end_share(c, SW_ENDED_BY_APP, 1);
    close(c->fd);
    if (c->pidfd >= 0) close(c->pidfd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->pidfd = -1;
}

/* ---- grants ---- */

static void store_read(void) {
    struct stat st;
    if (stat(store_path, &st)) { store.n = 0; memset(&store_mtime, 0, sizeof store_mtime); return; }
    sg_load(store_path, &store);
    store_mtime = st.st_mtim;
}

/* Would a start be refused now, whatever the app's grant? */
static int blocked_now(void) {
    if (session) return session;
    if (holder) return SW_BUSY;
    if (mirror_viewers()) return SW_REMOTE_DESKTOP;
    return SW_OK;
}

static void go_badge(Conn *c) {
    int gr = c->flags & (SW_F_VIDEO | SW_F_AUDIO);
    if (want_input(c) && sg_get(&store, c->id, c->exe, SG_INPUT) == SG_ALLOW) gr |= c->flags & (SW_F_INPUT | SW_F_CLIPBOARD);
    c->granted = gr;
    c->st = C_BADGE;
    c->since = now_ms();
    c->token = next_token++;
    shell_event("start", c);
}

/* Goes on from the store's current answer: called for a start and again after the prompt. */
static void decide(Conn *c, int asked) {
    int blocked = session ? session : mirror_viewers() ? SW_REMOTE_DESKTOP : SW_OK;
    if (blocked) { end_share(c, blocked, 1); return; }
    if (identify(c)) { end_share(c, SW_NO_IDENTITY, 1); return; }
    if (h.restricted && h.restricted(c->id)) { end_share(c, SW_RESTRICTED, 1); return; }
    SgState v = sg_get(&store, c->id, c->exe, SG_SCREEN);
    SgState in = want_input(c) ? sg_get(&store, c->id, c->exe, SG_INPUT) : SG_DENY;
    if (v == SG_DENY) { end_share(c, SW_DENIED, 1); return; }
    if (v == SG_ALLOW && (in != SG_ASK || asked || !h.person || !h.person())) { go_badge(c); return; }   /* no person: no input, not no share */
    if (asked) { end_share(c, SW_DENIED, 1); return; }
    if (!h.person || !h.person()) { end_share(c, SW_NO_PERSON, 1); return; }
    c->st = C_ASKING;
    c->since = now_ms();
    c->token = next_token++;
    shell_event("ask", c);
}

/* ---- messages ---- */

static void send_state(Conn *c) {
    uint8_t m[10] = { 0 };
    uint16_t v[3] = { (uint16_t)vw, (uint16_t)vh, (uint16_t)(mirror_refresh()) };
    m[0] = (uint8_t)(c == holder ? SW_OK : blocked_now());
    int g = 0;
    if (sg_get(&store, c->id, c->exe, SG_SCREEN) == SG_ALLOW) g |= SW_F_VIDEO | SW_F_AUDIO;
    if (sg_get(&store, c->id, c->exe, SG_INPUT) == SG_ALLOW) g |= SW_F_INPUT | SW_F_CLIPBOARD;
    m[1] = (uint8_t)g;
    m[2] = 3;
    memcpy(m + 4, v, 6);
    msg(c, SHARE_STATE, m, sizeof m, NULL, 0);
    flush(c);
}

static void on_start(Conn *c, const SwMsg *m) {
    if (c->st != C_IDLE) { say(c, SHARE_REFUSED, SW_BAD_REQUEST); return; }
    int b = blocked_now();
    if (b) { say(c, SHARE_REFUSED, b); return; }
    c->flags = m->flags;
    c->codec = m->codec;
    c->hz = m->hz;
    c->kbps = m->kbps;
    c->clip_gen = clip_gen_now;         /* only what is copied from now on */
    c->cur_x = c->cur_y = c->cur_mode = -1;
    c->st = C_ASKING;                   /* holds the session from here: whoever arrives next gets busy */
    holder = c;
    store_read();
    decide(c, 0);
}

static void input(Conn *c, const SwMsg *m) {
    double t = now_ms();
    /* no input through this door unless a share is running, unlocked, with input granted */
    if (c->st != C_ACTIVE || c != holder || session || !(c->granted & SW_F_INPUT)) { c->dropped++; return; }
    switch (m->type) {
    case RP_PTR:
        if (!sw_bucket_take(&c->b_ptr, t, PTR_RATE, PTR_RATE / 10)) { c->dropped++; return; }
        if (h.surface_up()) { c->dropped++; return; }
        h.pointer(m->x >= vw ? vw - 1 : m->x, m->y >= vh ? vh - 1 : m->y);
        break;
    case RP_BTN:
        if (!sw_bucket_take(&c->b_key, t, KEY_RATE, KEY_RATE / 4)) { c->dropped++; return; }
        /* a release always goes, so nothing stays pressed */
        if (m->down && (h.surface_up() || h.point_protected())) { c->dropped++; return; }
        if (m->down) c->buttons |= 1 << m->button;
        else if (!(c->buttons & (1 << m->button))) return;
        else c->buttons &= ~(1 << m->button);
        h.button(m->button, m->down);
        break;
    case RP_WHEEL:
    case RP_SCROLL:
        if (!sw_bucket_take(&c->b_ptr, t, PTR_RATE, PTR_RATE / 10)) { c->dropped++; return; }
        if (h.surface_up() || h.point_protected()) { c->dropped++; return; }
        if (m->type == RP_WHEEL) h.wheel(m->steps);
        else h.scroll(m->phase, m->dx / 256.0, m->dy / 256.0);
        break;
    case RP_KEY: {
        int on = (c->held[m->code / 8] >> (m->code % 8)) & 1;
        if (!sw_bucket_take(&c->b_key, t, KEY_RATE, KEY_RATE / 4) && (m->value || !on)) { c->dropped++; return; }
        if (m->value && (h.surface_up() || h.key_protected())) { c->dropped++; return; }
        if (!m->value && !on) return;   /* a release of what wasn't pressed here */
        if (m->value) c->held[m->code / 8] |= (unsigned char)(1u << (m->code % 8));
        else c->held[m->code / 8] &= (unsigned char)~(1u << (m->code % 8));
        h.key(m->code, m->value);
        break;
    }
    case SHARE_TEXT:
        if (!sw_bucket_take(&c->b_other, t, OTHER_RATE, OTHER_RATE) || h.surface_up() || h.key_protected()) { c->dropped++; return; }
        if (h.text) h.text(m->text, m->tlen);
        break;
    default: break;
    }
}

static void handle(Conn *c, const SwMsg *m) {
    double t = now_ms();
    switch (m->type) {
    case SHARE_START: on_start(c, m); break;
    case SHARE_QUERY:
        if (sw_bucket_take(&c->b_other, t, OTHER_RATE, OTHER_RATE)) send_state(c);
        break;
    case SHARE_STOP: end_share(c, SW_ENDED_BY_APP, 0); break;
    case SHARE_LOCK:
        /* the session is what ends the share: share_session runs inside mirror_session */
        if (c->st == C_ACTIVE && c == holder) mirror_session("{\"a\":\"lock\"}");
        break;
    case SHARE_AWAKE: if (c->st == C_ACTIVE) c->awake = m->on; break;
    case SHARE_HEAR: if (c->st == C_ACTIVE) c->hear = m->on; break;
    case RP_KEYFRAME:
        if (c->st == C_ACTIVE) { sw_key_request(&c->keys); c->need_key = 1; }
        break;
    case RP_RATE:
        if (c->st == C_ACTIVE && sw_bucket_take(&c->b_other, t, OTHER_RATE, OTHER_RATE)) { c->kbps = m->kbps; mirror_share_changed(); }
        break;
    case RP_RESIZE:
        if (c->st == C_ACTIVE && (c->granted & SW_F_INPUT) && sw_bucket_take(&c->b_other, t, OTHER_RATE / 10, 2)) h.resize(m->x, m->y);
        break;
    case RP_SETCLIP:
        if (c->st != C_ACTIVE || session || !(c->granted & SW_F_CLIPBOARD) || !sw_bucket_take(&c->b_other, t, OTHER_RATE, OTHER_RATE)) { c->dropped++; break; }
        h.clipboard(m->text, m->tlen);
        break;
    default: input(c, m); break;
    }
}

static void read_conn(Conn *c) {
    for (int rounds = 0; rounds < 8; rounds++) {
        if (IN_MAX - c->in_len < 4096) { drop_conn(c); return; }
        ssize_t r = recv(c->fd, c->in + c->in_len, IN_MAX - c->in_len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) return;
        if (r <= 0) { drop_conn(c); return; }
        c->in_len += (size_t)r;
        size_t off = 0;
        for (;;) {
            SwFrame f;
            SwMsg m;
            int k = sw_next(c->in + off, c->in_len - off, &f);
            if (k < 0) { drop_conn(c); return; }
            if (!k) break;
            if (sw_parse(&f, &m)) {     /* a malformed message ends the connection, share included */
                fprintf(stderr, "share: %s sent a bad message (type %d, %u bytes)\n", c->id, f.type, f.n);
                drop_conn(c);
                return;
            }
            handle(c, &m);
            off += f.used;
        }
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

/* ---- the listener ---- */

int share_start(const ShareHost *host) {
    h = *host;
    for (int i = 0; i < MAX_CONN; i++) { cn[i].fd = -1; cn[i].pidfd = -1; }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return -1;
    char dir[200];
    snprintf(dir, sizeof dir, "%s/lordos", rt);
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return -1;
    snprintf(sock_path, sizeof sock_path, "%s/share.sock", dir);
    unlink(sock_path);
    sg_path(getuid(), store_path, sizeof store_path);
    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (lfd < 0 || strlen(sock_path) >= sizeof a.sun_path) { if (lfd >= 0) close(lfd); lfd = -1; return -1; }
    memcpy(a.sun_path, sock_path, strlen(sock_path) + 1);
    mode_t old = umask(077);
    int r = bind(lfd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r || listen(lfd, 4)) { close(lfd); lfd = -1; return -1; }
    return 0;
}

void share_stop(void) {
    if (lfd < 0) return;
    for (int i = 0; i < MAX_CONN; i++) if (cn[i].fd >= 0) drop_conn(&cn[i]);
    close(lfd);
    unlink(sock_path);
    lfd = -1;
}

int share_pollfds(struct pollfd *p, int max) {
    int n = 0;
    if (lfd < 0 || max < 1) return 0;
    p[n++] = (struct pollfd){ lfd, POLLIN, 0 };
    for (int i = 0; i < MAX_CONN && n < max; i++)
        if (cn[i].fd >= 0) p[n++] = (struct pollfd){ cn[i].fd, (short)(POLLIN | (cn[i].out_len > cn[i].out_off ? POLLOUT : 0)), 0 };
    return n;
}

static void on_accept(void) {
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    struct ucred cr;
    socklen_t l = sizeof cr;
    int pidfd = -1;
    socklen_t pl = sizeof pidfd;
    Conn *c = NULL;
    for (int i = 0; i < MAX_CONN && !c; i++) if (cn[i].fd < 0) c = &cn[i];
    uint8_t *in = c ? malloc(IN_MAX) : NULL;
    if (!in || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) || cr.uid != getuid()) { free(in); close(fd); return; }
    /* a kernel without SO_PEERPIDFD gets the same answer as a process with no scope */
    if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &pl) || pidfd < 0) pidfd = -1;
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->pidfd = pidfd;
    c->in = in;
    sw_key_init(&c->keys, now_ms());
    if (pidfd < 0 || identify(c)) {
        uint8_t hd[6] = { 2, 0, 0, 0, SHARE_REFUSED, SW_NO_IDENTITY };
        if (send(fd, hd, sizeof hd, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { /* it's gone anyway */ }
        fprintf(stderr, "share: a process in no app scope was turned away\n");
        drop_conn(c);
    }
}

void share_dispatch(const struct pollfd *p, int n) {
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (p[i].fd == lfd) { on_accept(); continue; }
        Conn *c = NULL;
        for (int k = 0; k < MAX_CONN && !c; k++) if (cn[k].fd == p[i].fd) c = &cn[k];
        if (!c) continue;
        if (p[i].revents & POLLOUT) flush(c);
        if (p[i].revents & POLLIN) { read_conn(c); continue; }
        if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) drop_conn(c);
    }
}

/* ---- the session's state ---- */

void share_session(ShareSession what) {
    if (lfd < 0) return;
    if (what == SHARE_S_SHOWN) {        /* lent by lord-sessiond: the person signed in, or the session just began */
        session = 0;
        return;
    }
    if (what == SHARE_S_LOCK) session = SW_LOCKED;
    else if (session != SW_LOCKED) session = SW_AWAY;   /* a lock stays a lock when the monitor then leaves */
    for (int i = 0; i < MAX_CONN; i++) if (cn[i].fd >= 0) end_share(&cn[i], session, 1);
}

/* ---- mirror.c ---- */

static Conn *running(void) { return holder && holder->st == C_ACTIVE && !session ? holder : NULL; }

int share_watching(void) { return running() != NULL; }
int share_codec(void) { const Conn *c = running(); return c ? c->codec : 0; }
int share_audio_on(void) { const Conn *c = running(); return c ? (c->flags & SW_F_AUDIO) != 0 : 0; }
unsigned share_kbps(void) { const Conn *c = running(); return c ? c->kbps : 0; }
int share_fps(void) { const Conn *c = running(); return c ? c->hz : 0; }
const char *share_holder(void) { return holder && holder->st != C_IDLE ? holder->id : NULL; }

static int backlog(const Conn *c) { return c->out_len - c->out_off > VIDEO_LIMIT; }

void share_video(int codec, int key, uint32_t ms, const uint8_t *p, size_t n) {
    Conn *c = running();
    if (!c || c->codec != codec) return;
    if (c->need_key && !key) return;
    if (backlog(c)) { c->need_key = 1; sw_key_request(&c->keys); return; }
    uint8_t hd[8] = { (uint8_t)codec, (uint8_t)(key != 0), 0, 0 };
    memcpy(hd + 4, &ms, 4);
    msg(c, RP_VFRAME, hd, 8, p, n);
    if (key) c->need_key = 0;
    c->frames++;
    flush(c);
}

void share_audio(int codec, uint32_t samples, const uint8_t *p, size_t n) {
    Conn *c = running();
    if (!c || c->codec != codec || !c->hear || !(c->granted & SW_F_AUDIO) || backlog(c)) return;
    msg(c, RP_AFRAME, &samples, 4, p, n);
    flush(c);
}

void share_cursor(int x, int y, int mode) {
    Conn *c = running();
    if (!c || (c->cur_x == x && c->cur_y == y && c->cur_mode == mode)) return;
    uint8_t m[5];
    int16_t a = (int16_t)x, b = (int16_t)y;
    memcpy(m, &a, 2);
    memcpy(m + 2, &b, 2);
    m[4] = (uint8_t)mode;
    c->cur_x = x;
    c->cur_y = y;
    c->cur_mode = mode;
    msg(c, RP_CURSOR, m, 5, NULL, 0);
    flush(c);
}

void share_shape(const uint32_t *argb, int w, int hg, int hx, int hy) {
    last_shape = argb;
    shape_w = w; shape_h = hg; shape_hx = hx; shape_hy = hy;
    Conn *c = running();
    if (!c || !argb) return;
    uint16_t s[4] = { (uint16_t)w, (uint16_t)hg, (uint16_t)hx, (uint16_t)hy };
    msg(c, RP_SHAPE, s, 8, argb, (size_t)w * hg * 4);
    flush(c);
}

void share_size(int w, int hg) {
    vw = w;
    vh = hg;
    Conn *c = running();
    if (!c) return;
    uint16_t s[2] = { (uint16_t)w, (uint16_t)hg };
    msg(c, RP_SIZE, s, 4, NULL, 0);
    flush(c);
}

void share_vinfo(int codec, const void *m, size_t n) {
    Conn *c = running();
    if (!c || c->codec != codec) return;
    msg(c, RP_VINFO, m, n, NULL, 0);
    flush(c);
}

void share_changed(void) { if (holder) sw_key_changed(&holder->keys); }

void share_clipboard(const char *text, unsigned gen, int sensitive) {
    if (gen == clip_gen_now) return;
    clip_gen_now = gen;
    Conn *c = running();
    if (!c || !(c->granted & SW_F_CLIPBOARD) || sensitive || !text || gen == c->clip_gen) return;
    c->clip_gen = gen;
    size_t n = strlen(text);
    if (n > SW_MAX_MSG) return;
    msg(c, RP_CLIP, text, n, NULL, 0);
    flush(c);
}

/* ---- the shell ---- */

void share_shown(unsigned token) {
    Conn *c = holder;
    if (!c || c->st != C_BADGE || c->token != token) return;
    if (session) { end_share(c, session, 1); return; }
    c->st = C_ACTIVE;
    c->need_key = 1;
    fprintf(stderr, "share: %s started (%s, %u kbit/s)\n", c->id, c->codec == 1 ? "H.264" : "VP8", c->kbps);
    send_state(c);
    uint16_t s[2] = { (uint16_t)vw, (uint16_t)vh };
    msg(c, RP_SIZE, s, 4, NULL, 0);
    if (last_shape) {
        uint16_t q[4] = { (uint16_t)shape_w, (uint16_t)shape_h, (uint16_t)shape_hx, (uint16_t)shape_hy };
        msg(c, RP_SHAPE, q, 8, last_shape, (size_t)shape_w * shape_h * 4);
    }
    flush(c);
    recheck_at = now_ms() + RECHECK_MS;
    mirror_share_changed();
}

void share_hidden(void) { if (holder) end_share(holder, SW_NO_INDICATOR, 1); }
void share_shell_gone(void) { share_hidden(); }

void share_answer(unsigned token) {
    Conn *c = holder;
    if (!c || c->st != C_ASKING || c->token != token) return;
    store_read();
    decide(c, 1);
}

int share_user_stop(void) {
    if (!holder || holder->st == C_IDLE) return 0;
    end_share(holder, SW_STOPPED, 1);
    return 1;
}

void share_status(char *json, size_t n) {
    const Conn *c = holder;
    if (c && (c->st == C_BADGE || c->st == C_ACTIVE)) {
        char id[SG_ID_MAX * 6 + 3];
        json_quote(id, sizeof id, c->id);
        snprintf(json, n, "{\"app\":%s,\"token\":%u,\"input\":%s}", id, c->token, c->granted & SW_F_INPUT ? "true" : "false");
    } else snprintf(json, n, "null");
}

/* The store as it is now against what is running: revoking ends it, losing input takes the input away. */
static void recheck(Conn *c) {
    store_read();
    if (sg_get(&store, c->id, c->exe, SG_SCREEN) != SG_ALLOW) { end_share(c, SW_REVOKED, 1); return; }
    if (h.restricted && h.restricted(c->id)) { end_share(c, SW_RESTRICTED, 1); return; }
    if ((c->granted & (SW_F_INPUT | SW_F_CLIPBOARD)) && sg_get(&store, c->id, c->exe, SG_INPUT) != SG_ALLOW) {
        release_input(c);
        c->granted &= ~(SW_F_INPUT | SW_F_CLIPBOARD);
        send_state(c);
        shell_event("input", NULL);
    }
}

void share_reload(void) {
    if (!holder) return;
    if (holder->st == C_ACTIVE) recheck(holder);
    else store_read();
}

void share_tick(void) {
    Conn *c = holder;
    if (!c) return;
    double t = now_ms();
    if (c->st == C_BADGE && t - c->since >= INDICATOR_MS) { end_share(c, SW_NO_INDICATOR, 1); return; }
    if (c->st == C_ASKING && t - c->since >= PROMPT_MS) { end_share(c, SW_NO_PERSON, 1); return; }
    if (c->st != C_ACTIVE) return;
    if (t >= recheck_at) {
        recheck_at = t + RECHECK_MS;
        recheck(c);
        if (holder != c) return;
        if (c->awake && h.activity) h.activity();
    }
    if (sw_key_due(&c->keys, t)) mirror_share_keyframe(c->codec);
}

double share_timeout(void) {
    const Conn *c = holder;
    if (!c) return -1;
    double t = now_ms();
    if (c->st == C_BADGE) return c->since + INDICATOR_MS - t;
    if (c->st == C_ASKING) return c->since + PROMPT_MS - t;
    double d = recheck_at - t, k = sw_key_wait(&c->keys, t);
    if (k >= 0 && k < d) d = k;
    return d < 0 ? 0 : d;
}
