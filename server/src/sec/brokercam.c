/* brokercam.c — the camera for isolated .lpk apps, server side (see broker.h BOP_CAMERA and
 * docs/PRIVACY.md, "Camera").
 *
 * After consent, lord-server opens /dev/videoN and hands it ONLY to lord-media (MK_CAMERA), which
 * decodes into its frame ring (MM_RING: a sealed memfd with MP_RING_SLOTS BGRA slots). The app
 * never sees the device: it gets a READ-ONLY reopen of the ring (/proc/self/fd/N, O_RDONLY, so it
 * can only map it PROT_READ) and, per frame, an event with the frame id. The app acknowledges what
 * it shows; every ACK is validated here (current generation, a slot we really forwarded and that
 * is still pending, rate limited) before it reaches lord-media, so a misbehaving app can only
 * starve itself. Photos and videos are written to a file the user picks in the system's save
 * dialog (powerbox): lord-server opens it and gives the fd to lord-media, never to the app.
 * Revoking the permission kills the helper and closes the stream at once. */
#include "broker_int.h"
#include "perms.h"
#include "restrict.h"
#include "../fw/rules.h"
#include "../errcode.h"
#include "json.h"
#include "../media/media_int.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CAMS 16
#define ACK_RATE 120.0          /* ACKs per second an app may send (bucket refill) */
#define ACK_BURST 30.0

typedef struct {
    int used, conn, sid;        /* broker connection, media source in lord-server */
    int gen, w, h, slots, ringfd;
    uint64_t sent[MP_RING_SLOTS], seq;      /* frames forwarded to the app and not acked yet */
    double tokens, tok_at;
    long bad;                   /* rejected ACKs (log only) */
    int live;                   /* a live stream (MK_LIVE, media/live.h) rather than a camera */
    uint32_t pick_req;          /* powerbox pending for a photo/video */
    int pick_kind, pick_mirror, pick_mic;   /* 1 photo, 2 video */
    char out_name[256];         /* names of the saved files (never the folder) */
    char photo_name[256], video_name[256];
} Cam;
static Cam cams[MAX_CAMS];
static uint32_t next_pick = 1;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static Cam *cam_of_sid(int conn, int sid) {
    for (int i = 0; i < MAX_CAMS; i++) if (cams[i].used && cams[i].conn == conn && cams[i].sid == sid) return &cams[i];
    return NULL;
}

static void cam_event(Cam *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cam_event(Cam *c, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof buf) brokersrv_conn_event(c->conn, "cam", buf, (size_t)n);
}

static void cam_free(Cam *c, int kill_helper) {
    MediaSrc *s = media_src_get(c->sid);
    const char *app = brokersrv_conn_app(c->conn);
    int mic_on = s && s->mic;
    if (s) {
        if (kill_helper && s->pid > 0) kill(s->pid, SIGKILL);     /* revoked: the device is released now */
        s->bk = NULL;
        s->bk_msg = NULL;
        s->closing = 0;
        s->state = MS_DONE;
        media_src_close(s);
    }
    if (c->ringfd >= 0) close(c->ringfd);
    if (app && !c->live) brokersrv_device(app, "camera", 0);
    if (app && c->live && mic_on) brokersrv_device(app, "microphone", 0);
    memset(c, 0, sizeof *c);
    c->ringfd = -1;
}

static int take_token(Cam *c) {
    double t = now_s();
    c->tokens += (t - c->tok_at) * ACK_RATE;
    c->tok_at = t;
    if (c->tokens > ACK_BURST) c->tokens = ACK_BURST;
    if (c->tokens < 1) return 0;
    c->tokens -= 1;
    return 1;
}

static void self_ack(Cam *c, MediaSrc *s, uint32_t id) {
    int slot = (int)(id & 0xff);
    if (s->sock >= 0) mp_sendv(s->sock, MM_ACK, id, 0, 0, 0, 0);
    uint64_t q = slot < MP_RING_SLOTS ? c->sent[slot] : 0;
    for (int i = 0; i < MP_RING_SLOTS; i++) if (c->sent[i] && c->sent[i] <= q) c->sent[i] = 0;
}

