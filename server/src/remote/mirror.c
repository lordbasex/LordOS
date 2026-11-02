/* mirror.c — the lord-server session as seen from the remote desktop.
 *
 * lord-server (already as the session's user) listens on $XDG_RUNTIME_DIR/lordos/remote.sock;
 * it only accepts root (lord-sessiond: the monitor and the network bridge) and the same user (SO_PEERCRED). Through it:
 *   - the screen goes to the session's shared live encoder (media/mstream.c) for every viewer that
 *     subscribed to the video (RP_VIDEO); the encoded frames go back to each one (RP_VFRAME);
 *   - on each frame it compares with the previous copy in 64×64 tiles: what changed is announced at
 *     once (RP_DAMAGE) and, once the screen has been still for STILL_MS, sent as lossless rectangles
 *     (RP_RECT) to each viewer whose video plays (RP_VMODE), which the browser draws over the video;
 *   - nothing is captured or compared while no viewer takes the video (mirror_watchers());
 *   - it sends the cursor position and image separately, and the text clipboard;
 *   - it receives mouse, keyboard and clipboard, which main.c injects as if they were local.
 * lord-sessiond (docs/SESSIONS.md) connects as root and marks itself with RP_SEAT: it doesn't get the
 * screen, only the session's requests (lock, switch user, the login
 * screen), and it lends the monitor with RP_DISPLAY (the card and the input as fds).
 * Protocol: proto.h. */
#include "mirror.h"
#include "proto.h"
#include "../vault/vclient.h"
#include "../media/mstream.h"
#include "../par.h"
#include "../canvas.h"

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

#define TILE 64
#define MAX_CLIENTS 8
#define OUT_LIMIT (8u << 20)        /* with more pending than this, no new frame is built */
#define VIDEO_LIMIT (3u << 20)      /* a viewer this far behind skips video until the next keyframe */
#define STILL_MS 250                /* video mode: the lossless tiles go once the screen is this still */

typedef struct {
    int fd;
    unsigned char *in, *out;
    size_t in_len, in_cap, out_len, out_off, out_cap;
    int ready, need_size, need_shape, cur_x, cur_y, cur_mode;
    unsigned clip_gen;
    unsigned char *dirty;           /* one byte per tile */
    char who[96];                   /* "ip user" (RP_HELLO) */
    int seat;                       /* lord-sessiond (RP_SEAT) */
    uid_t uid;                      /* of the other end (SO_PEERCRED) */
    int fds[40], nfds;              /* fds received with the last message (RP_DISPLAY) */
    /* the video path (docs/REMOTE.md, "Video (WebRTC)") */
    int vcodec, vaudio, vneed_key;  /* subscribed to the session's encoder (1 H.264, 2 VP8) */
    unsigned vkbps;
    int vfps;                       /* the frame rate this viewer's screen shows (its refresh; 30 if unknown) */
    int vinfo_sent;                 /* RP_VINFO went to it at least once */
    int vmode;                      /* the video plays: lossless tiles once still, damage right away */
} Client;

static int lfd = -1;
static char sock_path[256];
static Client cl[MAX_CLIENTS];
static MirrorInput cb;
static uint32_t *fb;                /* last screen sent (copy) */
static int W, H, TW, TH;
static int cur_x, cur_y, cur_mode;
static uint32_t *shape;
static int shape_w, shape_h, shape_hx, shape_hy;
static unsigned shape_gen;
static char *clip_text;
static unsigned clip_len, clip_gen_seen;
static unsigned char *chg;          /* tiles changed in this frame */
static double last_change;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int still(void) { return now_ms() - last_change >= STILL_MS; }

static void send_damage(void);
static void video_update(void);

static void put(Client *c, const void *p, size_t n) {
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 65536;
        while (cap < c->out_len + n) cap *= 2;
        unsigned char *nb = realloc(c->out, cap);
        if (!nb) return;
        c->out = nb;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
}

