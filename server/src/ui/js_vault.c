/* js_vault.c — lord.vault: Passwords and the keyring through the confined helper axia-vault (see
 * src/vault/vclient.c and docs/PASSWORDS.md).
 *
 *   lord.vault.available()                     → bool
 *   lord.vault.request(json, { files })        → id; files: [{ path, mode: 'r' | 'w' }]
 *       'r' = an existing file in the home folder or on one of the user's disks (realpath + roots);
 *       'w' = a NEW file (O_EXCL) in a valid lord.fs destination. axia-server opens them and
 *       passes only the fds to axia-vault, which cannot open paths.
 *   lord.vault.poll()                          → JSON text [{ id, kind: 'rep'|'evt', data }] or null
 *   lord.vault.pending()                       → requests without a reply
 *   lord.vault.lock(why)                       → lock the vault (screen lock, user switch)
 *   lord.vault.fidoDevices()                   → [{ path, name, vid, pid }] (without opening them)
 *   lord.vault.fidoAttach(app)                 → { ok, name } — only with the app's usb permission:
 *       axia-server opens /dev/hidrawN and passes the fd to axia-vault; the app never sees it.
 *   lord.vault.promptDone(prompt, ok)          → answer to Secret Service's unlock prompt
 * Everything is validated here; none of this runs programs or opens caller paths without validating them. */
#include "js_vault.h"
#include "js_sys.h"
#include "js_err.h"
#include "../fsops.h"
#include "../sec/perms.h"
#include "../vault/vclient.h"
#include "../vault/fidodev.h"
#include "../vault/vwire.h"
#include "js_broker.h"
#include "../sec/broker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_read(const char *path) {
    char full[PATH_MAX * 2], real[PATH_MAX];
    const char *home = getenv("HOME");
    if (!path || !path[0]) return -1;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else if (home) snprintf(full, sizeof full, "%s/%s", home, path);
    else return -1;
    if (!realpath(full, real) || !fsops_allowed(real)) return -1;
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

static int open_write(const char *path) {
    char target[PATH_MAX + 300];
    if (!path || js_sys_resolve_target(path, target, sizeof target)) return -1;
    return open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, vc_available());
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsString(argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    size_t len;
    const char *j = JS_ToCStringLen(c, &len, argv[0]);
    if (!j) return JS_EXCEPTION;
    if (len > (32u << 20)) { JS_FreeCString(c, j); return js_throw_code(c, "vault.requestTooLarge", NULL); }
    int fds[4], nf = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue files = JS_GetPropertyStr(c, argv[1], "files");
        if (JS_IsArray(c, files)) {
            JSValue lv = JS_GetPropertyStr(c, files, "length");
            int32_t n = 0;
            JS_ToInt32(c, &n, lv);
            JS_FreeValue(c, lv);
            for (int i = 0; i < n && i < 4; i++) {
                JSValue f = JS_GetPropertyUint32(c, files, (uint32_t)i);
                JSValue pv = JS_GetPropertyStr(c, f, "path"), mv = JS_GetPropertyStr(c, f, "mode");
                const char *p = JS_IsString(pv) ? JS_ToCString(c, pv) : NULL;
                const char *m = JS_IsString(mv) ? JS_ToCString(c, mv) : NULL;
                int fd = !p ? -1 : (m && !strcmp(m, "w")) ? open_write(p) : open_read(p);
                int err = errno;
                if (p) JS_FreeCString(c, p);
                if (m) JS_FreeCString(c, m);
                JS_FreeValue(c, pv); JS_FreeValue(c, mv); JS_FreeValue(c, f);
                if (fd < 0) {
                    for (int k = 0; k < nf; k++) close(fds[k]);
                    JS_FreeValue(c, files);
                    JS_FreeCString(c, j);
                    return js_throw_code(c, err == EEXIST ? "fs.exists" : "fs.outsideHome", NULL);
                }
                fds[nf++] = fd;
            }
        }
        JS_FreeValue(c, files);
    }
    int id = vc_request(j, len, fds, nf);
    JS_FreeCString(c, j);
    if (id < 0) return js_throw_code(c, "vault.unavailable", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *s = vc_poll();
    if (!s) return JS_NULL;
    JSValue r = JS_NewString(c, s);
    vc_poll_free(s);
    return r;
}

static JSValue f_pending(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, vc_pending());
}

static JSValue f_lock(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *w = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    vc_lock_all(w ? w : "screen");
    if (w) JS_FreeCString(c, w);
    return JS_UNDEFINED;
}

static JSValue f_fido_devices(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    FidoDev d[8];
    int n = fidodev_list(d, 8);
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "path", JS_NewString(c, d[i].path));
        JS_SetPropertyStr(c, o, "name", JS_NewString(c, d[i].name));
        JS_SetPropertyStr(c, o, "vid", JS_NewInt32(c, (int)d[i].vid));
        JS_SetPropertyStr(c, o, "pid", JS_NewInt32(c, (int)d[i].pid));
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static JSValue f_fido_attach(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    int ok = app && perm_get(app, PERM_USB) == PERM_ALLOW;
    if (app) JS_FreeCString(c, app);
    if (!ok) return js_throw_code(c, "perm.usbDenied", NULL);
    char name[128] = "";
    int r = vc_fido_attach(name, sizeof name);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, r == 0));
    if (r) JS_SetPropertyStr(c, o, "error", JS_NewString(c, r == -1 ? "no-device" : "no-access"));
    else JS_SetPropertyStr(c, o, "name", JS_NewString(c, name));
    return o;
}

