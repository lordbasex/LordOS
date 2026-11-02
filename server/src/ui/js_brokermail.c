/* js_brokermail.c — lord.mail inside the process of the isolated system Mail, Calendar, Contacts
 * and Reminders (`axia-server --app <id>`). The same API as the session's lord.mail (js_mail.c), so
 * sdk/mail.js, sdk/calendar.js and sdk/contacts.js work unchanged, but every call goes over the
 * broker (BOP_MAIL, sec/brokermail.c) and axia-server checks it:
 *
 *   request(json, [fds])   → id; files are fds the app holds (from the system file dialog)
 *   poll() / pending()     results and events, which axia-server pushes as "mail" events in chunks
 *   available() / start()
 *   config() / saveConfig(json) / allowImages([urls])        Mail only
 *   bind(node, image) / release(image)   axia-server hands a sealed memfd with the pixels
 *   attachmentFd(rel)      → read-only fd of a cached attachment (Mail; to save it), or -1
 *   openAttachment(rel, name)   the session opens it with the user's apps (Mail)
 *   cacheDir()             "" (this process cannot read the cache)
 *   appMode                true
 *
 * The process has no network and no access to ~/.local/share/axia/mail or the accounts file. */
#include "js_broker.h"
#include "js_err.h"
#include "ui.h"
#include "../sec/broker.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIG_REQ (120 * 1024)       /* larger requests go in a memfd ("reqm") */

/* the batch being received ('C' chunks until 'F') and the complete items waiting for poll() */
static char *part, *ready;
static size_t part_n, part_cap, ready_n, ready_cap;

static int append(char **b, size_t *n, size_t *cap, const char *s, size_t len) {
    if (*n + len + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096;
        while (nc < *n + len + 1) nc *= 2;
        if (nc > (64u << 20)) return -1;
        char *nb = realloc(*b, nc);
        if (!nb) return -1;
        *b = nb;
        *cap = nc;
    }
    memcpy(*b + *n, s, len);
    *n += len;
    (*b)[*n] = 0;
    return 0;
}

void js_brokermail_chunk(const char *data, size_t len) {
    if (!len || (data[0] != 'C' && data[0] != 'F')) return;
    if (append(&part, &part_n, &part_cap, data + 1, len - 1)) { part_n = 0; return; }
    if (data[0] == 'C') return;
    /* a whole batch: "[item,item…]" → the items join the ready list */
    if (part_n >= 2 && part[0] == '[' && part[part_n - 1] == ']' && part_n > 2) {
        if (ready_n) append(&ready, &ready_n, &ready_cap, ",", 1);
        append(&ready, &ready_n, &ready_cap, part + 1, part_n - 2);
    }
    part_n = 0;
}

static int verb_rpc(const char *verb, const char *rest, size_t rl, const int *fds, int nfds, int *st, char **out, uint32_t *ol) {
    size_t vl = strlen(verb);
    char *p = malloc(vl + 1 + rl + 1);
    if (!p) return -1;
    memcpy(p, verb, vl + 1);
    if (rl) memcpy(p + vl + 1, rest, rl);
    p[vl + 1 + rl] = 0;
    int r = js_broker_rpc_fds(BOP_MAIL, p, (uint32_t)(vl + 1 + rl), fds, nfds, st, out, ol);
    free(p);
    return r;
}

static int simple(const char *verb, const char *rest) {
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t ol = 0;
    int r = verb_rpc(verb, rest, rest ? strlen(rest) : 0, NULL, 0, &st, &out, &ol);
    int ok = !r && st == BST_OK && out && ol && out[0] == '1';
    free(out);
    return ok;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewBool(c, simple("available", NULL)); }
