/* lordlog_test.c — LordOS's log files (src/lordlog.c): names, the folder a user's logs go to, the
 * root side's folder against symlink tricks, the cap, rotation, the line format and the stamping
 * child end to end. Everything in a temporary folder; runs as any user. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/lordlog.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char T[512];

static void sh(const char *fmt, const char *a) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, fmt, a, a, a);
    if (system(cmd)) { /* best effort */ }
}

static mode_t mode_of(const char *p) { struct stat st; return lstat(p, &st) ? (mode_t)-1 : st.st_mode & 07777; }

static void put(const char *p, const char *text) {
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *p) {
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static off_t size_of(const char *p) { struct stat st; return stat(p, &st) ? -1 : st.st_size; }

static void test_names(void) {
    CHECK(lordlog_name_ok("compositor"), "plain");
    CHECK(lordlog_name_ok("org.gnome.Calculator"), "dots");
    CHECK(lordlog_name_ok("gmail-default+1"), "dash, plus");
    CHECK(!lordlog_name_ok(""), "empty");
    CHECK(!lordlog_name_ok(".."), "dot dot");
    CHECK(!lordlog_name_ok(".hidden"), "leading dot");
    CHECK(!lordlog_name_ok("-x"), "leading dash");
    CHECK(!lordlog_name_ok("a/b"), "slash");
    CHECK(!lordlog_name_ok("a b"), "space");
    CHECK(!lordlog_name_ok("a\nb"), "newline");
    char longname[LORDLOG_NAME_MAX + 2];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(!lordlog_name_ok(longname), "too long");
    char out[LORDLOG_NAME_MAX + 1];
    lordlog_sanitize("../../etc/passwd", out, sizeof out);
    CHECK(!strcmp(out, "_._.._etc_passwd") && lordlog_name_ok(out) && !strchr(out, '/'), "path: %s", out);
    lordlog_sanitize(".bashrc", out, sizeof out);
    CHECK(!strcmp(out, "_bashrc"), "leading dot: %s", out);
    lordlog_sanitize("", out, sizeof out);
    CHECK(!strcmp(out, "_"), "empty: %s", out);
    lordlog_sanitize(NULL, out, sizeof out);
    CHECK(!strcmp(out, "_"), "null: %s", out);
    lordlog_sanitize("Señal app", out, sizeof out);
    CHECK(lordlog_name_ok(out) && !strchr(out, ' '), "utf-8 and space: %s", out);
    lordlog_sanitize(longname, out, sizeof out);
    CHECK(strlen(out) == LORDLOG_NAME_MAX && lordlog_name_ok(out), "cut: %zu", strlen(out));
    char small[4];
    lordlog_sanitize("abcdef", small, sizeof small);
    CHECK(!strcmp(small, "abc"), "small buffer: %s", small);
}

static void test_prepare(void) {
    uid_t me = geteuid();
    gid_t g = getegid();
    char root[PATH_MAX], p[PATH_MAX * 2], q[PATH_MAX * 3];
    snprintf(root, sizeof root, "%s/var-log-lordos", T);
    /* made from nothing: root 0755, users 0711, the user's 0700 */
    CHECK(lordlog_prepare_user_at(root, "alex", me, g, me) == 0, "first time: %s", strerror(errno));
    snprintf(p, sizeof p, "%s/users", root);
    CHECK(mode_of(root) == 0755 && mode_of(p) == 0711, "modes %o %o", mode_of(root), mode_of(p));
    snprintf(p, sizeof p, "%s/users/alex", root);
    CHECK(mode_of(p) == 0700, "user mode %o", mode_of(p));
    /* a loose mode is fixed on the fd; what is inside is never touched */
    chmod(p, 0755);
    snprintf(q, sizeof q, "%s/compositor.log", p);
    put(q, "x\n");
    chmod(q, 0644);
    CHECK(lordlog_prepare_user_at(root, "alex", me, g, me) == 0, "again");
    CHECK(mode_of(p) == 0700 && mode_of(q) == 0644, "fixed %o, inside %o", mode_of(p), mode_of(q));
    /* bad names */
    CHECK(lordlog_prepare_user_at(root, "../x", me, g, me) < 0, "../x");
    CHECK(lordlog_prepare_user_at(root, ".x", me, g, me) < 0, ".x");
    CHECK(lordlog_prepare_user_at(root, "a/b", me, g, me) < 0, "a/b");
    CHECK(lordlog_prepare_user_at(root, "", me, g, me) < 0, "empty");
    CHECK(lordlog_prepare_user_at("relative", "alex", me, g, me) < 0, "relative root");
    /* users/<user> is a symlink to a folder elsewhere: refused, and the target keeps its mode */
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s/elsewhere", T);
    mkdir(target, 0755);
    snprintf(p, sizeof p, "%s/users/bob", root);
    CHECK(!symlink(target, p), "symlink made");
    CHECK(lordlog_prepare_user_at(root, "bob", me, g, me) < 0, "symlink refused");
    CHECK(mode_of(target) == 0755, "target untouched: %o", mode_of(target));
    /* a symlink to a file, and a plain file */
    snprintf(q, sizeof q, "%s/elsewhere/secret", T);
    put(q, "s\n");
    chmod(q, 0600);
    snprintf(p, sizeof p, "%s/users/carol", root);
    CHECK(!symlink(q, p), "symlink to a file");
    CHECK(lordlog_prepare_user_at(root, "carol", me, g, me) < 0 && mode_of(q) == 0600, "file symlink refused");
    snprintf(p, sizeof p, "%s/users/dave", root);
    put(p, "");
    CHECK(lordlog_prepare_user_at(root, "dave", me, g, me) < 0, "a file refused");
    /* a dangling symlink is not followed into creating anything */
    snprintf(p, sizeof p, "%s/users/erin", root);
    snprintf(q, sizeof q, "%s/elsewhere/erin-made", T);
    CHECK(!symlink(q, p), "dangling symlink");
    CHECK(lordlog_prepare_user_at(root, "erin", me, g, me) < 0 && access(q, F_OK), "dangling refused, nothing made");
    /* root or users being symlinks, or writable by others, or someone else's */
    char root2[PATH_MAX];
    snprintf(root2, sizeof root2, "%s/root-link", T);
    CHECK(!symlink(root, root2), "root link");
    CHECK(lordlog_prepare_user_at(root2, "alex", me, g, me) < 0, "root symlink refused");
    char root3[PATH_MAX];
    snprintf(root3, sizeof root3, "%s/root3", T);
    mkdir(root3, 0755);
    snprintf(p, sizeof p, "%s/users", root3);
    CHECK(!symlink(target, p), "users link");
    CHECK(lordlog_prepare_user_at(root3, "alex", me, g, me) < 0, "users symlink refused");
    snprintf(q, sizeof q, "%s/alex", target);
    CHECK(access(q, F_OK), "nothing made through the users symlink");
    char root4[PATH_MAX];
    snprintf(root4, sizeof root4, "%s/root4", T);
    mkdir(root4, 0777);
    chmod(root4, 0777);
    CHECK(lordlog_prepare_user_at(root4, "alex", me, g, me) < 0, "world-writable root refused");
    chmod(root4, 0775);
    CHECK(lordlog_prepare_user_at(root4, "alex", me, g, me) < 0, "group-writable root refused");
    CHECK(lordlog_prepare_user_at(root, "alex", me, g, me + 1) < 0, "root not owned by the trusted user");
    /* not root: handing a folder to another uid fails cleanly, and leaves it as it was */
    if (me != 0) {
        snprintf(p, sizeof p, "%s/users/frank", root);
        mkdir(p, 0750);
        chmod(p, 0750);
        CHECK(lordlog_prepare_user_at(root, "frank", me + 1, g, me) < 0 && mode_of(p) == 0750, "chown refused, unchanged %o", mode_of(p));
    }
}

static void test_user_dir(void) {
    uid_t me = getuid();
    char root[PATH_MAX], home[PATH_MAX], out[PATH_MAX], want[PATH_MAX * 2];
    snprintf(root, sizeof root, "%s/ud-root", T);
    snprintf(home, sizeof home, "%s/ud-home", T);
    mkdir(home, 0700);
    /* no /var/log/lordos: ~/.cache/lordos, made */
    CHECK(lordlog_user_dir_at(root, "alex", me, home, out, sizeof out) == 0, "fallback");
    snprintf(want, sizeof want, "%s/.cache/lordos", home);
    CHECK(!strcmp(out, want) && mode_of(want) == 0700, "fallback path %s", out);
    /* prepared: there */
    CHECK(lordlog_prepare_user_at(root, "alex", me, getgid(), me) == 0, "prepare");
    CHECK(lordlog_user_dir_at(root, "alex", me, home, out, sizeof out) == 1, "system dir");
    snprintf(want, sizeof want, "%s/users/alex", root);
    CHECK(!strcmp(out, want), "system path %s", out);
    /* group access, a symlink, a bad name or someone else's: the fallback */
    chmod(want, 0750);
    CHECK(lordlog_user_dir_at(root, "alex", me, home, out, sizeof out) == 0, "0750 not used");
    chmod(want, 0700);
    CHECK(lordlog_user_dir_at(root, "alex", me + 1, home, out, sizeof out) <= 0, "another uid's not used");
    char link[PATH_MAX * 2];
    snprintf(link, sizeof link, "%s/users/bob", root);
    CHECK(!symlink(want, link), "link");
    CHECK(lordlog_user_dir_at(root, "bob", me, home, out, sizeof out) == 0, "symlink not used");
    CHECK(lordlog_user_dir_at(root, "../alex", me, home, out, sizeof out) == 0, "bad name not used");
    CHECK(lordlog_user_dir_at(root, NULL, me, home, out, sizeof out) == 0, "no name");
    CHECK(lordlog_user_dir_at(root, "alex", me, NULL, out, sizeof out) == 1, "no home needed");
    CHECK(lordlog_user_dir_at(root, "nobody-here", me, "relative", out, sizeof out) == -1 && !out[0], "nothing");
}

static void test_cap_rotate(void) {
    char p[PATH_MAX], p1[PATH_MAX + 4];
    snprintf(p, sizeof p, "%s/cap.log", T);
    snprintf(p1, sizeof p1, "%s.1", p);
    CHECK(lordlog_cap(p, 100) == 0, "missing: nothing to do");
    int fd = lordlog_open(p);
    CHECK(fd >= 0 && mode_of(p) == 0600, "opened 0600: %o", mode_of(p));
    char line[64];
    for (int i = 0; i < 100; i++) {                  /* 100 lines of 20 bytes */
        snprintf(line, sizeof line, "line %03d xxxxxxxxxx\n", i);
        if (write(fd, line, strlen(line)) < 0) break;
    }
    CHECK(size_of(p) == 2000, "size %lld", (long long)size_of(p));
    CHECK(lordlog_cap(p, 5000) == 0 && size_of(p) == 2000 && access(p1, F_OK), "below the cap: untouched");
    CHECK(lordlog_cap(p, 1000) == 1, "capped");
    CHECK(size_of(p) == 0, "emptied: %lld", (long long)size_of(p));
    char *old = slurp(p1);
    CHECK(old && strlen(old) <= 1000 && strlen(old) >= 980 && !strncmp(old, "line 05", 7) && strstr(old, "line 099"),
          ".1 has the newest part from a line start: %.20s (%zu)", old ? old : "", old ? strlen(old) : 0);
    /* a writer holding it with O_APPEND goes on at the start */
    if (write(fd, "after\n", 6) != 6) {}
    CHECK(size_of(p) == 6, "append after cap: %lld", (long long)size_of(p));
    close(fd);
    /* open caps a big one first */
    put(p, "");
    fd = open(p, O_WRONLY | O_APPEND);
    for (int i = 0; i < 1100; i++) if (write(fd, "0123456789abcde\n", 16) < 0) break;  /* 17.6 KB */
    close(fd);
    CHECK(lordlog_cap(p, 16 * 1024) == 1 && size_of(p) == 0, "cap at 16 KB");
    /* symlinks are never followed */
    char target[PATH_MAX], lp[PATH_MAX];
    snprintf(target, sizeof target, "%s/cap-target", T);
    snprintf(lp, sizeof lp, "%s/link.log", T);
    put(target, "0123456789012345678901234567890123456789\n");
    CHECK(!symlink(target, lp), "link");
    CHECK(lordlog_cap(lp, 10) < 0 && size_of(target) == 41, "cap refuses a symlink");
    CHECK(lordlog_open(lp) < 0 && size_of(target) == 41, "open refuses a symlink");
    char fifo[PATH_MAX];
    snprintf(fifo, sizeof fifo, "%s/fifo.log", T);
    CHECK(!mkfifo(fifo, 0600) && lordlog_open(fifo) < 0, "open refuses a fifo");
    /* rotation: the previous one as .1, an empty one is not rotated */
    char r[PATH_MAX], r1[PATH_MAX + 4];
    snprintf(r, sizeof r, "%s/rot.log", T);
    snprintf(r1, sizeof r1, "%s.1", r);
    put(r, "previous session\n");
    lordlog_rotate(r);
    CHECK(access(r, F_OK) && !strcmp(slurp(r1) ? slurp(r1) : "", "previous session\n"), "rotated");
    put(r, "");
    lordlog_rotate(r);
    CHECK(!access(r, F_OK) && !strcmp(slurp(r1) ? slurp(r1) : "", "previous session\n"), "an empty one stays");
    unlink(r);
    CHECK(!symlink(target, r), "rot link");
    lordlog_rotate(r);
    CHECK(size_of(target) == 41 && !strcmp(slurp(r1) ? slurp(r1) : "", "previous session\n"), "a symlink is not rotated");
}

static void test_format(void) {
    char sub[64];
    const char *msg;
    CHECK(lordlog_subsystem("xwayland: started", "compositor", sub, sizeof sub, &msg) == 1 && !strcmp(sub, "xwayland") && !strcmp(msg, "started"),
          "prefix: %s / %s", sub, msg);
    CHECK(lordlog_subsystem("lord-sessiond[123]: new session", "x", sub, sizeof sub, &msg) == 1 && !strcmp(sub, "lord-sessiond[123]") &&
          !strcmp(msg, "new session"), "pid: %s", sub);
    CHECK(lordlog_subsystem("no prefix here", "compositor", sub, sizeof sub, &msg) == 0 && !strcmp(sub, "compositor") &&
          !strcmp(msg, "no prefix here"), "none");
    CHECK(lordlog_subsystem("https://example.org: down", "c", sub, sizeof sub, &msg) == 0, "an URL is not one");
    CHECK(lordlog_subsystem("Error: boom", "c", sub, sizeof sub, &msg) == 0 && !strcmp(sub, "c"), "a level word is not one");
    CHECK(lordlog_subsystem("js:", "c", sub, sizeof sub, &msg) == 1 && !strcmp(sub, "js") && !*msg, "empty message");
    CHECK(lordlog_subsystem("a very long name that goes on: x", "c", sub, sizeof sub, &msg) == 0, "spaces");
    CHECK(lordlog_subsystem("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa: x", "c", sub, sizeof sub, &msg) == 0, "too long");
    char tiny[4];
    CHECK(lordlog_subsystem("xwayland: x", "c", tiny, sizeof tiny, &msg) == 0, "doesn't fit");
    CHECK(!strcmp(lordlog_level("could not open the card"), "WARN"), "warn");
    CHECK(!strcmp(lordlog_level("start failed: no such file"), "ERROR"), "error");
    CHECK(!strcmp(lordlog_level("TypeError: x is undefined"), "ERROR"), "a JS exception");
    CHECK(!strcmp(lordlog_level("Segfault in thread"), "ERROR"), "case");
    CHECK(!strcmp(lordlog_level("ready"), "INFO"), "info");
    CHECK(!strcmp(lordlog_level("terrorist"), "INFO"), "word start only");
    char out[256];
    size_t n = lordlog_format(out, sizeof out, 1762876436.5, "INFO", "xwayland", "started");
    int y, mo, d, h, mi, s, ms, oh, om;
    char sign, lv[8], rest[64];
    CHECK(n == strlen(out) && out[n - 1] == '\n', "newline");
    CHECK(sscanf(out, "%4d-%2d-%2dT%2d:%2d:%2d.%3d%c%2d:%2d %7s %63[^\n]", &y, &mo, &d, &h, &mi, &s, &ms, &sign, &oh, &om, lv, rest) == 12 &&
          y == 2025 && ms == 500 && (sign == '+' || sign == '-') && !strcmp(lv, "INFO") && !strcmp(rest, "xwayland: started"), "shape: %s", out);
    char small[16];
    n = lordlog_format(small, sizeof small, 0, "INFO", "x", "a long message that does not fit");
    CHECK(n == 15 && small[14] == '\n' && !small[15], "cut with a newline: %zu", n);
}

/* lord-server's session start in a child: its stderr through the lord-log child into the files */
static void session_once(const char *home, const char *a, const char *b) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", home, 1);
        char root[PATH_MAX];
        snprintf(root, sizeof root, "%s/no-such-root", T);
        lordlog_root = root;                        /* never the machine's real /var/log/lordos */
        if (lordlog_session_start()) _exit(2);
        fprintf(stderr, "lord-server: %s\n", a);
        fprintf(stderr, "js: TypeError: %s\n    at f (theme.js:1)\n", b);
        fprintf(stderr, "an error without a prefix failed\n");
        printf("stdout line\n");
        fflush(stdout);
        char big[10000];
        memset(big, 'z', sizeof big - 1);
        big[sizeof big - 1] = 0;
        fprintf(stderr, "%s\n", big);
        fprintf(stderr, "no newline at the end");
        fflush(stderr);                             /* _exit does not flush a line-buffered stderr */
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "session child: %d", st);
}

