/* vmcatalog_test.c — the download catalog's checks (src/vm/vmcatalog.c) as the admin helper makes them: every entry of
 * the real assets/machines/systems.json for both architectures, names against their patterns (and what must never
 * pass: paths, other files, other architectures), the makers' list forms, and catalogs that must be refused.
 *
 * Then the makers' signatures (vmcat_verify_list, pkg/pgp.c): the lists and signatures as published on 2026-11-06
 * (tests/vmimage) with the keys the catalog pins (packaging/keys/machines), and what must be refused: changed lists, a
 * missing signature, another list's signature, another maker's key, a key file that lacks a pinned key, an expired key,
 * a signature from the future, a clear-signed list with text added outside its signature.
 *
 *   build/vmcatalog-test ../assets/machines/systems.json tests/vmimage ../packaging/keys/machines */
#include "../src/vm/vmcatalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
#define T(name, cond) do { if (cond) pass++; else { fail++; printf("FAIL  %s\n", name); } } while (0)

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char buf[1 << 20];
    *n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[*n] = 0;
    return buf;
}
/* a file of dir into a new buffer (malloc), or NULL */
static char *load(const char *dir, const char *name, size_t *n) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = malloc(1 << 20);
    *n = b ? fread(b, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (b) b[*n] = 0;
    return b;
}

#define NOW   1794009600LL       /* 2026-11-07 */
#define Y2029 1840752000LL       /* 2028-05-01, after the Kali key's end (2028-04-17) */
#define Y2026 1767225600LL       /* 2026-01-01, before every signature here */

static int verify(const VmCatEntry *e, const char *keys, size_t kn, const char *list, size_t ln, const char *sig, size_t sn,
                  long long now, char *why, size_t wn) {
    char *body = NULL;
    size_t bl = 0;
    int r = vmcat_verify_list(e, keys, kn, list, ln, sig, sn, now, &body, &bl, why, wn);
    if (r == 1 && (!body || !bl)) r = 9;
    free(body);
    return r;
}

