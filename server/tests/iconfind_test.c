/* iconfind_test.c — the order icon names are looked up in (src/iconfind.c), over a made-up tree: PNGs of hicolor
 * (the user's before the system's), /usr/share/pixmaps, then SVG (scalable/ of hicolor, then Adwaita), the
 * AppStream PNG for apps, the symbolic ones last; names that must find nothing; which SVG paths may go to
 * lord-media. Each step removes the file the previous one found. */
#include "../src/iconfind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static char root[1024], home[1100];

static void mkdirs(const char *path) {
    char p[PATH_MAX * 2];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

/* creates root + rel (rel starts with '/') */
static void touch(const char *rel) {
    char p[PATH_MAX * 2];
    snprintf(p, sizeof p, "%s%s", root, rel);
    mkdirs(p);
    FILE *f = fopen(p, "w");
    if (f) { fputs("x", f); fclose(f); }
}

static void drop(const char *rel) {
    char p[PATH_MAX * 2];
    snprintf(p, sizeof p, "%s%s", root, rel);
    unlink(p);
}

/* the lookup must find root + want ("" = nothing), then that file is removed */
static void expect(int status, const char *name, const char *want) {
    IconRoots r;
    iconfind_roots(&r, home, root);
    char out[PATH_MAX], full[PATH_MAX * 2];
    int got = status ? iconfind_status(&r, name, out, sizeof out) : iconfind_app(&r, name, out, sizeof out);
    snprintf(full, sizeof full, "%s%s", want[0] ? root : "", want);
    if (got != !!want[0] || strcmp(out, full)) {
        fails++;
        printf("FAIL %s %s: got '%s', wanted '%s'\n", status ? "status" : "app", name, out, full);
    }
    if (want[0]) drop(want);
}

static void test_status(void) {
    const char *files[] = {
        "/home/.local/share/icons/hicolor/22x22/status/vol.png",
        "/usr/share/icons/hicolor/32x32/status/vol.png",
        "/usr/share/icons/hicolor/22x22/status/vol.png",
        "/usr/share/icons/hicolor/32x32/apps/vol.png",
        "/usr/share/pixmaps/vol.png",
        "/usr/share/icons/hicolor/scalable/status/vol.svg",
        "/usr/share/icons/Adwaita/scalable/status/vol.svg",
        "/usr/share/icons/hicolor/scalable/apps/vol.svg",
        "/usr/share/pixmaps/vol.svg",
        "/usr/share/icons/hicolor/symbolic/status/vol-symbolic.svg",
        "/usr/share/icons/hicolor/scalable/status/vol-symbolic.svg",
        "/usr/share/icons/Adwaita/symbolic/status/vol-symbolic.svg",
        "/usr/share/icons/Adwaita/symbolic/apps/vol-symbolic.svg",
        NULL };
    for (int i = 0; files[i]; i++) touch(files[i]);
    /* the user's theme first, at every size (32 px is the bar's first), then the system's; status/ before apps/ */
    for (int i = 0; files[i]; i++) expect(1, "vol", files[i]);
    expect(1, "vol", "");
    /* a name that already says -symbolic */
    touch("/usr/share/icons/Adwaita/symbolic/status/net-symbolic.svg");
    expect(1, "net-symbolic", "/usr/share/icons/Adwaita/symbolic/status/net-symbolic.svg");
    /* never a path, never anything outside the plain characters */
    touch("/usr/share/pixmaps/x.png");
    expect(1, "../pixmaps/x", "");
    expect(1, "/usr/share/pixmaps/x", "");
    expect(1, "x y", "");
    expect(1, ".x", "");
    expect(1, "", "");
}

static void test_app(void) {
    const char *files[] = {
        "/home/.local/share/icons/hicolor/48x48/apps/ed.png",
        "/usr/share/icons/hicolor/64x64/apps/ed.png",
        "/usr/share/icons/hicolor/256x256/apps/ed.png",
        "/usr/share/pixmaps/ed.png",
        "/home/.local/share/icons/hicolor/scalable/apps/ed.svg",
        "/usr/share/icons/hicolor/scalable/apps/ed.svg",
        "/usr/share/icons/Adwaita/scalable/apps/ed.svg",
        "/usr/share/pixmaps/ed.svg",
        "/var/lib/lordos/pkg/icons/64/editor_ed.png",
        "/usr/share/icons/hicolor/symbolic/apps/ed-symbolic.svg",
        NULL };
    for (int i = 0; files[i]; i++) touch(files[i]);
    /* the user's tree comes first at every size, then the system's (64 is the first size) */
    for (int i = 0; files[i]; i++) expect(0, "ed", files[i]);
    expect(0, "ed", "");
    /* an absolute Icon= path: a PNG or an SVG that is there */
    char abs[PATH_MAX * 2];
    touch("/opt/ed/icon.svg");
    snprintf(abs, sizeof abs, "%s/opt/ed/icon.svg", root);
    IconRoots r;
    iconfind_roots(&r, home, root);
    char out[PATH_MAX];
    if (!iconfind_app(&r, abs, out, sizeof out) || strcmp(out, abs)) { fails++; printf("FAIL an absolute SVG: '%s'\n", out); }
    snprintf(abs, sizeof abs, "%s/opt/ed/none.svg", root);
    if (iconfind_app(&r, abs, out, sizeof out)) { fails++; printf("FAIL a missing absolute SVG\n"); }
    expect(0, "../ed", "");
}

static void test_allowed(void) {
    struct { const char *path; int ok; } cases[] = {
        { "/usr/share/icons/Adwaita/symbolic/status/a-symbolic.svg", 1 },
        { "/usr/share/pixmaps/a.svg", 1 },
        { "/opt/app/icon.svg", 1 },
        { "/home/u/.local/share/icons/hicolor/scalable/apps/a.svg", 1 },
        { "/home/u/.icons/a.svg", 1 },
        { "/home/u/Documents/a.svg", 0 },
        { "/home/u/.local/share/icons/a.png", 0 },
        { "/etc/a.svg", 0 },
        { "/usr/share/../../etc/a.svg", 0 },
        { "usr/share/a.svg", 0 },
        { "/home/user2/.icons/a.svg", 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        if (iconfind_svg_allowed(cases[i].path, "/home/u") != cases[i].ok) { fails++; printf("FAIL allowed %s\n", cases[i].path); }
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/iconfind.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(home, sizeof home, "%s/home", root);
    test_status();
    test_app();
    test_allowed();
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) { /* a temporary folder left behind */ }
    if (fails) { printf("iconfind-test: %d failed\n", fails); return 1; }
    printf("iconfind-test: ok\n");
    return 0;
}
