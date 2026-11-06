/* vmcatalog.c — see vmcatalog.h. */
#include "vmcatalog.h"
#include "../sec/json.h"
#include "../pkg/pgp.h"
#include "../errcode.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_IMAGE_MB (64ull * 1024)          /* 64 GiB: far above any installer */

static const JVal *get(const JVal *o, const char *k) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const JVal *m = o->kid; m; m = m->next) if (!strcmp(m->key, k)) return m;
    return NULL;
}
static const char *str(const JVal *o, const char *k) {
    const JVal *v = get(o, k);
    return v && v->type == J_STR ? v->str : NULL;
}

/* https://host/…/ with printable characters only, no spaces, query, fragment, user info or ".." */
static int dir_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 12 || n >= VMCAT_URL || strncmp(s, "https://", 8) || s[n - 1] != '/') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '@' || c == '\\' || c == '"' || c == '\'') return 0;
    }
    return !strstr(s, "/../") && !strstr(s, "/./") && strchr(s + 8, '/') > s + 8;
}

static int re_ok(const char *re) {
    size_t n = re ? strlen(re) : 0;
    if (n < 3 || n >= VMCAT_RE || re[0] != '^' || re[n - 1] != '$') return 0;
    regex_t r;
    if (regcomp(&r, re, REG_EXTENDED | REG_NOSUB)) return 0;
    regfree(&r);
    return 1;
}

int vmcat_name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 127 || s[0] == '.' || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("._+~-", s[i])) return 0;
    return 1;
}

int vmcat_match(const char *re, const char *name) {
    if (!vmcat_name_ok(name) || !re_ok(re)) return 0;
    regex_t r;
    if (regcomp(&r, re, REG_EXTENDED | REG_NOSUB)) return 0;
    int ok = !regexec(&r, name, 0, NULL, 0);
    regfree(&r);
    return ok;
}

