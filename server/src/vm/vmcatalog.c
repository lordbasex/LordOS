/* vmcatalog.c — see vmcatalog.h. */
#include "vmcatalog.h"
#include "../sec/json.h"
#include "../pkg/pgp.h"
#include "../imager/imgverify.h"
#include "../errcode.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_IMAGE_MB (64ull * 1024)          /* 64 GiB: far above any installer */
/* the only place a list may be mirrored ("list"): LordOS's own site, for LordOS's own signed releases */
#define LIST_MIRROR "https://lordos.org/releases/"

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

int vmcat_hash_ok(const VmCatEntry *e, const char *s) {
    size_t want = e && e->hash == VMCAT_SHA512 ? 128 : 64;
    if (!s || strlen(s) != want) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

int vmcat_release_cmp(const char *a, const char *b) {
    while (*a || *b) {
        unsigned long x = 0, y = 0;
        int dx = 0, dy = 0;
        while (*a >= '0' && *a <= '9' && dx < 9) { x = x * 10 + (unsigned long)(*a++ - '0'); dx++; }
        while (*b >= '0' && *b <= '9' && dy < 9) { y = y * 10 + (unsigned long)(*b++ - '0'); dy++; }
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++; else if (*a) return 0;
        if (*b == '.') b++; else if (*b) return 0;
    }
    return 0;
}

int vmcat_release_ok(const VmCatEntry *e, const char *rel) {
    if (!e || !e->rel_dir[0] || !rel || strlen(rel) > 31 || !vmcat_match(e->rel_re, rel)) return 0;
    for (const char *p = rel; *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '.')) return 0;
    return vmcat_release_cmp(rel, e->rel_floor) >= 0;
}

int vmcat_release_in(const VmCatEntry *e, const char *name, char *out, size_t n) {
    if (!e || !e->rel_dir[0] || !vmcat_name_ok(name) || !out || n < 2) return -1;
    char found[32] = "";
    for (const char *p = name; *p; p++) {
        if (!(*p >= '0' && *p <= '9') || (p > name && ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))) continue;
        size_t k = 0, dots = 0;
        while ((p[k] >= '0' && p[k] <= '9') || (p[k] == '.' && p[k + 1] >= '0' && p[k + 1] <= '9')) dots += p[k++] == '.';
        char run[32];
        if (!dots || k >= sizeof run) { p += k ? k - 1 : 0; continue; }
        memcpy(run, p, k);
        run[k] = 0;
        p += k - 1;
        if (!vmcat_match(e->rel_re, run)) continue;
        if (found[0] && strcmp(found, run)) return -1;          /* two different releases in one name */
        memcpy(found, run, k + 1);
    }
    if (!found[0] || strlen(found) >= n) return -1;
    memcpy(out, found, strlen(found) + 1);
    return 0;
}

int vmcat_release_dir(VmCatEntry *e, const char *rel) {
    if (!vmcat_release_ok(e, rel)) return -1;
    if (snprintf(e->dir, sizeof e->dir, "%s%s/", e->rel_dir, rel) >= (int)sizeof e->dir) return -1;
    memcpy(e->file_dir, e->dir, sizeof e->dir);
    return 0;
}