static void signatures(const char *cat, size_t n, const char *fx, const char *kd) {
    static const struct { const char *id, *arch, *list, *sig; } cases[] = {
        { "debian", "amd64", "debian-amd64.sums", "debian-amd64.sig" }, { "debian", "arm64", "debian-arm64.sums", "debian-arm64.sig" },
        { "ubuntu", "amd64", "ubuntu-amd64.sums", "ubuntu-amd64.sig" }, { "ubuntu", "arm64", "ubuntu-arm64.sums", "ubuntu-arm64.sig" },
        { "kali", "amd64", "kali.sums", "kali.sig" }, { "kali", "arm64", "kali.sums", "kali.sig" },
        { "fedora", "amd64", "fedora-amd64.checksum", NULL }, { "fedora", "arm64", "fedora-arm64.checksum", NULL },
    };
    char lab[160], why[200];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        VmCatEntry e;
        size_t ln = 0, sn = 0, kn = 0;
        if (vmcat_find(cat, n, cases[i].id, cases[i].arch, &e)) { T("signature: entry", 0); continue; }
        char *list = load(fx, cases[i].list, &ln), *sig = cases[i].sig ? load(fx, cases[i].sig, &sn) : NULL, *keys = load(kd, e.keys, &kn);
        snprintf(lab, sizeof lab, "%s %s: fixtures and key file", cases[i].id, cases[i].arch);
        T(lab, list && keys && (!cases[i].sig || sig));
        if (!list || !keys) { free(list); free(sig); free(keys); continue; }
        snprintf(lab, sizeof lab, "%s %s: %s", cases[i].id, cases[i].arch, e.sig_type == VMCAT_SIG_CLEAR ? "clear-signed" : "detached");
        T(lab, (e.sig_type == VMCAT_SIG_CLEAR) == !cases[i].sig);
        snprintf(lab, sizeof lab, "%s %s: the published list verifies", cases[i].id, cases[i].arch);
        int r = verify(&e, keys, kn, list, ln, sig, sn, NOW, why, sizeof why);
        T(lab, r == 1);
        if (r != 1) printf("      %s\n", why);
        if (e.sig_type == VMCAT_SIG_CLEAR) {
            char *body = NULL;
            size_t bl = 0;
            vmcat_verify_list(&e, keys, kn, list, ln, NULL, 0, NOW, &body, &bl, why, sizeof why);
            snprintf(lab, sizeof lab, "%s %s: only the signed text is kept", cases[i].id, cases[i].arch);
            T(lab, body && strstr(body, "SHA256 (") && !strstr(body, "BEGIN PGP") && !strstr(body, "Hash: "));
            free(body);
        }
        /* one hash digit changed */
        char *ch = malloc(ln + 1);
        memcpy(ch, list, ln + 1);
        char *d = strstr(ch, "SHA256 (") ? strstr(strstr(ch, "SHA256 ("), ") = ") + 4 : ch;
        *d = *d == '0' ? '1' : '0';
        snprintf(lab, sizeof lab, "%s %s: a changed hash is refused", cases[i].id, cases[i].arch);
        r = verify(&e, keys, kn, ch, ln, sig, sn, NOW, why, sizeof why);
        T(lab, r == -1 && strstr(why, "pkg.pgp.badSignature"));
        /* a line added */
        char *add = malloc(ln + 200);
        int al;
        if (e.sig_type == VMCAT_SIG_CLEAR) {
            const char *at = strstr(list, "-----BEGIN PGP SIGNATURE-----");
            al = snprintf(add, ln + 200, "%.*sSHA256 (evil.iso) = %064d\n%s", (int)(at - list), list, 0, at);
        } else al = snprintf(add, ln + 200, "%s%064d  evil.iso\n", list, 0);
        snprintf(lab, sizeof lab, "%s %s: a line added is refused", cases[i].id, cases[i].arch);
        T(lab, verify(&e, keys, kn, add, (size_t)al, sig, sn, NOW, why, sizeof why) == -1);
        if (e.sig_type == VMCAT_SIG_CLEAR) {
            al = snprintf(add, ln + 200, "%s%064d  evil.iso\n", list, 0);
            snprintf(lab, sizeof lab, "%s %s: text after the signature is refused", cases[i].id, cases[i].arch);
            T(lab, verify(&e, keys, kn, add, (size_t)al, NULL, 0, NOW, why, sizeof why) == -1);
            const char *body = strstr(list, "\n\n") + 2;
            snprintf(lab, sizeof lab, "%s %s: the list without its signature is unsigned", cases[i].id, cases[i].arch);
            T(lab, verify(&e, keys, kn, body, strlen(body), NULL, 0, NOW, why, sizeof why) == 0);
        } else {
            snprintf(lab, sizeof lab, "%s %s: no signature file is unsigned", cases[i].id, cases[i].arch);
            T(lab, verify(&e, keys, kn, list, ln, NULL, 0, NOW, why, sizeof why) == 0);
            snprintf(lab, sizeof lab, "%s %s: an empty signature is refused", cases[i].id, cases[i].arch);
            T(lab, verify(&e, keys, kn, list, ln, "", 0, NOW, why, sizeof why) == -1);
            snprintf(lab, sizeof lab, "%s %s: garbage for a signature is refused", cases[i].id, cases[i].arch);
            T(lab, verify(&e, keys, kn, list, ln, "-----BEGIN PGP SIGNATURE-----\n\n!!!\n-----END PGP SIGNATURE-----\n", 52, NOW, why, sizeof why) == -1);
        }
        /* the signature with another maker's key: Kali's for everyone, Debian's for Kali */
        VmCatEntry o;
        vmcat_find(cat, n, strcmp(cases[i].id, "kali") ? "kali" : "debian", "amd64", &o);
        size_t okn = 0;
        char *okeys = load(kd, o.keys, &okn);
        snprintf(lab, sizeof lab, "%s %s: another maker's pinned key is refused", cases[i].id, cases[i].arch);
        T(lab, okeys && verify(&o, okeys, okn, list, ln, sig, sn, NOW, why, sizeof why) != 1);
        /* the right fingerprints with a key file that lacks them */
        snprintf(lab, sizeof lab, "%s %s: a key file without the pinned key is refused", cases[i].id, cases[i].arch);
        T(lab, okeys && verify(&e, okeys, okn, list, ln, sig, sn, NOW, why, sizeof why) == -1 && strstr(why, "pkg.pgp.unknownKeys"));
        /* the right key file, but another fingerprint pinned: its key may not sign */
        VmCatEntry w = e;
        snprintf(w.fprs[0], sizeof w.fprs[0], "%s", o.fprs[0]);
        snprintf(lab, sizeof lab, "%s %s: a key that isn't pinned can't sign", cases[i].id, cases[i].arch);
        T(lab, verify(&w, keys, kn, list, ln, sig, sn, NOW, why, sizeof why) == -1);
        snprintf(lab, sizeof lab, "%s %s: a signature from the future is refused", cases[i].id, cases[i].arch);
        T(lab, verify(&e, keys, kn, list, ln, sig, sn, Y2026, why, sizeof why) != 1);
        snprintf(lab, sizeof lab, "%s %s: an armoured key file that isn't one is refused", cases[i].id, cases[i].arch);
        T(lab, verify(&e, "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nAAAA\n-----END PGP PUBLIC KEY BLOCK-----\n", 72, list, ln, sig, sn, NOW, why, sizeof why) == -1);
        if (!strcmp(cases[i].id, "kali")) {
            snprintf(lab, sizeof lab, "kali %s: after its key's end the list is refused", cases[i].arch);
            T(lab, verify(&e, keys, kn, list, ln, sig, sn, Y2029, why, sizeof why) != 1 && strstr(why, "pkg.pgp.keyExpired"));
        }
        free(okeys); free(add); free(ch); free(list); free(sig); free(keys);
    }
    /* Debian's amd64 signature over its arm64 list */
    VmCatEntry e;
    size_t a = 0, b = 0, k = 0;
    vmcat_find(cat, n, "debian", "arm64", &e);
    char *list = load(fx, "debian-arm64.sums", &a), *sig = load(fx, "debian-amd64.sig", &b), *keys = load(kd, e.keys, &k);
    T("another list's signature is refused", list && sig && keys && verify(&e, keys, k, list, a, sig, b, NOW, why, sizeof why) == -1);
    free(list); free(sig); free(keys);
}