static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewBool(c, simple("start", NULL)); }

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int fds[16], nfds = 0;
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
        int32_t n = 0;
        JS_ToInt32(c, &n, lv);
        JS_FreeValue(c, lv);
        if (n < 0 || n > 15) { JS_FreeCString(c, j); return js_throw_code(c, "mail.tooManyFiles", "max", "15", NULL); }
        for (int i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[1], (uint32_t)i);
            int32_t fd = -1;
            if (JS_IsNumber(v)) JS_ToInt32(c, &fd, v);
            JS_FreeValue(c, v);
            if (fd < 0) { JS_FreeCString(c, j); return js_throw_code(c, "fs.outsideHome", NULL); }   /* paths mean nothing here */
            fds[nfds++] = fd;
        }
    }
    int st = BST_ERROR, r;
    char *out = NULL;
    uint32_t ol = 0;
    if (len > BIG_REQ) {
        int m = memfd_create("lordos-mail-req", MFD_CLOEXEC);
        size_t w = 0;
        while (m >= 0 && w < len) { ssize_t k = write(m, j + w, len - w); if (k <= 0) break; w += (size_t)k; }
        if (m < 0 || w != len) { if (m >= 0) close(m); JS_FreeCString(c, j); return js_throw_code(c, "mail.requestTooLarge", NULL); }
        int all[16];
        all[0] = m;
        memcpy(all + 1, fds, (size_t)nfds * sizeof(int));
        r = verb_rpc("reqm", NULL, 0, all, nfds + 1, &st, &out, &ol);
        close(m);
    } else r = verb_rpc("req", j, len, fds, nfds, &st, &out, &ol);
    JS_FreeCString(c, j);
    int id = !r && st == BST_OK && out ? atoi(out) : -1;
    JSValue err = JS_UNDEFINED;
    if (!r && st != BST_OK) err = out && ol && lerr_is(out) ? js_throw_line(c, out) : js_throw_code(c, st == BST_DENIED ? "mail.denied" : "sys.badArgument", NULL);
    free(out);
    if (!JS_IsUndefined(err)) return err;
    return JS_NewInt32(c, id);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    if (!ready_n) return JS_NULL;
    char *s = malloc(ready_n + 3);
    if (!s) return JS_NULL;
    s[0] = '[';
    memcpy(s + 1, ready, ready_n);
    s[ready_n + 1] = ']';
    s[ready_n + 2] = 0;
    size_t n = ready_n + 2;
    ready_n = 0;
    JSValue v = JS_ParseJSON(c, s, n, "<mail>");
    free(s);
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return v;
}

static JSValue f_pending(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewInt32(c, 0); }

static JSValue f_config(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t ol = 0;
    int r = verb_rpc("config", NULL, 0, NULL, 0, &st, &out, &ol);
    JSValue v = !r && st == BST_OK && out ? JS_NewStringLen(c, out, ol) : JS_NewString(c, "");
    free(out);
    return v;
}

static JSValue f_save_config(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t ol = 0;
    int r = verb_rpc("save", j, len, NULL, 0, &st, &out, &ol);
    JS_FreeCString(c, j);
    JSValue v = JS_TRUE;
    if (r || st != BST_OK) v = out && ol && lerr_is(out) ? js_throw_line(c, out) : js_throw_code(c, "mail.badAccounts", NULL);
    free(out);
    return v;
}

static JSValue f_allow_images(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsArray(c, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    int32_t n = 0;
    JS_ToInt32(c, &n, lv);
    JS_FreeValue(c, lv);
    if (n > 200) n = 200;
    char *buf = NULL;
    size_t bn = 0, bc = 0;
    for (int i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(c, argv[0], (uint32_t)i);
        if (JS_IsString(v)) {
            size_t l = 0;
            const char *s = JS_ToCStringLen(c, &l, v);
            if (s && l && l < 2048 && !memchr(s, 0, l)) { append(&buf, &bn, &bc, s, l); append(&buf, &bn, &bc, "", 1); }
            if (s) JS_FreeCString(c, s);
        }
        JS_FreeValue(c, v);
    }
    int st;
    if (bn) verb_rpc("images", buf, bn, NULL, 0, &st, NULL, NULL);
    free(buf);
    return JS_UNDEFINED;
}

/* lord.mail.bind(node, image): the pixels come in a sealed memfd from axia-server */
static JSValue f_bind(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t nid = -1, handle = 0;
    if (argc > 0) JS_ToInt32(c, &nid, argv[0]);
    if (argc > 1) JS_ToInt32(c, &handle, argv[1]);
    Node *n = node_get(nid);
    if (!n || n->type != NODE_IMAGE) return JS_FALSE;
    if (handle <= 0) { image_release(n->img); n->img = NULL; node_invalidate(n); return JS_TRUE; }
    char req[24], info[64] = "";
    int k = snprintf(req, sizeof req, "img%c%d", 0, (int)handle);
    int fd = js_broker_rpc_fd(BOP_MAIL, req, (uint32_t)k, info, sizeof info);
    if (fd < 0) return JS_FALSE;
    int w = 0, h = 0;
    sscanf(info, "%d %d", &w, &h);
    JSValue ok = JS_FALSE;
    off_t size = lseek(fd, 0, SEEK_END);
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192 && size >= (off_t)w * h * 4) {
        void *px = mmap(NULL, (size_t)w * h * 4, PROT_READ, MAP_SHARED, fd, 0);
        if (px != MAP_FAILED) {
            Image *img = image_argb(n->img, w, h, px);
            munmap(px, (size_t)w * h * 4);
            if (img) { n->img = img; node_invalidate(n); ok = JS_TRUE; }
        }
    }
    close(fd);
    return ok;
}

