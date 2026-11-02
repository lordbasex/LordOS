/* js_broker.c — el objeto `lord` del proceso de una app .lpk aislada (ver js_broker.h).
 *
 * Dibujar es local (las primitivas createNode/setStyle… las registra js.c). Todo lo demás
 * (fs, portapapeles, permisos, dispositivos) va por el broker a lord-server, que verifica el
 * permiso de cada llamada. Así la API del SDK para quien escribe la app no cambia. */
#include "js_broker.h"
#include "../sec/broker.h"
#include "../sec/perms.h"
#include "../sec/json.h"
#include "../lpk.h"
#include "../keyboard.h"
#include "js.h"
#include "js_sys.h"
#include "js_err.h"

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include "../sec/codesig.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <math.h>
#include <stdatomic.h>
#include <unistd.h>

static int broker_fd = -1;
static uint32_t next_req = 1;
static char app_id[48];
static char pkg_dir[PATH_MAX];   /* carpeta del paquete (solo lectura); se fija antes de la caja de arena */

static int pkg_system;           /* 1: a system app (assets/sysapps): its app.js is LordOS code, checked */
static int code_refused;         /* its app.js was refused by the verified-code check */

void js_broker_set_pkgdir(const char *dir, int system) { snprintf(pkg_dir, sizeof pkg_dir, "%s", dir); pkg_system = system == 1; }
int js_broker_code_refused(void) { return code_refused; }

static char *pkg_read(const char *rel, size_t max, size_t *len);

/* Eventos que llegan mientras se espera una respuesta: se procesan en el bucle (js_broker_poll). */
static struct { char *buf; size_t len; } queued[32];
static int nqueued;
static void queue_event(const char *p, size_t n) {
    /* lord-maild replies (js_brokermail.c) are taken at once: never dropped while an RPC waits */
    if (n > 5 && !memcmp(p, "mail", 5)) { js_brokermail_chunk(p + 5, n - 5); return; }
    if (nqueued >= 32) return;
    char *b = malloc(n + 1);
    if (!b) return;
    memcpy(b, p, n);
    b[n] = 0;
    queued[nqueued].buf = b;
    queued[nqueued++].len = n;
}

static void broker_connect(void) {
    const char *s = getenv(BROKER_FD_ENV);
    broker_fd = s ? atoi(s) : -1;
}

int js_broker_fd(void) { return broker_fd; }

/* RPC síncrono: manda el pedido y espera la respuesta. status recibe BrokerStatus; out/outlen el
 * payload (malloc; el llamador libera). 0 si llegó respuesta. */
static int rpc_fds(int op, const void *payload, uint32_t len, const int *fds, int nfds, int *status, char **out, uint32_t *outlen) {
    if (broker_fd < 0 || nfds < 0 || nfds > 16) { if (status) *status = BST_ERROR; return -1; }
    BrokerHdr h = { .reqid = next_req++, .op = (uint16_t)op, .status = BROKER_REQ, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)payload, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    char cbuf[CMSG_SPACE(16 * sizeof(int))];
    if (nfds) {                                             /* files for lord-maild (BOP_MAIL) */
        memset(cbuf, 0, sizeof cbuf);
        m.msg_control = cbuf;
        m.msg_controllen = CMSG_SPACE((size_t)nfds * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
        memcpy(CMSG_DATA(c), fds, (size_t)nfds * sizeof(int));
    }
    if (sendmsg(broker_fd, &m, MSG_NOSIGNAL) < 0) { if (status) *status = BST_ERROR; return -1; }
    for (;;) {
        BrokerHdr r;
        char *buf = malloc(BROKER_MAX_MSG + 1);
        if (!buf) { if (status) *status = BST_ERROR; return -1; }
        struct iovec riov[2] = { { &r, sizeof r }, { buf, BROKER_MAX_MSG } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2 };
        ssize_t n = recvmsg(broker_fd, &rm, 0);
        if (n < (ssize_t)sizeof r) { free(buf); if (status) *status = BST_ERROR; return -1; }
        if (r.reqid != h.reqid) {                           /* evento sin pedido: se guarda para después */
            if (r.op == BOP_EVENT) queue_event(buf, (uint32_t)n - sizeof r);
            free(buf);
            continue;
        }
        uint32_t pl = (uint32_t)n - sizeof r;
        buf[pl] = 0;
        if (status) *status = r.status;
        if (out) { *out = buf; if (outlen) *outlen = pl; } else free(buf);
        return 0;
    }
}
static int rpc(int op, const void *payload, uint32_t len, int *status, char **out, uint32_t *outlen) {
    return rpc_fds(op, payload, len, NULL, 0, status, out, outlen);
}

/* Un evento: "tipo\0json". El teclado se aplica acá (keyboard.c); el resto va a JS
 * (__lordBrokerEvent: tokens del tema, idioma, estado completo). */
/* The layout itself comes from the compositor's keymap (wl_keyboard.keymap, appclient.c):
 * kb_set() here would run inside the sandbox, where xkbcommon cannot read its rules. Only the
 * active group is taken from the broker. */
static void apply_kb_state(const JVal *kb) {
    const char *l = json_str(kb, "layouts");
    if (l && l[0]) kb_set_group((int)json_num(kb, "group", 0));
}
static void apply_keyboard(const char *json, size_t len) {
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) return;
    apply_kb_state(d.root);
    json_free(&d);
}

