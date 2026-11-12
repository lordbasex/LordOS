/* js_doc.c — lord.doc: PDF, archives, fonts and images through the confined lord-doc helper
 * (server/src/doc, see docs/DOCUMENTS.md). Here we only validate paths and arguments and open
 * the files; neither Poppler nor libarchive is loaded into lord-server. Everything is asynchronous: each
 * function returns ids and the results arrive with lord.doc.poll().
 *
 *   available()                              is the helper there?
 *   openPdf(path) / openBlob(blob)           → sid (session with the PDF; then request(sid, 'open'))
 *   request(sid, op, json, prio, timeoutMs)  → request id (op: open render select find outline
 *                                              pageinfo setfield annot undo blob list)
 *   savePdf(sid, path)                       → id (new copy, never overwrites)
 *   arcList(path | [volumes])                → { sid, req } (a split archive: every volume, in order)
 *   arcExtract(path | [volumes], folder, json, exists) → { sid, req } (new folder unless exists=true)
 *   arcCreate([paths], dest.zip)             → { sid, req }
 *   util(op, json, path?, output?)           → id (fontinfo fontrender fontgrid maketext makeimage
 *                                              decode convert), in a utilities session
 *   cancel(sid, req), close(sid), poll(), pending()
 *   bind(node, image) / release(image)       a returned image in an <Image>
 *   blobRelease(blob), blobSave(blob, path)
 */
#include "js_doc.h"
#include "js_sys.h"
#include "js_err.h"
#include "ui.h"
#include "../doc/doc.h"
#include "../doc/proto.h"
#include "../doc/wire.h"
#include "../fsops.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int util_sid;

/* ---- rutas ---- */

static int home_real(char *out) {
    const char *h = getenv("HOME");
    return h && realpath(h, out) ? 0 : -1;
}

/* Read path: relative to the home folder or absolute, real and inside the roots. */
static int resolve_read(const char *path, char *out) {
    char full[PATH_MAX * 2], home[PATH_MAX];
    if (!path || !path[0]) return -1;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else if (!home_real(home)) snprintf(full, sizeof full, "%s/%s", home, path);
    else return -1;
    if (!realpath(full, out)) return -1;
    return fsops_allowed(out) ? 0 : -1;
}

/* Besides the home folder: the system fonts (Fonts shows them). */
static int resolve_font(const char *path, char *out) {
    if (!resolve_read(path, out)) return 0;
    char full[PATH_MAX];
    if (!path || path[0] != '/' || !realpath(path, full)) return -1;
    static const char *const roots[] = { "/usr/share/fonts/", "/usr/local/share/fonts/", NULL };
    for (int i = 0; roots[i]; i++)
        if (!strncmp(full, roots[i], strlen(roots[i]))) { snprintf(out, PATH_MAX, "%s", full); return 0; }
    const char *assets = js_sys_assets();
    if (assets) {
        char a[PATH_MAX];
        snprintf(a, sizeof a, "%s/fonts/", assets);
        char ra[PATH_MAX];
        if (realpath(a, ra) && !strncmp(full, ra, strlen(ra)) && full[strlen(ra)] == '/') { snprintf(out, PATH_MAX, "%s", full); return 0; }
    }
    return -1;
}

