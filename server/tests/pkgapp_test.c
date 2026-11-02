/* pkgapp_test.c — a packaged app's check on disk (sec/sign.c, sec/pkgapp.c) and a status item's description
 * (sec/statusitem.c). Builds a signed folder in a temporary place and changes it in every way that must fail. The
 * real root-owned folder, the sandbox and a real session are in tests/companion_e2e.sh. */
#include "sec/pkgapp.h"
#include "sec/statusitem.h"
#include "sec/json.h"
#include "pkg/crypto.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void sig_test_official(const char *keyhex);
void pkgapp_test_root(const char *path);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char root[128], app[200];
static uint8_t author[32], official[32], stranger[32];
static char akey[65], okey[65];

static void put(const char *rel, const void *d, size_t n, int mode) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", app, rel);
    unlink(p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, d, n) != (ssize_t)n) { perror(p); exit(2); }
    close(fd);
}

static const char manifest[] = "{\"id\":\"test-app\",\"version\":\"1.0\",\"name\":\"T\",\"companion\":\"bin/engine\",\"permissions\":[\"network\",\"screen-share\",\"remote-input\"]}";
static const char engine[] = "\177ELF pretend";

static void entry(char *o, size_t n, const char *path, const void *d, size_t len, int comma) {
    uint8_t h[32];
    char hx[65];
    sha256(d, len, h);
    hex_encode(h, 32, hx);
    size_t l = strlen(o);
    snprintf(o + l, n - l, "%s{\"path\":\"%s\",\"size\":%zu,\"sha256\":\"%s\"}", comma ? "," : "", path, len, hx);
}

/* A good package signed by seed. extra: one more listed path with this content (not created). */
static void build(const uint8_t *seed, const char *keyhex) {
    char d[512];
    snprintf(d, sizeof d, "rm -rf %s && mkdir -p %s/bin && chmod -R go-w %s", app, app, app);
    if (system(d)) exit(2);
    put("manifest.json", manifest, strlen(manifest), 0644);
    put("bin/engine", engine, sizeof engine - 1, 0755);
    char c[4096];
    snprintf(c, sizeof c, "{\"format\":1,\"id\":\"test-app\",\"version\":\"1.0\",\"signer\":{\"name\":\"Test\",\"key\":\"%s\"},\"files\":[", keyhex);
    entry(c, sizeof c, "manifest.json", manifest, strlen(manifest), 0);
    entry(c, sizeof c, "bin/engine", engine, sizeof engine - 1, 1);
    strcat(c, "]}\n");
    put("contenido.json", c, strlen(c), 0644);
    char sig[400];
    sig_make_detached(seed, c, strlen(c), sig, sizeof sig);
    put("firma.json", sig, strlen(sig), 0644);
}

static void catalog(const char *extra) {
    char j[1024], sig[400], p[512];
    snprintf(j, sizeof j, "{\"format\":1,\"serial\":%d,\"authors\":[{\"key\":\"%s\",\"name\":\"Author\"}]%s}", 7, akey, extra);
    sig_make_detached(official, j, strlen(j), sig, sizeof sig);
    snprintf(p, sizeof p, "%s/.local/share/lordos/catalog/catalog.json", root);
    FILE *f = fopen(p, "w");
    fputs(j, f);
    fclose(f);
    snprintf(p, sizeof p, "%s/.local/share/lordos/catalog/catalog.sig", root);
    f = fopen(p, "w");
    fputs(sig, f);
    fclose(f);
    usleep(20000);
}

static int verify(char *err, size_t n, int *fd) {
    PkgManifest m;
    LpkSigInfo si;
    err[0] = 0;
    return pkgapp_verify("test-app", &m, &si, fd, err, n);
}

static void expect_code(const char *what, const char *code) {
    char err[200];
    int fd = 99;
    int r = verify(err, sizeof err, &fd);
    CHECK(r == -1 && fd == -1 && strstr(err, code), "%s: r=%d fd=%d err=%s", what, r, fd, err);
}