int vmcat_sha_ok(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

int vmcat_find(const char *json, size_t n, const char *id, const char *arch, VmCatEntry *e) {
    if (!json || !id || !arch || !e || (strcmp(arch, "amd64") && strcmp(arch, "arm64"))) return -1;
    memset(e, 0, sizeof *e);
    JDoc d;
    if (json_parse(json, n, &d, NULL, 0)) return -1;
    int rc = -1;
    const JVal *list = get(d.root, "systems");
    for (const JVal *s = list && list->type == J_ARR ? list->kid : NULL; s; s = s->next) {
        const char *sid = str(s, "id");
        if (!sid || strcmp(sid, id)) continue;
        const JVal *a = get(get(s, "arch"), arch);
        const char *dir = str(a, "dir"), *fdir = str(a, "fileDir"), *sums = str(a, "sums"), *sre = str(a, "sumsPattern"), *fre = str(a, "file");
        const JVal *mx = get(a, "maxMb");
        if (!a || !dir_ok(dir) || (fdir && !dir_ok(fdir)) || !re_ok(fre) || !mx || mx->type != J_NUM || mx->num < 1 || mx->num > (double)MAX_IMAGE_MB) break;
        if (!!sums == !!sre || (sums && (!vmcat_name_ok(sums) || strlen(sums) >= sizeof e->sums)) || (sre && !re_ok(sre))) break;
        /* the list's signature: every entry has one, with its key file and at least one pinned fingerprint */
        const JVal *sg = get(s, "signature"), *fl = get(s, "fingerprints");
        const char *st = str(sg, "type"), *sn = str(sg, "name"), *kf = str(s, "keys");
        if (!st || !kf || !vmcat_name_ok(kf) || strlen(kf) >= sizeof e->keys || !fl || fl->type != J_ARR) break;
        if (!strcmp(st, "detached")) {
            if (!sn || !vmcat_name_ok(sn) || strlen(sn) >= sizeof e->sig_name) break;
            e->sig_type = VMCAT_SIG_DETACHED;
            snprintf(e->sig_name, sizeof e->sig_name, "%s", sn);
        } else if (!strcmp(st, "clearsigned") && !sn) e->sig_type = VMCAT_SIG_CLEAR;
        else break;
        int bad = 0;
        for (const JVal *f = fl->kid; f && !bad; f = f->next) {
            if (f->type != J_STR || strlen(f->str) != 40 || e->nfprs == VMCAT_FPRS) { bad = 1; break; }
            for (int i = 0; i < 40; i++) {
                if (!isxdigit((unsigned char)f->str[i])) { bad = 1; break; }
                e->fprs[e->nfprs][i] = (char)toupper((unsigned char)f->str[i]);
            }
            e->fprs[e->nfprs++][40] = 0;
        }
        if (bad || !e->nfprs) break;
        snprintf(e->keys, sizeof e->keys, "%s", kf);
        snprintf(e->dir, sizeof e->dir, "%s", dir);
        snprintf(e->file_dir, sizeof e->file_dir, "%s", fdir ? fdir : dir);
        snprintf(e->sums, sizeof e->sums, "%s", sums ? sums : "");
        snprintf(e->sums_re, sizeof e->sums_re, "%s", sre ? sre : "");
        snprintf(e->file_re, sizeof e->file_re, "%s", fre);
        e->max_bytes = (unsigned long long)mx->num * 1024ull * 1024ull;
        rc = 0;
        break;
    }
    json_free(&d);
    if (rc) memset(e, 0, sizeof *e);
    return rc;
}

/* one line, without its end (\n, \r\n) */
static int line_has(const char *l, size_t n, const char *name, const char *sha) {
    size_t nl = strlen(name);
    /* GNU: <hash>  <name> | <hash> *<name> */
    if (n == 64 + 2 + nl && !strncasecmp(l, sha, 64) && l[64] == ' ' && (l[65] == ' ' || l[65] == '*') && !memcmp(l + 66, name, nl)) return 1;
    /* BSD: SHA256 (<name>) = <hash> */
    if (n == 8 + nl + 4 + 64 && !memcmp(l, "SHA256 (", 8) && !memcmp(l + 8, name, nl) && !memcmp(l + 8 + nl, ") = ", 4) &&
        !strncasecmp(l + 12 + nl, sha, 64)) return 1;
    return 0;
}

int vmcat_sums_has(const char *sums, size_t n, const char *name, const char *sha) {
    if (!sums || !vmcat_name_ok(name) || !vmcat_sha_ok(sha)) return 0;
    for (size_t i = 0; i < n;) {
        const char *nl = memchr(sums + i, '\n', n - i);
        size_t end = nl ? (size_t)(nl - sums) : n, len = end - i;
        if (len && sums[i + len - 1] == '\r') len--;
        if (memchr(sums + i, 0, len) == NULL && line_has(sums + i, len, name, sha)) return 1;
        i = end + 1;
    }
    return 0;
}

int vmcat_verify_list(const VmCatEntry *e, const char *keys, size_t keys_len, const char *list, size_t list_len,
                      const char *sig, size_t sig_len, int64_t now, char **body, size_t *body_len, char *err, size_t errlen) {
    *body = NULL;
    *body_len = 0;
    err[0] = 0;
    if (!e || !e->nfprs || !keys || !list) { lerr_set(err, errlen, "pkg.pgp.unknownKeys", "count", "0", NULL); return -1; }
    static const char clear[] = "-----BEGIN PGP SIGNED MESSAGE-----";
    if (e->sig_type == VMCAT_SIG_DETACHED && !sig) return 0;
    if (e->sig_type == VMCAT_SIG_CLEAR && (list_len < sizeof clear - 1 || memcmp(list, clear, sizeof clear - 1))) return 0;
    if (e->sig_type != VMCAT_SIG_DETACHED && e->sig_type != VMCAT_SIG_CLEAR) return -1;
    PgpKeyring kr = { 0 };
    const char *fp[VMCAT_FPRS];
    for (int i = 0; i < e->nfprs; i++) fp[i] = e->fprs[i];
    int rc = -1;
    if (pgp_keyring_add_mem(&kr, keys, keys_len)) lerr_set(err, errlen, "pkg.pgp.malformed", NULL);
    /* only the pinned keys may sign, and every pinned key must be in the file (else the catalog and the file disagree) */
    else if (pgp_keyring_pin(&kr, fp, e->nfprs) != e->nfprs) lerr_set(err, errlen, "pkg.pgp.unknownKeys", "count", lerr_int(e->nfprs), NULL);
    else if (e->sig_type == VMCAT_SIG_DETACHED) {
        int g = pgp_verify_detached(&kr, list, list_len, sig, sig_len, now, err, errlen);
        if (g > 0 && (*body = malloc(list_len + 1))) {
            memcpy(*body, list, list_len);
            (*body)[list_len] = 0;
            *body_len = list_len;
            rc = 1;
        }
    } else {
        char *b = NULL;
        size_t bl = 0;
        int g = pgp_verify_clearsigned(&kr, list, list_len, now, &b, &bl, err, errlen);
        char *z = g > 0 ? realloc(b, bl + 1) : NULL;        /* pgp.c's text has no NUL at its end */
        if (z) { z[bl] = 0; *body = z; *body_len = bl; rc = 1; } else free(b);
    }
    pgp_keyring_free(&kr);
    if (rc < 0 && !err[0]) lerr_set(err, errlen, "pkg.pgp.badSignature", NULL);
    return rc;
}
