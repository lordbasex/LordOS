/* js_rdesk.c — lord.rdesk: the RDP and VNC apps (docs/FILES.md, "Remote desktop"). lord-server starts lord-rdesk in a
 * scope of its own, gives it the job once and shares the picture, which the engine shows as a media node.
 *
 *   open(proto, host, port, user, domain, password, w, h, folders, { admin, scale, sound: 'here'|'there'|'none', depth }) → id, or 0
 *   poll(id) → { state, w, h, cert, media }    reads what the helper said; called by the app on a timer
 *   key(id, keysym, scancode, down) · pointer(id, x, y, buttons) · cert(id, ok) · close(id)
 *   snapshot(id, device) → bool   the session's picture as the device's card picture (rdesk/thumb.c)
 *   forgetSnapshot(device)         removes it (the device was removed) */
#include "js_rdesk.h"
#include "../media/media_int.h"
#include "../rdesk/proto.h"
#include "../doc/wire.h"
#include "../sec/cgroup.h"
#include "../lan/rconnect.h"
#include "../rdesk/thumb.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#define MAX_SESS 4
typedef struct { int used, sock, state, w, h, media; pid_t pid; char cert[160]; MAudio *audio; } Sess;   /* audio: the sound ring until the picture's media node takes it */
static Sess sess[MAX_SESS];

static Sess *get(JSContext *c, JSValueConst v) {
    int i = 0;
    JS_ToInt32(c, &i, v);
    return i >= 1 && i <= MAX_SESS && sess[i - 1].used ? &sess[i - 1] : NULL;
}

/* a user name as RDP takes it: the lan connector's rule, plus '@' for a full sign-in name (user@example.com) */
static int rdesk_user_ok(const char *s) {
    size_t n = strlen(s);
    if (n > 128 || s[0] == '-' || s[0] == '@') return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || ((s[i] | 32) >= 'a' && (s[i] | 32) <= 'z') || strchr("._-@", s[i]))) return 0;
    return 1;
}