static void msg(Client *c, int type, const void *a, size_t an, const void *b, size_t bn) {
    unsigned char h[5];
    uint32_t len = (uint32_t)(1 + an + bn);
    memcpy(h, &len, 4);
    h[4] = (unsigned char)type;
    put(c, h, 5);
    if (an) put(c, a, an);
    if (bn) put(c, b, bn);
}

static void flush(Client *c) {
    while (c->out_off < c->out_len) {
        ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        c->out_off += (size_t)w;
    }
    if (c->out_off == c->out_len) c->out_off = c->out_len = 0;
    else if (c->out_off > (1u << 20)) {               /* compact */
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
}

static void notify_clients(void) {
    if (!cb.clients) return;
    char json[1024] = "[";
    size_t o = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (cl[i].fd < 0 || !cl[i].who[0] || cl[i].seat) continue;
        char ip[64] = "", user[33] = "";
        sscanf(cl[i].who, "%63s %32s", ip, user);
        for (char *p = ip; *p; p++) if (*p == '"' || *p == '\\') *p = '?';
        for (char *p = user; *p; p++) if (*p == '"' || *p == '\\') *p = '?';
        o += (size_t)snprintf(json + o, sizeof json - o, "%s{\"ip\":\"%s\",\"user\":\"%s\"}", o > 1 ? "," : "", ip, user);
        if (o >= sizeof json - 80) break;
    }
    snprintf(json + o, sizeof json - o, "]");
    cb.clients(json);
}

/* Who wants which encoder: viewers per codec, sound, and the slowest viewer's rate. */
static void video_update(void) {
    for (int k = 1; k < MS_CODECS; k++) {
        int n = 0, audio = 0, fps = 0;
        unsigned kbps = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            const Client *c = &cl[i];
            if (c->fd < 0 || c->vcodec != k) continue;
            n++;
            audio |= c->vaudio;
            if (!kbps || c->vkbps < kbps) kbps = c->vkbps;
            if (c->vfps > fps) fps = c->vfps;     /* the fastest screen: a slower one shows what it can */
        }
        mstream_want(k, n, audio, kbps ? kbps : 4000, fps);
    }
}

static void close_fds(Client *c) {
    for (int i = 0; i < c->nfds; i++) if (c->fds[i] >= 0) close(c->fds[i]);
    c->nfds = 0;
}

static void drop(Client *c) {
    int had = c->who[0] != 0;
    close_fds(c);
    close(c->fd);
    free(c->in);
    free(c->out);
    free(c->dirty);
    int video = c->vcodec;
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (had) notify_clients();
    if (video) video_update();
}

static void all_dirty(Client *c) {
    free(c->dirty);
    c->dirty = TW && TH ? malloc((size_t)TW * TH) : NULL;
    if (c->dirty) memset(c->dirty, 1, (size_t)TW * TH);
}

