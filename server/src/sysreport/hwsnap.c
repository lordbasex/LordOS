/* hwsnap.c — see hwsnap.h. */
#include "hwsnap.h"
#include "../pkg/crypto.h"
#include "../sec/codesig.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void seterr(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

const char *hwsnap_state_name(HwSnapState s) {
    switch (s) {
    case HWSNAP_OK: return "ok";
    case HWSNAP_MISSING: return "missing";
    case HWSNAP_UNSIGNED: return "unsigned";
    case HWSNAP_BAD_SIG: return "bad-signature";
    default: return "invalid";
    }
}

static int is_count(const JVal *v) {
    return v && v->type == J_NUM && v->num >= 0 && v->num <= 1e9 && v->num == (double)(long long)v->num;
}

/* x.y.z or x.y.z.w, each part 1–4 digits */
static int is_version(const char *s) {
    if (!s) return 0;
    int parts = 0;
    while (*s) {
        int d = 0;
        while (*s >= '0' && *s <= '9') { s++; d++; }
        if (d < 1 || d > 4) return 0;
        parts++;
        if (*s == '.') { s++; if (!*s) return 0; }
        else if (*s) return 0;
    }
    return parts == 3 || parts == 4;
}

static int is_date(const char *s) {
    if (!s || strlen(s) != 10) return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

/* printable text without control characters (UTF-8 bytes ≥ 0x80 allowed), 1..max bytes */
static int is_text(const char *s, size_t len, size_t max, int allow_empty) {
    if (!s || len > max || (!len && !allow_empty)) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int is_component(const char *s) {
    static const char *const ok[] = { "gpu", "display", "storage", "network", "wifi", "bluetooth", "audio", "power", "isolation", "suspend" };
    for (size_t i = 0; s && i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(s, ok[i])) return 1;
    return 0;
}

static int check_device(const JVal *d, char *err, size_t errlen) {
    if (!d || d->type != J_OBJ || d->len != 9) { seterr(err, errlen, "a device is not an object of 9 fields%s", NULL); return -1; }
    const JVal *key = json_get(d, "key"), *name = json_get(d, "name");
    if (!key || key->type != J_STR || !is_text(key->str, key->len, HWSNAP_KEY_MAX, 0) || !strchr(key->str, ':')) {
        seterr(err, errlen, "bad device key%s", NULL); return -1;
    }
    if (!name || name->type != J_STR || !is_text(name->str, name->len, HWSNAP_NAME_MAX, 1)) { seterr(err, errlen, "bad name for %s", key->str); return -1; }
    if (!is_component(json_str(d, "component"))) { seterr(err, errlen, "bad component for %s", key->str); return -1; }
    if (!is_version(json_str(d, "lordos"))) { seterr(err, errlen, "bad version for %s", key->str); return -1; }
    static const char *const counts[] = { "works", "limits", "no", "reports", "references" };
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++)
        if (!is_count(json_get(d, counts[i]))) { seterr(err, errlen, "bad count for %s", key->str); return -1; }
    double works = json_num(d, "works", 0), limits = json_num(d, "limits", 0), no = json_num(d, "no", 0);
    double reports = json_num(d, "reports", 0), refs = json_num(d, "references", 0);
    if (works + limits + no != reports || refs > reports || reports < 1) { seterr(err, errlen, "counts don't add up for %s", key->str); return -1; }
    return 0;
}

HwSnapState hwsnap_check(const char *json, size_t len, const char *sig, size_t siglen, const uint8_t pub[32],
                         char *err, size_t errlen) {
    if (!json || !len || len > HWSNAP_MAX) { seterr(err, errlen, "snapshot size%s", NULL); return HWSNAP_INVALID; }
    if (!sig) { seterr(err, errlen, "no signature%s", NULL); return HWSNAP_UNSIGNED; }
    uint8_t s[64];
    if (siglen > HWSNAP_SIG_MAX || codesig_sig_decode(sig, siglen, s)) { seterr(err, errlen, "signature is not 64 bytes of base64%s", NULL); return HWSNAP_BAD_SIG; }
    if (!ed25519_verify(pub, s, (const uint8_t *)json, len)) { seterr(err, errlen, "bad signature%s", NULL); return HWSNAP_BAD_SIG; }

    JDoc doc;
    char jerr[96];
    if (json_parse(json, len, &doc, jerr, sizeof jerr)) { seterr(err, errlen, "snapshot: %s", jerr); return HWSNAP_INVALID; }
    HwSnapState st = HWSNAP_INVALID;
    const JVal *r = doc.root, *devs = json_get(r, "devices");
    const char *fmt = json_str(r, "format");
    if (!r || r->type != J_OBJ || r->len != 5) { seterr(err, errlen, "snapshot: not an object of 5 fields%s", NULL); goto out; }
    if (!fmt || strcmp(fmt, HWSNAP_FORMAT) || json_num(r, "version", 0) != 1) { seterr(err, errlen, "snapshot: not " HWSNAP_FORMAT " version 1%s", NULL); goto out; }
    if (!is_date(json_str(r, "generated")) || !is_count(json_get(r, "reports"))) { seterr(err, errlen, "snapshot: bad date or report count%s", NULL); goto out; }
    if (!devs || devs->type != J_ARR || devs->len > HWSNAP_DEVICES_MAX) { seterr(err, errlen, "snapshot: bad device list%s", NULL); goto out; }
    for (const JVal *d = devs->kid; d; d = d->next)
        if (check_device(d, err, errlen)) goto out;
    st = HWSNAP_OK;
out:
    json_free(&doc);
    return st;
}

/* one regular file under dirfd, no links, at most max bytes; NULL with *missing = 1 when it isn't there */
static char *read_at(int dirfd, const char *name, size_t max, size_t *len, int *missing) {
    *missing = 0;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { *missing = errno == ENOENT || errno == ELOOP; return NULL; }
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { *missing = 1; goto out; }
    if (st.st_size < 0 || (size_t)st.st_size > max) goto out;
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) goto out;
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    char extra;
    if (got != (size_t)st.st_size || read(fd, &extra, 1) != 0) { free(buf); buf = NULL; goto out; }   /* changed while reading */
    buf[got] = 0;
    *len = got;
out:
    close(fd);
    return buf;
}

HwSnapState hwsnap_load(const char *dir, const uint8_t pub[32], char **json, size_t *len, char *err, size_t errlen) {
    *json = NULL;
    *len = 0;
    int dfd = dir ? open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    if (dfd < 0) { seterr(err, errlen, "no snapshot folder%s", NULL); return HWSNAP_MISSING; }
    size_t jl = 0, sl = 0;
    int missing = 0, sig_missing = 0;
    char *j = read_at(dfd, HWSNAP_FILE, HWSNAP_MAX, &jl, &missing);
    char *s = j ? read_at(dfd, HWSNAP_SIG, HWSNAP_SIG_MAX, &sl, &sig_missing) : NULL;
    close(dfd);
    HwSnapState st;
    if (!j) { st = missing ? HWSNAP_MISSING : HWSNAP_INVALID; seterr(err, errlen, missing ? "no snapshot%s" : "snapshot unreadable or too big%s", NULL); }
    else if (!s) { st = sig_missing ? HWSNAP_UNSIGNED : HWSNAP_BAD_SIG; seterr(err, errlen, sig_missing ? "no signature%s" : "signature unreadable%s", NULL); }
    else st = hwsnap_check(j, jl, s, sl, pub, err, errlen);
    free(s);
    if (st == HWSNAP_OK) { *json = j; *len = jl; }
    else free(j);
    return st;
}