static int open_file(const char *real) {
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

/* New file for writing (never overwrites: O_EXCL). */
static int open_new(const char *path) {
    char target[PATH_MAX + 300];
    if (!path || js_sys_resolve_target(path, target, sizeof target)) return -1;
    return open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
}

static JSValue throw_path(JSContext *c) { return js_throw_code(c, "fs.outsideHome", NULL); }

static const char *arg_str(JSContext *c, int argc, JSValueConst *argv, int i) {
    return i < argc && JS_IsString(argv[i]) ? JS_ToCString(c, argv[i]) : NULL;
}

static int arg_int(JSContext *c, int argc, JSValueConst *argv, int i, int def) {
    int32_t v = def;
    if (i < argc && JS_IsNumber(argv[i])) JS_ToInt32(c, &v, argv[i]);
    return v;
}

/* JSON arguments (text JS already built with JSON.stringify), with a cap */
static const char *arg_json(JSContext *c, int argc, JSValueConst *argv, int i, size_t *len) {
    *len = 0;
    if (i >= argc || !JS_IsString(argv[i])) return NULL;
    const char *s = JS_ToCStringLen(c, len, argv[i]);
    if (s && *len > (size_t)DOC_JSON_MAX) { JS_FreeCString(c, s); *len = 0; return NULL; }
    return s;
}

static const struct { const char *name; uint32_t op; int timeout; } pdf_ops[] = {
    { "open", DQ_PDF_OPEN, 60000 }, { "render", DQ_PDF_RENDER, 30000 }, { "select", DQ_PDF_SELECT, 15000 },
    { "find", DQ_PDF_FIND, 0 }, { "outline", DQ_PDF_OUTLINE, 30000 }, { "pageinfo", DQ_PDF_PAGEINFO, 15000 },
    { "setfield", DQ_PDF_SETFIELD, 15000 }, { "annot", DQ_PDF_ANNOT, 15000 }, { "undo", DQ_PDF_UNDO, 15000 },
    { "blob", DQ_PDF_BLOB, 60000 }, { "list", DQ_ARC_LIST, 0 }, { "sizes", DQ_PDF_SIZES, 60000 },
};

static const struct { const char *name; uint32_t op; int timeout; int out; } util_ops[] = {
    { "fontinfo", DQ_FONT_INFO, 15000, 0 }, { "fontrender", DQ_FONT_RENDER, 15000, 0 }, { "fontgrid", DQ_FONT_GRID, 15000, 0 },
    { "maketext", DQ_MAKE_TEXT, 60000, 0 }, { "makeimage", DQ_MAKE_IMAGE, 60000, 0 }, { "decode", DQ_IMG_DECODE, 30000, 0 },
    { "convert", DQ_IMG_CONVERT, 60000, 1 }, { "pdfthumb", DQ_PDF_THUMB, 20000, 0 }, { "warm", DQ_WARM, 20000, 0 },
};

/* ---- functions ---- */

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, doc_available() == 0);
}

static JSValue f_open_pdf(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg_str(c, argc, argv, 0);
    char real[PATH_MAX];
    int bad = !p || resolve_read(p, real);
    if (p) JS_FreeCString(c, p);
    if (bad) return throw_path(c);
    int fd = open_file(real);
    if (fd < 0) return js_throw_code(c, "fs.openFailed", "errno", lerr_errno(errno), NULL);
    DocInitFd init = { fd, "file" };
    int sid = doc_session("pdf", &init, 1);
    return sid > 0 ? JS_NewInt32(c, sid) : js_throw_code(c, "doc.noHelper", NULL);
}

static JSValue f_open_blob(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int fd = doc_blob_dup(arg_int(c, argc, argv, 0, 0), NULL);
    if (fd < 0) return js_throw_code(c, "doc.badBlob", NULL);
    DocInitFd init = { fd, "file" };
    int sid = doc_session("pdf", &init, 1);
    return sid > 0 ? JS_NewInt32(c, sid) : js_throw_code(c, "doc.noHelper", NULL);
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int sid = arg_int(c, argc, argv, 0, 0);
    const char *op = arg_str(c, argc, argv, 1);
    if (!op) return js_throw_code(c, "sys.badArgument", NULL);
    uint32_t code = 0;
    int timeout = 0;
    for (size_t i = 0; i < sizeof pdf_ops / sizeof pdf_ops[0]; i++)
        if (!strcmp(op, pdf_ops[i].name)) { code = pdf_ops[i].op; timeout = pdf_ops[i].timeout; }
    JS_FreeCString(c, op);
    if (!code) return js_throw_code(c, "sys.badArgument", NULL);
    size_t len;
    const char *json = arg_json(c, argc, argv, 2, &len);
    int prio = arg_int(c, argc, argv, 3, 0);
    int to = arg_int(c, argc, argv, 4, timeout);
    if (to < 0 || to > 600000) to = timeout;
    int id = doc_request(sid, code, json, len, NULL, 0, prio, to);
    if (json) JS_FreeCString(c, json);
    return JS_NewInt32(c, id);
}

static JSValue f_save_pdf(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int sid = arg_int(c, argc, argv, 0, 0);
    const char *p = arg_str(c, argc, argv, 1);
    int fd = open_new(p);
    if (p) JS_FreeCString(c, p);
    if (fd < 0) return errno == EEXIST ? js_throw_code(c, "fs.exists", NULL) : throw_path(c);
    int id = doc_request(sid, DQ_PDF_SAVE, "{}", 2, &fd, 1, 5, 120000);
    return JS_NewInt32(c, id);
}

static JSValue pair(JSContext *c, int sid, int req) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "sid", JS_NewInt32(c, sid));
    JS_SetPropertyStr(c, o, "req", JS_NewInt32(c, req));
    return o;
}