/* If the connection asked for a frame and there are pending tiles: sends them (runs per row) and RP_END. */
static void pump(Client *c) {
    if (c->fd < 0) return;
    if (c->seat) { flush(c); return; }         /* lord-sessiond: only the requests (send_json) */
    if (c->need_size && W) {
        uint16_t s[2] = { (uint16_t)W, (uint16_t)H };
        msg(c, RP_SIZE, s, 4, NULL, 0);
        c->need_size = 0;
    }
    if (c->need_shape && shape) {
        uint16_t s[4] = { (uint16_t)shape_w, (uint16_t)shape_h, (uint16_t)shape_hx, (uint16_t)shape_hy };
        msg(c, RP_SHAPE, s, 8, shape, (size_t)shape_w * shape_h * 4);
        c->need_shape = 0;
    }
    if (c->cur_x != cur_x || c->cur_y != cur_y || c->cur_mode != cur_mode) {
        unsigned char m[5];
        int16_t x = (int16_t)cur_x, y = (int16_t)cur_y;
        memcpy(m, &x, 2);
        memcpy(m + 2, &y, 2);
        m[4] = (unsigned char)cur_mode;
        msg(c, RP_CURSOR, m, 5, NULL, 0);
        c->cur_x = cur_x;
        c->cur_y = cur_y;
        c->cur_mode = cur_mode;
    }
    if (clip_text && c->clip_gen != clip_gen_seen) {
        msg(c, RP_CLIP, clip_text, clip_len, NULL, 0);
        c->clip_gen = clip_gen_seen;
    }
    /* the lossless refresh: only for a viewer whose video plays, only once the screen is still */
    if (c->vmode && c->ready && c->dirty && fb && c->out_len - c->out_off < OUT_LIMIT && still()) {
        int any = 0;
        for (int ty = 0; ty < TH; ty++) {
            for (int tx = 0; tx < TW;) {
                if (!c->dirty[ty * TW + tx]) { tx++; continue; }
                int t0 = tx;
                while (tx < TW && c->dirty[ty * TW + tx]) c->dirty[ty * TW + tx++] = 0;
                int x = t0 * TILE, y = ty * TILE;
                int w = tx * TILE > W ? W - x : tx * TILE - x;
                int h = y + TILE > H ? H - y : TILE;
                uint16_t r[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
                unsigned char hdr[5];
                uint32_t len = (uint32_t)(1 + 8 + (size_t)w * h * 4);
                memcpy(hdr, &len, 4);
                hdr[4] = RP_RECT;
                put(c, hdr, 5);
                put(c, r, 8);
                for (int j = 0; j < h; j++) put(c, fb + (size_t)(y + j) * W + x, (size_t)w * 4);
                any = 1;
            }
        }
        if (any) {
            msg(c, RP_END, NULL, 0, NULL, 0);
            c->ready = 0;
        }
    }
    flush(c);
}

static void handle(Client *c, int type, const unsigned char *d, uint32_t n) {
    switch (type) {
    case RP_READY: c->ready = 1; break;
    case RP_PTR:
        if (n >= 4 && cb.pointer) { int16_t x, y; memcpy(&x, d, 2); memcpy(&y, d + 2, 2); cb.pointer(x, y); }
        break;
    case RP_BTN: if (n >= 2 && cb.button && d[0] <= 2) cb.button(d[0], d[1] != 0); break;
    case RP_WHEEL: if (n >= 1 && cb.wheel) cb.wheel((signed char)d[0]); break;
    case RP_SCROLL:
        if (n >= 9 && cb.scroll && d[0] >= 1 && d[0] <= 3) {
            int32_t v[2];
            memcpy(v, d + 1, 8);
            cb.scroll(d[0], v[0] / 256.0, v[1] / 256.0);
        }
        break;
    case RP_PINCH:
        if (n >= 9 && cb.pinch && d[0] >= 1 && d[0] <= 3) {
            int32_t v[2];
            memcpy(v, d + 1, 8);
            double sc = v[0] / 65536.0;
            if (sc > 0.05 && sc < 20) cb.pinch(d[0], sc, v[1] / 256.0);
        }
        break;
    case RP_KEY:
        if (n >= 3 && cb.key) {
            uint16_t code;
            memcpy(&code, d, 2);
            if (code > 0 && code < 0x2ff) cb.key(code, d[2] > 2 ? 1 : d[2]);
        }
        break;
    case RP_SETCLIP: if (cb.clipboard && n <= (1u << 20)) cb.clipboard((const char *)d, n); break;
    case RP_HELLO:
        snprintf(c->who, sizeof c->who, "%.*s", (int)(n < sizeof c->who - 1 ? n : sizeof c->who - 1), (const char *)d);
        for (char *p = c->who; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
        notify_clients();
        break;
    case RP_SEAT:
        if (c->uid == 0) { c->seat = 1; c->ready = 0; notify_clients(); }
        break;
    case RP_DISPLAY:
        if (c->seat && c->nfds > 0 && cb.display) {
            cb.display(c->fds, c->nfds, n >= 2 ? d[1] : 0);  /* the fds now belong to the session */
            c->nfds = 0;
        }
        break;
    case RP_INPUT_ADD:
        if (c->seat && c->nfds > 0 && cb.input_add) {
            cb.input_add(c->fds, c->nfds);          /* the fds now belong to the session */
            c->nfds = 0;
        }
        break;
    case RP_UNDISPLAY:
        if (c->seat && cb.undisplay) cb.undisplay();
        break;
    case RP_VIDEO:
        /* u8 codec (0 none, 1 H.264, 2 VP8), u8 sound, u16 0, u32 kbit/s: this viewer's video */
        if (n >= 8 && d[0] <= 2) {
            uint32_t k;
            memcpy(&k, d + 4, 4);
            int was = c->vcodec;
            c->vcodec = d[0];
            c->vaudio = d[1] != 0;
            c->vkbps = k < 300 ? 300 : k > 20000 ? 20000 : k;
            uint16_t hz;
            memcpy(&hz, d + 2, 2);
            c->vfps = hz >= 24 && hz <= 360 ? hz : 30;
            if (c->vcodec && c->vcodec != was) c->vneed_key = 1;
            video_update();
            if (c->vcodec && fb) mstream_frame(fb, W, W, H, NULL);
            else if (c->vcodec && cb.refresh) cb.refresh();     /* the first viewer: a whole frame */
        }
        break;
    case RP_KEYFRAME: if (c->vcodec) { mstream_keyframe(c->vcodec); c->vneed_key = 1; } break;
    case RP_RATE:
        if (n >= 4 && c->vcodec) {
            uint32_t k;
            memcpy(&k, d, 4);
            c->vkbps = k < 300 ? 300 : k > 20000 ? 20000 : k;
            video_update();
        }
        break;
    case RP_VMODE:
        if (n >= 1) {
            c->vmode = d[0] == 1;
            all_dirty(c);                       /* the lossless layer starts whole */
            c->ready = 1;
        }
        break;
    case RP_RESIZE:
        if (n >= 4 && cb.resize) {
            uint16_t s[2];
            memcpy(s, d, 4);
            cb.resize(s[0], s[1]);
        }
        break;
    case RP_KEYRING:
        /* lord-sessiond: the password just verified opens the session's keyring (lord-vault) */
        if (c->uid == 0 && n > 0 && n <= 1024) {
            char p[1025];
            memcpy(p, d, n);
            p[n] = 0;
            vc_session_password(p);
            explicit_bzero(p, sizeof p);
        }
        explicit_bzero((void *)d, n);
        break;
    case RP_NOTICE:
        /* only lord-sessiond (root, seat); short and printable ASCII, the JS side validates fields */
        if (cb.notice && c->uid == 0 && c->seat && n > 0 && n < 512) {
            char j[512];
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) if (d[i] < 0x20 || d[i] > 0x7e) ok = 0;
            if (ok) { memcpy(j, d, n); j[n] = 0; cb.notice(j); }
        }
        break;
    case RP_GREETER_IN:
        if (cb.greeter && c->seat && n < 65536) {
            char *j = malloc(n + 1);
            if (j) { memcpy(j, d, n); j[n] = 0; cb.greeter(j); free(j); }
        }
        break;
    default: break;
    }
}

static void read_client(Client *c) {
    for (;;) {
        if (c->in_cap - c->in_len < 65536) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 131072;
            if (cap > (2u << 20)) cap = 2u << 20;
            if (cap <= c->in_cap) { drop(c); return; }
            unsigned char *nb = realloc(c->in, cap);
            if (!nb) { drop(c); return; }
            c->in = nb;
            c->in_cap = cap;
        }
        struct iovec iov = { c->in + c->in_len, c->in_cap - c->in_len };
        char cbuf[CMSG_SPACE(sizeof(int) * 40)];
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t r = recvmsg(c->fd, &mh, MSG_CMSG_CLOEXEC);
        for (struct cmsghdr *cm = r > 0 ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            int k = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k; i++) {
                int f;
                memcpy(&f, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
                /* only root (lord-sessiond) passes fds; from others they're discarded */
                if (c->uid == 0 && c->nfds < 40) c->fds[c->nfds++] = f; else close(f);
            }
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) break;
        if (r <= 0) { drop(c); return; }
        c->in_len += (size_t)r;
        size_t off = 0;
        while (c->in_len - off >= 5) {
            uint32_t len;
            memcpy(&len, c->in + off, 4);
            if (len < 1 || len > (1u << 20) + 16) { drop(c); return; }
            if (c->in_len - off < 4 + (size_t)len) break;
            handle(c, c->in[off + 4], c->in + off + 5, len - 1);
            if (c->fd < 0) return;
            off += 4 + len;
        }
        close_fds(c);                       /* fds a RP_DISPLAY didn't use: out */
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
    pump(c);
}

static void video_out(int codec, int key, uint32_t ms, const uint8_t *p, size_t n) {
    unsigned char h[8] = { (unsigned char)codec, (unsigned char)(key != 0), 0, 0 };
    memcpy(h + 4, &ms, 4);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &cl[i];
        if (c->fd < 0 || c->vcodec != codec) continue;
        if (c->vneed_key && !key) continue;
        if (c->out_len - c->out_off > VIDEO_LIMIT) { c->vneed_key = 1; mstream_keyframe(codec); continue; }
        msg(c, RP_VFRAME, h, 8, p, n);
        if (key) c->vneed_key = 0;
        flush(c);
    }
}