static JSValue f_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t handle = 0;
    if (argc > 0) JS_ToInt32(c, &handle, argv[0]);
    char req[24];
    int k = snprintf(req, sizeof req, "release%c%d", 0, (int)handle);
    if (handle > 0) js_broker_send(BOP_MAIL, req, (uint32_t)k);   /* its reply is dropped by js_broker_poll */
    return JS_UNDEFINED;
}

static JSValue f_cache_dir(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewString(c, ""); }

static JSValue f_attachment_fd(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *rel = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    if (!rel) return JS_NewInt32(c, -1);
    char req[1100];
    int k = snprintf(req, sizeof req, "att%c%s", 0, rel);
    JS_FreeCString(c, rel);
    if (k <= 0 || (size_t)k >= sizeof req) return JS_NewInt32(c, -1);
    return JS_NewInt32(c, js_broker_rpc_fd(BOP_MAIL, req, (uint32_t)k, NULL, 0));
}

static JSValue f_open_attachment(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *rel = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    const char *name = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    int ok = 0;
    if (rel) {
        char req[1400];
        int k = snprintf(req, sizeof req, "open%c%s%c%.255s", 0, rel, 0, name ? name : "");
        int st = BST_ERROR;
        if (k > 0 && (size_t)k < sizeof req) ok = !js_broker_rpc(BOP_MAIL, req, (uint32_t)k, &st, NULL, NULL) && st == BST_OK;
    }
    if (rel) JS_FreeCString(c, rel);
    if (name) JS_FreeCString(c, name);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry mail_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available), JS_CFUNC_DEF("start", 0, f_start),
    JS_CFUNC_DEF("request", 2, f_request), JS_CFUNC_DEF("poll", 0, f_poll), JS_CFUNC_DEF("pending", 0, f_pending),
    JS_CFUNC_DEF("config", 0, f_config), JS_CFUNC_DEF("saveConfig", 1, f_save_config),
    JS_CFUNC_DEF("allowImages", 1, f_allow_images), JS_CFUNC_DEF("bind", 2, f_bind), JS_CFUNC_DEF("release", 1, f_release),
    JS_CFUNC_DEF("cacheDir", 0, f_cache_dir), JS_CFUNC_DEF("attachmentFd", 1, f_attachment_fd),
    JS_CFUNC_DEF("openAttachment", 2, f_open_attachment),
};

void js_brokermail_register(JSContext *c, JSValue lord, const char *app_id) {
    /* only the system apps that use axia-maild (axia-server refuses everyone else anyway) */
    if (!app_id || (strcmp(app_id, "mail") && strcmp(app_id, "calendar") && strcmp(app_id, "contacts") && strcmp(app_id, "reminders"))) return;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, mail_funcs, sizeof mail_funcs / sizeof mail_funcs[0]);
    JS_SetPropertyStr(c, o, "appMode", JS_TRUE);
    JS_SetPropertyStr(c, lord, "mail", o);
}