static void dispatch_event(const char *buf, size_t n) {
    const char *nul = memchr(buf, 0, n);
    if (!nul) return;
    const char *kind = buf, *json = nul + 1;
    size_t jl = n - (size_t)(json - buf);
    if (!strcmp(kind, "cam")) { js_brokercam_event(json, jl); return; }     /* camera frames (js_brokercam.c) */
    if (!strcmp(kind, "mail")) { js_brokermail_chunk(json, jl); return; }  /* lord-maild replies (js_brokermail.c) */
    if (!strcmp(kind, "keyboard")) { apply_keyboard(json, jl); return; }
    if (!strcmp(kind, "state")) {                    /* resincronizar: teclado acá, el resto a JS */
        JDoc d;
        if (!json_parse(json, jl, &d, NULL, 0)) {
            const JVal *kb = json_get(d.root, "keyboard");
            if (kb && kb->type == J_OBJ) apply_kb_state(kb);
            json_free(&d);
        }
    }
    js_broker_app_event(kind, json, jl);
}

/* events queued while an RPC was waiting: the loop must not sleep on them */
int js_broker_pending(void) { return nqueued > 0; }

void js_broker_poll(void) {
    for (int i = 0; i < nqueued; i++) { dispatch_event(queued[i].buf, queued[i].len); free(queued[i].buf); }
    nqueued = 0;
    if (broker_fd < 0) return;
    for (;;) {
        BrokerHdr r;
        static char buf[BROKER_MAX_MSG + 1];
        struct iovec riov[2] = { { &r, sizeof r }, { buf, BROKER_MAX_MSG } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2 };
        ssize_t n = recvmsg(broker_fd, &rm, MSG_DONTWAIT);
        if (n < (ssize_t)sizeof r) break;
        if (r.op != BOP_EVENT) continue;              /* una respuesta tardía: nadie la espera */
        buf[(size_t)n - sizeof r] = 0;                /* JS_ParseJSON needs the text NUL-terminated */
        dispatch_event(buf, (size_t)n - sizeof r);
    }
}

/* ¿la app declaró `network` y lo tiene permitido? (se decide antes de la caja de arena) */
int js_broker_wants_network(void) {
    const char *home = getenv("HOME");
    if (!home || !pkg_dir[0] || !app_id[0]) return 0;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/manifest.json", pkg_dir);        /* the folder app_package_dir found, whichever root */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char m[64 * 1024];
    ssize_t n = read(fd, m, sizeof m - 1);
    close(fd);
    if (n <= 0) return 0;
    m[n] = 0;
    JDoc d;
    int declared = 0;
    if (!json_parse(m, (size_t)n, &d, NULL, 0)) {
        const JVal *perms = json_get(d.root, "permissions");
        for (const JVal *p = perms && perms->type == J_ARR ? perms->kid : NULL; p; p = p->next)
            if (p->type == J_STR && !strcmp(p->str, "network")) declared = 1;
        json_free(&d);
    }
    if (!declared) return 0;
    perm_init(home);
    return perm_get(app_id, PERM_NETWORK) == PERM_ALLOW;
}

/* ---- lord.fs (carpeta de datos de la app, por el broker) ---- */

static JSValue f_fs_home(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    if (rpc(BOP_FS_HOME, NULL, 0, &st, &out, &len) || st != BST_OK) { free(out); return JS_NewString(c, ""); }
    JSValue v = JS_NewStringLen(c, out, len);
    free(out);
    return v;
}

static JSValue f_fs_read(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *p = JS_ToCString(c, argv[0]);
    const char *mode = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    int binary = mode && !strcmp(mode, "binary");
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    int r = p ? rpc(BOP_FS_READ, p, (uint32_t)strlen(p), &st, &out, &len) : -1;
    if (p) JS_FreeCString(c, p);
    if (mode) JS_FreeCString(c, mode);
    if (r || st != BST_OK) { free(out); return st == BST_DENIED ? js_throw_code(c, "app.outsideFolder", NULL) : js_throw_code(c, "app.readFailed", NULL); }
    JSValue v = binary ? JS_NewArrayBufferCopy(c, (uint8_t *)out, len) : JS_NewStringLen(c, out, len);
    free(out);
    return v;
}