static void audio_out(int codec, uint32_t samples, const uint8_t *p, size_t n) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &cl[i];
        if (c->fd < 0 || c->vcodec != codec || !c->vaudio || c->out_len - c->out_off > VIDEO_LIMIT) continue;
        msg(c, RP_AFRAME, &samples, 4, p, n);
        flush(c);
    }
}

int mirror_start(int w, int h, const MirrorInput *in) {
    if (in) cb = *in;
    static const MStreamOut so = { video_out, audio_out };
    mstream_init(&so);
    for (int i = 0; i < MAX_CLIENTS; i++) cl[i].fd = -1;
    W = w;
    H = h;
    TW = (W + TILE - 1) / TILE;
    TH = (H + TILE - 1) / TILE;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return -1;
    char dir[200];
    snprintf(dir, sizeof dir, "%s/lordos", rt);
    mkdir(dir, 0700);
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    snprintf(sock_path, sizeof sock_path, "%s/remote.sock", dir);
    unlink(sock_path);
    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof a.sun_path) { close(lfd); lfd = -1; return -1; }
    memcpy(a.sun_path, sock_path, strlen(sock_path) + 1);
    mode_t old = umask(077);
    int r = lfd >= 0 ? bind(lfd, (struct sockaddr *)&a, sizeof a) : -1;
    umask(old);
    if (r || listen(lfd, 4)) {
        if (lfd >= 0) close(lfd);
        lfd = -1;
        return -1;
    }
    return 0;
}