/* Messages of this camera's lord-media: they go to the app, not to the session's JS. */
static int on_helper_msg(void *src, const MediaMsg *m, const char *payload, int *fds) {
    MediaSrc *s = src;
    Cam *c = s->bk;
    if (!c) return 0;
    char esc[512];
    switch (m->type) {
    case MM_RING: {
        int64_t w = m->v[1], h = m->v[2], slots = m->v[3];
        struct stat st;
        int seals = fds[0] >= 0 ? fcntl(fds[0], F_GET_SEALS) : -1;
        if (fds[0] < 0 || w < 2 || h < 2 || w > MP_MAX_DIM || h > MP_MAX_DIM || slots < 1 || slots > MP_RING_SLOTS ||
            fstat(fds[0], &st) || (uint64_t)st.st_size < (uint64_t)w * (uint64_t)h * 4 * (uint64_t)slots || seals < 0 || !(seals & F_SEAL_SHRINK)) {
            fprintf(stderr, "broker[%s]: camera ring rejected (unsealed or too small)\n", brokersrv_conn_app(c->conn));
            return 1;
        }
        if (c->ringfd >= 0) close(c->ringfd);
        c->ringfd = fds[0];
        fds[0] = -1;                                 /* ours now */
        c->gen = (int)m->v[0];
        c->w = (int)w;
        c->h = (int)h;
        c->slots = (int)slots;
        memset(c->sent, 0, sizeof c->sent);
        cam_event(c, "ring %d", c->sid);
        return 1;
    }
    case MM_FRAME: {
        int gen = (int)(m->id >> 8), slot = (int)(m->id & 0xff);
        if (c->ringfd < 0 || gen != c->gen || slot >= c->slots) return 1;
        c->sent[slot] = ++c->seq;
        char ev[64];
        int n = snprintf(ev, sizeof ev, "frame %d %u", c->sid, (unsigned)m->id);
        /* the app's socket is full: free the slots ourselves (only the app loses frames) */
        if (brokersrv_conn_event(c->conn, "cam", ev, (size_t)n)) self_ack(c, s, m->id);
        return 1;
    }
    case MM_INFO:
        /* lord-media's JSON: parsed again here, and only well-formed JSON goes on to the app */
        if (payload && m->len && m->len < 1024 && payload[0] == '{') {
            JDoc d;
            if (!json_parse(payload, strlen(payload), &d, NULL, 0)) { json_free(&d); cam_event(c, "event %d info %s", c->sid, payload); }
        }
        return 1;
    case MM_AUDIO:
        return 0;                                    /* media.c serves the stream */
    case MM_STATS:
        if (c->live) cam_event(c, "event %d stats {\"decoded\":%lld,\"shown\":%lld,\"dropped\":%lld,\"decodeMs\":%.2f}", c->sid, (long long)m->v[0],
                               (long long)m->v[1], (long long)m->v[2], m->v[3] / 100.0);
        return 1;
    case MM_STATE: {
        static const char *const names[] = { "loading", "playing", "paused", "ended", "recording", "done", "failed" };
        int st = (int)m->v[0];
        s->state = st;
        if (s->audio) maudio_playing(s->audio, st == MS_PLAYING);
        if (st >= 0 && st <= MS_FAILED) cam_event(c, "event %d state {\"state\":\"%s\"}", c->sid, names[st]);
        return 1;
    }
    case MM_LEVEL:
        cam_event(c, "event %d level {\"level\":%lld,\"ms\":%lld}", c->sid, (long long)m->v[0], (long long)m->v[1]);
        return 1;
    case MM_RESULT: {
        int ok = m->v[0] == 0;
        const char *name = m->id == 1 ? c->photo_name : c->video_name;
        json_quote(esc, sizeof esc, ok ? name : "");
        cam_event(c, "event %d %s {\"ok\":%s,\"path\":%s}", c->sid, m->id == 1 ? "photo" : "video", ok ? "true" : "false", esc);
        if (m->id == 2 && s->mic) { maudio_close(s->mic); s->mic = NULL; }
        return 1;
    }
    case MM_ERROR:
        cam_event(c, "event %d error {\"message\":\"camera.failed\"}", c->sid);
        return 1;
    default:
        return 1;                                    /* nothing else of a camera reaches the session */
    }
}

static int dev_ok(const char *d) {
    if (!d || strncmp(d, "/dev/video", 10)) return 0;
    size_t n = strlen(d + 10);
    if (n < 1 || n > 2) return 0;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)d[10 + i])) return 0;
    return 1;
}