/* The archive: one path, or an array with every volume of a split archive in order
 * (name.7z.001, name.7z.002…; name.z01…name.zip). Each one is validated and opened here like a
 * single archive; the helper gets the fds (slots 0..n-1), never the paths. Returns n, or -1 with
 * the exception already thrown (and nothing left open). */
static int open_volumes(JSContext *c, JSValueConst v, DocInitFd *init, JSValue *exc) {
    int n = 1;
    int arr = JS_IsArray(c, v) > 0;
    if (arr) {
        JSValue lv = JS_GetPropertyStr(c, v, "length");
        int32_t len = 0;
        JS_ToInt32(c, &len, lv);
        JS_FreeValue(c, lv);
        if (len < 1 || len > DOC_INIT_FDS - 1) { *exc = js_throw_code(c, "doc.itemCount", "max", lerr_int(DOC_INIT_FDS - 1), NULL); return -1; }
        n = len;
    }
    for (int i = 0; i < n; i++) {
        JSValue item = arr ? JS_GetPropertyUint32(c, v, (uint32_t)i) : JS_DupValue(c, v);
        const char *p = JS_IsString(item) ? JS_ToCString(c, item) : NULL;
        JS_FreeValue(c, item);
        char real[PATH_MAX];
        int bad = !p || resolve_read(p, real);
        if (p) JS_FreeCString(c, p);
        int fd = bad ? -1 : open_file(real);
        if (fd < 0) {
            int e = errno;
            for (int k = 0; k < i; k++) close(init[k].fd);
            *exc = bad ? throw_path(c) : js_throw_code(c, "fs.openFailed", "errno", lerr_errno(e), NULL);
            return -1;
        }
        init[i].fd = fd;
        init[i].role = "file";
    }
    return n;
}

/* {"slot":0} for one file, {"vols":[0,1,…]} for a split archive */
static void vols_json(JBuf *j, int n) {
    if (n == 1) { jb_raw(j, "\"slot\":0"); return; }
    jb_raw(j, "\"vols\":[");
    for (int i = 0; i < n; i++) jb_printf(j, "%s%d", i ? "," : "", i);
    jb_raw(j, "]");
}

static JSValue f_arc_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return throw_path(c);
    DocInitFd *init = calloc(DOC_INIT_FDS, sizeof *init);
    if (!init) return JS_ThrowOutOfMemory(c);
    JSValue exc = JS_UNDEFINED;
    int n = open_volumes(c, argv[0], init, &exc);
    if (n < 0) { free(init); return exc; }
    int sid = doc_session("arc", init, n);
    free(init);
    if (sid < 0) return js_throw_code(c, "doc.noHelper", NULL);
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{");
    vols_json(&j, n);
    jb_raw(&j, "}");
    int req = j.oom ? -1 : doc_request(sid, DQ_ARC_LIST, j.p, j.n, NULL, 0, 0, 0);
    jb_free(&j);
    return pair(c, sid, req);
}

/* arcExtract(archive, folder, json, exists): the folder is created (fails if it already exists) unless
 * exists=true (the Quick Preview cache); the json may carry only and rawname. The archive is a path or
 * the array of a split archive's volumes. */
