/* vmcatalog_test.c — the download catalog's checks (src/vm/vmcatalog.c) as the admin helper makes them: every entry of
 * the real assets/machines/systems.json for both architectures, names against their patterns (and what must never
 * pass: paths, other files, other architectures), the makers' list forms, and catalogs that must be refused.
 *
 *   build/vmcatalog-test ../assets/machines/systems.json */
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
    const char *good = "{\"systems\":[{\"id\":\"x\",\"arch\":{\"amd64\":{\"dir\":\"https://a.example/r/\",\"sums\":\"S\",\"file\":\"^a\\\\.iso$\",\"maxMb\":10}}}]}";
    T("good catalog", !vmcat_find(good, strlen(good), "x", "amd64", &e) && !strcmp(e.file_dir, "https://a.example/r/") && e.max_bytes == 10ull << 20);
    T("pattern needs a whole name", !vmcat_match(e.file_re, "a.iso2") && vmcat_match(e.file_re, "a.iso"));

    printf("== vmcatalog-test: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