/* a file name suggested by the app for the save dialog: a plain name, nothing else */
static void clean_name(const char *in, const char *def, char *out, size_t n) {
    const char *b = in ? strrchr(in, '/') : NULL;
    b = b ? b + 1 : in;
    size_t k = 0;
    for (; b && *b && k + 1 < n && k < 120; b++) if ((unsigned char)*b >= 0x20 && *b != '\\') out[k++] = *b;
    out[k] = 0;
    if (!k || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, n, "%s", def);
}

/* A socketpair for a live link; [0] goes to the companion, [1] to lord-media. Both ends are nonblocking: neither waits for the other. */
static int link_pair(int sp[2]) { return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sp); }

/* "live tag": the app shows, in a <MediaView>, what its companion receives. lord-media decodes it. The companion gets its
 * end on fd 6 and the app names nothing but a number of its own, so neither can reach another app's stream. No
 * permission: it plays what the app brought. */
static void live_open(int conn, int fd, uint32_t reqid, const char *app, const char *payload) {
    int tag = 0;
    if (sscanf(payload, "%*s %d", &tag) != 1 || tag < 1 || tag > 99999) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
    Cam *c = NULL;
    int mine = 0;
    for (int i = 0; i < MAX_CAMS; i++) { if (cams[i].used && cams[i].conn == conn) mine++; if (!cams[i].used && !c) c = &cams[i]; }
    int sp[2], ring = -1;
    if (!c || mine >= 4 || !brokersrv_conn_has_link(conn) || link_pair(sp)) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
    MediaSrc *s = media_src_new(SK_CAMERA, app);
    if (s) s->audio = maudio_open(0, app, "Remote computer", "video", &ring);
    int fds[2] = { sp[1], ring };
    if (s) s->sock = media_spawn(MK_LIVE, "", fds, ring >= 0 ? 2 : 1, &s->pid);
    close(sp[1]);
    char line[48];
    snprintf(line, sizeof line, "live %d", tag);
    if (!s || s->sock < 0 || brokersrv_conn_link(conn, line, sp[0])) {
        close(sp[0]);
        if (s) { if (s->pid > 0) kill(s->pid, SIGKILL); media_src_close(s); }
        brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1);
        return;
    }
    close(sp[0]);
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->live = 1;
    c->conn = conn;
    c->sid = s->id;
    c->ringfd = -1;
    c->tokens = ACK_BURST;
    c->tok_at = now_s();
    s->bk = c;
    s->bk_msg = on_helper_msg;
    char r[16];
    int n = snprintf(r, sizeof r, "%d", c->sid);
    fprintf(stderr, "broker[%s]: live stream %d (lord-media %d)\n", app, tag, (int)s->pid);
    brokersrv_reply(fd, reqid, BST_OK, r, (uint32_t)n, -1);
}