static JSValue f_fs_write(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *p = JS_ToCString(c, argv[0]);
    if (!p) return JS_EXCEPTION;
    size_t plen = strlen(p), dlen = 0;
    const char *data = NULL;
    uint8_t *ab = JS_GetArrayBuffer(c, &dlen, argv[1]);
    char *dyn = NULL;
    if (ab) data = (const char *)ab;
    else { JS_FreeValue(c, JS_GetException(c)); data = JS_ToCStringLen(c, &dlen, argv[1]); dyn = (char *)data; }
    size_t total = plen + 1 + dlen;
    char *buf = malloc(total);
    int st = BST_ERROR, r = -1;
    if (buf) { memcpy(buf, p, plen); buf[plen] = 0; memcpy(buf + plen + 1, data, dlen); r = rpc(BOP_FS_WRITE, buf, (uint32_t)total, &st, NULL, NULL); }
    free(buf);
    JS_FreeCString(c, p);
    if (dyn) JS_FreeCString(c, dyn);
    if (r || st != BST_OK) return st == BST_DENIED ? js_throw_code(c, "app.outsideFolder", NULL) : js_throw_code(c, "app.writeFailed", NULL);
    return JS_UNDEFINED;
}

static JSValue f_fs_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *p = JS_ToCString(c, argv[0]);
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    int r = rpc(BOP_FS_LIST, p ? p : ".", p ? (uint32_t)strlen(p) : 1, &st, &out, &len);
    if (p) JS_FreeCString(c, p);
    if (r || st != BST_OK) { free(out); return JS_NewArray(c); }
    JSValue v = JS_ParseJSON(c, out ? out : "[]", len, "<fs.list>");
    free(out);
    return JS_IsException(v) ? (JS_FreeValue(c, JS_GetException(c)), JS_NewArray(c)) : v;
}

/* ---- lord.clipboard ---- */

static JSValue f_clip_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    if (rpc(BOP_CLIP_GET, NULL, 0, &st, &out, &len) || st != BST_OK) {
        /* a firewall restriction comes back as its code line (fw.restricted.clipboard) */
        char line[160] = "";
        if (st == BST_DENIED && out && len && len < sizeof line) { memcpy(line, out, len); line[len] = 0; }
        free(out);
        return lerr_is(line) ? js_throw_line(c, line) : js_throw_code(c, "app.clipboardDenied", NULL);
    }
    JSValue v = JS_NewStringLen(c, out, len);
    free(out);
    return v;
}
static JSValue f_clip_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t sl = 0;
    const char *s = JS_ToCStringLen(c, &sl, argv[0]);
    int st;
    /* lord.clipboard.set(text, { secure, clearAfter }): a secret (Passwords) is marked sensitive and
     * cleared by lord-server after clearAfter seconds if it is still there */
    int secure = 0, clear_after = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(c, argv[1], "secure");
        secure = JS_ToBool(c, v);
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[1], "clearAfter");
        int32_t ca = 0;
        if (JS_IsNumber(v)) JS_ToInt32(c, &ca, v);
        JS_FreeValue(c, v);
        clear_after = ca > 0 && ca <= 600 ? ca : 0;
    }
    if (s && secure) {
        char *buf = malloc(sl + 16);
        if (buf) {
            int k = snprintf(buf, 16, "%d", clear_after) + 1;
            memcpy(buf + k, s, sl);
            rpc(BOP_CLIP_SECRET, buf, (uint32_t)(k + sl), &st, NULL, NULL);
            explicit_bzero(buf, (size_t)k + sl);
            free(buf);
        }
    } else if (s) rpc(BOP_CLIP_SET, s, (uint32_t)sl, &st, NULL, NULL);
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

/* For js_vault.c (Passwords in app mode): lord-server opens the FIDO2 key for lord-vault after the
 * `usb` permission. 0 and the device name, or -1 with err ("denied", "no-device"…). */
int js_broker_vault_fido(char *name, size_t n, char *err, size_t en) {
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    int r = rpc(BOP_VAULT_FIDO, NULL, 0, &st, &out, &len);
    if (!r && st == BST_OK) { snprintf(name, n, "%.*s", (int)len, out ? out : ""); free(out); return 0; }
    /* denied by the permission, or by a firewall restriction (a code line, "fw.restricted.usb …") */
    snprintf(err, en, "%s", st == BST_DENIED ? (out && len && lerr_is(out) ? "fido-restricted" : "fido-denied") : out && len ? out : "no-device");
    free(out);
    return -1;
}

/* ---- lord.broker: pedir un permiso (aviso del sistema) y marcar un dispositivo en uso ---- */