static JSValue f_prompt_done(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    uint32_t p = 0;
    if (argc > 0) JS_ToUint32(c, &p, argv[0]);
    vc_prompt_done(p, argc > 1 && JS_ToBool(c, argv[1]));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry vault_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("request", 2, f_request),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("pending", 0, f_pending),
    JS_CFUNC_DEF("lock", 1, f_lock),
    JS_CFUNC_DEF("fidoDevices", 0, f_fido_devices),
    JS_CFUNC_DEF("fidoAttach", 1, f_fido_attach),
    JS_CFUNC_DEF("promptDone", 2, f_prompt_done),
};

void js_vault_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, vault_funcs, sizeof vault_funcs / sizeof vault_funcs[0]);
    JS_SetPropertyStr(c, lord, "vault", o);
    vc_start();               /* the keyring and Secret Service run from the start of the session */
}

/* ================= app mode =================
 * The Passwords app runs as its own sandboxed process (axia-server --app passwords). axia-server
 * gave it a channel of its own to axia-vault (vc_attach_app, env LORD_VAULT_FD): requests and
 * replies, including revealed secrets, travel between the two sandboxed processes only; axia-server
 * never sees them. Same lord.vault API as in the shell, except that files are fds the user picked
 * in the system dialog (powerbox): request(json, { fds: [fd…] }). */

static int app_fd = -1;
static uint32_t app_next = 1;
static int app_inflight;
static VwRx app_rx;

static JSValue fa_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, app_fd >= 0);
}

static JSValue fa_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (app_fd < 0) return js_throw_code(c, "vault.unavailable", NULL);
    if (argc < 1 || !JS_IsString(argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    size_t len;
    const char *j = JS_ToCStringLen(c, &len, argv[0]);
    if (!j) return JS_EXCEPTION;
    int fds[VW_MAX_FDS], nf = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue arr = JS_GetPropertyStr(c, argv[1], "fds");
        if (JS_IsArray(c, arr)) {
            JSValue lv = JS_GetPropertyStr(c, arr, "length");
            int32_t n = 0;
            JS_ToInt32(c, &n, lv);
            JS_FreeValue(c, lv);
            for (int i = 0; i < n && nf < VW_MAX_FDS; i++) {
                JSValue v = JS_GetPropertyUint32(c, arr, (uint32_t)i);
                int32_t fd = -1;
                JS_ToInt32(c, &fd, v);
                JS_FreeValue(c, v);
                if (fd > 2) fds[nf++] = fd;
            }
        }
        JS_FreeValue(c, arr);
    }
    uint32_t id = app_next++;
    if (app_next > 0x7fffffff) app_next = 1;
    int r = vw_send(app_fd, VW_REQ, id, j, len, fds, nf);
    JS_FreeCString(c, j);
    for (int i = 0; i < nf; i++) close(fds[i]);        /* the powerbox fds are spent */
    if (r) return js_throw_code(c, "vault.unavailable", NULL);
    app_inflight++;
    return JS_NewInt32(c, (int32_t)id);
}

static JSValue fa_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (app_fd < 0) return JS_NULL;
    size_t cap = 0, o = 0;
    char *out = NULL;
    for (;;) {
        VMsg m;
        int r = vw_feed(app_fd, &app_rx, &m, &vw_malloc);
        if (r == 0) break;
        if (r < 0) {                                     /* axia-vault went away */
            close(app_fd);
            app_fd = -1;
            static const char crash[] = "{\"id\":0,\"kind\":\"evt\",\"data\":{\"ev\":\"crash\"}}";
            size_t need = o + sizeof crash + 4;
            char *nb = realloc(out, need);
            if (nb) { out = nb; cap = need; o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "," : "[", crash); }
            break;
        }
        for (int i = 0; i < m.nfds; i++) close(m.fds[i]);
        if (m.kind == VW_REP && app_inflight > 0) app_inflight--;
        size_t need = o + m.len + 80;
        if (need > cap) {
            size_t nc = cap ? cap * 2 : 4096;
            while (nc < need) nc *= 2;
            char *nb = malloc(nc);
            if (!nb) { vw_msg_free(&m, &vw_malloc); break; }
            if (out) { memcpy(nb, out, o); explicit_bzero(out, cap); free(out); }
            out = nb;
            cap = nc;
        }
        o += (size_t)snprintf(out + o, cap - o, "%s{\"id\":%u,\"kind\":\"%s\",\"data\":", o ? "," : "[", m.id, m.kind == VW_EVT ? "evt" : "rep");
        if (m.len) { memcpy(out + o, m.json, m.len); o += m.len; } else { memcpy(out + o, "{}", 2); o += 2; }
        out[o++] = '}';
        vw_msg_free(&m, &vw_malloc);                     /* wipes the text */
    }
    if (!out) return JS_NULL;
    out[o++] = ']';
    JSValue v = JS_NewStringLen(c, out, o);
    explicit_bzero(out, cap);
    free(out);
    return v;
}