static JSValue f_arc_extract(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return throw_path(c);
    const char *d = arg_str(c, argc, argv, 1);
    char target[PATH_MAX + 300];
    int bad = !d || js_sys_resolve_target(d, target, sizeof target);
    if (d) JS_FreeCString(c, d);
    if (bad) return throw_path(c);
    DocInitFd *init = calloc(DOC_INIT_FDS, sizeof *init);
    if (!init) return JS_ThrowOutOfMemory(c);
    JSValue exc = JS_UNDEFINED;
    int n = open_volumes(c, argv[0], init, &exc);
    if (n < 0) { free(init); return exc; }
    int existing = argc > 3 && JS_ToBool(c, argv[3]) > 0;
    int dfd = -1, e = 0;
    if (!existing && mkdir(target, 0755)) e = errno;
    else if ((dfd = open(target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) e = errno;
    if (dfd < 0) {
        for (int i = 0; i < n; i++) close(init[i].fd);
        free(init);
        return e == EEXIST ? js_throw_code(c, "fs.exists", NULL) : js_throw_code(c, "sys.errno", "errno", lerr_errno(e), NULL);
    }
    init[n].fd = dfd;
    init[n].role = "rw";
    int sid = doc_session("arc", init, n + 1);
    free(init);
    if (sid < 0) return js_throw_code(c, "doc.noHelper", NULL);
    /* {"slot":0 or "vols":[…], "dir":n, ...what JS sent} */
    size_t len;
    const char *json = arg_json(c, argc, argv, 2, &len);
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{");
    vols_json(&j, n);
    jb_printf(&j, ",\"dir\":%d", n);
    if (json && len > 2 && json[0] == '{') { jb_raw(&j, ","); jb_rawn(&j, json + 1, len - 1); }
    else jb_raw(&j, "}");
    int req = j.oom ? -1 : doc_request(sid, DQ_ARC_EXTRACT, j.p, j.n, NULL, 0, 0, 0);
    jb_free(&j);
    if (json) JS_FreeCString(c, json);
    return pair(c, sid, req);
}

static JSValue f_arc_create(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsArray(c, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    JSValue lv = JS_GetPropertyStr(c, argv[0], "length");
    int32_t n = 0;
    JS_ToInt32(c, &n, lv);
    JS_FreeValue(c, lv);
    if (n < 1 || n > DOC_INIT_FDS - 1) return js_throw_code(c, "doc.itemCount", "max", lerr_int(DOC_INIT_FDS - 1), NULL);
    DocInitFd *init = calloc((size_t)n + 1, sizeof *init);
    if (!init) return JS_ThrowOutOfMemory(c);
    JBuf items;
    jb_init(&items);
    int k = 0, bad = 0;
    for (int i = 0; i < n && !bad; i++) {
        JSValue v = JS_GetPropertyUint32(c, argv[0], (uint32_t)i);
        const char *p = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
        JS_FreeValue(c, v);
        char real[PATH_MAX];
        bad = !p || resolve_read(p, real);
        if (p) JS_FreeCString(c, p);
        if (bad) break;
        int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) { if (fd >= 0) close(fd); bad = 1; break; }
        const char *base = strrchr(real, '/');
        base = base && base[1] ? base + 1 : "x";
        /* slot k: the fds go in this order (doc.c keeps the order when sending them) */
        init[k].fd = fd;
        init[k].role = S_ISDIR(st.st_mode) ? "ro" : "file";
        jb_printf(&items, "%s{\"slot\":%d,\"name\":", k ? "," : "", k);
        jb_str(&items, base);
        jb_raw(&items, "}");
        k++;
    }
    const char *out = arg_str(c, argc, argv, 1);
    int ofd = bad ? -1 : open_new(out);
    int eexist = errno == EEXIST;
    if (out) JS_FreeCString(c, out);
    if (bad || ofd < 0) {
        for (int i = 0; i < k; i++) close(init[i].fd);
        free(init);
        jb_free(&items);
        return !bad && eexist ? js_throw_code(c, "fs.exists", NULL) : throw_path(c);
    }
    init[k].fd = ofd;
    init[k].role = "out";
    JBuf j;
    jb_init(&j);
    jb_printf(&j, "{\"out\":%d,\"format\":\"zip\",\"items\":[", k);
    if (items.p) jb_raw(&j, items.p);
    jb_raw(&j, "]}");
    jb_free(&items);
    int sid = doc_session("arc", init, k + 1);
    free(init);
    int req = sid > 0 && !j.oom ? doc_request(sid, DQ_ARC_CREATE, j.p, j.n, NULL, 0, 0, 0) : -1;
    jb_free(&j);
    if (sid < 0) return js_throw_code(c, "doc.noHelper", NULL);
    return pair(c, sid, req);
}

static JSValue f_util(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *op = arg_str(c, argc, argv, 0);
    if (!op) return js_throw_code(c, "sys.badArgument", NULL);
    int k = -1;
    for (size_t i = 0; i < sizeof util_ops / sizeof util_ops[0]; i++) if (!strcmp(op, util_ops[i].name)) k = (int)i;
    int font = !strncmp(op, "font", 4);
    JS_FreeCString(c, op);
    if (k < 0) return js_throw_code(c, "sys.badArgument", NULL);
    int fds[2], nfds = 0;
    const char *p = arg_str(c, argc, argv, 2);
    if (p) {
        char real[PATH_MAX];
        int bad = font ? resolve_font(p, real) : resolve_read(p, real);
        JS_FreeCString(c, p);
        if (bad) return throw_path(c);
        int fd = open_file(real);
        if (fd < 0) return js_throw_code(c, "fs.openFailed", "errno", lerr_errno(errno), NULL);
        fds[nfds++] = fd;
    }
    if (util_ops[k].out) {
        const char *o = arg_str(c, argc, argv, 3);
        int ofd = open_new(o);
        if (o) JS_FreeCString(c, o);
        if (ofd < 0) { for (int i = 0; i < nfds; i++) close(fds[i]); return errno == EEXIST ? js_throw_code(c, "fs.exists", NULL) : throw_path(c); }
        fds[nfds++] = ofd;
    }
    size_t len;
    const char *json = arg_json(c, argc, argv, 1, &len);
    /* a single, long-lived utilities session; if it died, another one */
    if (util_sid <= 0 || !doc_alive(util_sid)) {
        if (util_sid > 0) doc_close(util_sid);
        util_sid = doc_session("util", NULL, 0);
    }
    int id = util_sid > 0 ? doc_request(util_sid, util_ops[k].op, json, len, fds, nfds, 0, util_ops[k].timeout) : -1;
    if (util_sid <= 0) for (int i = 0; i < nfds; i++) close(fds[i]);
    if (json) JS_FreeCString(c, json);
    if (id < 0) return js_throw_code(c, "doc.noHelper", NULL);
    JSValue o = pair(c, util_sid, id);
    return o;
}

static JSValue f_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    return JS_NewBool(c, doc_cancel(arg_int(c, argc, argv, 0, 0), arg_int(c, argc, argv, 1, 0)));
}

static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int sid = arg_int(c, argc, argv, 0, 0);
    if (sid > 0 && sid != util_sid) doc_close(sid);
    return JS_UNDEFINED;
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = doc_poll();
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<doc>");
    free(j);
    return v;
}

static JSValue f_pending(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, doc_pending());
}