void mirror_stop(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) drop(&cl[i]);
    if (lfd >= 0) { close(lfd); unlink(sock_path); }
    lfd = -1;
    free(fb);
    fb = NULL;
    free(shape);
    shape = NULL;
    free(clip_text);
    clip_text = NULL;
    free(chg);
    chg = NULL;
}

int mirror_clients(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && !cl[i].seat) n++;
    return n;
}

/* The frame rate the remote viewers' screens show (the fastest), 0 without video viewers. */
int mirror_fps(void) {
    int f = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && !cl[i].seat && cl[i].vcodec && cl[i].vfps > f) f = cl[i].vfps;
    return f;
}

int mirror_watchers(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && !cl[i].seat && cl[i].vcodec) n++;
    return n;
}

int mirror_has_seat(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && cl[i].seat) return 1;
    return 0;
}

static void send_json(int type, const char *json, int seat_only) {
    if (lfd < 0 || !json) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (cl[i].fd < 0 || (seat_only && !cl[i].seat)) continue;
        msg(&cl[i], type, json, strlen(json), NULL, 0);
        pump(&cl[i]);
    }
}

void mirror_session(const char *json) {
    /* locking the screen or switching user closes the Passwords vault (not the keyring) */
    if (strstr(json, "\"lock\"")) vc_lock_all("screen");
    else if (strstr(json, "\"switch\"")) vc_lock_all("switch");
    send_json(RP_SESSION, json, 0);
}
void mirror_greeter(const char *json) { send_json(RP_GREETER, json, 1); }
void mirror_shown(void) {
    if (lfd < 0) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (cl[i].fd < 0 || !cl[i].seat) continue;
        msg(&cl[i], RP_SHOWN, NULL, 0, NULL, 0);
        pump(&cl[i]);
    }
}