static const char *const SHA = "a7ef94ac2fb9a7fec454552abd629b7cc9d5155c886165a45649f5ce6167e355";

int main(int argc, char **argv) {
    size_t n = 0;
    char *cat = argc > 1 ? slurp(argv[1], &n) : NULL;
    T("catalog read", cat && n > 100);
    if (!cat) { printf("== vmcatalog-test: no catalog\n"); return 1; }

    static const char *const ids[] = { "debian", "ubuntu", "fedora", "kali" };
    static const char *const arches[] = { "amd64", "arm64" };
    /* a real name of each entry for each architecture (seen in the makers' lists on 2026-11-06) */
    static const char *const names[4][2] = {
        { "debian-13.7.0-amd64-netinst.iso", "debian-13.7.0-arm64-netinst.iso" },
        { "ubuntu-26.04.1-desktop-amd64.iso", "ubuntu-26.04.1-desktop-arm64.iso" },
        { "Fedora-Workstation-Live-45-1.4.x86_64.iso", "Fedora-Workstation-Live-45-1.4.aarch64.iso" },
        { "kali-linux-2026.3-installer-amd64.iso", "kali-linux-2026.3-installer-arm64.iso" },
    };
    for (int i = 0; i < 4; i++)
        for (int a = 0; a < 2; a++) {
            char lab[96];
            VmCatEntry e;
            snprintf(lab, sizeof lab, "%s %s: found", ids[i], arches[a]);
            int ok = !vmcat_find(cat, n, ids[i], arches[a], &e);
            T(lab, ok);
            if (!ok) continue;
            snprintf(lab, sizeof lab, "%s %s: https folder", ids[i], arches[a]);
            T(lab, !strncmp(e.dir, "https://", 8) && !strncmp(e.file_dir, "https://", 8));
            snprintf(lab, sizeof lab, "%s %s: one kind of list", ids[i], arches[a]);
            T(lab, !!e.sums[0] != !!e.sums_re[0]);
            snprintf(lab, sizeof lab, "%s %s: the real name matches", ids[i], arches[a]);
            T(lab, vmcat_match(e.file_re, names[i][a]));
            snprintf(lab, sizeof lab, "%s %s: the other architecture's doesn't", ids[i], arches[a]);
            T(lab, !vmcat_match(e.file_re, names[i][!a]));
            snprintf(lab, sizeof lab, "%s %s: a path doesn't", ids[i], arches[a]);
            char evil[200];
            snprintf(evil, sizeof evil, "../%s", names[i][a]);
            T(lab, !vmcat_match(e.file_re, evil));
            snprintf(evil, sizeof evil, "%s.torrent", names[i][a]);
            snprintf(lab, sizeof lab, "%s %s: its .torrent doesn't", ids[i], arches[a]);
            T(lab, !vmcat_match(e.file_re, evil));
            snprintf(lab, sizeof lab, "%s %s: a size limit", ids[i], arches[a]);
            T(lab, e.max_bytes >= (1ull << 30) && e.max_bytes <= (64ull << 30));
        }
    VmCatEntry e;
    T("unknown id", vmcat_find(cat, n, "nothing", "amd64", &e));
    T("unknown arch", vmcat_find(cat, n, "debian", "riscv64", &e));
    T("fedora: its list's name", !vmcat_find(cat, n, "fedora", "amd64", &e) && vmcat_match(e.sums_re, "Fedora-Workstation-45-1.4-x86_64-CHECKSUM") &&
      !vmcat_match(e.sums_re, "Fedora-Workstation-Live-45-1.4.x86_64.iso") && !vmcat_match(e.sums_re, "Fedora-Workstation-45-1.4-aarch64-CHECKSUM"));
    T("fedora: images from the mirrors", !strncmp(e.file_dir, "https://download.fedoraproject.org/", 35));
    T("debian: SHA256SUMS", !vmcat_find(cat, n, "debian", "arm64", &e) && !strcmp(e.sums, "SHA256SUMS") && !e.sums_re[0]);
    T("debian: not the edu or mac image", !vmcat_match(e.file_re, "debian-edu-13.7.0-arm64-netinst.iso") && !vmcat_match(e.file_re, "debian-mac-13.7.0-amd64-netinst.iso"));
    T("ubuntu: 26.04 and 26.04.1", !vmcat_find(cat, n, "ubuntu", "amd64", &e) && vmcat_match(e.file_re, "ubuntu-26.04-desktop-amd64.iso") &&
      vmcat_match(e.file_re, "ubuntu-26.04.1-desktop-amd64.iso") && !vmcat_match(e.file_re, "ubuntu-26.04.1-live-server-amd64.iso"));

    /* names */
    T("name: plain", vmcat_name_ok("debian-13.7.0-amd64-netinst.iso"));
    T("name: slash", !vmcat_name_ok("a/b.iso"));
    T("name: dot first", !vmcat_name_ok(".iso"));
    T("name: dash first", !vmcat_name_ok("-o.iso"));
    T("name: space", !vmcat_name_ok("a b.iso"));
    T("name: empty", !vmcat_name_ok(""));
    char longn[200];
    memset(longn, 'a', 150); longn[150] = 0;
    T("name: too long", !vmcat_name_ok(longn));

    /* the makers' lists */
    char list[1024];
    snprintf(list, sizeof list, "%s  debian-13.7.0-amd64-netinst.iso\n%s  debian-edu-13.7.0-amd64-netinst.iso\n", SHA,
             "d33ed0c64bde5ff43445c0c530f9da599954d8585771c55345cb840d7997d4a7");
    T("gnu: listed", vmcat_sums_has(list, strlen(list), "debian-13.7.0-amd64-netinst.iso", SHA));
    char up[65];
    for (int i = 0; i < 64; i++) up[i] = (char)(SHA[i] >= 'a' ? SHA[i] - 32 : SHA[i]);
    up[64] = 0;
    T("gnu: hash in capitals", vmcat_sums_has(list, strlen(list), "debian-13.7.0-amd64-netinst.iso", up));
    T("gnu: another file's hash", !vmcat_sums_has(list, strlen(list), "debian-edu-13.7.0-amd64-netinst.iso", SHA));
    T("gnu: a name that is a prefix", !vmcat_sums_has(list, strlen(list), "debian-13.7.0-amd64-netinst.is", SHA));
    T("gnu: not listed", !vmcat_sums_has(list, strlen(list), "debian-13.8.0-amd64-netinst.iso", SHA));
    snprintf(list, sizeof list, "%s *ubuntu-26.04.1-desktop-amd64.iso\r\n", SHA);
    T("gnu binary mark, CRLF", vmcat_sums_has(list, strlen(list), "ubuntu-26.04.1-desktop-amd64.iso", SHA));
    snprintf(list, sizeof list, "-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\n# Fedora-Workstation-Live-45-1.4.x86_64.iso: 2925092864 bytes\n"
             "SHA256 (Fedora-Workstation-Live-45-1.4.x86_64.iso) = %s\n-----BEGIN PGP SIGNATURE-----\n", SHA);
    T("bsd: listed", vmcat_sums_has(list, strlen(list), "Fedora-Workstation-Live-45-1.4.x86_64.iso", SHA));
    T("bsd: other name", !vmcat_sums_has(list, strlen(list), "Fedora-Workstation-Live-45-1.4.aarch64.iso", SHA));
    snprintf(list, sizeof list, "%s  x.iso extra\n %s  x.iso\n%s\tx.iso\n", SHA, SHA, SHA);
    T("odd lines refused", !vmcat_sums_has(list, strlen(list), "x.iso", SHA));
    T("bad hash refused", !vmcat_sums_has(list, strlen(list), "x.iso", "zz"));
    char nul[200];
    int k = snprintf(nul, sizeof nul, "%s  x.iso", SHA);
    nul[3] = 0;
    T("NUL in the line", !vmcat_sums_has(nul, (size_t)k, "x.iso", SHA));
    T("empty list", !vmcat_sums_has("", 0, "x.iso", SHA));

    /* catalogs that must be refused */
    static const char *const badcats[] = {
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"http://a.example/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/x?y/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/../\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"S\",\"file\":\"a\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"S\",\"file\":\"^a(\\\\$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"S\",\"sumsPattern\":\"^S$\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"../S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":0}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"sums\":\"S\",\"file\":\"^a$\"}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/\",\"fileDir\":\"ftp://b/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://user@a.example/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "not json",
    };
    for (size_t i = 0; i < sizeof badcats / sizeof badcats[0]; i++) {
        char lab[48];
        snprintf(lab, sizeof lab, "bad catalog %zu refused", i);
        T(lab, vmcat_find(badcats[i], strlen(badcats[i]), "x", "amd64", &e) && !e.dir[0]);
    }
    static const char *const badsig[] = {
        "{\"systems\":[{\"id\":\"x\",\"keys\":\"k.asc\",\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"detached\"},\"keys\":\"k.asc\",\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"detached\",\"name\":\"../S.sig\"},\"keys\":\"k.asc\",\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"none\"},\"keys\":\"k.asc\",\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"clearsigned\"},\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"clearsigned\"},\"keys\":\"/etc/k.asc\",\"fingerprints\":[\"DF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"clearsigned\"},\"keys\":\"k.asc\",\"fingerprints\":[],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"clearsigned\"},\"keys\":\"k.asc\",\"fingerprints\":[\"6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
        "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"clearsigned\"},\"keys\":\"k.asc\",\"fingerprints\":[\"ZF9B9C49EAA9298432589D76DA87E80D6294BE9B\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a$\",\"maxMb\":10}}}]}",
    };
    for (size_t i = 0; i < sizeof badsig / sizeof badsig[0]; i++) {
        char lab[48];
        snprintf(lab, sizeof lab, "bad signature entry %zu refused", i);
        T(lab, vmcat_find(badsig[i], strlen(badsig[i]), "x", "amd64", &e) && !e.dir[0] && !e.nfprs);
    }
    const char *good = "{\"systems\":[{\"id\":\"x\",\"signature\":{\"type\":\"detached\",\"name\":\"S.sig\"},\"keys\":\"k.asc\",\"fingerprints\":[\"df9b9c49eaa9298432589d76da87e80d6294be9b\"],\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a\\\\.iso$\",\"maxMb\":10}}}]}";
    T("good catalog", !vmcat_find(good, strlen(good), "x", "amd64", &e) && !strcmp(e.file_dir, "https://a.example/r/") && e.max_bytes == 10ull << 20 &&
      e.sig_type == VMCAT_SIG_DETACHED && !strcmp(e.sig_name, "S.sig") && !strcmp(e.keys, "k.asc") && e.nfprs == 1 &&
      !strcmp(e.fprs[0], "DF9B9C49EAA9298432589D76DA87E80D6294BE9B"));
    T("pattern needs a whole name", !vmcat_match(e.file_re, "a.iso2") && vmcat_match(e.file_re, "a.iso"));

    if (argc > 3) signatures(cat, n, argv[2], argv[3]); else T("signature fixtures given", 0);

    printf("== vmcatalog-test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