/* bind(node, image): the returned image becomes the content of an <Image> (copy). */
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
    const uint32_t *px = doc_image_lock(handle, &w, &h);
    if (!px) { doc_image_unlock(); return JS_FALSE; }
    Image *img = image_argb(n->img, w, h, px);
    doc_image_unlock();
    if (!img) return JS_FALSE;
    n->img = img;
    node_invalidate(n);
    return JS_TRUE;
}

static JSValue f_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    doc_image_release(arg_int(c, argc, argv, 0, 0));
    return JS_UNDEFINED;
}

static JSValue f_blob_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    doc_blob_release(arg_int(c, argc, argv, 0, 0));
    return JS_UNDEFINED;
}

static JSValue f_blob_save(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg_str(c, argc, argv, 1);
    int fd = open_new(p);
    if (p) JS_FreeCString(c, p);
    if (fd < 0) return errno == EEXIST ? js_throw_code(c, "fs.exists", NULL) : throw_path(c);
    int r = doc_blob_copy(arg_int(c, argc, argv, 0, 0), fd);
    close(fd);
    return JS_NewBool(c, r == 0);
}

static JSValue f_stats(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "imageBytes", JS_NewFloat64(c, (double)doc_image_bytes()));
    JS_SetPropertyStr(c, o, "pending", JS_NewInt32(c, doc_pending()));
    return o;
}

static const JSCFunctionListEntry doc_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("openPdf", 1, f_open_pdf),
    JS_CFUNC_DEF("openBlob", 1, f_open_blob),
    JS_CFUNC_DEF("request", 5, f_request),
    JS_CFUNC_DEF("savePdf", 2, f_save_pdf),
    JS_CFUNC_DEF("arcList", 1, f_arc_list),
    JS_CFUNC_DEF("arcExtract", 4, f_arc_extract),
    JS_CFUNC_DEF("arcCreate", 2, f_arc_create),
    JS_CFUNC_DEF("util", 4, f_util),
    JS_CFUNC_DEF("cancel", 2, f_cancel),
    JS_CFUNC_DEF("close", 1, f_close),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("pending", 0, f_pending),
    JS_CFUNC_DEF("bind", 2, f_bind),
    JS_CFUNC_DEF("release", 1, f_release),
    JS_CFUNC_DEF("blobRelease", 1, f_blob_release),
    JS_CFUNC_DEF("blobSave", 2, f_blob_save),
    JS_CFUNC_DEF("stats", 0, f_stats),
};

void js_doc_register(JSContext *c, JSValue lord) {
    /* the helper reads LordOS's fonts for Quick Preview and the samples */
    const char *assets = js_sys_assets();
    if (assets && !getenv("LORD_DOC_FONTS")) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/fonts", assets);
        setenv("LORD_DOC_FONTS", p, 0);
    }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, doc_funcs, sizeof doc_funcs / sizeof doc_funcs[0]);
    JS_SetPropertyStr(c, lord, "doc", o);
}
