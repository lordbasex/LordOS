/* js_vmcli.c — lord.vmcli: the `machines` command's session channel (vm/vmcli.h; docs/VIRTUALIZATION.md "From the
 * terminal"; approved by the maintainer 2026-11-21; AGENTS.md "The machines command's session channel").
 *
 * `machines start NAME` and `machines open NAME` go through the person's running session so they do exactly what the
 * Machines app does (its window, the bridged network's and the vGPU's admin steps, vm-memlock). The endpoint:
 * $XDG_RUNTIME_DIR/lordos-machines.sock, AF_UNIX stream, made 0600 under umask 0177, never replacing anything that isn't
 * a socket; only in a session's lord-server (never the login screen's; an --app process has no lord.vmcli). A peer is
 * kept only when SO_PEERCRED says it runs as this lord-server's own uid; at most VC_PEERS at a time (more are closed at
 * once). Each sends one request line (vmcli_req_parse: a closed verb and a machine folder's path, nothing else) within
 * VC_READ_MS; a line that doesn't parse, more bytes than one line, or anything after it closes the connection. The
 * Machines app (apps/machines/cli.js) takes the request with poll(), finds the folder among its own library's machines
 * (anything else is refused there) and answers once with answer(); lord-server writes that one line and closes.
 *
 * It runs no command, takes no name, option or path beyond the machine's folder, and reads no file for the peer.
 *
 * JS: lord.vmcli.active() → the endpoint is listening; poll() → [{ id, verb: 'open'|'start'|'start-detached'|'release',
 * dir }] (also accepts and reads peers; cli.js calls it every 300 ms); answer(id, code, text) → whether that request was
 * still waiting (its peer may have gone). */
#include "js_vmcli.h"
#include "js_sys.h"
#include "../vm/vmcli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define VC_PEERS    4
#define VC_READ_MS  5000            /* to send the request line */
#define VC_WAIT_MS  (15 * 60000)    /* for the app's answer: a start may wait on the administrator's password */

typedef struct {
    int fd, state;                  /* 0 free, 1 reading its line, 2 its request is with the app, 3 taken by poll() */
    uint32_t id;
    long long deadline;
    char buf[VMCLI_LINE_MAX];
    size_t n;
    VmCliReq req;
} VcPeer;

static int tried, lfd = -1;
static VcPeer peers[VC_PEERS];
static uint32_t next_id;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void drop(VcPeer *p) {
    if (p->fd >= 0) close(p->fd);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

static void setup(void) {
    tried = 1;
    for (int i = 0; i < VC_PEERS; i++) peers[i].fd = -1;
    if (js_sys_greeter()) return;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[108];
    if (!rt || rt[0] != '/' || snprintf(path, sizeof path, "%s/%s", rt, VMCLI_SOCK) >= (int)sizeof path) return;
    struct stat st;
    if (!lstat(path, &st)) { if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return; unlink(path); }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    mode_t old = umask(0177);
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r || chmod(path, 0600) || listen(s, VC_PEERS)) { close(s); return; }
    lfd = s;
    fprintf(stderr, "vmcli: the machines command's channel is listening (%s)\n", path);
}

static void take_peers(void) {
    for (;;) {
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) return;
        struct ucred cr;
        socklen_t cl = sizeof cr;
        VcPeer *p = NULL;
        for (int i = 0; i < VC_PEERS && !p; i++) if (!peers[i].state) p = &peers[i];
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cl != sizeof cr || cr.uid != getuid() || !p) { close(c); continue; }
        memset(p, 0, sizeof *p);
        p->fd = c;
        p->state = 1;
        p->deadline = now_ms() + VC_READ_MS;
    }
}

static void read_peers(void) {
    long long now = now_ms();
    for (int i = 0; i < VC_PEERS; i++) {
        VcPeer *p = &peers[i];
        if (!p->state) continue;
        if (now > p->deadline) { drop(p); continue; }
        if (p->state != 1) {
            /* waiting for the app: the peer says nothing more; gone or talking, it's dropped */
            char x;
            ssize_t r = recv(p->fd, &x, 1, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR) || r > 0) drop(p);
            continue;
        }
        ssize_t r = recv(p->fd, p->buf + p->n, sizeof p->buf - p->n, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { drop(p); continue; }
        if (r < 0) continue;
        p->n += (size_t)r;
        char *nl = memchr(p->buf, '\n', p->n);
        if (!nl) { if (p->n == sizeof p->buf) drop(p); continue; }
        size_t len = (size_t)(nl - p->buf) + 1;
        if (len != p->n || vmcli_req_parse(p->buf, len, &p->req)) {
            char ans[VMCLI_ANSWER_MAX + 1];
            size_t k = vmcli_answer_format(VMCLI_USAGE, "", ans, sizeof ans);
            if (send(p->fd, ans, k, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { /* gone: nothing to say */ }
            drop(p);
            continue;
        }
        p->id = ++next_id ? next_id : ++next_id;
        p->state = 2;
        p->deadline = now + VC_WAIT_MS;
    }
}

static JSValue f_active(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!tried) setup();
    return JS_NewBool(c, lfd >= 0);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!tried) setup();
    JSValue arr = JS_NewArray(c);
    if (lfd < 0) return arr;
    take_peers();
    read_peers();
    uint32_t n = 0;
    for (int i = 0; i < VC_PEERS; i++) {
        VcPeer *p = &peers[i];
        if (p->state != 2) continue;
        p->state = 3;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewUint32(c, p->id));
        JS_SetPropertyStr(c, o, "verb", JS_NewString(c, vmcli_verb_name(p->req.verb)));
        JS_SetPropertyStr(c, o, "dir", JS_NewString(c, p->req.dir));
        JS_SetPropertyUint32(c, arr, n++, o);
    }
    return arr;
}

static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    uint32_t id = 0;
    int32_t code = VMCLI_FAILED;
    if (argc < 2 || JS_ToUint32(c, &id, argv[0]) || JS_ToInt32(c, &code, argv[1]) || !id) return JS_FALSE;
    VcPeer *p = NULL;
    for (int i = 0; i < VC_PEERS && !p; i++) if (peers[i].state == 3 && peers[i].id == id) p = &peers[i];
    if (!p) return JS_FALSE;
    const char *text = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(c, argv[2]) : NULL;
    char ans[VMCLI_ANSWER_MAX + 1];
    size_t k = vmcli_answer_format(code, text ? text : "", ans, sizeof ans);
    if (text) JS_FreeCString(c, text);
    int ok = send(p->fd, ans, k, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)k;
    drop(p);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("active", 0, f_active), JS_CFUNC_DEF("poll", 0, f_poll), JS_CFUNC_DEF("answer", 3, f_answer),
};

void js_vmcli_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "vmcli", o);
}