/* "mic sid 0|1": the microphone of a live stream, with the permission checked here and not in the app. */
static void live_mic(Cam *c, MediaSrc *s, int fd, uint32_t reqid, const char *app, int on) {
    if (!c->live || !s || s->sock < 0) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
    if (!on) {
        if (s->mic) { mp_send(s->sock, MM_RECORD, 0, NULL, NULL, 0, NULL, 0); maudio_close(s->mic); s->mic = NULL; brokersrv_device(app, "microphone", 0); }
        brokersrv_reply(fd, reqid, BST_OK, NULL, 0, -1);
        return;
    }
    if (restrict_perm_pkg(app, PERM_MICROPHONE) != PERM_ALLOW || restrict_blocks_pkg(app, FW_R_NO_MIC)) { brokersrv_reply(fd, reqid, BST_DENIED, NULL, 0, -1); return; }
    if (!s->mic) {
        int ring = -1;
        s->mic = maudio_open(1, app, "Call", "", &ring);
        if (!s->mic || mp_send(s->sock, MM_RECORD, 0, NULL, NULL, 0, &ring, 1)) { maudio_close(s->mic); s->mic = NULL; brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        brokersrv_device(app, "microphone", 1);
    }
    brokersrv_reply(fd, reqid, BST_OK, NULL, 0, -1);
}

void brokercam_handle(int conn, int fd, uint32_t reqid, const char *payload, uint32_t len) {
    (void)len;
    const char *app = brokersrv_conn_app(conn);
    char cmd[16] = "";
    sscanf(payload, "%15s", cmd);
    int allowed = app && restrict_perm_pkg(app, PERM_CAMERA) == PERM_ALLOW;     /* a restriction beats the permission */
    if (!strcmp(cmd, "list")) {
        static char json[8192];
        if (!allowed) snprintf(json, sizeof json, "[]");
        else media_cameras(json, sizeof json);
        brokersrv_reply(fd, reqid, BST_OK, json, (uint32_t)strlen(json), -1);
        return;
    }
    if (!strcmp(cmd, "live")) {
        if (!app) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        live_open(conn, fd, reqid, app, payload);
        return;
    }
    if (!strcmp(cmd, "open")) {
        char dev[32] = "", codec[8] = "";
        int w = 1280, h = 720, tag = 0, kbps = 0;
        sscanf(payload, "%*s %31s %d %d %d %7s %d", dev, &w, &h, &tag, codec, &kbps);
        if (app && restrict_blocks_pkg(app, FW_R_NO_CAMERA)) {
            char line[160];
            lerr_set(line, sizeof line, restrict_code(FW_R_NO_CAMERA), "app", app, NULL);
            fprintf(stderr, "broker[%s]: camera refused by a restriction\n", app);
            brokersrv_reply(fd, reqid, BST_DENIED, line, (uint32_t)strlen(line), -1);
            return;
        }
        if (!allowed) { brokersrv_reply(fd, reqid, BST_DENIED, NULL, 0, -1); return; }
        if (!dev_ok(dev)) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        if (w < 160 || w > 3840) w = 1280;
        if (h < 120 || h > 2160) h = 720;
        Cam *c = NULL;
        int mine = 0;
        for (int i = 0; i < MAX_CAMS; i++) { if (cams[i].used && cams[i].conn == conn) mine++; if (!cams[i].used && !c) c = &cams[i]; }
        if (!c || mine >= 4) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        int dfd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (dfd < 0) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        MediaSrc *s = media_src_new(SK_CAMERA, app);
        if (!s) { close(dfd); brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        int render = media_render_fd();
        char params[96];
        snprintf(params, sizeof params, "width=%d\nheight=%d\nhw=%d\nhwaccel=%s\n", w, h, render >= 0, media_hwaccel());
        int fds[2] = { dfd, render };
        /* the device fd goes to lord-media only */
        s->sock = media_spawn(MK_CAMERA, params, fds, 2, &s->pid);
        close(dfd);
        if (render >= 0) close(render);
        if (s->sock < 0) { media_src_close(s); brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->conn = conn;
        c->sid = s->id;
        c->ringfd = -1;
        c->tokens = ACK_BURST;
        c->tok_at = now_s();
        s->bk = c;
        s->bk_msg = on_helper_msg;
        /* a call: the same camera, encoded, to the companion's end of a link */
        int sp[2];
        char line[48];
        snprintf(line, sizeof line, "camera %d", tag);
        if (tag > 0 && tag <= 99999 && !link_pair(sp)) {
            int64_t v[6] = { !strcmp(codec, "vp8"), kbps, 0, 0, 0, 0 };
            if (mp_send(s->sock, MM_LINK, 0, v, NULL, 0, &sp[1], 1) || brokersrv_conn_link(conn, line, sp[0]))
                fprintf(stderr, "broker[%s]: the camera has no way to its companion\n", app);
            close(sp[0]);
            close(sp[1]);
        }
        brokersrv_device(app, "camera", 1);
        char r[16];
        int n = snprintf(r, sizeof r, "%d", c->sid);
        fprintf(stderr, "broker[%s]: camera %s opened (lord-media %d)\n", app, dev, (int)s->pid);
        brokersrv_reply(fd, reqid, BST_OK, r, (uint32_t)n, -1);
        return;
    }
    int sid = 0;
    sscanf(payload, "%*s %d", &sid);
    Cam *c = cam_of_sid(conn, sid);
    MediaSrc *s = c ? media_src_get(c->sid) : NULL;
    if (!strcmp(cmd, "ack")) {                       /* no reply: the app never waits for it */
        unsigned id = 0;
        sscanf(payload, "%*s %*d %u", &id);
        int gen = (int)(id >> 8), slot = (int)(id & 0xff);
        if (!s || s->sock < 0) return;
        if (!take_token(c) || gen != c->gen || slot >= c->slots || !c->sent[slot]) {
            if (++c->bad == 1 || c->bad % 1000 == 0) fprintf(stderr, "broker[%s]: invalid camera ACK ignored (%ld)\n", app, c->bad);
            return;
        }
        self_ack(c, s, id);
        return;
    }
    if (!strcmp(cmd, "mic")) {
        int on = 0;
        sscanf(payload, "%*s %*d %d", &on);
        if (!c) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        live_mic(c, s, fd, reqid, app, on);
        return;
    }
    if (!strcmp(cmd, "size")) {                      /* no reply */
        int w = 0, h = 0;
        sscanf(payload, "%*s %*d %d %d", &w, &h);
        if (s && s->sock >= 0 && w >= 16 && h >= 16 && w <= MP_MAX_DIM && h <= MP_MAX_DIM && take_token(c)) mp_sendv(s->sock, MM_SIZE, 0, w, h, 0, 0);
        return;
    }
    if (!strcmp(cmd, "ring")) {
        if (!s || c->ringfd < 0) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        /* READ-ONLY reopen of the memfd: the app can only map it PROT_READ */
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", c->ringfd);
        int ro = open(proc, O_RDONLY | O_CLOEXEC);
        if (ro < 0) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        char info[64];
        int n = snprintf(info, sizeof info, "%d %d %d %d", c->gen, c->w, c->h, c->slots);
        brokersrv_reply(fd, reqid, BST_OK, info, (uint32_t)n, ro);
        close(ro);
        return;
    }
    if (!strcmp(cmd, "photo") || !strcmp(cmd, "record")) {
        int mirror = 0, mic = 0;
        const char *rest = payload;
        if (!strcmp(cmd, "photo")) sscanf(payload, "%*s %*d %d", &mirror);
        else sscanf(payload, "%*s %*d %d %d", &mirror, &mic);
        for (int k = 0; k < (cmd[0] == 'p' ? 3 : 4) && rest; k++) { rest = strchr(rest, ' '); if (rest) rest++; }
        if (app && restrict_blocks_pkg(app, FW_R_NO_FILES)) {
            /* photos and videos are user files, saved through the same system dialog as the picker */
            char line[160];
            lerr_set(line, sizeof line, restrict_code(FW_R_NO_FILES), "app", app, NULL);
            brokersrv_reply(fd, reqid, BST_DENIED, line, (uint32_t)strlen(line), -1);
            return;
        }
        if (!s || s->sock < 0 || !allowed || c->pick_req) { brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1); return; }
        clean_name(rest, cmd[0] == 'p' ? "foto.jpg" : "video.mp4", c->out_name, sizeof c->out_name);
        /* the save dialog is the system's; the chosen file is opened here and goes to lord-media */
        c->pick_req = 0x80000000u | (next_pick++ & 0x3fffffffu);   /* never the keyring's 0xC0000000 tag */
        c->pick_kind = cmd[0] == 'p' ? 1 : 2;
        c->pick_mirror = mirror != 0;
        c->pick_mic = mic != 0;
        brokersrv_powerbox_save(c->pick_req, app, c->out_name);
        brokersrv_reply(fd, reqid, BST_OK, NULL, 0, -1);
        return;
    }
    if (!strcmp(cmd, "recstop")) {
        int ok = s && s->sock >= 0 && mp_send(s->sock, MM_RECORD, 2, NULL, NULL, 0, NULL, 0) == 0;
        brokersrv_reply(fd, reqid, ok ? BST_OK : BST_ERROR, NULL, 0, -1);
        return;
    }
    if (!strcmp(cmd, "close")) {
        if (c) cam_free(c, 0);
        brokersrv_reply(fd, reqid, BST_OK, NULL, 0, -1);
        return;
    }
    brokersrv_reply(fd, reqid, BST_ERROR, NULL, 0, -1);
}

int brokercam_pick_result(uint32_t reqid, const char *path) {
    if (!(reqid & 0x80000000u)) return 0;
    for (int i = 0; i < MAX_CAMS; i++) {
        Cam *c = &cams[i];
        if (!c->used || c->pick_req != reqid) continue;
        int kind = c->pick_kind;
        c->pick_req = 0;
        MediaSrc *s = media_src_get(c->sid);
        const char *app = brokersrv_conn_app(c->conn);
        int f = -1;
        struct stat st;
        if (path && path[0] && s && s->sock >= 0 && app && restrict_perm_pkg(app, PERM_CAMERA) == PERM_ALLOW &&
            !restrict_blocks_pkg(app, FW_R_NO_FILES))
            f = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (f >= 0 && (fstat(f, &st) || !S_ISREG(st.st_mode))) { close(f); f = -1; }
        const char *base = path ? strrchr(path, '/') : NULL;
        base = base ? base + 1 : (path ? path : "");
        if (f < 0) {
            cam_event(c, "event %d %s {\"ok\":false,\"path\":\"\",\"canceled\":%s}", c->sid, kind == 1 ? "photo" : "video", path && path[0] ? "false" : "true");
            return 1;
        }
        snprintf(kind == 1 ? c->photo_name : c->video_name, sizeof c->photo_name, "%s", base);
        int r;
        if (kind == 1) {
            int64_t v[6] = { c->pick_mirror, 0, 0, 0, 0, 0 };
            r = mp_send(s->sock, MM_PHOTO, 1, v, NULL, 0, &f, 1);
        } else {
            int ring = -1;
            if (c->pick_mic && restrict_perm_pkg(app, PERM_MICROPHONE) == PERM_ALLOW) {
                maudio_close(s->mic);
                s->mic = maudio_open(1, app, "Video", "", &ring);
            }
            int64_t v[6] = { c->pick_mirror, 0, 0, 0, 0, 0 };
            int fds[2] = { f, ring };
            r = mp_send(s->sock, MM_RECORD, 2, v, NULL, 0, fds, ring >= 0 ? 2 : 1);
        }
        close(f);
        if (r) cam_event(c, "event %d %s {\"ok\":false,\"path\":\"\"}", c->sid, kind == 1 ? "photo" : "video");
        return 1;
    }
    return 1;           /* a camera pick that is gone (camera closed meanwhile) */
}

void brokercam_conn_closed(int conn) {
    for (int i = 0; i < MAX_CAMS; i++) if (cams[i].used && cams[i].conn == conn) cam_free(&cams[i], 1);
}

void brokercam_revoke(const char *app) {
    for (int i = 0; i < MAX_CAMS; i++) {
        Cam *c = &cams[i];
        const char *a = c->used ? brokersrv_conn_app(c->conn) : NULL;
        if (!a || strcmp(a, app) || c->live) continue;
        cam_event(c, "closed %d revoked", c->sid);
        fprintf(stderr, "broker[%s]: camera permission revoked: lord-media %d killed\n", app, (int)(media_src_get(c->sid) ? media_src_get(c->sid)->pid : 0));
        cam_free(c, 1);
    }
}

/* The microphone was revoked or restricted: a video recording that takes sound from it stops now
 * (the file is finished properly) and the stream is closed. */
void brokercam_revoke_mic(const char *app) {
    for (int i = 0; i < MAX_CAMS; i++) {
        Cam *c = &cams[i];
        const char *a = c->used ? brokersrv_conn_app(c->conn) : NULL;
        MediaSrc *s = a && !strcmp(a, app) ? media_src_get(c->sid) : NULL;
        if (!s || !s->mic) continue;
        if (s->sock >= 0) mp_send(s->sock, MM_RECORD, 2, NULL, NULL, 0, NULL, 0);
        maudio_close(s->mic);
        s->mic = NULL;
        if (c->live) { brokersrv_device(app, "microphone", 0); cam_event(c, "event %d mic {\"revoked\":true}", c->sid); }
        fprintf(stderr, "broker[%s]: microphone revoked: camera recording stopped\n", app);
    }
}

void brokercam_tick(void) {
    for (int i = 0; i < MAX_CAMS; i++) {
        Cam *c = &cams[i];
        if (!c->used) continue;
        MediaSrc *s = media_src_get(c->sid);
        if (!s || s->sock < 0) {                     /* lord-media ended or crashed */
            cam_event(c, "closed %d ended", c->sid);
            cam_free(c, 0);
        }
    }
}