int mirror_pollfds(struct pollfd *p, int max) {
    int n = 0;
    if (lfd < 0 || max < 1) return 0;
    p[n++] = (struct pollfd){ lfd, POLLIN, 0 };
    for (int i = 0; i < MAX_CLIENTS && n < max; i++)
        if (cl[i].fd >= 0) p[n++] = (struct pollfd){ cl[i].fd, (short)(POLLIN | (cl[i].out_len > cl[i].out_off ? POLLOUT : 0)), 0 };
    return n;
}

static Client *by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd == fd) return &cl[i];
    return NULL;
}

void mirror_dispatch(const struct pollfd *p, int n) {
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (p[i].fd == lfd) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) continue;
            struct ucred cr;
            socklen_t cl_len = sizeof cr;
            Client *slot = by_fd(-1);
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl_len) || (cr.uid != 0 && cr.uid != getuid()) || !slot) { close(fd); continue; }
            memset(slot, 0, sizeof *slot);
            slot->fd = fd;
            slot->uid = cr.uid;
            slot->ready = 1;
            slot->need_size = 1;
            slot->need_shape = shape != NULL;
            slot->cur_x = slot->cur_y = -1;
            slot->cur_mode = -1;
            all_dirty(slot);
            pump(slot);
            if (cb.refresh) cb.refresh();           /* so the next frame arrives whole */
            continue;
        }
        Client *c = by_fd(p[i].fd);
        if (!c) continue;
        if (p[i].revents & (POLLERR | POLLHUP)) { drop(c); continue; }
        if (p[i].revents & POLLOUT) { flush(c); pump(c); }
        if (c->fd >= 0 && (p[i].revents & POLLIN)) read_client(c);
    }
}

#define FULL_CHECK_MS 500           /* with damage hints, every tile is still compared this often */

/* One frame's comparison, split by rows of tiles over the pool (par.h): every tile has its own bytes in
 * fb, chg and each client's dirty map, so the rows don't share anything but "something changed". */
typedef struct { const uint32_t *px; int stride, tx0, tx1; int any; } Cmp;
static void cmp_rows(void *ctx, int ty0, int ty1) {
    Cmp *k = ctx;
    int any = 0;
    for (int ty = ty0; ty < ty1; ty++)
        for (int tx = k->tx0; tx < k->tx1; tx++) {
            int x = tx * TILE, y = ty * TILE;
            int tw = x + TILE > W ? W - x : TILE, th = y + TILE > H ? H - y : TILE;
            int changed = 0;
            for (int j = 0; j < th && !changed; j++)
                changed = memcmp(fb + (size_t)(y + j) * W + x, k->px + (size_t)(y + j) * k->stride + x, (size_t)tw * 4) != 0;
            if (!changed) continue;
            for (int j = 0; j < th; j++)
                memcpy(fb + (size_t)(y + j) * W + x, k->px + (size_t)(y + j) * k->stride + x, (size_t)tw * 4);
            for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && cl[i].dirty) cl[i].dirty[ty * TW + tx] = 1;
            if (chg) chg[ty * TW + tx] = 1;
            any = 1;
        }
    if (any) __atomic_store_n(&k->any, 1, __ATOMIC_RELAXED);
}