/* RPC que recibe un fd por SCM_RIGHTS (powerbox). Devuelve el fd, o -1; name el nombre elegido. */
static int rpc_fd(int op, const void *payload, uint32_t len, char *name, size_t nn) {
    if (broker_fd < 0) return -1;
    BrokerHdr h = { .reqid = next_req++, .op = (uint16_t)op, .status = BROKER_REQ, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)payload, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    if (sendmsg(broker_fd, &m, MSG_NOSIGNAL) < 0) return -1;
    /* events that arrive meanwhile can be large (mail chunks): a full-size buffer */
    static char *buf;
    if (!buf && !(buf = malloc(BROKER_MAX_MSG + 1))) return -1;
    for (;;) {
        BrokerHdr r;
        char cmsg[CMSG_SPACE(sizeof(int))];
        struct iovec riov[2] = { { &r, sizeof r }, { buf, BROKER_MAX_MSG } };
        struct msghdr rm = { .msg_iov = riov, .msg_iovlen = 2, .msg_control = cmsg, .msg_controllen = sizeof cmsg };
        ssize_t n = recvmsg(broker_fd, &rm, MSG_CMSG_CLOEXEC);
        if (n < (ssize_t)sizeof r) return -1;
        if (r.reqid != h.reqid) {
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&rm); cm; cm = CMSG_NXTHDR(&rm, cm))   /* never keep an fd nobody asked for */
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) { int f; memcpy(&f, CMSG_DATA(cm), sizeof f); close(f); }
            if (r.op == BOP_EVENT) queue_event(buf, (uint32_t)n - sizeof r);
            continue;
        }
        int fd = -1;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&rm); cm; cm = CMSG_NXTHDR(&rm, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(cm), sizeof fd);
        uint32_t pl = (uint32_t)n - sizeof r;
        if (r.status != BST_OK) {
            if (fd >= 0) close(fd);
            /* the reason, when it is a code line (a firewall restriction): the caller can throw it */
            if (name && nn) { size_t k = pl < nn - 1 ? pl : nn - 1; memcpy(name, buf, k); name[k] = 0; if (!lerr_is(name)) name[0] = 0; }
            return -1;
        }
        if (name && nn) { size_t k = pl < nn - 1 ? pl : nn - 1; memcpy(name, buf, k); name[k] = 0; }
        return fd;
    }
}

/* For js_brokercam.c (js_broker.h): the same RPCs, plus a send that does not wait for a reply. */
int js_broker_rpc(int op, const void *payload, uint32_t len, int *status, char **out, uint32_t *outlen) { return rpc(op, payload, len, status, out, outlen); }
int js_broker_rpc_fds(int op, const void *payload, uint32_t len, const int *fds, int nfds, int *status, char **out, uint32_t *outlen) {
    return rpc_fds(op, payload, len, fds, nfds, status, out, outlen);
}
int js_broker_rpc_fd(int op, const void *payload, uint32_t len, char *info, size_t n) { return rpc_fd(op, payload, len, info, n); }
int js_broker_send(int op, const void *payload, uint32_t len) {
    if (broker_fd < 0) return -1;
    BrokerHdr h = { .reqid = next_req++, .op = (uint16_t)op, .status = BROKER_REQ, .len = len };
    struct iovec iov[2] = { { &h, sizeof h }, { (void *)payload, len } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    return sendmsg(broker_fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 ? -1 : 0;
}

/* lord.broker.pick(save, nombreSugerido) → { fd, name } | null (el diálogo lo dibuja lord-server) */
/* ---- the companion (a packaged app only; broker.h, app/companion.c) ----
 * One socket message is one JSON text, 64 KB at most. What arrives goes to __lordBrokerEvent("companion", data). */
#define COMPANION_MSG (64 * 1024)
static int comp_fd = -1;

int js_broker_companion_fd(void) { return comp_fd; }

void js_broker_companion_read(void) {
    static char buf[COMPANION_MSG + 1];
    for (int i = 0; i < 16 && comp_fd >= 0; i++) {
        ssize_t n = recv(comp_fd, buf, COMPANION_MSG, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) {
            close(comp_fd);
            comp_fd = -1;
            static const char gone[] = "{\"companion\":\"ended\",\"code\":\"\"}";
            js_broker_app_event("companion", gone, sizeof gone - 1);
            return;
        }
        if (n > COMPANION_MSG) continue;                  /* too long: not delivered in part */
        buf[n] = 0;
        js_broker_app_event("companion", buf, (size_t)n);
    }
}

/* lord.broker.companion() → true if lord-server took the request; the answer comes as a companion event. */
static JSValue f_broker_companion(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (comp_fd >= 0) return JS_TRUE;
    char line[160] = "";
    int fd = rpc_fd(BOP_COMPANION, NULL, 0, line, sizeof line);
    if (fd < 0) return line[0] ? js_throw_line(c, line) : JS_FALSE;
    comp_fd = fd;
    return JS_TRUE;
}

/* lord.broker.companionSend(text) → false if it isn't running, or can't take it now. */
static JSValue f_broker_companion_send(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    size_t n = 0;
    const char *s = JS_ToCStringLen(c, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    int ok = comp_fd >= 0 && n && n <= COMPANION_MSG && send(comp_fd, s, n, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)n;
    JS_FreeCString(c, s);
    return JS_NewBool(c, ok);
}

/* lord.broker.statusItem(json) → false if refused; "" takes it off. lord-server validates and rewrites it. */
static JSValue f_broker_status_item(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    size_t n = 0;
    const char *s = JS_ToCStringLen(c, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    int st = BST_ERROR;
    int r = rpc(BOP_STATUS, s, (uint32_t)n, &st, NULL, NULL);
    JS_FreeCString(c, s);
    return JS_NewBool(c, !r && st == BST_OK);
}

static JSValue f_broker_pick(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int save = JS_ToBool(c, argv[0]);
    const char *sg = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : "";
    char pl[300], name[256] = "";
    int n = snprintf(pl, sizeof pl, "%d", save);
    pl[n++] = 0;
    n += snprintf(pl + n, sizeof pl - n, "%s", sg ? sg : "");
    int fd = rpc_fd(BOP_PICK, pl, (uint32_t)n, name, sizeof name);
    if (sg && sg[0]) JS_FreeCString(c, sg);
    if (fd < 0) return name[0] ? js_throw_line(c, name) : JS_NULL;    /* restricted: fw.restricted.files; cancelled: null */
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "fd", JS_NewInt32(c, fd));
    JS_SetPropertyStr(c, o, "name", JS_NewString(c, name));
    return o;
}

static JSValue f_fd_read(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int32_t fd = -1;
    JS_ToInt32(c, &fd, argv[0]);
    if (fd < 0) return js_throw_code(c, "app.badFd", NULL);
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; if (cap > 64u << 20) { free(buf); close(fd); return js_throw_code(c, "fs.tooLarge", "max", "64 MB", NULL); } char *nb = realloc(buf, cap); if (!nb) { free(buf); close(fd); return JS_ThrowOutOfMemory(c); } buf = nb; }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    JSValue v = JS_NewArrayBufferCopy(c, (uint8_t *)buf, len);
    free(buf);
    return v;
}
static JSValue f_fd_write(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int32_t fd = -1;
    JS_ToInt32(c, &fd, argv[0]);
    size_t len = 0;
    uint8_t *ab = JS_GetArrayBuffer(c, &len, argv[1]);
    const char *data = (const char *)ab, *dyn = NULL;
    if (!ab) { JS_FreeValue(c, JS_GetException(c)); data = JS_ToCStringLen(c, &len, argv[1]); dyn = data; }
    size_t w = 0;
    while (data && w < len) { ssize_t r = write(fd, data + w, len - w); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; w += (size_t)r; }
    if (dyn) JS_FreeCString(c, dyn);
    return JS_NewBool(c, data && w == len);
}
/* lord.broker.fdSize(fd) → bytes of a file the app holds (from the system file dialog), or -1 */
static JSValue f_fd_size(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t fd = -1;
    if (argc > 0) JS_ToInt32(c, &fd, argv[0]);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) return JS_NewInt32(c, -1);
    return JS_NewFloat64(c, (double)st.st_size);
}
static JSValue f_fd_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int32_t fd = -1;
    JS_ToInt32(c, &fd, argv[0]);
    if (fd >= 0) close(fd);
    return JS_UNDEFINED;
}