static JSValue fa_pending(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, app_inflight);
}

static JSValue fa_lock(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    if (app_fd >= 0 && !vw_send(app_fd, VW_REQ, app_next++, "{\"op\":\"lock\"}", 13, NULL, 0)) app_inflight++;
    return JS_UNDEFINED;
}

static JSValue fa_fido_attach(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char name[128] = "", err[64] = "";
    int r = js_broker_vault_fido(name, sizeof name, err, sizeof err);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, r == 0));
    if (r) JS_SetPropertyStr(c, o, "error", JS_NewString(c, err));
    else JS_SetPropertyStr(c, o, "name", JS_NewString(c, name));
    return o;
}

static JSValue fa_noop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; return JS_UNDEFINED; }

static const JSCFunctionListEntry vault_app_funcs[] = {
    JS_CFUNC_DEF("available", 0, fa_available),
    JS_CFUNC_DEF("request", 2, fa_request),
    JS_CFUNC_DEF("poll", 0, fa_poll),
    JS_CFUNC_DEF("pending", 0, fa_pending),
    JS_CFUNC_DEF("lock", 1, fa_lock),
    JS_CFUNC_DEF("fidoAttach", 1, fa_fido_attach),
    JS_CFUNC_DEF("promptDone", 2, fa_noop),
};

/* ---- lord.keyring of an isolated .lpk app (any app without its own vault channel) ----
 *   lord.keyring.request(op, service, account, label, secret) → JSON text of axia-vault's reply
 *       op: 'get' | 'set' | 'delete' | 'list'. It goes through the broker (BOP_KEYRING); axia-server
 *       sets the owner from the verified app id, so the app only reaches its own items (or those
 *       the user agreed to share with it). Errors are thrown as codes (keyring.*, vault.unavailable).
 * The reply buffer may hold a secret: it is wiped once it is a JS string. */

static JSValue fk_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *v[5] = { 0 };
    size_t l[5] = { 0 }, total = 0;
    for (int i = 0; i < 5; i++) {
        if (i < argc && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i])) {
            v[i] = JS_ToCStringLen(c, &l[i], argv[i]);
            if (!v[i]) { for (int k = 0; k < i; k++) if (v[k]) JS_FreeCString(c, v[k]); return JS_EXCEPTION; }
        }
        total += l[i] + 1;
    }
    char *p = total <= BROKER_MAX_MSG ? malloc(total) : NULL;
    size_t o = 0;
    for (int i = 0; i < 5 && p; i++) {
        if (l[i] && memchr(v[i], 0, l[i])) { explicit_bzero(p, total); free(p); p = NULL; break; }   /* no NUL inside a field */
        if (l[i]) memcpy(p + o, v[i], l[i]);
        o += l[i];
        if (i < 4) p[o++] = 0;
    }
    for (int i = 0; i < 5; i++) if (v[i]) JS_FreeCString(c, v[i]);
    if (!p) return js_throw_code(c, "keyring.badRequest", NULL);
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t ol = 0;
    int r = js_broker_rpc(BOP_KEYRING, p, (uint32_t)o, &st, &out, &ol);
    explicit_bzero(p, total);
    free(p);
    if (r) { free(out); return js_throw_code(c, "vault.unavailable", NULL); }
    JSValue res;
    if (st == BST_OK) res = JS_NewStringLen(c, out ? out : "{}", out ? ol : 2);
    else {
        char line[128];
        snprintf(line, sizeof line, "%.*s", (int)(out && ol < sizeof line ? ol : 0), out ? out : "");
        res = js_throw_line(c, lerr_is(line) ? line : "keyring.failed");
    }
    if (out) { explicit_bzero(out, ol); free(out); }
    return res;
}

static const JSCFunctionListEntry keyring_app_funcs[] = { JS_CFUNC_DEF("request", 5, fk_request) };

void js_vault_register_app(JSContext *c, JSValue lord) {
    const char *s = getenv("LORD_VAULT_FD");
    if (!s) {                                            /* a .lpk app: its own keyring items only */
        JSValue k = JS_NewObject(c);
        JS_SetPropertyFunctionList(c, k, keyring_app_funcs, sizeof keyring_app_funcs / sizeof keyring_app_funcs[0]);
        JS_SetPropertyStr(c, lord, "keyring", k);
        return;
    }
    app_fd = atoi(s);
    if (app_fd <= 2) { app_fd = -1; return; }
    fcntl(app_fd, F_SETFD, FD_CLOEXEC);
    unsetenv("LORD_VAULT_FD");
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, vault_app_funcs, sizeof vault_app_funcs / sizeof vault_app_funcs[0]);
    JS_SetPropertyStr(c, o, "appMode", JS_TRUE);
    JS_SetPropertyStr(c, lord, "vault", o);
}