static int wait_for(const char *p, const char *text) {
    for (int i = 0; i < 200; i++) {
        char *s = slurp(p);
        if (s && strstr(s, text)) return 1;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void test_session(void) {
    char home[PATH_MAX], comp[PATH_MAX * 2], shell[PATH_MAX * 2], comp1[PATH_MAX * 2 + 4], app[PATH_MAX * 2];
    snprintf(home, sizeof home, "%s/s-home", T);
    mkdir(home, 0700);
    snprintf(comp, sizeof comp, "%s/.cache/lordos/compositor.log", home);
    snprintf(shell, sizeof shell, "%s/.cache/lordos/shell.log", home);
    snprintf(comp1, sizeof comp1, "%s.1", comp);
    session_once(home, "first session", "one");
    CHECK(wait_for(comp, "no newline at the end"), "first written");
    char *c = slurp(comp);
    CHECK(c && strstr(c, " INFO  lord-server: first session\n"), "stamped with its subsystem:\n%s", c ? c : "");
    CHECK(c && strstr(c, " ERROR compositor: an error without a prefix failed\n"), "default subsystem, level");
    CHECK(c && strstr(c, " compositor: stdout line\n"), "stdout too");
    CHECK(c && !strstr(c, "TypeError"), "the shell's lines are not here");
    CHECK(c && strstr(c, "zzzzzzzz") && strstr(c, " compositor: no newline at the end\n"), "long line and the last bytes");
    CHECK(wait_for(shell, "theme.js"), "shell written");
    char *s = slurp(shell);
    CHECK(s && strstr(s, " js: TypeError: one\n") && strstr(s, " js:     at f (theme.js:1)\n"), "shell with its stack:\n%s", s ? s : "");
    CHECK(mode_of(comp) == 0600 && mode_of(shell) == 0600, "0600");
    /* the next session keeps this one as .1 */
    session_once(home, "second session", "two");
    CHECK(wait_for(comp, "second session"), "second written");
    c = slurp(comp);
    CHECK(c && !strstr(c, "first session"), "new file");
    char *c1 = slurp(comp1);
    CHECK(c1 && strstr(c1, "first session"), "previous session kept as .1");
    /* lordlog_path: apps/ and web/ under the same folder, names sanitized */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", home, 1);
        char root[PATH_MAX];
        snprintf(root, sizeof root, "%s/no-such-root", T);
        lordlog_root = root;
        char p[PATH_MAX];
        int ok = !lordlog_path("apps", "../evil app", p, sizeof p) && strstr(p, "/.cache/lordos/apps/_._evil_app.log") &&
                 !lordlog_path("web", "gmail-default", p, sizeof p) && strstr(p, "/.cache/lordos/web/gmail-default.log") &&
                 !lordlog_path(NULL, "xwayland", p, sizeof p) && strstr(p, "/.cache/lordos/xwayland.log") &&
                 lordlog_path("../x", "a", p, sizeof p) && !lordlog_dir_is_system();
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "lordlog_path");
    snprintf(app, sizeof app, "%s/.cache/lordos/apps", home);
    CHECK(mode_of(app) == 0700, "apps/ 0700: %o", mode_of(app));
}

int main(void) {
    snprintf(T, sizeof T, "%s/lordlog-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(T)) { perror("mkdtemp"); return 1; }
    umask(022);
    test_names();
    test_prepare();
    test_user_dir();
    test_cap_rotate();
    test_format();
    test_session();
    if (!getenv("LORDLOG_TEST_KEEP")) sh("rm -rf '%s'", T);
    printf("lordlog-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