static int helper_path(char *bin, size_t n) {
    ssize_t l = readlink("/proc/self/exe", bin, n - 16);
    if (l <= 0) return -1;
    bin[l] = 0;
    char *s = strrchr(bin, '/');
    if (!s) return -1;
    strcpy(s + 1, "lord-rdesk");
    struct stat st;
    return stat(bin, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

/* The session's size, and the folders it shares: real folders under the person's home, never the home itself nor a
 * hidden one, so that keys and settings aren't one mistaken choice away. argv[6], [7]: width, height. argv[8]: an array
 * of [name, path, readOnly]. What doesn't qualify isn't shared. */
static void add_view(JSContext *c, JBuf *b, int argc, JSValueConst *argv) {
    int w = 0, h = 0, n = 0;
    if (argc > 7) { JS_ToInt32(c, &w, argv[6]); JS_ToInt32(c, &h, argv[7]); }
    if (w >= 640 && h >= 480 && w <= RD_MAX_W && h <= RD_MAX_H) jb_printf(b, ",\"w\":%d,\"h\":%d", w, h);
    if (argc < 9 || !JS_IsArray(c, argv[8])) return;
    const char *home = getenv("HOME");
    char root[PATH_MAX];
    if (!home || !realpath(home, root)) return;
    size_t rl = strlen(root);
    for (uint32_t i = 0; i < 8; i++) {
        JSValue row = JS_GetPropertyUint32(c, argv[8], i);
        JSValue vn = JS_GetPropertyUint32(c, row, 0), vp = JS_GetPropertyUint32(c, row, 1), vr = JS_GetPropertyUint32(c, row, 2);
        const char *name = JS_ToCString(c, vn), *path = JS_ToCString(c, vp);
        int ro = JS_ToBool(c, vr);
        char real[PATH_MAX];
        struct stat st;
        int ok = name && path && name[0] && strlen(name) < 32 && realpath(path, real) && !strncmp(real, root, rl) && real[rl] == '/' &&
                 !strstr(real + rl, "/.") && !stat(real, &st) && S_ISDIR(st.st_mode);
        for (const char *p = name; ok && *p; p++) if (!(((unsigned char)*p >= '0' && (unsigned char)*p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z') || *p == ' ' || *p == '-' || *p == '_')) ok = 0;
        if (ok) {
            jb_printf(b, ",\"drive%dname\":", n); jb_str(b, name);
            jb_printf(b, ",\"drive%dpath\":", n); jb_str(b, real);
            jb_printf(b, ",\"drive%dro\":%d", n, ro > 0);
            n++;
        }
        if (name) JS_FreeCString(c, name);
        if (path) JS_FreeCString(c, path);
        JS_FreeValue(c, vn); JS_FreeValue(c, vp); JS_FreeValue(c, vr); JS_FreeValue(c, row);
    }
    jb_printf(b, ",\"drives\":%d", n);
}

/* argv[9]: { admin, scale, sound: 'here'|'there'|'none', depth } (RDP). Returns the sound wanted (RD_SOUND_*). */
static int opt_int(JSContext *c, JSValueConst o, const char *k) { JSValue v = JS_GetPropertyStr(c, o, k); int i = 0; JS_ToInt32(c, &i, v); JS_FreeValue(c, v); return i; }
static int add_options(JSContext *c, JBuf *b, int argc, JSValueConst *argv, int have_ring) {
    if (argc < 10 || !JS_IsObject(argv[9])) return RD_SOUND_NONE;
    JSValue va = JS_GetPropertyStr(c, argv[9], "admin");
    int admin = JS_ToBool(c, va) > 0, scale = opt_int(c, argv[9], "scale"), depth = opt_int(c, argv[9], "depth");
    JS_FreeValue(c, va);
    jb_printf(b, ",\"admin\":%d", admin);
    if (scale >= 100 && scale <= 500) jb_printf(b, ",\"scale\":%d", scale);
    if (depth == 16 || depth == 24 || depth == 32) jb_printf(b, ",\"depth\":%d", depth);
    int sound = RD_SOUND_NONE;
    if (have_ring >= 0) {
        JSValue vs = JS_GetPropertyStr(c, argv[9], "sound");
        const char *so = JS_ToCString(c, vs);
        if (so && !strcmp(so, "here") && have_ring) sound = RD_SOUND_HERE;
        else if (so && !strcmp(so, "there")) sound = RD_SOUND_THERE;
        if (so) JS_FreeCString(c, so);
        JS_FreeValue(c, vs);
    }
    jb_printf(b, ",\"sound\":%d", sound);
    return sound;
}
/* does this session want its sound played here? */
static int wants_sound(JSContext *c, int argc, JSValueConst *argv) {
    if (argc < 10 || !JS_IsObject(argv[9])) return 0;
    JSValue vs = JS_GetPropertyStr(c, argv[9], "sound");
    const char *so = JS_ToCString(c, vs);
    int r = so && !strcmp(so, "here");
    if (so) JS_FreeCString(c, so);
    JS_FreeValue(c, vs);
    return r;
}

static JSValue f_open(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 6) return JS_NewInt32(c, 0);
    const char *proto = JS_ToCString(c, argv[0]), *host = JS_ToCString(c, argv[1]), *user = JS_ToCString(c, argv[3]), *dom = JS_ToCString(c, argv[4]), *pw = JS_ToCString(c, argv[5]);
    int port = 0, id = 0, slot = -1;
    JS_ToInt32(c, &port, argv[2]);
    for (int i = 0; i < MAX_SESS; i++) if (!sess[i].used) { slot = i; break; }
    char bin[PATH_MAX];
    if (slot >= 0 && proto && host && user && dom && pw && (!strcmp(proto, "rdp") || !strcmp(proto, "vnc")) && rconnect_host_ok(host) && rdesk_user_ok(user) &&
        rconnect_user_ok(dom) && port >= 0 && port < 65536 && strlen(pw) < 256 && !helper_path(bin, sizeof bin)) {
        int sv[2];
        if (!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
            pid_t p = fork();
            if (!p) {
                int ch = fcntl(sv[1], F_DUPFD, 10);
                dup2(ch, 3);
                int null = open("/dev/null", O_RDWR);
                if (null >= 0) { dup2(null, 0); dup2(null, 1); }
                syscall(SYS_close_range, 4, ~0u, 0);
                sigset_t none;
                sigemptyset(&none);
                sigprocmask(SIG_SETMASK, &none, NULL);
                /* the same limits for both protocols: libfreerdp is what needs them */
                CgroupLimits lim = { .mem_high_mb = 256, .mem_max_mb = 512, .cpu_percent = 0 };
                if (cgroup_enter("app-lordos-lordos-rdesk.scope", &lim) && getenv("LORDOS_SESSIOND")) _exit(126);
                char *args[] = { "lord-rdesk", NULL };
                execve(bin, args, environ);
                _exit(127);
            }
            close(sv[1]);
            /* the sound plays here: lord-server's own stream, fed from a ring the helper writes (media/maudio.c) */
            MAudio *audio = NULL;
            int ring = -1;
            if (!strcmp(proto, "rdp") && wants_sound(c, argc, argv)) audio = maudio_open(0, "Remote Desk", host, "video", &ring);
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"proto\":"); jb_str(&b, proto);
            jb_raw(&b, ",\"host\":"); jb_str(&b, host);
            jb_printf(&b, ",\"port\":%d,\"user\":", port); jb_str(&b, user);
            jb_raw(&b, ",\"domain\":"); jb_str(&b, dom);
            jb_raw(&b, ",\"password\":"); jb_str(&b, pw);
            add_view(c, &b, argc, argv);
            int sound = add_options(c, &b, argc, argv, !strcmp(proto, "rdp") ? audio != NULL : -1);
            jb_raw(&b, "}");
            if (p > 0 && !b.oom && send(sv[0], b.p, b.n, MSG_NOSIGNAL) == (ssize_t)b.n &&
                (sound != RD_SOUND_HERE || !rd_send_fd(sv[0], RD_AUDIO_SHM, ring))) {
                sess[slot] = (Sess){ .used = 1, .sock = sv[0], .pid = p, .state = RS_CONNECTING, .audio = sound == RD_SOUND_HERE ? audio : NULL };
                if (sound != RD_SOUND_HERE) maudio_close(audio);
                id = slot + 1;
            } else { close(sv[0]); maudio_close(audio); if (p > 0) { kill(p, SIGKILL); waitpid(p, NULL, 0); } }
            if (b.p) explicit_bzero(b.p, b.cap);
            jb_free(&b);
        }
    }
    if (proto) JS_FreeCString(c, proto);
    if (host) JS_FreeCString(c, host);
    if (user) JS_FreeCString(c, user);
    if (dom) JS_FreeCString(c, dom);
    if (pw) JS_FreeCString(c, pw);
    return JS_NewInt32(c, id);
}

static void set_size(Sess *s, int w, int h) {
    if (w < 1 || h < 1 || w > RD_MAX_W || h > RD_MAX_H) { s->state = RS_FAILED; return; }
    size_t len = (size_t)w * h * 4;
    int fd = rd_picture_memfd(len);
    if (fd < 0) { s->state = RS_FAILED; return; }
    uint8_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); s->state = RS_FAILED; return; }
    MediaSrc *m = s->media ? media_src_get(s->media) : media_src_new(SK_IMAGE, "rdesk");
    if (!m) { munmap(map, len); close(fd); s->state = RS_FAILED; return; }
    if (m->map) munmap(m->map, m->map_len);
    free(m->cache); m->cache = NULL; m->cache_w = m->cache_h = 0;
    m->map = map; m->map_len = len; m->px = (uint32_t *)map; m->w = w; m->h = h; m->opaque = 1; m->frame_seq++;
    s->media = m->id; s->w = w; s->h = h;
    /* the media node keeps the sound from now on: lord-server's loop serves its stream, and closing the node ends it */
    if (s->audio && !m->audio) { m->audio = s->audio; s->audio = NULL; maudio_playing(m->audio, 1); }
    if (rd_send_picture(s->sock, fd)) s->state = RS_FAILED;
    close(fd);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_NULL;
    int frames = 0;
    int budget = RD_POLL_MAX;
    for (;;) {
        RdMsg m;
        int r = rd_take(s->sock, &m, &budget);
        if (r < 0) { if (s->state < RS_DENIED) s->state = s->state == RS_CONNECTED ? RS_ENDED : RS_FAILED; break; }
        if (!r) break;
        if (m.type == RD_SIZE) set_size(s, m.a, m.b);
        else if (m.type == RD_FRAME) frames++;
        else if (m.type == RD_STATE && m.a >= 0 && m.a <= RS_LAST) s->state = m.a;
        else if (m.type == RD_CERT) { m.text[sizeof m.text - 1] = 0; snprintf(s->cert, sizeof s->cert, "%s", m.text); }
    }
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    if (frames && ms) { ms->frame_seq++; media_src_damage(ms); }
    if (ms && ms->audio) maudio_service(ms->audio);        /* what the helper wrote since, without waiting for PipeWire to ask */
    JSValue o = JS_NewObject(c);
    static const char *const names[] = { "connecting", "connected", "denied", "failed", "ended", "unreachable", "notfound", "dropped" };
    _Static_assert(sizeof names / sizeof names[0] == RS_LAST + 1, "a name for each state");
    JS_SetPropertyStr(c, o, "state", JS_NewString(c, names[s->state]));
    JS_SetPropertyStr(c, o, "w", JS_NewInt32(c, s->w));
    JS_SetPropertyStr(c, o, "h", JS_NewInt32(c, s->h));
    JS_SetPropertyStr(c, o, "media", JS_NewInt32(c, s->media));
    JS_SetPropertyStr(c, o, "cert", JS_NewString(c, s->cert));
    return o;
}

static void put(Sess *s, uint32_t type, int a, int b, int d) { RdMsg m = { type, a, b, d, "" }; send(s->sock, &m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT); }
static int ival(JSContext *c, JSValueConst v) { int i = 0; JS_ToInt32(c, &i, v); return i; }
static JSValue f_key(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; Sess *s = get(c, argv[0]); if (s) put(s, RD_KEY, ival(c, argv[1]), ival(c, argv[2]), ival(c, argv[3])); return JS_UNDEFINED; }
static JSValue f_ptr(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; Sess *s = get(c, argv[0]); if (s) put(s, RD_PTR, ival(c, argv[1]), ival(c, argv[2]), ival(c, argv[3])); return JS_UNDEFINED; }
static JSValue f_cert(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; Sess *s = get(c, argv[0]); if (s) { s->cert[0] = 0; put(s, RD_CERT_OK, ival(c, argv[1]), 0, 0); } return JS_UNDEFINED; }
static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_UNDEFINED;
    put(s, RD_CLOSE, 0, 0, 0);
    close(s->sock);
    kill(s->pid, SIGKILL);
    waitpid(s->pid, NULL, 0);
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    if (m) media_src_close(m);
    maudio_close(s->audio);
    memset(s, 0, sizeof *s);
    return JS_UNDEFINED;
}

static JSValue f_snapshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc > 1 ? get(c, argv[0]) : NULL;
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    if (!m || !m->px || m->w < 1 || m->h < 1 || (size_t)m->w * m->h * 4 > m->map_len) return JS_FALSE;
    const char *dev = JS_ToCString(c, argv[1]);
    int ok = dev && !rd_thumb_save(getenv("HOME"), dev, m->px, m->w, m->h, RD_THUMB_W);
    if (dev) JS_FreeCString(c, dev);
    return JS_NewBool(c, ok);
}

static JSValue f_forget_snapshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *dev = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = dev && !rd_thumb_remove(getenv("HOME"), dev);
    if (dev) JS_FreeCString(c, dev);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("open", 6, f_open), JS_CFUNC_DEF("poll", 1, f_poll), JS_CFUNC_DEF("key", 4, f_key),
    JS_CFUNC_DEF("pointer", 4, f_ptr), JS_CFUNC_DEF("cert", 2, f_cert), JS_CFUNC_DEF("close", 1, f_close),
    JS_CFUNC_DEF("snapshot", 2, f_snapshot), JS_CFUNC_DEF("forgetSnapshot", 1, f_forget_snapshot),
};

void js_rdesk_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "rdesk", o);
}
