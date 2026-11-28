/* vmexport_test.c — Export's manifest (build, JSON, strict parse, verify against hostile folders, a mutation fuzz
 * pass), the OVF descriptor and .mf, and the tar writer read back by the system's tar and by a parser of its own
 * (a pax 'x' header for a long name and for a 9 GiB sparse member). Under ASan and UBSan; scratch data in
 * /var/tmp/lordos-vmexport-test/, removed at the end. */
#include "../src/vm/vmexport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char root[256], mdir[300];

static void put(const char *rel, const void *p, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", mdir, rel);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, p, n) != (ssize_t)n) { perror(path); exit(2); }
    close(fd);
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *p = NULL;
    size_t cap = 0, k = 0, r;
    do {
        if (k + 65536 > cap) { cap = cap * 2 + 65536; p = realloc(p, cap + 1); }
        r = fread(p + k, 1, 65536, f);
        k += r;
    } while (r);
    fclose(f);
    p[k] = 0;
    *n = k;
    return p;
}

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int sh(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

/* ---- paths ---- */

static void test_paths(void) {
    static const char *const good[] = {
        "machine.json", "snapshots.json", "cpu-usage.json", "firmware-code.fd", "disks/system.qcow2", "disks/system.raw",
        "disks/data-1.qcow2", "disks/data-4.raw", "disks/nvram.qcow2", "disks/tpm/tpm2-00.permall",
        "disks/tpm/snap-s1.permall", "disks/tpm/snap-s20261124-1530-ab.permall",
        "disks/tpm/snap-s1234567890123456789012345678901.permall",           /* a 32-byte tag */
    };
    static const char *const bad[] = {
        "", "../x", "/etc/passwd", "disks/../machine.json", "disks/tpm/snap-../x", "disks//system.qcow2",
        "./machine.json", ".machine.json", "disks/.system.qcow2", "disks/data-5.qcow2", "disks/data-0.raw",
        "disks/data-1.qcow2/", "disks/data-12.qcow2", "disks/system.vmdk", "manifest.json", "disks", "disks/tpm",
        "disks/tpm/snap-.permall", "disks/tpm/snap-s.permall", "disks/tpm/snap-x1.permall", "disks/tpm/snap-S1.permall",
        "disks/tpm/snap-s1_a.permall", "disks/tpm/snap-s12345678901234567890123456789012.permall",
        "disks/tpm/snap-s1.permall.new", "disks/tpm/snap-s1/x.permall", "machine.json\n", "Machine.json",
        "disks/system.qcow2 ", "disks/tpm/snap-s\xc3\xa9.permall", "m\xc3\xa1quina.json", "disks/thumb.png",
        "log/lord-vm.log", "disks/tpm/tpm2-00.permall~", "machine.json/",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) { CHECK(vmx_path_ok(good[i])); if (!vmx_path_ok(good[i])) fprintf(stderr, "  good: %s\n", good[i]); }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { CHECK(!vmx_path_ok(bad[i])); if (vmx_path_ok(bad[i])) fprintf(stderr, "  bad: %s\n", bad[i]); }
    CHECK(!vmx_path_ok(NULL));
    char longp[200];
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = 0;
    CHECK(!vmx_path_ok(longp));

    CHECK(vmx_name_ok("Debian 13") && vmx_name_ok("M\xc3\xa1quina \xe2\x9c\x93") && vmx_name_ok("\xf0\x9f\x90\xa7 penguin"));
    CHECK(!vmx_name_ok("") && !vmx_name_ok("a\nb") && !vmx_name_ok("a\x7f") && !vmx_name_ok("\xc2\x85")   /* C1 NEL */
          && !vmx_name_ok("\xc0\xaf") && !vmx_name_ok("\xed\xa0\x80") && !vmx_name_ok("\xf4\x90\x80\x80")
          && !vmx_name_ok("\xe2\x9c") && !vmx_name_ok("\xff"));
    char n65[66];
    memset(n65, 'n', 65);
    n65[65] = 0;
    CHECK(!vmx_name_ok(n65));
    n65[64] = 0;
    CHECK(vmx_name_ok(n65));
    CHECK(vmx_uuid_ok("0f8fad5b-d9cb-469f-a165-70867728950e"));
    CHECK(!vmx_uuid_ok("0F8FAD5B-D9CB-469F-A165-70867728950E") && !vmx_uuid_ok("0f8fad5bd9cb469fa16570867728950e")
          && !vmx_uuid_ok("0f8fad5b-d9cb-469f-a165-70867728950e0") && !vmx_uuid_ok("0f8fad5b-d9cb-469f-a165-70867728950"));
    CHECK(vmx_created_ok("2026-11-24T10:20:30Z"));
    CHECK(!vmx_created_ok("2026-13-24T10:20:30Z") && !vmx_created_ok("2026-11-24 10:20:30Z")
          && !vmx_created_ok("2026-11-24T10:20:30") && !vmx_created_ok("2026-11-24T24:00:00Z") && !vmx_created_ok("2026-11-00T10:20:30Z"));
}

/* ---- the manifest ---- */

static const char *const FILES[] = {
    "machine.json", "snapshots.json", "cpu-usage.json", "firmware-code.fd", "disks/system.qcow2", "disks/data-1.raw",
    "disks/nvram.qcow2", "disks/tpm/tpm2-00.permall", "disks/tpm/snap-s1.permall",
};
#define NFILES (int)(sizeof FILES / sizeof *FILES)

static uint64_t last_done, last_total;
static void prog(void *ctx, uint64_t done, uint64_t total) { (void)ctx; CHECK(done <= total); last_done = done; last_total = total; }

static void make_machine(void) {
    snprintf(mdir, sizeof mdir, "%s/Test.lordvm", root);
    sh("rm -rf '%s'", mdir);
    mkdir(mdir, 0755);
    char p[400];
    snprintf(p, sizeof p, "%s/disks", mdir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/disks/tpm", mdir); mkdir(p, 0700);
    put("machine.json", "{\"schema\":14}\n", 14);
    put("snapshots.json", "[]\n", 3);
    put("cpu-usage.json", "", 0);                                 /* an empty file is still a file */
    put("firmware-code.fd", "OVMF", 4);
    size_t big = 3 * 1024 * 1024 + 17;                            /* more than one read chunk */
    unsigned char *b = malloc(big);
    for (size_t i = 0; i < big; i++) b[i] = (unsigned char)(i * 131 + (i >> 9));
    put("disks/system.qcow2", b, big);
    free(b);
    put("disks/data-1.raw", "data", 4);
    put("disks/nvram.qcow2", "nvram", 5);
    put("disks/tpm/tpm2-00.permall", "tpm", 3);
    put("disks/tpm/snap-s1.permall", "tpm-old", 7);
}

static int dirfd_of(const char *p) { return open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC); }

static int verify_now(const VmxManifest *m, char *why, size_t wn) {
    int d = dirfd_of(mdir);
    int r = vmx_manifest_verify(d, m, NULL, NULL, NULL, why, wn);
    close(d);
    return r;
}

static void test_manifest(VmxManifest *m, char **jsonp) {
    make_machine();
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "0f8fad5b-d9cb-469f-a165-70867728950e");
    snprintf(m->name, sizeof m->name, "Debian \"13\" \\ \xc3\xa1");
    snprintf(m->created, sizeof m->created, "2026-11-24T10:20:30Z");
    m->snapshots = 1;
    char why[256] = "";
    int d = dirfd_of(mdir);
    CHECK(vmx_manifest_build(d, FILES, NFILES, m, prog, NULL, why, sizeof why) == 0);
    if (*why) fprintf(stderr, "  build: %s\n", why);
    CHECK(m->n == NFILES && last_done == last_total && last_total == 3 * 1024 * 1024 + 17 + 14 + 3 + 4 + 4 + 5 + 3 + 7);
    /* the empty file's SHA-256 is the well-known one */
    CHECK(!strcmp(m->e[2].sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    /* build refuses: a bad name, a repeat, a symlink, a missing file */
    const char *rep[] = { "machine.json", "machine.json" };
    VmxManifest t = *m;
    CHECK(vmx_manifest_build(d, rep, 2, &t, NULL, NULL, why, sizeof why) == -1);
    const char *hostile[] = { "../x" };
    CHECK(vmx_manifest_build(d, hostile, 1, &t, NULL, NULL, why, sizeof why) == -1);
    const char *missing[] = { "disks/data-2.qcow2" };
    CHECK(vmx_manifest_build(d, missing, 1, &t, NULL, NULL, why, sizeof why) == -1);
    close(d);

    char *json = vmx_manifest_json(m);
    CHECK(json != NULL);
    if (!json) return;
    VmxManifest back;
    CHECK(vmx_manifest_parse(json, strlen(json), &back, why, sizeof why) == 0);
    CHECK(!memcmp(&back, m, sizeof back));
    char *again = vmx_manifest_json(&back);
    CHECK(again && !strcmp(again, json));                         /* stable bytes */
    free(again);
    put("manifest.json", json, strlen(json));
    *jsonp = json;

    /* verify: ok, and the descriptors are the hashed files at offset 0 */
    int fds[VMX_MAX_FILES];
    d = dirfd_of(mdir);
    last_done = last_total = 0;
    CHECK(vmx_manifest_verify(d, m, fds, prog, NULL, why, sizeof why) == 0);
    close(d);
    CHECK(last_done == last_total && last_total > 0);
    char got[16] = "";
    CHECK(read(fds[0], got, sizeof got) == 14 && !memcmp(got, "{\"schema\":14}\n", 14));
    for (int i = 0; i < m->n; i++) close(fds[i]);

    /* a changed byte */
    put("disks/data-1.raw", "dbta", 4);
    CHECK(verify_now(m, why, sizeof why) == -1 && strstr(why, "checksum"));
    put("disks/data-1.raw", "data", 4);
    CHECK(verify_now(m, why, sizeof why) == 0);
    /* a longer file */
    put("disks/data-1.raw", "data!", 5);
    CHECK(verify_now(m, why, sizeof why) == -1 && strstr(why, "size"));
    put("disks/data-1.raw", "data", 4);
    /* a missing file */
    char p[512], q[512];
    snprintf(p, sizeof p, "%s/disks/nvram.qcow2", mdir);
    snprintf(q, sizeof q, "%s/nvram.keep", root);
    CHECK(rename(p, q) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    /* a symlink in its place (to a file with the right bytes: still refused) */
    CHECK(symlink(q, p) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    unlink(p);
    /* a FIFO in its place: refused, and the open doesn't hang */
    CHECK(mkfifo(p, 0644) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    unlink(p);
    /* a hard link to a file outside */
    CHECK(link(q, p) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1 && strstr(why, "links"));
    unlink(p);
    CHECK(rename(q, p) == 0);
    CHECK(verify_now(m, why, sizeof why) == 0);
    /* extra files and folders */
    static const char *const extra[] = { "extra", ".hidden", "disks/extra.qcow2", "disks/tpm/.lock", "disks/data-2.qcow2" };
    for (size_t i = 0; i < sizeof extra / sizeof *extra; i++) {
        put(extra[i], "x", 1);
        CHECK(verify_now(m, why, sizeof why) == -1 && strstr(why, "not part"));
        snprintf(p, sizeof p, "%s/%s", mdir, extra[i]);
        unlink(p);
    }
    snprintf(p, sizeof p, "%s/log", mdir);
    mkdir(p, 0755);
    CHECK(verify_now(m, why, sizeof why) == -1);
    rmdir(p);
    snprintf(p, sizeof p, "%s/disks/tpm/sub", mdir);
    mkdir(p, 0755);
    CHECK(verify_now(m, why, sizeof why) == -1);
    rmdir(p);
    /* a symlink to the machine's own file under an unlisted name */
    snprintf(p, sizeof p, "%s/disks/data-2.raw", mdir);
    CHECK(symlink("data-1.raw", p) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    unlink(p);
    /* a symlink folder: disks/tpm moved away and linked back */
    snprintf(p, sizeof p, "%s/disks/tpm", mdir);
    snprintf(q, sizeof q, "%s/tpm.real", root);
    CHECK(rename(p, q) == 0);
    CHECK(symlink(q, p) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    unlink(p);
    CHECK(rename(q, p) == 0);
    /* manifest.json as a symlink or folder is not a regular file either */
    snprintf(p, sizeof p, "%s/manifest.json", mdir);
    unlink(p);
    CHECK(symlink("machine.json", p) == 0);
    CHECK(verify_now(m, why, sizeof why) == -1);
    unlink(p);
    put("manifest.json", json, strlen(json));
    CHECK(verify_now(m, why, sizeof why) == 0);
}

/* replaces the first `from` with `to` in json and parses the result */
static int parse_edit(const char *json, const char *from, const char *to) {
    const char *at = strstr(json, from);
    if (!at) { fprintf(stderr, "  no %s in the manifest\n", from); return 99; }
    size_t a = (size_t)(at - json), fl = strlen(from), tl = strlen(to), n = strlen(json);
    char *s = malloc(n - fl + tl + 1);
    memcpy(s, json, a);
    memcpy(s + a, to, tl);
    memcpy(s + a + tl, at + fl, n - a - fl + 1);
    VmxManifest m;
    char why[256];
    int r = vmx_manifest_parse(s, strlen(s), &m, why, sizeof why);
    free(s);
    return r;
}

static void test_parse(const char *json) {
    CHECK(parse_edit(json, "\"version\": 1", "\"version\": 1") == 0);
    CHECK(parse_edit(json, "\"version\": 1", "\"version\": 1, \"extra\": 1") == -1);
    CHECK(parse_edit(json, "\"version\": 1", "\"version\": 2") == -1);
    CHECK(parse_edit(json, "\"version\": 1", "\"version\": \"1\"") == -1);
    CHECK(parse_edit(json, "\"version\": 1", "\"version\": 1, \"version\": 1") == -1);
    CHECK(parse_edit(json, "\"version\": 1,", "") == -1);                     /* missing key */
    CHECK(parse_edit(json, "lordvm-export", "lordvm-exporT") == -1);
    CHECK(parse_edit(json, "0f8fad5b", "0F8FAD5B") == -1);
    CHECK(parse_edit(json, "0f8fad5b-", "0f8fad5b") == -1);
    CHECK(parse_edit(json, "2026-11-24T", "2026-11-24 ") == -1);
    CHECK(parse_edit(json, "\"encrypted\": false", "\"encrypted\": 0") == -1);
    CHECK(parse_edit(json, "\"snapshots\": true", "\"snapshots\": null") == -1);
    CHECK(parse_edit(json, "\"path\": \"machine.json\"", "\"path\": \"../machine.json\"") == -1);
    CHECK(parse_edit(json, "\"path\": \"machine.json\"", "\"path\": \"machine.json\\u0000x\"") == -1);
    CHECK(parse_edit(json, "\"path\": \"snapshots.json\"", "\"path\": \"machine.json\"") == -1);    /* duplicate */
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": -1") == -1);
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": 14.5") == -1);
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": 1125899906842625") == -1);   /* 2^50 + 1 */
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": 1125899906842624") == 0);    /* 2^50 */
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": 1e400") == -1);
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": \"14\"") == -1);
    CHECK(parse_edit(json, "\"size\": 14,", "") == -1);
    CHECK(parse_edit(json, "\"size\": 14", "\"size\": 14, \"mode\": 420") == -1);
    CHECK(parse_edit(json, "\"sha256\": \"", "\"sha256\": \"A") == -1);         /* 65 digits, the first uppercase */
    CHECK(parse_edit(json, "e3b0c442", "E3B0C442") == -1);
    CHECK(parse_edit(json, "e3b0c442", "g3b0c442") == -1);
    CHECK(parse_edit(json, "e3b0c442", "e3b0c44") == -1);                       /* 63 digits */
    CHECK(parse_edit(json, "\"name\": \"", "\"name\": \"\\n") == -1);
    CHECK(parse_edit(json, "\"files\": [", "\"files\": [ 1,") == -1);
    CHECK(parse_edit(json, "\"files\": [", "\"files\": [ {},") == -1);
    CHECK(parse_edit(json, "{\n", "[{\n") == -1);
    VmxManifest m;
    char why[128];
    CHECK(vmx_manifest_parse("{}", 2, &m, why, sizeof why) == -1);
    CHECK(vmx_manifest_parse("", 0, &m, why, sizeof why) == -1);
    CHECK(vmx_manifest_parse(json, VMX_MANIFEST_MAX + 1, &m, why, sizeof why) == -1);
    /* an empty and a too-long file list */
    char *s = malloc(200000);
    int k = snprintf(s, 200000, "{\"format\":\"lordvm-export\",\"version\":1,\"id\":\"0f8fad5b-d9cb-469f-a165-70867728950e\","
                     "\"name\":\"x\",\"created\":\"2026-11-24T10:20:30Z\",\"encrypted\":false,\"snapshots\":false,\"files\":[]}");
    CHECK(vmx_manifest_parse(s, (size_t)k, &m, why, sizeof why) == -1);
    k = snprintf(s, 200000, "{\"format\":\"lordvm-export\",\"version\":1,\"id\":\"0f8fad5b-d9cb-469f-a165-70867728950e\","
                 "\"name\":\"x\",\"created\":\"2026-11-24T10:20:30Z\",\"encrypted\":false,\"snapshots\":false,\"files\":[");
    for (int i = 0; i < VMX_MAX_FILES + 1; i++)
        k += snprintf(s + k, 200000 - (size_t)k, "%s{\"path\":\"disks/tpm/snap-s%d.permall\",\"size\":1,\"sha256\":\"%064d\"}", i ? "," : "", i, 0);
    k += snprintf(s + k, 200000 - (size_t)k, "]}");
    CHECK(vmx_manifest_parse(s, (size_t)k, &m, why, sizeof why) == -1);
    free(s);
}