/* "kind": "group" and its "archive" (19.6): the members' patterns and the caps lord-vmunpack applies */
static int parse_group(const JVal *s, VmCatEntry *e) {
    const char *gk = str(s, "group"), *ed = str(s, "edition");
    const JVal *ar = get(s, "archive"), *ms = get(ar, "members"), *ma = get(ar, "maxApparentGb"), *md = get(ar, "maxDataGb");
    const char *fmt = str(ar, "format"), *gw = str(ms, "gateway"), *ws = str(ms, "workstation");
    if (!gk || strcmp(gk, "whonix") || !ed || !ed[0] || strlen(ed) >= sizeof e->edition || !fmt || strcmp(fmt, "tar.xz") || !re_ok(gw) || !re_ok(ws) ||
        !ma || ma->type != J_NUM || ma->num < 1 || ma->num > 1024 || !md || md->type != J_NUM || md->num < 1 || md->num > ma->num) return -1;
    for (const char *p = ed; *p; p++) if (!isalnum((unsigned char)*p)) return -1;
    for (const JVal *k = ms->kid; k; k = k->next) if (strcmp(k->key, "gateway") && strcmp(k->key, "workstation")) return -1;
    e->group = 1;
    snprintf(e->group_kind, sizeof e->group_kind, "%s", gk);
    snprintf(e->edition, sizeof e->edition, "%s", ed);
    snprintf(e->member_re[0], sizeof e->member_re[0], "%s", gw);
    snprintf(e->member_re[1], sizeof e->member_re[1], "%s", ws);
    e->max_apparent = (unsigned long long)ma->num << 30;
    e->max_data = (unsigned long long)md->num << 30;
    static const char *const roles[2] = { "gateway", "workstation" };
    for (int i = 0; i < 2; i++) {
        const JVal *m = get(get(s, "machine"), roles[i]), *v = get(m, "vcpus"), *mb = get(m, "memoryMb");
        e->member_vcpus[i] = 1;
        e->member_mem_mb[i] = 2048;
        if (v && (v->type != J_NUM || v->num < 1 || v->num > 64)) return -1;
        if (mb && (mb->type != J_NUM || mb->num < 768 || mb->num > 65536)) return -1;
        if (v) e->member_vcpus[i] = (int)v->num;
        if (mb) e->member_mem_mb[i] = (int)mb->num;
    }
    return 0;
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
        const JVal *pins = get(a, "pinned");
        if (pins) {
            /* files named by the catalog itself (vmcatalog.h): no list, no signature of the maker's */
            if (pins->type != J_ARR) break;
            int bad = 0;
            for (const JVal *p = pins->kid; p && !bad; p = p->next) {
                const char *pn = str(p, "name"), *pu = str(p, "url"), *ps = str(p, "sha256");
                const JVal *pb = get(p, "bytes");
                if (e->npins == VMCAT_PINS || !pn || !vmcat_name_ok(pn) || strlen(pn) >= sizeof e->pin[0].name || !pu || strncmp(pu, "https://", 8) ||
                    strlen(pu) >= sizeof e->pin[0].url || strpbrk(pu, " \"\\<>") || !ps || !vmcat_sha_ok(ps) || !pb || pb->type != J_NUM ||
                    pb->num < 1 || pb->num > (double)MAX_IMAGE_MB * 1024 * 1024) { bad = 1; break; }
                for (int k = 0; k < e->npins; k++) if (!strcmp(e->pin[k].name, pn)) bad = 1;
                int k = e->npins++;
                snprintf(e->pin[k].name, sizeof e->pin[k].name, "%s", pn);
                snprintf(e->pin[k].url, sizeof e->pin[k].url, "%s", pu);
                for (int i = 0; i < 64; i++) e->pin[k].sha[i] = (char)tolower((unsigned char)ps[i]);
                e->pin[k].sha[64] = 0;
                e->pin[k].bytes = (unsigned long long)pb->num;
                if (e->pin[k].bytes > e->max_bytes) e->max_bytes = e->pin[k].bytes;
            }
            if (bad || !e->npins) break;
            rc = 0;
            break;
        }
        const char *dir = str(a, "dir"), *fdir = str(a, "fileDir"), *sums = str(a, "sums"), *sre = str(a, "sumsPattern"), *fre = str(a, "file");
        const JVal *mx = get(a, "maxMb"), *rel = get(a, "releases");
        if (rel) {
            /* the newest verified release folder under releases.dir, never older than releases.floor (vmcatalog.h) */
            const char *rd = str(rel, "dir"), *rp = str(rel, "pattern"), *rf = str(rel, "floor");
            if (dir || fdir || !dir_ok(rd) || !re_ok(rp) || !rf || !vmcat_match(rp, rf) || strlen(rf) >= sizeof e->rel_floor) break;
            snprintf(e->rel_dir, sizeof e->rel_dir, "%s", rd);
            snprintf(e->rel_re, sizeof e->rel_re, "%s", rp);
            snprintf(e->rel_floor, sizeof e->rel_floor, "%s", rf);
            dir = rd;                                /* until a release is chosen (vmcat_release_dir): the folder of releases */
        }
        if (!a || !dir_ok(dir) || (fdir && !dir_ok(fdir)) || !re_ok(fre) || !mx || mx->type != J_NUM || mx->num < 1 || mx->num > (double)MAX_IMAGE_MB) break;
        const char *hs = str(s, "hash");
        if (get(s, "hash") && (!hs || (strcmp(hs, "sha256") && strcmp(hs, "sha512")))) break;
        e->hash = hs && !strcmp(hs, "sha512") ? VMCAT_SHA512 : VMCAT_SHA256;
        const char *kd = str(s, "kind");
        if (kd && !strcmp(kd, "group") && parse_group(s, e)) break;
        if (e->group && (e->hash != VMCAT_SHA512 || !sre)) break;   /* a group's archive: a SHA-512 list found by its pattern */
        if (!!sums == !!sre || (sums && (!vmcat_name_ok(sums) || strlen(sums) >= sizeof e->sums)) || (sre && !re_ok(sre))) break;
        /* the list's signature: every entry has one, with its key file and at least one pinned fingerprint; LordOS's own
         * releases are signed with the LordOS release key, compiled into LordOS, so that entry names no key file */
        const JVal *sg = get(s, "signature"), *fl = get(s, "fingerprints");
        const char *st = str(sg, "type"), *sn = str(sg, "name"), *kf = str(s, "keys");
        if (st && !strcmp(st, "lordos-release")) {
            if (get(s, "keys") || fl || !sn || !vmcat_name_ok(sn) || strlen(sn) >= sizeof e->sig_name) break;
            e->sig_type = VMCAT_SIG_LORDOS;
            snprintf(e->sig_name, sizeof e->sig_name, "%s", sn);
        } else if (!st || !kf || !vmcat_name_ok(kf) || strlen(kf) >= sizeof e->keys || !fl || fl->type != J_ARR) break;
        else if (!strcmp(st, "detached")) {
            /* a fixed name, or the list's own name plus a suffix (".asc": Whonix signs each list beside it) */
            const char *sx = str(sg, "suffix");
            if (!!sn == !!sx) break;
            if (sn && (!vmcat_name_ok(sn) || strlen(sn) >= sizeof e->sig_name)) break;
            if (sx && (strlen(sx) < 2 || strlen(sx) >= sizeof e->sig_suffix || sx[0] != '.' || !vmcat_name_ok(sx + 1))) break;
            e->sig_type = VMCAT_SIG_DETACHED;
            snprintf(e->sig_name, sizeof e->sig_name, "%s", sn ? sn : "");
            snprintf(e->sig_suffix, sizeof e->sig_suffix, "%s", sx ? sx : "");
        } else if (!strcmp(st, "clearsigned") && !sn) e->sig_type = VMCAT_SIG_CLEAR;
        else break;
        int bad = 0;
        for (const JVal *f = e->sig_type == VMCAT_SIG_LORDOS ? NULL : fl->kid; f && !bad; f = f->next) {
            if (f->type != J_STR || strlen(f->str) != 40 || e->nfprs == VMCAT_FPRS) { bad = 1; break; }
            for (int i = 0; i < 40; i++) {
                if (!isxdigit((unsigned char)f->str[i])) { bad = 1; break; }
                e->fprs[e->nfprs][i] = (char)toupper((unsigned char)f->str[i]);
            }
            e->fprs[e->nfprs++][40] = 0;
        }
        if (bad || (!e->nfprs && e->sig_type != VMCAT_SIG_LORDOS)) break;
        /* the list mirrored on lordos.org (vmcatalog.h): only for a LordOS release with a fixed list name, only under
         * LIST_MIRROR, and with all three names */
        const JVal *lm = get(a, "list");
        if (lm) {
            const char *ld = str(lm, "dir"), *ls = str(lm, "sums"), *lg = str(lm, "signature");
            if (lm->type != J_OBJ || e->sig_type != VMCAT_SIG_LORDOS || !sums || !dir_ok(ld) || strncmp(ld, LIST_MIRROR, strlen(LIST_MIRROR)) ||
                strlen(ld) <= strlen(LIST_MIRROR) || !ls || !vmcat_name_ok(ls) || strlen(ls) >= sizeof e->list_sums ||
                !lg || !vmcat_name_ok(lg) || strlen(lg) >= sizeof e->list_sig) break;
            snprintf(e->list_dir, sizeof e->list_dir, "%s", ld);
            snprintf(e->list_sums, sizeof e->list_sums, "%s", ls);
            snprintf(e->list_sig, sizeof e->list_sig, "%s", lg);
        }
        snprintf(e->keys, sizeof e->keys, "%s", kf ? kf : "");
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

int vmcat_list_source(const VmCatEntry *e, const char *name, int i, char *list, char *sig, size_t n) {
    if (!e || !list || !sig || !n || !vmcat_name_ok(name) || i < 0) return -1;
    list[0] = sig[0] = 0;
    int mirror = e->list_dir[0] && e->sums[0] && !strcmp(name, e->sums);
    if (mirror && i == 0) {
        if (snprintf(list, n, "%s%s", e->list_dir, e->list_sums) >= (int)n || snprintf(sig, n, "%s%s", e->list_dir, e->list_sig) >= (int)n) return -1;
        return 0;
    }
    if (i != mirror || !e->dir[0]) return -1;
    if (snprintf(list, n, "%s%s", e->dir, name) >= (int)n) return -1;
    if (e->sig_suffix[0]) { if (snprintf(sig, n, "%s%s%s", e->dir, name, e->sig_suffix) >= (int)n) return -1; }
    else if ((e->sig_type == VMCAT_SIG_DETACHED || e->sig_type == VMCAT_SIG_LORDOS) && snprintf(sig, n, "%s%s", e->dir, e->sig_name) >= (int)n) return -1;
    return 0;
}

/* one line, without its end (\n, \r\n); sha is 64 or 128 hex digits (SHA-256 or SHA-512) */
static int line_has(const char *l, size_t n, const char *name, const char *sha) {
    size_t nl = strlen(name), hl = strlen(sha);
    const char *bsd = hl == 128 ? "SHA512 (" : "SHA256 (";
    /* GNU: <hash>  <name> | <hash> *<name> */
    if (n == hl + 2 + nl && !strncasecmp(l, sha, hl) && l[hl] == ' ' && (l[hl + 1] == ' ' || l[hl + 1] == '*') && !memcmp(l + hl + 2, name, nl)) return 1;
    /* BSD: SHA256 (<name>) = <hash> */
    if (n == 8 + nl + 4 + hl && !memcmp(l, bsd, 8) && !memcmp(l + 8, name, nl) && !memcmp(l + 8 + nl, ") = ", 4) &&
        !strncasecmp(l + 12 + nl, sha, hl)) return 1;
    return 0;
}

int vmcat_pin_find(const VmCatEntry *e, const char *name, const char *sha) {
    if (!e || !name || !sha || !vmcat_sha_ok(sha)) return -1;
    for (int k = 0; k < e->npins; k++) if (!strcmp(e->pin[k].name, name) && !strcasecmp(e->pin[k].sha, sha)) return k;
    return -1;
}

int vmcat_sums_has(const char *sums, size_t n, const char *name, const char *sha) {
    VmCatEntry e512 = { .hash = VMCAT_SHA512 };
    if (!sums || !vmcat_name_ok(name) || (!vmcat_sha_ok(sha) && !vmcat_hash_ok(&e512, sha))) return 0;
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
    if (e && e->sig_type == VMCAT_SIG_LORDOS) {
        /* LordOS's own release: SHA256SUMS.sig, base64 of the Ed25519 signature over the list's exact bytes, checked by
         * the code that checks the LordOS Imager's official images (imgverify.c) with the key the helper passes: the
         * compiled-in release key (codesig_release_key) */
        if (!keys || keys_len != 32 || !list) { lerr_set(err, errlen, "imager.badSignature", NULL); return -1; }
        if (!sig) return 0;
        if (img_sig_verify((const uint8_t *)list, list_len, sig, sig_len, (const uint8_t *)keys)) { lerr_set(err, errlen, "imager.badSignature", NULL); return -1; }
        if (!(*body = malloc(list_len + 1))) return -1;
        memcpy(*body, list, list_len);
        (*body)[list_len] = 0;
        *body_len = list_len;
        return 1;
    }
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
