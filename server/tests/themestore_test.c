/* themestore_test.c — community themes' install path (docs/THEMES-GUIDE.md "Community themes"), under ASan and UBSan:
 *   - zipread.c: archives with "..", an absolute name, a backslash, a link, a device, two folders at the top, a file
 *     twice, encryption, a size over the limit, a wrong CRC-32 and a truncated archive are refused; a good one is read;
 *   - lord-themec itself (the binary, sandboxed, the .zip on fd 0): the okminimal fixture is built; the same theme with
 *     eval, lord.term.create, lord.fs.write, an import of another package, a host element or a remote URL is refused
 *     with the rule that says why; a .zip with ".." or a link is refused before the compiler;
 *   - themestore.c: its answer installed into a fresh folder (theme.js's SHA-256 in install.json, no link followed);
 *     a changed theme.js is not read; malformed answers (a bad name, "..", trailing bytes, theme.js missing, a size
 *     over the limit) are refused; names outside the allowed set are refused.
 * Run from server/: ./build/themestore-test build/lord-themec */
#include "../src/ui/themestore.h"
#include "../src/themec/zipread.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

/* ---- a small .zip writer (stored entries) ---- */
typedef struct { uint8_t *b; size_t n, cap; uint8_t cd[1 << 16]; size_t cdn; int count; } Zip;
static void put(Zip *z, const void *p, size_t n) {
    if (z->n + n > z->cap) { z->cap = (z->n + n) * 2 + 4096; z->b = realloc(z->b, z->cap); }
    memcpy(z->b + z->n, p, n);
    z->n += n;
}
static void le16(uint8_t *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 255; }
/* mode: the Unix mode in the external attributes (0: none); crc_fix: a wrong CRC-32; flags: the general flags */
static void zadd(Zip *z, const char *name, const void *data, size_t len, unsigned mode, int bad_crc, unsigned flags, uint32_t usize_claim) {
    uint8_t h[30] = { 0x50, 0x4b, 0x03, 0x04 };
    size_t nl = strlen(name);
    uint32_t crc = zip_crc32(data, len) ^ (bad_crc ? 1u : 0u);
    uint32_t us = usize_claim ? usize_claim : (uint32_t)len;
    le16(h + 4, 20); le16(h + 6, flags); le16(h + 8, 0); le32(h + 14, crc); le32(h + 18, (uint32_t)len); le32(h + 22, us); le16(h + 26, (unsigned)nl);
    uint32_t off = (uint32_t)z->n;
    put(z, h, 30); put(z, name, nl); put(z, data, len);
    uint8_t c[46] = { 0x50, 0x4b, 0x01, 0x02 };
    le16(c + 4, mode ? (3 << 8) | 20 : 20); le16(c + 6, 20); le16(c + 8, flags); le16(c + 10, 0); le32(c + 16, crc);
    le32(c + 20, (uint32_t)len); le32(c + 24, us); le16(c + 28, (unsigned)nl); le32(c + 38, (uint32_t)mode << 16); le32(c + 42, off);
    memcpy(z->cd + z->cdn, c, 46); z->cdn += 46;
    memcpy(z->cd + z->cdn, name, nl); z->cdn += nl;
    z->count++;
}
static void zend(Zip *z) {
    uint8_t e[22] = { 0x50, 0x4b, 0x05, 0x06 };
    uint32_t off = (uint32_t)z->n;
    put(z, z->cd, z->cdn);
    le16(e + 8, (unsigned)z->count); le16(e + 10, (unsigned)z->count); le32(e + 12, (uint32_t)z->cdn); le32(e + 16, off);
    put(z, e, 22);
}
static void zfree(Zip *z) { free(z->b); memset(z, 0, sizeof *z); }

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)l + 1);
    *n = fread(b, 1, (size_t)l, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

/* the okminimal fixture as a .zip under the folder <id>/, index.jsx with `extra` appended */
static void fixture_zip(Zip *z, const char *id, const char *extra) {
    static const char *const names[] = { "theme.json", "index.jsx", "tokens.js", "LICENSE", NULL };
    for (int i = 0; names[i]; i++) {
        char p[256], n[128];
        snprintf(p, sizeof p, "tests/community_themes/okminimal/%s", names[i]);
        size_t len;
        char *b = slurp(p, &len);
        if (!b) { printf("FAIL no fixture %s\n", p); fails++; continue; }
        if (extra && !strcmp(names[i], "index.jsx")) {
            b = realloc(b, len + strlen(extra) + 1);
            memcpy(b + len, extra, strlen(extra) + 1);
            len += strlen(extra);
        }
        snprintf(n, sizeof n, "%s/%s", id, names[i]);
        zadd(z, n, b, len, 0100644, 0, 0, 0);
        free(b);
    }
}

static int zip_refused(Zip *z, const char *needle) {
    ZipTheme t;
    char why[200] = "";
    int r = zip_read_theme(z->b, z->n, &t, why, sizeof why);
    if (!r) zip_free(&t);
    return r && (!needle || strstr(why, needle));
}

/* lord-themec on a zip: its whole answer */
static uint8_t *themec(const char *bin, Zip *z, size_t *len) {
    char path[] = "/tmp/themec-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, z->b, z->n) != (ssize_t)z->n) return NULL;
    lseek(fd, 0, SEEK_SET);
    unlink(path);
    int out[2];
    if (pipe(out)) return NULL;
    pid_t pid = fork();
    if (!pid) {
        dup2(fd, 0); dup2(out[1], 1);
        int null = open("/dev/null", O_WRONLY); dup2(null, 2);
        for (int i = 3; i < 64; i++) close(i);
        char *const args[] = { (char *)"lord-themec", NULL }, *const envp[] = { NULL };
        execve(bin, args, envp);
        _exit(127);
    }
    close(fd);
    close(out[1]);
    size_t cap = 1 << 20, n = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (n == cap) b = realloc(b, cap *= 2);
        ssize_t r = read(out[0], b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(out[0]);
    int st;
    waitpid(pid, &st, 0);
    *len = n;
    return b;
}

static int report_has(const uint8_t *ans, size_t n, const char *needle) {
    char rep[70000];
    if (ts_stream_parse(ans, n, rep, sizeof rep, NULL, NULL, NULL, 0)) return 0;
    return strstr(rep, needle) != NULL;
}

int main(int argc, char **argv) {
    const char *bin = argc > 1 ? argv[1] : "build/lord-themec";

    /* (QA of 0.8.9.6.2, BUG-H) the person's wallpaper a community theme's Image may show: those exact names only */
    {
        const char *H = "/home/alex", *K = "/var/lib/lordos/wallpapers/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.jpg";
        CHECK(jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287f.jpg", H) &&
              jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287f.thumb.jpg", H) && jst_wallpaper_file(K, H),
              "wallpaper: the person's own picture, its thumbnail and a catalog picture");
        CHECK(!jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287f.png", H) &&
              !jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7DDDF09BA4CF287F.jpg", H) &&
              !jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287.jpg", H) &&
              !jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287f.jpg/x", H) &&
              !jst_wallpaper_file("/home/alex/.ssh/id_ed25519", H) && !jst_wallpaper_file("/home/bob/.local/share/lordos/wallpapers/7dddf09ba4cf287f.jpg", H) &&
              !jst_wallpaper_file("/home/alexx/.local/share/lordos/wallpapers/7dddf09ba4cf287f.jpg", H) &&
              !jst_wallpaper_file("/home/alex/.local/share/lordos/wallpapers/7dddf09ba4cf287f.jpg", "") &&
              !jst_wallpaper_file("/var/lib/lordos/wallpapers/0123.jpg", H) && !jst_wallpaper_file("/var/lib/lordos/wallpapers/../x.jpg", H) &&
              !jst_wallpaper_file("/etc/passwd", H) && !jst_wallpaper_file(NULL, H),
              "wallpaper: any other name, user, folder or file refused");
    }

    /* names */
    CHECK(ts_valid_name("theme.js") && ts_valid_name("assets/a/b.png") && ts_valid_name("locales/zh-Hans.json"), "names: theme files accepted");
    CHECK(ts_valid_name("THIRD_PARTY.md") && ts_valid_name("PROVENANCE.md") && !ts_valid_name("OTHER.md"), "names: the credits files (THIRD_PARTY.md, PROVENANCE.md) kept");
    CHECK(!ts_valid_name("../x") && !ts_valid_name("/etc/passwd") && !ts_valid_name("assets/../theme.js") &&
          !ts_valid_name("assets/x.svg") && !ts_valid_name("install.json") && !ts_valid_name("index.jsx") &&
          !ts_valid_name("assets//x.png") && !ts_valid_name("locales/x/en.json") && !ts_valid_name(".hidden"), "names: others refused");
    CHECK(ts_valid_id("Aurora_2") && !ts_valid_id("") && !ts_valid_id("a/b") && !ts_valid_id("..") &&
          !ts_valid_id("abcdefghijklmnopqrstuvwxyz0"), "ids");

    /* zipread */
    Zip z = { 0 };
    fixture_zip(&z, "okmin", NULL); zend(&z);
    ZipTheme t;
    char why[200];
    int r = zip_read_theme(z.b, z.n, &t, why, sizeof why);
    CHECK(!r && !strcmp(t.id, "okmin") && t.n == 4, "zip: a good archive is read (%s)", r ? why : t.id);
    if (!r) zip_free(&t);
    zfree(&z);
    static const struct { const char *what, *name, *needle; unsigned mode; int crc; unsigned flags; uint32_t claim; } bad[] = {
        { "\"..\" in a name", "okmin/../../etc/x.js", "\"..\"", 0100644, 0, 0, 0 },
        { "an absolute name", "/okmin/x.js", "\"..\"", 0100644, 0, 0, 0 },
        { "a backslash", "okmin\\x.js", "\"..\"", 0100644, 0, 0, 0 },
        { "a symbolic link", "okmin/assets/x.png", "links", 0120777, 0, 0, 0 },
        { "a device", "okmin/dev", "links", 0020644, 0, 0, 0 },
        { "a second folder at the top", "other/x.js", "more than one folder", 0100644, 0, 0, 0 },
        { "a file twice", "okmin/theme.json", "twice", 0100644, 0, 0, 0 },
        { "a wrong CRC-32", "okmin/assets/y.png", "CRC", 0100644, 1, 0, 0 },
        { "encryption", "okmin/z.js", "encrypted", 0100644, 0, 1, 0 },
        { "a size over the limit", "okmin/big.png", "larger", 0100644, 0, 0, ZIP_FILE_MAX + 1 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Zip b = { 0 };
        fixture_zip(&b, "okmin", NULL);
        zadd(&b, bad[i].name, "abc", 3, bad[i].mode, bad[i].crc, bad[i].flags, bad[i].claim);
        zend(&b);
        CHECK(zip_refused(&b, bad[i].needle), "zip: %s refused", bad[i].what);
        zfree(&b);
    }
    Zip tr = { 0 };
    fixture_zip(&tr, "okmin", NULL); zend(&tr);
    tr.n -= 30;
    CHECK(zip_refused(&tr, NULL), "zip: a truncated archive refused");
    zfree(&tr);

    /* lord-themec */
    if (access(bin, X_OK)) { printf("FAIL no %s\n", bin); return 1; }
    Zip ok = { 0 };
    fixture_zip(&ok, "okmin", NULL); zend(&ok);
    size_t an = 0;
    uint8_t *ans = themec(bin, &ok, &an);
    char rep[70000] = "";
    int parsed = ans && !ts_stream_parse(ans, an, rep, sizeof rep, NULL, NULL, why, sizeof why);
    CHECK(parsed && strstr(rep, "\"ok\":true") && strstr(rep, "\"id\":\"okmin\""), "themec: the okminimal fixture is built (%.200s)", parsed ? rep : why);
    static const struct { const char *what, *extra, *rule; } refused[] = {
        { "eval", "\neval('1 + 1');\n", "code.eval" },
        { "lord.term.create", "\nlord.term.create();\n", "lord.forbidden" },
        { "lord.fs.write", "\nlord.fs.write('/tmp/x', 'y');\n", "lord.forbidden" },
        { "new Function", "\nnew Function('return 1');\n", "code.function" },
        { "globalThis", "\nglobalThis.x = 1;\n", "code.globalThis" },
        { "an import of another package", "\nimport fs from 'node:fs';\n", "\"build\"" },
        { "a host element", "\nconst h = <div />;\n", "host elements" },
        { "a remote URL", "\nconst u = 'https://example.com/x.js';\n", "code.url" },
        { "a constructor trick", "\nconst F = (() => {}).constructor;\n", "code.prototype" },
        { "dynamic import()", "\nimport('./tokens.js');\n", "code.dynamicImport" },
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        Zip b = { 0 };
        fixture_zip(&b, "okmin", refused[i].extra); zend(&b);
        size_t n = 0;
        uint8_t *a = themec(bin, &b, &n);
        int no_code = a && n && report_has(a, n, "\"ok\":false") && !memmem(a, n, "F theme.js", 10);
        CHECK(no_code && report_has(a, n, refused[i].rule), "themec: %s refused (%s)", refused[i].what, refused[i].rule);
        free(a);
        zfree(&b);
    }
    for (int k = 0; k < 2; k++) {
        Zip b = { 0 };
        fixture_zip(&b, "okmin", NULL);
        if (k) zadd(&b, "okmin/assets/link.png", "theme.js", 8, 0120777, 0, 0, 0);
        else zadd(&b, "okmin/../escape.js", "x", 1, 0100644, 0, 0, 0);
        zend(&b);
        size_t n = 0;
        uint8_t *a = themec(bin, &b, &n);
        CHECK(a && report_has(a, n, "\"rule\":\"zip\"") && !memmem(a, n, "F theme.js", 10), "themec: a .zip with %s refused before the compiler", k ? "a link" : "\"..\"");
        free(a);
        zfree(&b);
    }

    /* themestore: install the good answer */
    char root[] = "/tmp/themestore-test-XXXXXX";
    if (!mkdtemp(root)) return 1;
    int rfd = open(root, O_RDONLY | O_DIRECTORY);
    TsRecord rec = { 0 };
    snprintf(rec.id, sizeof rec.id, "okmin");
    snprintf(rec.author, sizeof rec.author, "LordOS tests");
    rec.caps = TS_CAP_BATTERY;
    r = parsed ? ts_install(rfd, ans, an, &rec, why, sizeof why) : -1;
    CHECK(!r, "store: installed (%s)", r ? why : "ok");
    TsRecord back;
    CHECK(!ts_record_read(rfd, "okmin", &back) && !strcmp(back.sha256, rec.sha256) && back.caps == TS_CAP_BATTERY && !back.trusted,
          "store: install.json written by lord-server (SHA-256, capabilities, not yet confirmed)");
    size_t cl = 0;
    char *code = ts_code_read(rfd, &back, &cl);
    CHECK(code && cl > 100 && strstr(code, "__lordThemeSdk"), "store: theme.js reads back and matches its SHA-256");
    free(code);
    /* again over the installed one: replaced whole */
    r = ts_install(rfd, ans, an, &rec, why, sizeof why);
    CHECK(!r, "store: a new version replaces the installed one");
    char p[512];
    snprintf(p, sizeof p, "%s/okmin/theme.js", root);
    FILE *f = fopen(p, "a");
    if (f) { fputs("\n/* changed */", f); fclose(f); }
    ts_record_read(rfd, "okmin", &back);
    code = ts_code_read(rfd, &back, &cl);
    CHECK(!code, "store: a changed theme.js is not read");
    free(code);
    unlink(p);
    if (symlink("/etc/hostname", p)) perror("symlink");
    code = ts_code_read(rfd, &back, &cl);
    CHECK(!code, "store: theme.js as a link is not followed");
    free(code);
    CHECK(!ts_remove(rfd, "okmin") && access(p, F_OK), "store: removed");

    /* malformed answers */
    static const char *const streams[] = {
        "LORDTHEME 1\nR 2\n{}\nF ../x.png 1\nAEND\n",
        "LORDTHEME 1\nR 2\n{}\nF install.json 1\nAEND\n",
        "LORDTHEME 1\nR 2\n{}\nF theme.json 2\n{}END\njunk",
        "LORDTHEME 1\nR 2\n{}\nF theme.json 2\n{}END\n",
        "LORDTHEME 1\nR 2\n{}\nF theme.js 99999999\nEND\n",
        "LORDTHEME 2\nR 2\n{}\nEND\n",
        "LORDTHEME 1\nR 5\n{}\nEND\n",
    };
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; i++)
        CHECK(ts_stream_parse((const uint8_t *)streams[i], strlen(streams[i]), NULL, 0, NULL, NULL, NULL, 0), "store: malformed answer %zu refused", i + 1);

    close(rfd);
    rmdir(root);
    free(ans);
    zfree(&ok);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "ok", fails);
    return fails ? 1 : 0;
}