static void test_tree(void) {
    char err[200];
    int fd = -1;
    build(author, akey);
    catalog("");
    CHECK(!verify(err, sizeof err, &fd) && fd >= 0, "a good package: %s", err);
    /* the descriptor that comes back is the binary, and can be read again */
    char back[32] = "";
    CHECK(fd >= 0 && pread(fd, back, sizeof back, 0) == (ssize_t)sizeof engine - 1 && !memcmp(back, engine, sizeof engine - 1), "the hashed descriptor");
    if (fd >= 0) close(fd);
    PkgManifest m;
    CHECK(!pkgapp_manifest(app, "test-app", &m) && !strcmp(m.companion, "bin/engine") && m.network && m.screen && m.input, "the manifest's fields");
    CHECK(pkgapp_manifest(app, "other-app", &m), "another id");

    put("bin/engine", "\177ELF pretenD", sizeof engine - 1, 0755);
    expect_code("one byte changed", "sig.fileModified");
    build(author, akey);
    put("bin/engine", engine, sizeof engine - 2, 0755);
    expect_code("shorter", "sig.fileModified");
    build(author, akey);
    { const char *m2 = "{\"id\":\"test-app\",\"version\":\"1.0\",\"companion\":\"bin/enginf\"}"; put("manifest.json", m2, strlen(m2), 0644); }
    expect_code("another manifest", "sig.");
    build(author, akey);
    put("evil.js", "x", 1, 0644);
    expect_code("a file more", "sig.extraFiles");
    build(author, akey);
    char p[500], q[500];
    snprintf(p, sizeof p, "%s/bin/engine", app);
    unlink(p);
    expect_code("the binary missing", "sig.");
    build(author, akey);
    if (symlink("/bin/sh", p)) { /* it exists: the unlink below */ }
    unlink(p);
    if (symlink("/bin/sh", p)) perror("symlink");
    expect_code("the binary is a link", "sig.");
    build(author, akey);
    chmod(p, 0777);
    expect_code("writable by others", "sig.");
    build(author, akey);
    chmod(p, 04755);
    expect_code("set-uid", "sig.");
    build(author, akey);
    snprintf(q, sizeof q, "%s/firma.json", app);
    unlink(q);
    expect_code("no signature", "sig.contents.missing");
    build(author, akey);
    snprintf(q, sizeof q, "%s/contenido.json", app);
    char tmp[4096];
    int f = open(q, O_RDONLY);
    ssize_t n = read(f, tmp, sizeof tmp);
    close(f);
    char *w = memmem(tmp, (size_t)n, "\"Test\"", 6);
    if (w) w[4] = 's';
    put("contenido.json", tmp, (size_t)n, 0644);
    expect_code("the list touched", "sig.mismatch");

    char skey[65];
    uint8_t pub[32];
    ed25519_public(stranger, pub);
    hex_encode(pub, 32, skey);
    build(stranger, skey);
    expect_code("a key not in the catalog", "companion.notVerified");
    build(official, okey);
    expect_code("an official key is not a Verified author", "companion.notVerified");

    build(author, akey);
    char rev[200];
    snprintf(rev, sizeof rev, ",\"revoked\":{\"keys\":[\"%s\"]}", akey);
    catalog(rev);
    expect_code("revoked key", "sig.revokedKey");
    catalog(",\"revoked\":{\"packages\":[{\"id\":\"test-app\",\"version\":\"1.0\"}]}");
    expect_code("revoked version", "sig.revokedVersion");
    catalog("");

    /* no companion in the manifest */
    CHECK(!verify(err, sizeof err, NULL), "the app alone, no descriptor: %s", err);
    char bad[64];
    snprintf(bad, sizeof bad, "%s", "../x");
    CHECK(pkgapp_dir(bad, q, sizeof q), "a path as an id");
    chmod(app, 0777);
    CHECK(pkgapp_dir("test-app", q, sizeof q), "a folder others can write");
    chmod(app, 0755);
    snprintf(p, sizeof p, "%s.moved", app);
    rename(app, p);
    if (symlink(p, app)) perror("symlink");
    CHECK(pkgapp_dir("test-app", q, sizeof q), "the folder is a link");
    unlink(app);
    rename(p, app);
}

