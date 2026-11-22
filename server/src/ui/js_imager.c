/* js_imager.c — lord.imager: what the LordOS Imager (apps/imager, docs/IMAGER.md) needs from C in the
 * session. No root, nothing written, no programs run; everything that writes a disk or downloads is
 * an admin action (imager/imager_admin.c).
 *
 *   lord.imager.hashPassword(pass)        yescrypt crypt(3) hash ("$y$…", Debian's default), or null.
 *                                         pass: 1..1024 bytes, no NUL. The password never leaves
 *                                         this call; only the hash goes into the first-run file.
 *   lord.imager.wifiPsk(ssid, pass)       the WPA2 key, PBKDF2-HMAC-SHA1(pass, ssid, 4096, 32), as
 *                                         64 hex digits, or null. ssid 1..32 bytes, pass 8..63
 *                                         printable ASCII (IEEE 802.11i)
 *   lord.imager.checkSums(sums, sig)      verifies SHA256SUMS (text) with SHA256SUMS.sig against the
 *                                         release key compiled into LordOS: { ok, files: {name: hex} }
 *   lord.imager.imageInfo(path)           { kind: 'raw'|'xz', size, raw } of an image in the user's
 *                                         folders (raw: uncompressed size from the .xz index), or null */
#include "js_imager.h"
#include "js_secret.h"
#include "../fsops.h"
#include "../imager/imgverify.h"
#include "../net/netpriv.h"
#include "../pkg/crypto.h"
#include "../sec/codesig.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static JSValue f_hash(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    size_t n = 0;
    char *p = js_secret_dup(c, argv[0], &n);
    if (!p) return JS_NULL;
    JSValue r = JS_NULL;
    if (n >= 1 && n <= 1024 && strlen(p) == n) {
        char *salt = crypt_gensalt_ra("$y$", 0, NULL, 0);
        struct crypt_data *cd = calloc(1, sizeof *cd);
        const char *h = salt && cd ? crypt_r(p, salt, cd) : NULL;
        if (h && h[0] == '$' && h[1] == 'y') r = JS_NewString(c, h);
        if (cd) { memset(cd, 0, sizeof *cd); free(cd); }
        free(salt);
    }
    js_secret_free(p, n);
    return r;
}

static JSValue f_psk(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_NULL;
    size_t sn = 0, pn = 0;
    const char *ssid = JS_ToCStringLen(c, &sn, argv[0]);
    char *pass = js_secret_dup(c, argv[1], &pn);
    JSValue r = JS_NULL;
    int ok = ssid && pass && sn >= 1 && sn <= 32 && strlen(ssid) == sn && pn >= 8 && pn <= 63;
    for (size_t i = 0; ok && i < pn; i++) if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7e) ok = 0;
    if (ok) {
        uint8_t psk[32];
        char hex[65];
        pbkdf2_sha1((const uint8_t *)pass, pn, (const uint8_t *)ssid, sn, 4096, psk, 32);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", psk[i]);
        r = JS_NewString(c, hex);
        memset(psk, 0, sizeof psk);
        memset(hex, 0, sizeof hex);
    }
    js_secret_free(pass, pn);
    if (ssid) JS_FreeCString(c, ssid);
    return r;
}

/* { ok, files } — files only when the signature is good */
static JSValue f_check_sums(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "ok", JS_FALSE);
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return o;
    size_t n, sn;
    const char *sums = JS_ToCStringLen(c, &n, argv[0]);
    const char *sig = JS_ToCStringLen(c, &sn, argv[1]);
    if (sums && sig && img_sig_verify((const uint8_t *)sums, n, sig, sn, codesig_release_key) == 0) {
        JSValue files = JS_NewObject(c);
        int bad = 0, count = 0;
        for (size_t i = 0; i < n && !bad;) {
            size_t e = i;
            while (e < n && sums[e] != '\n') e++;
            size_t len = e - i;
            if (len && sums[i + len - 1] == '\r') len--;
            if (len >= 67 && len < 67 + 200) {
                char name[260], hex[65];
                memcpy(hex, sums + i, 64);
                hex[64] = 0;
                memcpy(name, sums + i + 66, len - 66);
                name[len - 66] = 0;
                uint8_t h[32];
                if (img_valid_name(name) && img_sums_find(sums, n, name, h) == 0) {
                    hex_encode(h, 32, hex);
                    JS_SetPropertyStr(c, files, name, JS_NewString(c, hex));
                    count++;
                } else bad = 1;
            } else if (len) bad = 1;
            i = e + 1;
        }
        if (!bad && count) { JS_SetPropertyStr(c, o, "ok", JS_TRUE); JS_SetPropertyStr(c, o, "files", files); }
        else JS_FreeValue(c, files);
    }
    if (sums) JS_FreeCString(c, sums);
    if (sig) JS_FreeCString(c, sig);
    return o;
}

typedef struct { int fd; } Fd;
static int fd_read(void *ud, uint64_t off, void *buf, size_t n) {
    Fd *f = ud;
    for (size_t o = 0; o < n;) {
        ssize_t k = pread(f->fd, (char *)buf + o, n - o, (off_t)(off + o));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        o += (size_t)k;
    }
    return 0;
}

static JSValue f_image_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    const char *p = JS_ToCString(c, argv[0]);
    if (!p) return JS_NULL;
    char real[PATH_MAX];
    JSValue r = JS_NULL;
    int ok = p[0] == '/' && strlen(p) < PATH_MAX && realpath(p, real) && fsops_allowed(real);
    JS_FreeCString(c, p);
    if (!ok) return r;
    const char *base = strrchr(real, '/') + 1;
    int kind = img_kind(base);
    int fd = kind ? open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0) {
        uint64_t raw = (uint64_t)st.st_size;
        Fd f = { fd };
        int good = kind == 1 || img_xz_size(fd_read, &f, (uint64_t)st.st_size, &raw) == 0;
        r = JS_NewObject(c);
        JS_SetPropertyStr(c, r, "kind", JS_NewString(c, kind == 2 ? "xz" : "raw"));
        JS_SetPropertyStr(c, r, "size", JS_NewFloat64(c, (double)st.st_size));
        JS_SetPropertyStr(c, r, "raw", good ? JS_NewFloat64(c, (double)raw) : JS_NULL);
        JS_SetPropertyStr(c, r, "name", JS_NewString(c, base));
        JS_SetPropertyStr(c, r, "path", JS_NewString(c, real));
    }
    if (fd >= 0) close(fd);
    return r;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("hashPassword", 1, f_hash),
    JS_CFUNC_DEF("wifiPsk", 2, f_psk),
    JS_CFUNC_DEF("checkSums", 2, f_check_sums),
    JS_CFUNC_DEF("imageInfo", 1, f_image_info),
};

void js_imager_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "imager", o);
}