static JSValue f_broker_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *p = JS_ToCString(c, argv[0]);
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    int r = p ? rpc(BOP_PERM, p, (uint32_t)strlen(p), &st, &out, &len) : -1;
    int allow = !r && st == BST_OK && out && len && out[0] == '1';
    if (p) JS_FreeCString(c, p);
    free(out);
    return JS_NewBool(c, allow);
}
/* Micrófono: pide el anillo a lord-server (con consentimiento), lo mapea de solo lectura y lee el
 * nivel. La app nunca abre PipeWire ni toca el fd del dispositivo. */
#include <sys/mman.h>
#include "../media/aring.h"
static ARingHdr *mic_ring;
static uint32_t mic_size;
static uint64_t mic_cursor;
static int mic_fd = -1;

static JSValue f_mic_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char name[256] = "";
    int ro = rpc_fd(BOP_MIC, "1", 1, name, sizeof name);
    if (ro < 0) return name[0] ? js_throw_line(c, name) : JS_NewBool(c, 0);     /* fw.restricted.microphone */
    unsigned size = 0, rate = 0, ch = 0;
    sscanf(name, "%u %u %u", &size, &rate, &ch);
    size_t maplen = AR_HDR + size;
    void *m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, ro, 0);   /* solo lectura: no toca rpos */
    if (m == MAP_FAILED) { close(ro); return JS_NewBool(c, 0); }
    if (mic_ring) munmap(mic_ring, AR_HDR + mic_size);
    if (mic_fd >= 0) close(mic_fd);
    mic_ring = m; mic_size = size; mic_fd = ro;
    mic_cursor = atomic_load(&mic_ring->wpos);          /* arranca desde ahora */
    return JS_NewBool(c, 1);
}
static JSValue f_mic_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    if (mic_ring) { munmap(mic_ring, AR_HDR + mic_size); mic_ring = NULL; }
    if (mic_fd >= 0) { close(mic_fd); mic_fd = -1; }
    int st; rpc(BOP_MIC, "0", 1, &st, NULL, NULL);
    return JS_UNDEFINED;
}
/* lord.broker.micLevel() → RMS (0..1) de lo nuevo desde la última llamada (como un ring broadcast:
 * lee con su propio cursor, nunca rpos; detecta overrun por wpos). */
