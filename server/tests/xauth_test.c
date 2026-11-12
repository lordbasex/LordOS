/* xauth_test.c — the session's X cookie file (src/wl/xauth.c, GitHub #107): the Xauthority format,
 * its permissions, O_EXCL and no link followed, and that isolated and restricted apps lose both
 * DISPLAY and XAUTHORITY (xauth_env_drop, used by src/app/appspawn.c and src/apps.c). */
#include "wl/xauth.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const unsigned char COOKIE[XAUTH_COOKIE_LEN] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0xff };

static size_t field(const unsigned char *p, const char *want, size_t wl) {
    size_t n = (size_t)p[0] << 8 | p[1];
    CHECK(n == wl);
    CHECK(n == wl && !memcmp(p + 2, want, wl));
    return 2 + n;
}

static void test_encode(void) {
    unsigned char b[512];
    size_t n = xauth_encode(b, sizeof b, "lordos", 12, COOKIE);
    /* 2 entries: family, address, "12", "MIT-MAGIC-COOKIE-1", 16 bytes */
    size_t e = 2 + 2 + 2 + 2 + 2 + 18 + 2 + 16;
    CHECK(n == e + 6 + e);
    const unsigned char *p = b;
    CHECK(p[0] == 1 && p[1] == 0);                          /* FamilyLocal, 256 */
    p += 2;
    p += field(p, "lordos", 6);
    p += field(p, "12", 2);
    p += field(p, "MIT-MAGIC-COOKIE-1", 18);
    p += field(p, (const char *)COOKIE, 16);
    CHECK(p[0] == 0xff && p[1] == 0xff);                    /* FamilyWild */
    p += 2;
    p += field(p, "", 0);
    p += field(p, "12", 2);
    p += field(p, "MIT-MAGIC-COOKIE-1", 18);
    p += field(p, (const char *)COOKIE, 16);
    CHECK((size_t)(p - b) == n);
    /* no host name: only the wild entry */
    CHECK(xauth_encode(b, sizeof b, NULL, 0, COOKIE) == e - 1);       /* "0": one digit */
    CHECK(xauth_encode(b, sizeof b, "", 0, COOKIE) == e - 1);
    CHECK(b[0] == 0xff && b[1] == 0xff && b[4] == 0 && b[5] == 1 && b[6] == '0');
    /* too small, bad arguments */
    CHECK(xauth_encode(b, e + 6 + e - 1, "lordos", 12, COOKIE) == 0);
    CHECK(xauth_encode(b, sizeof b, "lordos", -1, COOKIE) == 0);
    CHECK(xauth_encode(b, sizeof b, "lordos", 1, NULL) == 0);
    char big[300];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK(xauth_encode(b, sizeof b, big, 1, COOKIE) == 0);
}

static int read_all(const char *p, unsigned char *b, size_t cap) {
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, b, cap);
    close(fd);
    return (int)r;
}

static int count_tmp(const char *dir) {
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "ls -a '%s' | grep -c '\\.tmp-' > /dev/null", dir);
    return system(cmd) == 0;                                /* 1: a temporary file was left */
}

