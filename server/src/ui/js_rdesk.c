/* js_rdesk.c — lord.rdesk: the RDP and VNC apps (docs/FILES.md, "Remote desktop"). axia-server starts axia-rdesk in a
 * scope of its own, gives it the job once and shares the picture, which the engine shows as a media node.
 *
 *   open(proto, host, port, user, domain, password) → id, or 0
 *   poll(id) → { state, w, h, cert, media }    reads what the helper said; called by the app on a timer
 *   key(id, keysym, scancode, down) · pointer(id, x, y, buttons) · cert(id, ok) · close(id) */
#include "js_rdesk.h"
#include "../media/media_int.h"
#include "../rdesk/proto.h"
#include "../doc/wire.h"
#include "../sec/cgroup.h"
#include "../lan/rconnect.h"

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
typedef struct { int used, sock, state, w, h, media; pid_t pid; char cert[160]; } Sess;
static Sess sess[MAX_SESS];

static Sess *get(JSContext *c, JSValueConst v) {
    int i = 0;
    JS_ToInt32(c, &i, v);
    return i >= 1 && i <= MAX_SESS && sess[i - 1].used ? &sess[i - 1] : NULL;
}

static int helper_path(char *bin, size_t n) {
    ssize_t l = readlink("/proc/self/exe", bin, n - 16);
    if (l <= 0) return -1;
    bin[l] = 0;
    char *s = strrchr(bin, '/');
    if (!s) return -1;
    strcpy(s + 1, "axia-rdesk");
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

static JSValue f_open(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 6) return JS_NewInt32(c, 0);
    const char *proto = JS_ToCString(c, argv[0]), *host = JS_ToCString(c, argv[1]), *user = JS_ToCString(c, argv[3]), *dom = JS_ToCString(c, argv[4]), *pw = JS_ToCString(c, argv[5]);
    int port = 0, id = 0, slot = -1;
    JS_ToInt32(c, &port, argv[2]);
    for (int i = 0; i < MAX_SESS; i++) if (!sess[i].used) { slot = i; break; }
    char bin[PATH_MAX];
    if (slot >= 0 && proto && host && user && dom && pw && (!strcmp(proto, "rdp") || !strcmp(proto, "vnc")) && rconnect_host_ok(host) && rconnect_user_ok(user) &&
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
                char *args[] = { "axia-rdesk", NULL };
                execve(bin, args, environ);
                _exit(127);
            }
            close(sv[1]);
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"proto\":"); jb_str(&b, proto);
            jb_raw(&b, ",\"host\":"); jb_str(&b, host);
            jb_printf(&b, ",\"port\":%d,\"user\":", port); jb_str(&b, user);
            jb_raw(&b, ",\"domain\":"); jb_str(&b, dom);
            jb_raw(&b, ",\"password\":"); jb_str(&b, pw);
            add_view(c, &b, argc, argv);
            jb_raw(&b, "}");
            if (p > 0 && !b.oom && send(sv[0], b.p, b.n, MSG_NOSIGNAL) == (ssize_t)b.n) {
                sess[slot] = (Sess){ .used = 1, .sock = sv[0], .pid = p, .state = RS_CONNECTING };
                id = slot + 1;
            } else { close(sv[0]); if (p > 0) { kill(p, SIGKILL); waitpid(p, NULL, 0); } }
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
    free(m->cache); m->cache = NULL;
    m->map = map; m->map_len = len; m->px = (uint32_t *)map; m->w = w; m->h = h; m->opaque = 1; m->frame_seq++;
    s->media = m->id; s->w = w; s->h = h;
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
        else if (m.type == RD_STATE && m.a >= 0 && m.a <= RS_UNREACHABLE) s->state = m.a;
        else if (m.type == RD_CERT) { m.text[sizeof m.text - 1] = 0; snprintf(s->cert, sizeof s->cert, "%s", m.text); }
    }
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    if (frames && ms) { ms->frame_seq++; media_src_damage(ms); }
    JSValue o = JS_NewObject(c);
    static const char *const names[] = { "connecting", "connected", "denied", "failed", "ended", "unreachable" };
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
    memset(s, 0, sizeof *s);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("open", 6, f_open), JS_CFUNC_DEF("poll", 1, f_poll), JS_CFUNC_DEF("key", 4, f_key),
    JS_CFUNC_DEF("pointer", 4, f_ptr), JS_CFUNC_DEF("cert", 2, f_cert), JS_CFUNC_DEF("close", 1, f_close),
};

void js_rdesk_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "rdesk", o);
}