static JSValue f_mic_level(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!mic_ring) return JS_NewFloat64(c, 0);
    uint64_t w = atomic_load(&mic_ring->wpos);
    if (w - mic_cursor > mic_size) mic_cursor = w - mic_size;    /* overrun: perdimos datos */
    uint64_t avail = (w - mic_cursor) & ~(uint64_t)1;
    if (avail > 8192) { mic_cursor = w - 8192; avail = 8192; }   /* solo lo más reciente */
    const int16_t *data = (const int16_t *)ar_data(mic_ring);
    double sum = 0;
    uint64_t n = avail / 2;
    for (uint64_t i = 0; i < n; i++) { int16_t s = data[((mic_cursor / 2) + i) % (mic_size / 2)]; sum += (double)s * s; }
    mic_cursor = w & ~(uint64_t)1;
    double rms = n ? sqrt(sum / n) / 32768.0 : 0;
    return JS_NewFloat64(c, rms);
}

/* lord.broker.openApp(app, json, newWindow) → true: a system app opens another one with args
 * (Mail → Calendar, Contacts → a new message). lord-server allows it only between Mail, Calendar,
 * Contacts and Reminders, and only small args without paths (sec/brokermail.c). */
static JSValue f_broker_open_app(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    size_t jl = 0;
    const char *json = argc > 1 && JS_IsString(argv[1]) ? JS_ToCStringLen(c, &jl, argv[1]) : NULL;
    int ok = 0;
    if (app && json && strlen(app) < 48 && jl <= 1500) {
        char pl[1600];
        int n = snprintf(pl, sizeof pl, "%s", app) + 1;
        pl[n++] = argc > 2 && JS_ToBool(c, argv[2]) ? '1' : '0';
        pl[n++] = 0;
        memcpy(pl + n, json, jl);
        n += (int)jl;
        int st = BST_ERROR;
        ok = !rpc(BOP_SYS_OPEN, pl, (uint32_t)n, &st, NULL, NULL) && st == BST_OK;
    }
    if (app) JS_FreeCString(c, app);
    if (json) JS_FreeCString(c, json);
    return JS_NewBool(c, ok);
}

static JSValue f_broker_menus(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    size_t len = 0;
    const char *json = JS_ToCStringLen(c, &len, argv[0]);
    int st;
    if (json) rpc(BOP_MENU, json, (uint32_t)len, &st, NULL, NULL);
    if (json) JS_FreeCString(c, json);
    return JS_UNDEFINED;
}
/* lord.broker.textFocus(on, y, h): tells the session a field took or lost focus. A hint only. */
static JSValue f_broker_text_focus(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t y = -1, h = 0;
    int on = argc > 0 && JS_ToBool(c, argv[0]), st;
    if (argc > 1) JS_ToInt32(c, &y, argv[1]);
    if (argc > 2) JS_ToInt32(c, &h, argv[2]);
    char msg[48];
    snprintf(msg, sizeof msg, "%d %d %d", on, y, h);
    rpc(BOP_TEXT_FOCUS, msg, (uint32_t)strlen(msg), &st, NULL, NULL);
    return JS_UNDEFINED;
}
static JSValue f_broker_device(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *kind = JS_ToCString(c, argv[0]);
    int on = JS_ToBool(c, argv[1]);
    char msg[32];
    int st;
    if (kind) { snprintf(msg, sizeof msg, "%s %d", kind, on); rpc(BOP_DEVICE, msg, (uint32_t)strlen(msg), &st, NULL, NULL); }
    if (kind) JS_FreeCString(c, kind);
    return JS_UNDEFINED;
}

/* lord.broker.state() → { theme, locale, keyboard }: el estado actual, al arrancar. El teclado se
 * aplica acá mismo; tema e idioma los aplica el app host. */
static JSValue f_broker_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int st = BST_ERROR; char *out = NULL; uint32_t len = 0;
    if (rpc(BOP_STATE, NULL, 0, &st, &out, &len) || st != BST_OK) { free(out); return JS_NULL; }
    JDoc d;
    if (!json_parse(out, len, &d, NULL, 0)) {
        const JVal *kb = json_get(d.root, "keyboard");
        if (kb && kb->type == J_OBJ) apply_kb_state(kb);
        json_free(&d);
    }
    JSValue r = JS_ParseJSON(c, out, len, "<estado>");
    free(out);
    if (JS_IsException(r)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return r;
}

/* ---- lord.locale (solo lectura): catálogos de LordOS (assets/locales, solo lectura por Landlock)
 * y los de la propia app (locales/<idioma>.json del paquete) ---- */

static int safe_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 40) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}

