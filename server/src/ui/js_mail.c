/* js_mail.c — lord.mail: Mail, Calendar, Contacts and Reminders through the sandboxed helper
 * axia-maild (server/src/mail, docs/MAIL.md). Here we only validate arguments, open the files
 * the user picked and write the accounts file; IMAP, SMTP, CalDAV, CardDAV, MIME and HTML are
 * handled in axia-maild. Everything is asynchronous: request() returns an id and results arrive
 * through poll().
 *
 *   available()                    is axia-maild installed?
 *   start()                        starts it (it stays up for new-mail notifications)
 *   request(json, [paths])         → id; paths: files to attach or a contact photo (home or media only)
 *   poll()                         → [{id, type: done|err|evt|crash, data}] or null
 *   pending()
 *   config()                       → the accounts file text ("" if none). Never holds passwords.
 *   saveConfig(json)               writes ~/.config/axia/mail/accounts.json (validated JSON,
 *                                  ≤ 512 KB, 0600, atomic) and reloads the allowed servers
 *   allowImages([urls])            lets axia-maild reach these image hosts for 10 minutes
 *   bind(node, image) / release(image)
 *   cacheDir()                     ~/.local/share/axia/mail (attachments saved for opening)
 */
#include "js_mail.h"
#include "js_sys.h"
#include "js_err.h"
#include "ui.h"
#include "../mail/mailc.h"
#include "../mail/proto.h"
#include "../sec/json.h"
#include "../fsops.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAIL_REQ_MAX (24u << 20)

static const char *arg_str(JSContext *c, int argc, JSValueConst *argv, int i, size_t *len) {
    if (i >= argc || !JS_IsString(argv[i])) return NULL;
    return JS_ToCStringLen(c, len, argv[i]);
}

static int arg_int(JSContext *c, int argc, JSValueConst *argv, int i, int def) {
    int32_t v = def;
    if (i < argc && JS_IsNumber(argv[i])) JS_ToInt32(c, &v, argv[i]);
    return v;
}

/* A file the user picked: relative to home or absolute, real, inside the allowed roots. */
static int open_user_file(const char *path) {
    char full[PATH_MAX * 2], real[PATH_MAX];
    const char *h = getenv("HOME");
    if (!path || !path[0]) return -1;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else if (h) snprintf(full, sizeof full, "%s/%s", h, path);
    else return -1;
    if (!realpath(full, real) || !fsops_allowed(real)) return -1;
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)(64 << 20)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, mail_available() == 0);
}

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, mail_start() == 0);
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = arg_str(c, argc, argv, 0, &len);
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    if (len > MAIL_REQ_MAX) { JS_FreeCString(c, j); return js_throw_code(c, "mail.requestTooLarge", NULL); }
    /* must be a JSON object with a short "op" */
    JDoc d;
    if (json_parse(j, len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ || !json_str(d.root, "op") || strlen(json_str(d.root, "op")) > 32) {
        if (d.root) json_free(&d);
        JS_FreeCString(c, j);
        return js_throw_code(c, "sys.badArgument", NULL);
    }
    json_free(&d);
    int fds[15], nfds = 0;
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
        int32_t n = 0;
        JS_ToInt32(c, &n, lv);
        JS_FreeValue(c, lv);
        if (n < 0 || n > 15) { JS_FreeCString(c, j); return js_throw_code(c, "mail.tooManyFiles", "max", "15", NULL); }
        for (int i = 0; i < n; i++) {
            JSValue pv = JS_GetPropertyUint32(c, argv[1], (uint32_t)i);
            const char *p = JS_IsString(pv) ? JS_ToCString(c, pv) : NULL;
            JS_FreeValue(c, pv);
            int fd = open_user_file(p);
            if (p) JS_FreeCString(c, p);
            if (fd < 0) {
                for (int k = 0; k < nfds; k++) close(fds[k]);
                JS_FreeCString(c, j);
                return js_throw_code(c, "fs.outsideHome", NULL);
            }
            fds[nfds++] = fd;
        }
    }
    int id = mail_request(j, len, fds, nfds);
    JS_FreeCString(c, j);
    return JS_NewInt32(c, id);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *s = mail_poll();
    if (!s) return JS_NULL;
    JSValue v = JS_ParseJSON(c, s, strlen(s), "<mail>");
    free(s);
    return v;
}

