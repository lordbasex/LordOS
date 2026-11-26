/* lordthemec.c — lord-themec: checks a community theme's source .zip and builds its code (docs/THEMES-GUIDE.md,
 * "Community themes"; AGENTS.md "lord-themec").
 *
 * lord-server (ui/js_theme.c, lord.themes.install) starts it from the fixed path next to it, with no arguments and an
 * empty environment, and gives it:
 *   fd 0  the .zip the person chose (a regular file lord-server opened, at most 32 MB)
 *   fd 1  where the answer goes; fd 2 is /dev/null
 * It applies the helpers' sandbox (no files, no network, no exec, a syscall allowlist, a memory cap) before it reads a
 * byte, reads the archive strictly (zipread.c), checks image signatures and UTF-8, then runs LordOS's theme compiler
 * (tools/themec/themec.js, compiled into this binary) in a QuickJS of its own: the static checks of lordos-theme-kit and
 * the build. The answer is the stream themestore.h describes (the report, then on success theme.js and the files to
 * install); lord-server reads it strictly, checks every name again and writes the files itself. */
#include "zipread.h"
#include "../sec/sandbox.h"
#include "quickjs.h"
#include "themec_js.h"          /* build/gen: tools/themec/themec.js as bytes (Makefile) */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#define ZIP_IN_MAX (32u << 20)