static char *read_path(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    char *b = malloc(max + 1);
    ssize_t n = b ? read(fd, b, max) : -1;
    close(fd);
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

static JSValue f_locale_catalog(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *lang = JS_ToCString(c, argv[0]), *ns = JS_ToCString(c, argv[1]);
    JSValue r = JS_NULL;
    if (lang && ns && safe_name(lang) && safe_name(ns)) {
        size_t len = 0;
        char *txt = NULL;
        if (!strcmp(ns, app_id)) {                    /* los textos de la propia app */
            char rel[64];
            snprintf(rel, sizeof rel, "locales/%s.json", lang);
            txt = pkg_read(rel, 1 << 20, &len);
        } else {                                      /* paquetes de idioma del usuario primero, como en la sesión */
            char path[PATH_MAX + 128];
            const char *home = getenv("HOME");
            snprintf(path, sizeof path, "%s/.local/share/lordos/locales/%s/%s.json", home ? home : "/nonexistent", lang, ns);
            txt = read_path(path, 1 << 20, &len);
            if (!txt) {
                snprintf(path, sizeof path, "%s/locales/%s/%s.json", js_sys_assets(), lang, ns);
                txt = read_path(path, 1 << 20, &len);
            }
        }
        if (txt) { r = JS_NewStringLen(c, txt, len); free(txt); }
    }
    if (lang) JS_FreeCString(c, lang);
    if (ns) JS_FreeCString(c, ns);
    return r;
}

static JSValue f_locale_languages(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int user = 0; user < 2; user++) {
    char root[PATH_MAX + 64];
    const char *home = getenv("HOME");
    if (user) snprintf(root, sizeof root, "%s/.local/share/lordos/locales", home ? home : "/nonexistent");
    else snprintf(root, sizeof root, "%s/locales", js_sys_assets());
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 200) {
        if (!safe_name(e->d_name)) continue;
        char path[PATH_MAX * 2];
        snprintf(path, sizeof path, "%s/%s/meta.json", root, e->d_name);
        char *txt = read_path(path, 4096, NULL);
        if (!txt) continue;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewString(c, e->d_name));
        JS_SetPropertyStr(c, o, "meta", JS_NewString(c, txt));
        JS_SetPropertyStr(c, o, "user", JS_NewBool(c, user));
        JS_SetPropertyUint32(c, arr, n++, o);
        free(txt);
    }
    if (d) closedir(d);
    }
    return arr;
}

static JSValue f_locale_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c), u = JS_NewObject(c), s = JS_NewObject(c);
    const char *lang = getenv("LANG");
    JS_SetPropertyStr(c, s, "LANG", JS_NewString(c, lang ? lang : ""));
    JS_SetPropertyStr(c, o, "user", u);
    JS_SetPropertyStr(c, o, "system", s);
    return o;
}

/* ---- lord.perms.get (consulta local: la app solo conoce lo suyo como «ask») ---- */
static JSValue f_perm_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv; return JS_NewString(c, "ask");
}
static JSValue f_perm_names(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < PERM_COUNT; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewString(c, perm_name((Perm)i)));
    return a;
}

/* ---- lord.settings: la app no tiene ajustes del sistema ---- */
static JSValue f_settings_load(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; return JS_NULL; }
static JSValue f_settings_save(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; return JS_UNDEFINED; }

/* ---- lord.apps.packageFile / loadPackage: la app lee SUS propios archivos (la carpeta del
 * paquete es de solo lectura por Landlock; no hace falta el broker) ---- */