/* random byte flips, insertions of JSON punctuation and cuts: never a crash, and anything accepted writes back */
static void test_fuzz(const char *json) {
    size_t n = strlen(json);
    char *s = malloc(n + 64);
    uint32_t seed = 12345;
    int accepted = 0;
    static const char punct[] = "{}[]\":,\\0123456789eE.-+ tfn\x80\xff";
    for (int it = 0; it < 5000; it++) {
        memcpy(s, json, n + 1);
        size_t len = n;
        int flips = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 4;
        for (int f = 0; f < flips; f++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % len;
            int kind = (int)(seed >> 28) % 3;
            if (kind == 0) s[at] = (char)(seed >> 3);
            else if (kind == 1) s[at] = punct[(seed >> 4) % (sizeof punct - 1)];
            else len = at + 1;
        }
        VmxManifest m;
        char why[128];
        if (vmx_manifest_parse(s, len, &m, why, sizeof why) == 0) {
            accepted++;
            char *j = vmx_manifest_json(&m);
            CHECK(j != NULL);
            free(j);
        }
    }
    printf("fuzz: 5000 mutations, %d still valid manifests\n", accepted);
    free(s);
}

/* ---- OVF and .mf ---- */

static void test_ovf(void) {
    VmxOvfInfo in = { .name = "Debian <13> & \"Tom's\"", .vcpus = 4, .memory_mb = 4096, .efi = 1, .os = "linux", .ndisks = 2 };
    in.disk[0] = (VmxOvfDisk){ "Debian-disk1.vmdk", 64ull << 30, 5ull << 30, 2ull << 30 };
    in.disk[1] = (VmxOvfDisk){ "Debian-disk2.vmdk", 10ull << 30, 0, 70000 };
    char *x = vmx_ovf(&in);
    CHECK(x != NULL);
    if (!x) return;
    CHECK(strstr(x, "<Name>Debian &lt;13&gt; &amp; &quot;Tom&apos;s&quot;</Name>") != NULL);
    CHECK(strstr(x, "ovf:id=\"101\"") && strstr(x, "vmx-10") && strstr(x, "#streamOptimized") && strstr(x, ">E1000<")
          && strstr(x, "vmw:key=\"firmware\" vmw:value=\"efi\"") && strstr(x, "ovf:capacity=\"68719476736\"")
          && strstr(x, "ovf:href=\"Debian-disk2.vmdk\" ovf:id=\"file2\" ovf:size=\"70000\"") && strstr(x, "ovf:/disk/vmdisk2"));
    CHECK(!strstr(x, "Tom's"));
    char path[400];
    snprintf(path, sizeof path, "%s/test.ovf", root);
    FILE *f = fopen(path, "w");
    fputs(x, f);
    fclose(f);
    if (!sh("command -v xmllint >/dev/null 2>&1")) {
        CHECK(sh("xmllint --noout '%s'", path) == 0);
        printf("ovf: well-formed (xmllint)\n");
    } else if (!sh("command -v python3 >/dev/null 2>&1")) {
        CHECK(sh("python3 -I -c 'import sys, xml.dom.minidom; xml.dom.minidom.parse(sys.argv[1])' '%s'", path) == 0);
        printf("ovf: well-formed (no xmllint here; checked with python3's XML parser)\n");
    } else printf("ovf: well-formedness not checked (no xmllint, no python3)\n");
    free(x);
    in.efi = 0;
    in.os = "windows";
    x = vmx_ovf(&in);
    CHECK(x && strstr(x, "ovf:id=\"120\"") && !strstr(x, "firmware"));
    free(x);
    CHECK(vmx_ovf_os_id("other") == 102 && vmx_ovf_os_id(NULL) == 102 && vmx_ovf_os_id("debian") == 96);
    VmxOvfInfo bad = in;
    bad.disk[1].file = "../x.vmdk";             CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.disk[1].file = "a\"b.vmdk";   CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.disk[1].file = in.disk[0].file; CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.name = "a\nb";                CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.vcpus = 0;                    CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.ndisks = 6;                   CHECK(vmx_ovf(&bad) == NULL);
    bad = in; bad.disk[0].capacity = 0;         CHECK(vmx_ovf(&bad) == NULL);

    const char *names[] = { "Debian.ovf", "Debian-disk1.vmdk" };
    const char *hex[] = { "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                          "0000000000000000000000000000000000000000000000000000000000000001" };
    char *mf = vmx_mf(names, hex, 2);
    CHECK(mf && !strcmp(mf, "SHA256(Debian.ovf)= e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
                            "SHA256(Debian-disk1.vmdk)= 0000000000000000000000000000000000000000000000000000000000000001\n"));
    free(mf);
    const char *badhex[] = { hex[0], "E3B0" };
    CHECK(vmx_mf(names, badhex, 2) == NULL);
    const char *badname[] = { "a/b.ovf" };
    CHECK(vmx_mf(badname, hex, 1) == NULL);
}

/* ---- tar ---- */

static unsigned long long oct(const unsigned char *p, int n) {
    unsigned long long v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static int sum_ok(const unsigned char *h) {
    unsigned s = 0;
    for (int i = 0; i < 512; i++) s += (i >= 148 && i < 156) ? ' ' : h[i];
    return s == oct(h + 148, 8);
}

static void test_tar_small(void) {
    char tarp[400], xdir[400], src[400];
    snprintf(tarp, sizeof tarp, "%s/small.tar", root);
    snprintf(xdir, sizeof xdir, "%s/x", root);
    snprintf(src, sizeof src, "%s/payload.bin", root);
    size_t pn = 1000003;
    unsigned char *pay = malloc(pn);
    for (size_t i = 0; i < pn; i++) pay[i] = (unsigned char)(i * 7 + 3);
    memset(pay + 100000, 0, 300000);                             /* a zero run: a hole in the archive, zeros on reading */
    int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(write(fd, pay, pn) == (ssize_t)pn);
    close(fd);

    char longname[160];
    snprintf(longname, sizeof longname, "dir/%s.vmdk",
             "a-name-that-is-much-longer-than-the-ninety-nine-bytes-a-ustar-name-field-holds-without-its-prefix-x");
    CHECK(strlen(longname) > 99);
    const char *ovf = "<?xml version=\"1.0\"?><Envelope/>\n", *mf = "SHA256(x)= 00\n";
    int out = open(tarp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long long mt = 1764000000;
    CHECK(vmx_tar_bytes(out, "Test.ovf", ovf, strlen(ovf), mt) == 0);
    CHECK(vmx_tar_bytes(out, "Test.mf", mf, strlen(mf), mt) == 0);
    CHECK(vmx_tar_bytes(out, "empty", "", 0, mt) == 0);
    int in = open(src, O_RDONLY);
    CHECK(vmx_tar_file(out, "Test-disk1.vmdk", in, (long long)pn, mt) == 0);
    lseek(in, 0, SEEK_SET);
    CHECK(vmx_tar_file(out, longname, in, (long long)pn, mt) == 0);
    /* refused: names, and a file shorter than its size */
    lseek(in, 0, SEEK_SET);
    off_t before = lseek(out, 0, SEEK_CUR);
    CHECK(vmx_tar_bytes(out, "../x", "a", 1, mt) == -1 && vmx_tar_bytes(out, "/abs", "a", 1, mt) == -1
          && vmx_tar_bytes(out, "a//b", "a", 1, mt) == -1 && vmx_tar_bytes(out, "a/./b", "a", 1, mt) == -1
          && vmx_tar_bytes(out, "a\nb", "a", 1, mt) == -1 && vmx_tar_bytes(out, "", "a", 1, mt) == -1
          && vmx_tar_bytes(out, "a/", "a", 1, mt) == -1);
    CHECK(lseek(out, 0, SEEK_CUR) == before);                     /* nothing written for those */
    CHECK(vmx_tar_end(out) == 0);
    close(out);
    char shortp[400];
    snprintf(shortp, sizeof shortp, "%s/short.tar", root);
    out = open(shortp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(vmx_tar_file(out, "short", in, (long long)pn + 1, mt) == -1);
    close(out);
    close(in);

    /* the first member is the .ovf (OVF 1.0 asks for it), each header's checksum right */
    size_t tn;
    unsigned char *t = (unsigned char *)slurp(tarp, &tn);
    CHECK(t && tn % 512 == 0 && !memcmp(t, "Test.ovf", 9) && sum_ok(t) && !memcmp(t + 257, "ustar\0" "00", 8) && t[156] == '0');
    free(t);

    char listp[400];
    snprintf(listp, sizeof listp, "%s/list.txt", root);
    CHECK(sh("tar -tvf '%s' > '%s'", tarp, listp) == 0);
    size_t ln;
    char *list = slurp(listp, &ln);
    CHECK(list && strstr(list, "Test.ovf") && strstr(list, "Test.mf") && strstr(list, longname)
          && strstr(list, "-rw-r--r--") && strstr(list, " 1000003 ") && !strstr(list, "PaxHeaders"));
    CHECK(list && strstr(list, "Test.ovf") < strstr(list, "Test.mf"));
    free(list);
    sh("rm -rf '%s'", xdir);
    mkdir(xdir, 0755);
    CHECK(sh("tar -xf '%s' -C '%s'", tarp, xdir) == 0);
    CHECK(sh("cmp -s '%s' '%s/Test-disk1.vmdk'", src, xdir) == 0);
    CHECK(sh("cmp -s '%s' '%s/%s'", src, xdir, longname) == 0);
    char p[600];
    size_t n;
    snprintf(p, sizeof p, "%s/Test.ovf", xdir);
    char *o = slurp(p, &n);
    CHECK(o && n == strlen(ovf) && !memcmp(o, ovf, n));
    free(o);
    snprintf(p, sizeof p, "%s/empty", xdir);
    struct stat st;
    CHECK(stat(p, &st) == 0 && st.st_size == 0);
    free(pay);
    printf("tar: small archive listed and extracted by %s", "the system's tar\n");
}

/* a 9 GiB sparse member: pax 'size' plus GNU base-256 in the ustar field; read back by tar -tvf and by our parser */
static void test_tar_big(void) {
    const long long big = 9LL << 30;
    char src[400], tarp[400];
    snprintf(src, sizeof src, "%s/big.raw", root);
    snprintf(tarp, sizeof tarp, "%s/big.tar", root);
    int fd = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && ftruncate(fd, big) == 0);
    CHECK(pwrite(fd, "HEAD", 4, 0) == 4 && pwrite(fd, "TAIL", 4, big - 4) == 4 && pwrite(fd, "MID", 3, 5LL << 30) == 3);
    close(fd);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int in = open(src, O_RDONLY), out = open(tarp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(vmx_tar_file(out, "big.vmdk", in, big, 1764000000) == 0);
    CHECK(vmx_tar_end(out) == 0);
    close(in);
    close(out);
    clock_gettime(CLOCK_MONOTONIC, &b);
    struct stat st;
    CHECK(stat(tarp, &st) == 0 && st.st_size == 1536 + big + 1024);
    CHECK(st.st_blocks * 512LL < 64LL << 20);                    /* holes: a few blocks on disk, not 9 GiB */

    int t = open(tarp, O_RDONLY);
    unsigned char h[512], u[512], data[8];
    CHECK(pread(t, h, 512, 0) == 512 && pread(t, u, 512, 1024) == 512);
    CHECK(h[156] == 'x' && sum_ok(h) && !memcmp(h + 257, "ustar", 6));
    unsigned long long xl = oct(h + 124, 12);
    char rec[512] = "";
    CHECK(xl > 0 && xl < 512 && pread(t, rec, xl, 512) == (ssize_t)xl);
    CHECK(!strcmp(rec, "19 size=9663676416\n"));                 /* the record's length counts itself */
    CHECK(u[156] == '0' && sum_ok(u) && !memcmp(u, "big.vmdk", 9) && u[124] == 0x80);
    unsigned long long v = 0;
    for (int i = 1; i < 12; i++) v = v << 8 | u[124 + i];
    CHECK(v == (unsigned long long)big);
    CHECK(pread(t, data, 4, 1536) == 4 && !memcmp(data, "HEAD", 4));
    CHECK(pread(t, data, 3, 1536 + (5LL << 30)) == 3 && !memcmp(data, "MID", 3));
    CHECK(pread(t, data, 4, 1536 + big - 4) == 4 && !memcmp(data, "TAIL", 4));
    close(t);
    char listp[400];
    snprintf(listp, sizeof listp, "%s/biglist.txt", root);
    CHECK(sh("tar -tvf '%s' > '%s'", tarp, listp) == 0);
    size_t ln;
    char *list = slurp(listp, &ln);
    CHECK(list && strstr(list, " 9663676416 ") && strstr(list, "big.vmdk"));
    free(list);
    printf("tar: 9 GiB sparse member written in %.1f s, %lld KiB on disk; size checked by tar -tvf and our parser "
           "(not extracted: that would write 9 GiB)\n",
           (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9, (long long)st.st_blocks / 2);
}

/* QA 0.8.9.6.2 (BUG-M): an export's manifest is the person's own file (not root's), read through its folder, and
 * every hostile shape is refused without hanging */
static void test_read(const char *json) {
    char d[400], p[460];
    snprintf(d, sizeof d, "%s/read", root);
    mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/manifest.json", d);
    FILE *f = fopen(p, "w");
    if (f) { fputs(json, f); fclose(f); }
    chmod(p, 0664);                                         /* a umask 0002 home, or a USB stick's loose modes */
    int dfd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    VmxManifest m;
    char why[256];
    CHECK(vmx_manifest_read(dfd, &m, why, sizeof why) == 0 && m.n == NFILES && m.snapshots == 1);
    if (*why) fprintf(stderr, "  read: %s\n", why);
    /* a link to a good manifest */
    char t[460];
    snprintf(t, sizeof t, "%s/real.json", d);
    rename(p, t);
    CHECK(!symlink("real.json", p) && vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    unlink(p);
    /* a FIFO: refused at once, never waited on */
    CHECK(!mkfifo(p, 0600) && vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    unlink(p);
    /* a folder, nothing, an empty file, one past the cap, broken JSON */
    CHECK(!mkdir(p, 0700) && vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    rmdir(p);
    CHECK(vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    f = fopen(p, "w"); if (f) fclose(f);
    CHECK(vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    f = fopen(p, "w");
    if (f) { for (long i = 0; i <= (long)VMX_MANIFEST_MAX; i++) fputc(' ', f); fclose(f); }
    CHECK(vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    f = fopen(p, "w"); if (f) { fputs("{\"format\":\"lordvm-export\",", f); fclose(f); }
    CHECK(vmx_manifest_read(dfd, &m, why, sizeof why) == -1);
    close(dfd);
}

int main(void) {
    snprintf(root, sizeof root, "/var/tmp/lordos-vmexport-test/%d", (int)getpid());
    mkdir("/var/tmp/lordos-vmexport-test", 0755);
    if (mkdir(root, 0755)) { perror(root); return 2; }
    test_paths();
    VmxManifest m;
    char *json = NULL;
    test_manifest(&m, &json);
    if (json) { test_parse(json); test_fuzz(json); test_read(json); }
    free(json);
    test_ovf();
    test_tar_small();
    test_tar_big();
    sh("rm -rf '%s'; rmdir /var/tmp/lordos-vmexport-test 2>/dev/null", root);
    printf("vmexport: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