void mirror_frame(const uint32_t *px, int stride, int w, int h, const int *dmg) {
    static double full_at;
    if (lfd < 0 || !mirror_watchers()) { free(fb); fb = NULL; return; }
    int resized = !fb || w != W || h != H, any = 0;
    int new_size = w != W || h != H;
    if (resized) {
        W = w;
        H = h;
        TW = (W + TILE - 1) / TILE;
        TH = (H + TILE - 1) / TILE;
        free(fb);
        fb = malloc((size_t)W * H * 4);
        if (!fb) return;
        for (int j = 0; j < H; j++) memcpy(fb + (size_t)j * W, px + (size_t)j * stride, (size_t)W * 4);
        for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) { if (new_size) cl[i].need_size = 1; all_dirty(&cl[i]); }
        free(chg);
        chg = calloc((size_t)TW * TH, 1);
        if (chg) memset(chg, 1, (size_t)TW * TH);
        any = 1;
    } else {
        if (!chg) chg = calloc((size_t)TW * TH, 1);
        if (chg) memset(chg, 0, (size_t)TW * TH);
        /* only the tiles the compositor drew over (the whole screen now and then, in case) */
        int tx0 = 0, ty0 = 0, tx1 = TW, ty1 = TH;
        double t = now_ms();
        if (dmg && t - full_at < FULL_CHECK_MS) {
            tx0 = dmg[0] < 0 ? 0 : dmg[0] / TILE;
            ty0 = dmg[1] < 0 ? 0 : dmg[1] / TILE;
            tx1 = dmg[2] <= 0 ? 0 : (dmg[2] + TILE - 1) / TILE;
            ty1 = dmg[3] <= 0 ? 0 : (dmg[3] + TILE - 1) / TILE;
            if (tx1 > TW) tx1 = TW;
            if (ty1 > TH) ty1 = TH;
        } else full_at = t;
        Cmp k = { px, stride, tx0, tx1, 0 };
        if ((long)(ty1 - ty0) * (tx1 - tx0) >= 64 && canvas_parallel_on()) par_rows(ty0, ty1, cmp_rows, &k);
        else cmp_rows(&k, ty0, ty1);
        any = k.any;
    }
    if (any) {
        last_change = now_ms();
        send_damage();
        int video = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && cl[i].vcodec) video = 1;
        if (video) {
            /* the changed tiles' box: the encoder's slots copy only that */
            int box[4] = { W, H, 0, 0 };
            for (int ty = 0; chg && ty < TH; ty++)
                for (int tx = 0; tx < TW; tx++) {
                    if (!chg[ty * TW + tx]) continue;
                    int x = tx * TILE, y = ty * TILE, x1 = x + TILE > W ? W : x + TILE, y1 = y + TILE > H ? H : y + TILE;
                    if (x < box[0]) box[0] = x;
                    if (y < box[1]) box[1] = y;
                    if (x1 > box[2]) box[2] = x1;
                    if (y1 > box[3]) box[3] = y1;
                }
            mstream_frame(fb, W, W, H, chg && box[2] > box[0] ? box : NULL);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) pump(&cl[i]);
}

/* Video mode: what changed in this frame, as rows of tiles, so the browser drops its lossless layer
 * there and shows the video (RP_DAMAGE: u16 x, y, w, h each). */
static void send_damage(void) {
    if (!chg) return;
    uint16_t r[4 * 256];
    int n = 0;
    for (int ty = 0; ty < TH && n < 256; ty++)
        for (int tx = 0; tx < TW && n < 256;) {
            if (!chg[ty * TW + tx]) { tx++; continue; }
            int t0 = tx;
            while (tx < TW && chg[ty * TW + tx]) tx++;
            int x = t0 * TILE, y = ty * TILE;
            r[n * 4] = (uint16_t)x;
            r[n * 4 + 1] = (uint16_t)y;
            r[n * 4 + 2] = (uint16_t)((tx * TILE > W ? W : tx * TILE) - x);
            r[n * 4 + 3] = (uint16_t)(y + TILE > H ? H - y : TILE);
            n++;
        }
    if (!n) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &cl[i];
        if (c->fd < 0 || !c->vmode) continue;
        msg(c, RP_DAMAGE, r, (size_t)n * 8, NULL, 0);
        flush(c);
    }
}