static char *pkg_read(const char *rel, size_t max, size_t *len) {
    /* usa la carpeta del paquete fijada antes de la caja de arena (no lpk_root(), que haría
     * mkdir y Landlock ya no lo permite) */
    if (!pkg_dir[0] || !lpk_valid_path(rel)) return NULL;
    char path[PATH_MAX + 256];
    snprintf(path, sizeof path, "%s/%.223s", pkg_dir, rel);       /* rel ya validado (< 224) */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    char *buf = malloc(max + 1);
    ssize_t n = buf ? read(fd, buf, max) : -1;
    close(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}
static JSValue f_pkg_file(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *rel = JS_ToCString(c, argv[1]);   /* (id, rel): el id es el propio */
    size_t len = 0;
    char *buf = rel ? pkg_read(rel, 32u * 1024 * 1024, &len) : NULL;
    if (rel) JS_FreeCString(c, rel);
    if (!buf) return js_throw_code(c, "app.noSuchFile", NULL);
    JSValue v = JS_NewArrayBufferCopy(c, (uint8_t *)buf, len);
    free(buf);
    return v;
}

/* lord.loadApp(): evalúa app.js y devuelve globalThis.__lordosApp. */
static JSValue f_load_app(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    size_t len = 0;
    char *src;
    if (pkg_system && code_refused) return js_throw_code(c, "app.codeRefused", NULL);
    if (pkg_system) {
        /* a system app: the same check as the theme (sec/codesig.c), from one descriptor */
        char path[PATH_MAX + 16];
        snprintf(path, sizeof path, "%s/app.js", pkg_dir);
        int before = codesig_refused_count();
        src = codesig_load(path, 16u * 1024 * 1024, &len, NULL, 0);
        if (!src && (codesig_refused_count() > before || codesig_state() == CODE_INVALID)) {
            code_refused = 1;                      /* appclient.c shows the recovery screen */
            return js_throw_code(c, "app.codeRefused", NULL);
        }
    } else src = pkg_read("app.js", 16u * 1024 * 1024, &len);
    if (!src) return js_throw_code(c, "app.loadFailed", NULL);
    JSValue r = JS_Eval(c, src, len, "app.js", JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(r)) return r;
    JS_FreeValue(c, r);
    JSValue g = JS_GetGlobalObject(c), app = JS_GetPropertyStr(c, g, "__lordosApp");
    JS_FreeValue(c, g);
    return app;
}

static const JSCFunctionListEntry fs_funcs[] = {
    JS_CFUNC_DEF("home", 0, f_fs_home), JS_CFUNC_DEF("read", 2, f_fs_read),
    JS_CFUNC_DEF("write", 2, f_fs_write), JS_CFUNC_DEF("list", 1, f_fs_list),
};
static const JSCFunctionListEntry clip_funcs[] = { JS_CFUNC_DEF("get", 0, f_clip_get), JS_CFUNC_DEF("set", 1, f_clip_set) };
static const JSCFunctionListEntry broker_funcs[] = {
    JS_CFUNC_DEF("request", 1, f_broker_request), JS_CFUNC_DEF("device", 2, f_broker_device), JS_CFUNC_DEF("state", 0, f_broker_state),
    JS_CFUNC_DEF("pick", 2, f_broker_pick), JS_CFUNC_DEF("fdRead", 1, f_fd_read), JS_CFUNC_DEF("fdWrite", 2, f_fd_write), JS_CFUNC_DEF("fdClose", 1, f_fd_close), JS_CFUNC_DEF("fdSize", 1, f_fd_size),
    JS_CFUNC_DEF("menus", 1, f_broker_menus), JS_CFUNC_DEF("textFocus", 3, f_broker_text_focus), JS_CFUNC_DEF("openApp", 3, f_broker_open_app),
    JS_CFUNC_DEF("companion", 0, f_broker_companion), JS_CFUNC_DEF("companionSend", 1, f_broker_companion_send), JS_CFUNC_DEF("statusItem", 1, f_broker_status_item),
    JS_CFUNC_DEF("micStart", 0, f_mic_start), JS_CFUNC_DEF("micStop", 0, f_mic_stop), JS_CFUNC_DEF("micLevel", 0, f_mic_level),
};
static const JSCFunctionListEntry locale_funcs[] = {
    JS_CFUNC_DEF("catalog", 2, f_locale_catalog), JS_CFUNC_DEF("languages", 0, f_locale_languages), JS_CFUNC_DEF("get", 0, f_locale_get),
};
static const JSCFunctionListEntry perms_funcs[] = { JS_CFUNC_DEF("get", 2, f_perm_get), JS_CFUNC_DEF("names", 0, f_perm_names) };
static const JSCFunctionListEntry settings_funcs[] = { JS_CFUNC_DEF("load", 0, f_settings_load), JS_CFUNC_DEF("save", 1, f_settings_save) };
static const JSCFunctionListEntry apps_funcs[] = { JS_CFUNC_DEF("packageFile", 2, f_pkg_file) };

static void add_obj(JSContext *c, JSValue p, const char *name, const JSCFunctionListEntry *f, int n) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, f, n);
    JS_SetPropertyStr(c, p, name, o);
}

void js_broker_register(JSContext *c, JSValue lord, const char *id) {
    snprintf(app_id, sizeof app_id, "%s", id);
    broker_connect();
    add_obj(c, lord, "fs", fs_funcs, sizeof fs_funcs / sizeof fs_funcs[0]);
    add_obj(c, lord, "clipboard", clip_funcs, sizeof clip_funcs / sizeof clip_funcs[0]);
    add_obj(c, lord, "broker", broker_funcs, sizeof broker_funcs / sizeof broker_funcs[0]);
    add_obj(c, lord, "perms", perms_funcs, sizeof perms_funcs / sizeof perms_funcs[0]);
    add_obj(c, lord, "settings", settings_funcs, sizeof settings_funcs / sizeof settings_funcs[0]);
    add_obj(c, lord, "apps", apps_funcs, sizeof apps_funcs / sizeof apps_funcs[0]);
    add_obj(c, lord, "locale", locale_funcs, sizeof locale_funcs / sizeof locale_funcs[0]);
    JS_SetPropertyStr(c, lord, "appId", JS_NewString(c, id));
    JS_SetPropertyStr(c, lord, "loadApp", JS_NewCFunction(c, f_load_app, "loadApp", 0));
    app_windows_register(c, lord);           /* several windows per isolated app */
    js_brokercam_register(c, lord);          /* lord.media: the camera through lord-server (js_brokercam.c) */
    js_brokermail_register(c, lord, id);     /* lord.mail: system Mail, Calendar, Contacts, Reminders (js_brokermail.c) */
}