static int out_all(const void *b, size_t n) {
    const char *p = b;
    for (size_t o = 0; o < n;) {
        ssize_t w = write(1, p + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}
static int out_str(const char *s) { return out_all(s, strlen(s)); }

static uint8_t *slurp(int fd, size_t max, size_t *len) {
    uint8_t *b = malloc(max + 1);
    size_t n = 0;
    if (!b) return NULL;
    for (;;) {
        if (n > max) { free(b); return NULL; }
        ssize_t r = read(fd, b + n, max + 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(b); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    *len = n;
    return b;
}

/* a refusal before the compiler ran: a report of one finding */
static int refuse(const char *id, const char *rule, const char *why) {
    char q[600], qid[100];
    size_t k = 0;
    for (const char *p = why; *p && k < sizeof q - 3; p++) { if (*p == '"' || *p == '\\') q[k++] = '\\'; q[k++] = (unsigned char)*p < 0x20 ? ' ' : *p; }
    q[k] = 0;
    k = 0;
    for (const char *p = id; *p && k < sizeof qid - 1; p++) if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') qid[k++] = *p;
    qid[k] = 0;
    char rep[1024];
    int n = snprintf(rep, sizeof rep, "{\"ok\":false,\"id\":\"%s\",\"errors\":1,\"warnings\":0,\"findings\":[{\"level\":\"error\",\"rule\":\"%s\",\"file\":null,\"line\":null,\"message\":\"%s\"}],\"meta\":null}",
                     qid, rule, q);
    char head[64];
    snprintf(head, sizeof head, "LORDTHEME 1\nR %d\n", n);
    return out_str(head) || out_all(rep, (size_t)n) || out_str("\nEND\n") ? 4 : 0;
}

/* well-formed UTF-8 (no overlong forms, surrogates or NUL) */
static int utf8_ok(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        if (c == 0) return 0;
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0 || i + (size_t)k >= n) return 0;
        for (int j = 1; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
        uint8_t d = s[i + 1];
        if ((c == 0xe0 && d < 0xa0) || (c == 0xed && d >= 0xa0) || (c == 0xf0 && d < 0x90) || (c == 0xf4 && d >= 0x90)) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}
static int is_text(const char *name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return ends_with(name, ".js") || ends_with(name, ".jsx") || ends_with(name, ".json") || ends_with(name, ".md") ||
           ends_with(name, ".txt") || !strcmp(base, "LICENSE") || !strcmp(base, "NOTICE") || !strcmp(base, "COPYING") ||
           !strcmp(base, "AUTHORS");
}
static int image_ok(const ZipFile *f) {
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (ends_with(f->name, ".png")) return f->size >= 8 && !memcmp(f->data, png, 8);
    if (ends_with(f->name, ".jpg") || ends_with(f->name, ".jpeg")) return f->size >= 3 && f->data[0] == 0xff && f->data[1] == 0xd8 && f->data[2] == 0xff;
    return 1;
}

int main(int argc, char **argv) {
    (void)argv;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    alarm(30);
    if (argc != 1) return 2;
    SandboxHelperCfg cfg = {
        .name = "lordos-themec",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 512,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);

    size_t zl = 0;
    uint8_t *zbuf = slurp(0, ZIP_IN_MAX, &zl);
    if (!zbuf) return refuse("", "zip.size", "the archive could not be read or is larger than 32 MB");
    ZipTheme z;
    char why[200] = "";
    if (zip_read_theme(zbuf, zl, &z, why, sizeof why)) { free(zbuf); return refuse("", "zip", why); }
    free(zbuf);
    for (int i = 0; i < z.n; i++) {
        if (!image_ok(&z.files[i])) { char w[300]; snprintf(w, sizeof w, "%s is not the image its extension says", z.files[i].name); int r = refuse(z.id, "files.image", w); zip_free(&z); return r; }
        if (is_text(z.files[i].name) && !utf8_ok(z.files[i].data, z.files[i].size)) { char w[300]; snprintf(w, sizeof w, "%s is not UTF-8 text", z.files[i].name); int r = refuse(z.id, "files.text", w); zip_free(&z); return r; }
    }

    /* the compiler, in a QuickJS of its own: no lord, no files, nothing but the archive's text */
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256u << 20);
    JS_SetMaxStackSize(rt, 2u << 20);
    JSContext *c = JS_NewContext(rt);
    JSValue r = JS_Eval(c, (const char *)themec_js, sizeof themec_js - 1, "<themec.js>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { zip_free(&z); return refuse(z.id, "compiler", "the theme compiler did not start"); }
    JS_FreeValue(c, r);
    JSValue input = JS_NewObject(c), files = JS_NewArray(c);
    JS_SetPropertyStr(c, input, "id", JS_NewString(c, z.id));
    for (int i = 0; i < z.n; i++) {
        JSValue f = JS_NewObject(c);
        JS_SetPropertyStr(c, f, "name", JS_NewString(c, z.files[i].name));
        JS_SetPropertyStr(c, f, "size", JS_NewInt64(c, (int64_t)z.files[i].size));
        if (is_text(z.files[i].name)) JS_SetPropertyStr(c, f, "text", JS_NewStringLen(c, (const char *)z.files[i].data, z.files[i].size));
        JS_SetPropertyUint32(c, files, (uint32_t)i, f);
    }
    JS_SetPropertyStr(c, input, "files", files);
    JSValue g = JS_GetGlobalObject(c), T = JS_GetPropertyStr(c, g, "LordThemec"), compile = JS_GetPropertyStr(c, T, "compile");
    JSValue res = JS_Call(c, compile, T, 1, &input);
    JS_FreeValue(c, input);
    if (JS_IsException(res)) { zip_free(&z); return refuse(z.id, "compiler", "the theme compiler failed on this source"); }
    JSValue report = JS_GetPropertyStr(c, res, "report"), code = JS_GetPropertyStr(c, res, "code"), keep = JS_GetPropertyStr(c, res, "keep");
    JSValue rj = JS_JSONStringify(c, report, JS_UNDEFINED, JS_UNDEFINED);
    size_t rl = 0, cl = 0;
    const char *rs = JS_ToCStringLen(c, &rl, rj);
    const char *cs = JS_IsString(code) ? JS_ToCStringLen(c, &cl, code) : NULL;
    int ok = rs && rl <= 65536;
    char head[64];
    snprintf(head, sizeof head, "LORDTHEME 1\nR %zu\n", ok ? rl : 0);
    int bad = !ok || out_str(head) || out_all(rs, rl) || out_str("\n");
    if (!bad && cs) {
        snprintf(head, sizeof head, "F theme.js %zu\n", cl);
        bad = out_str(head) || out_all(cs, cl);
        uint32_t nk = 0;
        JSValue lv = JS_GetPropertyStr(c, keep, "length");
        JS_ToUint32(c, &nk, lv);
        JS_FreeValue(c, lv);
        for (uint32_t k = 0; !bad && k < nk && k < ZIP_FILES_MAX; k++) {
            JSValue nv = JS_GetPropertyUint32(c, keep, k);
            const char *nm = JS_ToCString(c, nv);
            for (int i = 0; nm && i < z.n; i++) {
                if (strcmp(z.files[i].name, nm) || !strcmp(nm, "theme.js")) continue;
                char fh[ZIP_NAME_MAX + 32];
                snprintf(fh, sizeof fh, "F %s %zu\n", nm, z.files[i].size);
                bad = out_str(fh) || out_all(z.files[i].data, z.files[i].size);
                break;
            }
            if (nm) JS_FreeCString(c, nm);
            JS_FreeValue(c, nv);
        }
    }
    if (!bad) bad = out_str("END\n");
    if (rs) JS_FreeCString(c, rs);
    if (cs) JS_FreeCString(c, cs);
    JS_FreeValue(c, rj);
    JS_FreeValue(c, report);
    JS_FreeValue(c, code);
    JS_FreeValue(c, keep);
    JS_FreeValue(c, res);
    JS_FreeValue(c, compile);
    JS_FreeValue(c, T);
    JS_FreeValue(c, g);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    zip_free(&z);
    return bad ? 4 : 0;
}
