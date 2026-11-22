/* vmsig.c — the OpenPGP check of a person's system (vmsig.h), with LordOS's own OpenPGP code (pkg/pgp.c). */
#include "vmsig.h"
#include "../pkg/pgp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int hexc(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int vmsig_fpr_ok(const char *s) {
    if (!s || strlen(s) != 40) return 0;
    for (int i = 0; i < 40; i++) if (!hexc(s[i])) return 0;
    return 1;
}

static const char *const REASONS[] = { VMSIG_REASONS, NULL };

static void reason(char *why, size_t n, const char *r) { if (n) snprintf(why, n, "%s", r); }

/* pgp.c's code ("pkg.pgp.keyExpired key=…") as one of VMSIG_REASONS */
static const char *from_pgp(const char *err) {
    static const struct { const char *code, *reason; } map[] = {
        { "pkg.pgp.keyExpired", "keyExpired" }, { "pkg.pgp.keyRevoked", "keyRevoked" },
        { "pkg.pgp.keyNotSigning", "keyNotSigning" }, { "pkg.pgp.signatureExpired", "signatureExpired" },
        { "pkg.pgp.futureSignature", "futureSignature" }, { "pkg.pgp.unknownKeys", "otherKey" },
        { "pkg.pgp.malformed", "malformed" }, { "pkg.pgp.badEncoding", "malformed" },
    };
    size_t l = strcspn(err, " ");
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strlen(map[i].code) == l && !strncmp(err, map[i].code, l)) return map[i].reason;
    return "badSignature";
}

int vmsig_verify(const char *key, size_t kl, const char *sums, size_t sl, const char *sig, size_t gl,
                 const char *fpr, int64_t now, char out_fpr[41], char *why, size_t whyn) {
    out_fpr[0] = 0;
    reason(why, whyn, "");
    if (!vmsig_fpr_ok(fpr)) { reason(why, whyn, "unsupported"); return 0; }   /* v6 (64 hex) isn't read by pgp.c */
    if (!key || !kl) { reason(why, whyn, "noKey"); return 0; }
    if (kl > VMSIG_KEY_MAX || sl > VMSIG_SUMS_MAX || gl > VMSIG_SIG_MAX) { reason(why, whyn, "tooLarge"); return 0; }
    if (!sums || !sig || !gl) { reason(why, whyn, "malformed"); return 0; }
    PgpKeyring kr = { 0 };
    int ok = 0;
    if (pgp_keyring_add_untrusted(&kr, key, kl, now)) reason(why, whyn, "noKey");
    else {
        const char *fp[1] = { fpr };
        /* only that primary key (and the subkeys it really bound, with their back-signatures) may sign: any other key
         * in the block, and any subkey grafted under it, is dropped */
        if (pgp_keyring_pin(&kr, fp, 1) != 1) reason(why, whyn, "noKey");
        else {
            char err[200] = "";
            int g = pgp_verify_detached(&kr, sums, sl, sig, gl, now, err, sizeof err);
            if (g > 0) {
                for (int i = 0; i < 40; i++) out_fpr[i] = (char)(fpr[i] >= 'a' && fpr[i] <= 'f' ? fpr[i] - 32 : fpr[i]);
                out_fpr[40] = 0;
                ok = 1;
            } else reason(why, whyn, err[0] ? from_pgp(err) : "badSignature");
        }
    }
    pgp_keyring_free(&kr);
    return ok;
}

void vmsig_answer(int ok, const char *fpr_or_reason, char *out, size_t n) {
    snprintf(out, n, "%s %s\n", ok ? "valid" : "invalid", fpr_or_reason && *fpr_or_reason ? fpr_or_reason : "badSignature");
}

int vmsig_reply(const char *out, size_t n, const char *fpr, char *why, size_t whyn) {
    reason(why, whyn, "badSignature");
    if (!out || n < 2 || n > 80 || out[n - 1] != '\n' || memchr(out, 0, n)) return -1;
    char line[81];
    memcpy(line, out, n - 1);
    line[n - 1] = 0;
    if (strchr(line, '\n')) return -1;
    if (!strncmp(line, "valid ", 6)) {
        const char *f = line + 6;
        if (!vmsig_fpr_ok(f) || !vmsig_fpr_ok(fpr) || strcasecmp(f, fpr)) return -1;
        for (int i = 0; i < 40; i++) if (f[i] >= 'a' && f[i] <= 'f') return -1;   /* the helper writes upper case */
        reason(why, whyn, "");
        return 1;
    }
    if (!strncmp(line, "invalid ", 8)) {
        for (int i = 0; REASONS[i]; i++)
            if (!strcmp(line + 8, REASONS[i])) { reason(why, whyn, REASONS[i]); return 0; }
    }
    return -1;
}

static int line_hash(const char *l, size_t n, const char *sha) {
    /* GNU: <64 hex> then ' ' and ' ' or '*', then a name */
    if (n > 66 && (l[64] == ' ') && (l[65] == ' ' || l[65] == '*')) {
        int hex = 1;
        for (int i = 0; i < 64 && hex; i++) hex = hexc(l[i]);
        if (hex) return !strncasecmp(l, sha, 64);
    }
    /* BSD: SHA256 (<name>) = <64 hex> */
    if (n > 7 + 1 + 4 + 64 && !strncmp(l, "SHA256 (", 8) && !memcmp(l + n - 64 - 4, ") = ", 4)) {
        const char *h = l + n - 64;
        for (int i = 0; i < 64; i++) if (!hexc(h[i])) return 0;
        return !strncasecmp(h, sha, 64);
    }
    return 0;
}

int vmsig_sums_has_hash(const char *sums, size_t n, const char *sha) {
    if (!sums || !sha || strlen(sha) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!hexc(sha[i])) return 0;
    for (size_t i = 0; i < n;) {
        const char *nl = memchr(sums + i, '\n', n - i);
        size_t end = nl ? (size_t)(nl - sums) : n, len = end - i;
        if (len && sums[i + len - 1] == '\r') len--;
        if (!memchr(sums + i, 0, len) && line_hash(sums + i, len, sha)) return 1;
        i = end + 1;
    }
    return 0;
}