static void test_write(void) {
    char dir[] = "/tmp/xauth-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    chmod(dir, 0700);
    unsigned char data[256], back[256];
    size_t n = xauth_encode(data, sizeof data, "h", 3, COOKIE);
    char path[PATH_MAX], victim[PATH_MAX], link[PATH_MAX];

    /* 0600 even with an open umask; the exact bytes */
    mode_t old = umask(0);
    CHECK(xauth_write(dir, "lord-xauth.3", data, n, path, sizeof path) == 0);
    umask(old);
    struct stat st;
    CHECK(!lstat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600 && st.st_uid == getuid());
    CHECK(read_all(path, back, sizeof back) == (int)n && !memcmp(back, data, n));
    CHECK(!count_tmp(dir));

    /* an old file at that name is replaced */
    data[n - 1] ^= 0x55;
    CHECK(xauth_write(dir, "lord-xauth.3", data, n, path, sizeof path) == 0);
    CHECK(read_all(path, back, sizeof back) == (int)n && !memcmp(back, data, n));

    /* a link put at that name is replaced, never followed: its target keeps its content */
    snprintf(victim, sizeof victim, "%s/victim", dir);
    int vf = open(victim, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(vf >= 0 && write(vf, "keep", 4) == 4);
    if (vf >= 0) close(vf);
    snprintf(link, sizeof link, "%s/lord-xauth.4", dir);
    CHECK(symlink(victim, link) == 0);
    CHECK(xauth_write(dir, "lord-xauth.4", data, n, path, sizeof path) == 0);
    CHECK(!lstat(link, &st) && S_ISREG(st.st_mode));
    CHECK(read_all(victim, back, sizeof back) == 4 && !memcmp(back, "keep", 4));

    /* the temporary name: O_EXCL and O_NOFOLLOW, whatever is there (a file, a link) stays untouched */
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    CHECK(symlinkat(victim, dfd, "t-link") == 0);
    errno = 0;
    CHECK(xauth_create_at(dfd, "t-link", data, n) == -1 && errno == EEXIST);
    CHECK(read_all(victim, back, sizeof back) == 4 && !memcmp(back, "keep", 4));
    CHECK(symlinkat("/nonexistent-xauth-target", dfd, "t-dangling") == 0);
    CHECK(xauth_create_at(dfd, "t-dangling", data, n) == -1 && errno == EEXIST);
    CHECK(access("/nonexistent-xauth-target", F_OK) != 0);
    CHECK(xauth_create_at(dfd, "t-new", data, n) == 0);
    CHECK(xauth_create_at(dfd, "t-new", data, n) == -1 && errno == EEXIST);
    CHECK(xauth_create_at(dfd, "a/b", data, n) == -1 && errno == EINVAL);
    CHECK(xauth_create_at(dfd, "", data, n) == -1);
    close(dfd);

    /* a folder others can read, or a bad name: refused, nothing written */
    chmod(dir, 0750);
    CHECK(xauth_write(dir, "lord-xauth.5", data, n, path, sizeof path) == -1 && errno == EPERM && !path[0]);
    snprintf(link, sizeof link, "%s/lord-xauth.5", dir);
    CHECK(access(link, F_OK) != 0);
    chmod(dir, 0700);
    CHECK(xauth_write(dir, "../x", data, n, path, sizeof path) == -1 && errno == EINVAL);
    CHECK(xauth_write(dir, "..", data, n, path, sizeof path) == -1);
    CHECK(xauth_write("relative", "x", data, n, path, sizeof path) == -1);
    CHECK(xauth_write(dir, "lord-xauth.6", data, n, path, 8) == -1 && errno == ENAMETOOLONG);
    /* a directory at the name: it stays, and no temporary file is left */
    snprintf(link, sizeof link, "%s/lord-xauth.7", dir);
    CHECK(mkdir(link, 0700) == 0);
    CHECK(xauth_write(dir, "lord-xauth.7", data, n, path, sizeof path) == -1);
    CHECK(!lstat(link, &st) && S_ISDIR(st.st_mode));
    CHECK(!count_tmp(dir));

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
}

/* the file's text, for the source checks below */
static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void test_env_drop(void) {
    setenv("DISPLAY", ":1", 1);
    setenv("XAUTHORITY", "/run/user/1000/lord-xauth.1", 1);
    setenv("WAYLAND_DISPLAY", "wayland-1", 1);
    xauth_env_drop();
    CHECK(!getenv("DISPLAY"));
    CHECK(!getenv("XAUTHORITY"));
    CHECK(getenv("WAYLAND_DISPLAY") != NULL);
    /* the two launchers of programs without X use it, and nothing there unsets only DISPLAY */
    const char *const files[] = { "src/app/appspawn.c", "src/apps.c", NULL };
    for (int i = 0; files[i]; i++) {
        char *s = slurp(files[i]);
        CHECK(s != NULL);
        if (!s) continue;
        CHECK(strstr(s, "xauth_env_drop();") != NULL);
        CHECK(strstr(s, "unsetenv(\"DISPLAY\")") == NULL);
    }
    /* Xwayland gets -auth with the cookie's file, and the session XAUTHORITY */
    char *s = slurp("src/wl/xwayland.c");
    CHECK(s && strstr(s, "\"-auth\", XW.auth"));
    CHECK(s && strstr(s, "setenv(\"XAUTHORITY\""));
}

int main(void) {
    test_encode();
    test_write();
    test_env_drop();
    printf("xauth-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