static void test_status(void) {
    char o[3800];
    static const char *const good[] = {
        "{\"icon\":\"assets/i.png\"}",
        "{\"icon\":\"i.png\",\"title\":\"Hello\",\"alert\":true,\"menu\":[{\"id\":\"open\",\"label\":\"Open\"},{\"line\":true},{\"id\":\"q-1\",\"label\":\"Quit\",\"off\":true}]}",
        "{\"icon\":\"i.png\",\"title\":\"say \\\"hi\\\"\\\\\"}",
        "{\"icon\":\"i.png\",\"title\":\"Código\",\"junk\":1}",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        int r = statusitem_clean("test-app", good[i], strlen(good[i]), o, sizeof o);
        JDoc d;
        CHECK(!r && !json_parse(o, strlen(o), &d, NULL, 0), "good %zu: %s", i, o);
        if (!r) { CHECK(!json_get(d.root, "junk") && !strcmp(json_str(d.root, "app"), "test-app"), "only known fields: %s", o); json_free(&d); }
    }
    static const char *const bad[] = {
        "", "[]", "{}", "{\"icon\":\"../x.png\"}", "{\"icon\":\"/etc/x.png\"}", "{\"icon\":\"a/../x.png\"}", "{\"icon\":\"x.svg\"}", "{\"icon\":5}",
        "{\"icon\":\"i.png\",\"alert\":1}", "{\"icon\":\"i.png\",\"menu\":{}}", "{\"icon\":\"i.png\",\"menu\":[5]}",
        "{\"icon\":\"i.png\",\"menu\":[{\"id\":\"A b\",\"label\":\"x\"}]}", "{\"icon\":\"i.png\",\"menu\":[{\"id\":\"a\"}]}",
        "{\"icon\":\"i.png\",\"menu\":[{\"id\":\"a\",\"label\":\"\"}]}",
        "{\"icon\":\"i.png\",\"title\":\"a\\nb\"}", "{\"icon\":\"i.png\",\"title\":\"\\u202eevil\"}", "{\"icon\":\"i.png\",\"title\":\"\\u200b\"}",
        "{\"icon\":\"i.png\",\"title\":\"0123456789012345678901234567890123456789012345678901234567890123456789\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(statusitem_clean("test-app", bad[i], strlen(bad[i]), o, sizeof o), "bad %zu: %s", i, bad[i]);
    CHECK(statusitem_clean("Bad\"id", "{\"icon\":\"i.png\"}", 15, o, sizeof o), "the app's id is checked");
    char big[4096] = "{\"icon\":\"i.png\",\"menu\":[";
    for (int i = 0; i < 13; i++) { size_t l = strlen(big); snprintf(big + l, sizeof big - l, "%s{\"id\":\"a\",\"label\":\"b\"}", i ? "," : ""); }
    strcat(big, "]}");
    CHECK(statusitem_clean("test-app", big, strlen(big), o, sizeof o), "13 menu items");
    CHECK(!statusitem_id_ok("") && !statusitem_id_ok("A") && !statusitem_id_ok("a\"") && statusitem_id_ok("open-1"), "item ids");
    /* the output buffer limits it, never overruns */
    char small[40];
    CHECK(statusitem_clean("test-app", good[1], strlen(good[1]), small, sizeof small), "a small buffer refuses");
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/pkgapp-test-%d", (int)getpid());
    char c[400], apps[160];
    snprintf(c, sizeof c, "umask 022; mkdir -p %s/.local/share/lordos/catalog %s/apps", root, root);
    if (system(c)) return 2;
    snprintf(apps, sizeof apps, "%s/apps", root);
    snprintf(app, sizeof app, "%s/test-app", apps);
    setenv("HOME", root, 1);
    pkgapp_test_root(apps);
    for (int i = 0; i < 32; i++) { author[i] = (uint8_t)(i + 1); official[i] = (uint8_t)(i + 80); stranger[i] = (uint8_t)(i + 160); }
    uint8_t pub[32];
    ed25519_public(author, pub);
    hex_encode(pub, 32, akey);
    ed25519_public(official, pub);
    hex_encode(pub, 32, okey);
    sig_test_official(okey);
    test_tree();
    test_status();
    snprintf(c, sizeof c, "rm -rf %s", root);
    if (system(c)) return 2;
    printf("pkgapp: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