/* What the viewers' top bar shows about the video (RP_VINFO): sent when it changes, checked once a second. */
static int on_gpu;
void mirror_composed_on_gpu(int on) { on_gpu = on != 0; }
static void vinfo_update(void) {
    static double at;
    static unsigned char last[8];
    double t = now_ms();
    if (t - at < 1000) return;
    at = t;
    for (int k = 1; k <= 2; k++) {
        int hw = 0, fps = 0;
        long enc = 0;
        if (!mstream_info(k, &hw, &fps, &enc)) continue;
        uint16_t f = (uint16_t)fps, e = (uint16_t)(enc < 0 ? 0 : enc > 65535 ? 65535 : enc);
        unsigned char m[8] = { (unsigned char)hw, (unsigned char)on_gpu, 0, 0, 0, 0, (unsigned char)k, 0 };
        memcpy(m + 2, &f, 2);
        memcpy(m + 4, &e, 2);
        int fresh = memcmp(m, last, 8) != 0;
        memcpy(last, m, 8);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &cl[i];
            if (c->fd < 0 || c->vcodec != k || (!fresh && c->vinfo_sent)) continue;
            msg(c, RP_VINFO, m, 6, NULL, 0);
            c->vinfo_sent = 1;
            flush(c);
        }
    }
}

/* Video mode clients get their lossless tiles once the screen is still; the encoder may have a
 * frame waiting for a free slot. */
void mirror_tick(void) {
    if (lfd < 0) return;
    vinfo_update();
    if (still()) for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0 && cl[i].vmode && cl[i].ready) pump(&cl[i]);
    mstream_tick();
}

double mirror_timeout(void) {
    if (lfd < 0) return -1;
    double best = mstream_timeout();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client *c = &cl[i];
        if (c->fd < 0 || !c->vmode || !c->ready || !c->dirty) continue;
        int pend = 0;
        for (int k = 0; k < TW * TH && !pend; k++) pend = c->dirty[k];
        if (!pend) continue;
        double d = STILL_MS - (now_ms() - last_change);
        if (d < 1) d = 1;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void mirror_cursor(int x, int y, int mode, const uint32_t *argb, int w, int h, int hx, int hy, unsigned gen) {
    if (lfd < 0 || !mirror_watchers()) return;
    cur_x = x;
    cur_y = y;
    cur_mode = mode;
    if (mode == 1 && argb && gen != shape_gen && w > 0 && h > 0 && w <= 256 && h <= 256) {
        uint32_t *s = malloc((size_t)w * h * 4);
        if (s) {
            memcpy(s, argb, (size_t)w * h * 4);
            free(shape);
            shape = s;
            shape_w = w; shape_h = h; shape_hx = hx; shape_hy = hy;
            shape_gen = gen;
            for (int i = 0; i < MAX_CLIENTS; i++) if (cl[i].fd >= 0) cl[i].need_shape = 1;
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) pump(&cl[i]);
}

void mirror_clipboard(const char *text, unsigned gen) {
    if (lfd < 0 || gen == clip_gen_seen || !text) return;
    size_t n = strlen(text);
    if (n > (1u << 20)) n = 1u << 20;
    char *t = malloc(n + 1);
    if (!t) return;
    memcpy(t, text, n);
    t[n] = 0;
    free(clip_text);
    clip_text = t;
    clip_len = (unsigned)n;
    clip_gen_seen = gen;
    for (int i = 0; i < MAX_CLIENTS; i++) pump(&cl[i]);
}