static JSValue f_pending(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, mail_pending());
}

static JSValue f_config(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int fd = open(mail_config_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NewString(c, "");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MAIL_CONFIG_MAX) { close(fd); return JS_NewString(c, ""); }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r < 0) { free(b); return JS_NewString(c, ""); }
    JSValue v = JS_NewStringLen(c, b, (size_t)r);
    free(b);
    return v;
}

/* The accounts file holds security decisions (which servers axia-maild may reach, trusted
 * certificate fingerprints): JS cannot write it through lord.fs (fsops_protected), only here and,
 * for the isolated Mail app, through the broker (sec/brokermail.c). Both use mail_save_config. */
static JSValue f_save_config(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = arg_str(c, argc, argv, 0, &len);
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int r = mail_save_config(j, len);
    JS_FreeCString(c, j);
    if (r == 1) return js_throw_code(c, "mail.badAccounts", NULL);
    if (r) return js_throw_code(c, "sys.writeFailed", "path", mail_config_path(), NULL);
    return JS_TRUE;
}

static JSValue f_allow_images(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsArray(c, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    int32_t n = 0;
    JS_ToInt32(c, &n, lv);
    JS_FreeValue(c, lv);
    if (n < 0) n = 0;
    if (n > 200) n = 200;
    const char *urls[200];
    int k = 0;
    for (int i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(c, argv[0], (uint32_t)i);
        if (JS_IsString(v)) { const char *s = JS_ToCString(c, v); if (s) { if (strlen(s) < 2048) urls[k++] = s; else JS_FreeCString(c, s); } }
        JS_FreeValue(c, v);
    }
    mail_allow_hosts(urls, k, 600);
    for (int i = 0; i < k; i++) JS_FreeCString(c, urls[i]);
    return JS_UNDEFINED;
}

static JSValue f_bind(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Node *n = node_get(arg_int(c, argc, argv, 0, -1));
    if (!n || n->type != NODE_IMAGE) return JS_FALSE;
    int handle = arg_int(c, argc, argv, 1, 0);
    if (handle <= 0) {
        image_release(n->img);
        n->img = NULL;
        node_invalidate(n);
        return JS_TRUE;
    }
    int w, h;
    const uint32_t *px = mail_image_lock(handle, &w, &h);
    if (!px) { mail_image_unlock(); return JS_FALSE; }
    Image *img = image_argb(n->img, w, h, px);
    mail_image_unlock();
    if (!img) return JS_FALSE;
    n->img = img;
    node_invalidate(n);
    return JS_TRUE;
}

static JSValue f_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    mail_image_release(arg_int(c, argc, argv, 0, 0));
    return JS_UNDEFINED;
}

static JSValue f_cache_dir(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewString(c, mail_cache_path());
}

static const JSCFunctionListEntry mail_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("start", 0, f_start),
    JS_CFUNC_DEF("request", 2, f_request),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("pending", 0, f_pending),
    JS_CFUNC_DEF("config", 0, f_config),
    JS_CFUNC_DEF("saveConfig", 1, f_save_config),
    JS_CFUNC_DEF("allowImages", 1, f_allow_images),
    JS_CFUNC_DEF("bind", 2, f_bind),
    JS_CFUNC_DEF("release", 1, f_release),
    JS_CFUNC_DEF("cacheDir", 0, f_cache_dir),
};

void js_mail_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, mail_funcs, sizeof mail_funcs / sizeof mail_funcs[0]);
    JS_SetPropertyStr(c, lord, "mail", o);
}
