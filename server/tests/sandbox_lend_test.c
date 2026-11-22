/* sandbox_lend_test.c — a Flatpak app's shared runtime folders and the lent chosen file (sec/sandbox.c; docs/FLATPAK.md
 * "Shared runtime folders" and "Lending a chosen file"; approved by the maintainer 2026-11-22), with a fake app:
 *   1. two views of the same app (the same share folders) see the same .flatpak/<id> (Flatpak's per-app /tmp) and
 *      app/<id>; a view of another app doesn't;
 *   2. a file lent into a running view (the test plays lord-server on the lending socket) is readable and writable at
 *      <runtime>/app/<id>/doc/<grant>/<name>, while a file next to it in the same folder is not there, and the lent
 *      folder can't be written; with bubblewrap installed, the same from inside a bubblewrap sandbox that binds
 *      app/<id> as its runtime folder before the grant is made (as `flatpak run` does): the grant reaches it;
 *   3. a save: only the chosen file, writable, alone in its grant: nothing else of its folder is seen (Claude Security F5);
 *   4. a folder; a stale inode, a link on the way, a bad name and a path not written the canonical way ("/./", "//",
 *      Claude Security F1/F2) are refused.
 * Needs unprivileged user namespaces (exit 77 without). */
#include "../src/sec/sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static char base[1024], home[1100], rt[1100];

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { perror(path); exit(1); }
    if (write(fd, text, strlen(text)) < 0) {}
    close(fd);
}

static int get(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { out[0] = 0; return -1; }
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    out[r > 0 ? r : 0] = 0;
    return r < 0 ? -1 : 0;
}

static void share_dirs(const char *tag, char src[2][1400]) {
    char p[1300];
    snprintf(p, sizeof p, "%s/lordos", rt); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/lordos/fpshare", rt); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/lordos/fpshare/%s.0", rt, tag); mkdir(p, 0700);
    snprintf(src[0], 1400, "%s/flatpak", p); mkdir(src[0], 0700);
    snprintf(src[1], 1400, "%s/app", p); mkdir(src[1], 0700);
}

/* a line to and from the fake app */
static void say(int fd, const char *s) { if (write(fd, s, strlen(s) + 1) < 0) {} }
static int hear(int fd, char *out, size_t n, int ms) {
    size_t got = 0;
    while (got + 1 < n) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0) break;
        ssize_t r = read(fd, out + got, 1);
        if (r <= 0) break;
        if (!out[got]) return 0;
        got++;
    }
    out[got] = 0;
    return -1;
}

/* The fake app: does what the test says, one command per line, and answers one line.
 *   "w PATH TEXT"  write TEXT to PATH            → "ok" | errno text
 *   "r PATH"       read PATH                     → its text | "!" errno text
 *   "m PATH"       mkdir PATH                    → "ok" | errno text
 *   "l PATH"       the names in folder PATH      → "name,name," | "!" errno text
 *   "q"            leave */
static void fake_app(int in, int out) {
    char line[8192], ans[8192];
    for (;;) {
        if (hear(in, line, sizeof line, 20000)) _exit(0);
        if (line[0] == 'q') _exit(0);
        char *path = line + 2, *text = strchr(path, ' ');
        if (text) *text++ = 0;
        if (line[0] == 'w') {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd < 0 || write(fd, text ? text : "", text ? strlen(text) : 0) < 0) snprintf(ans, sizeof ans, "%s", strerror(errno));
            else snprintf(ans, sizeof ans, "ok");
            if (fd >= 0) close(fd);
        } else if (line[0] == 'r') {
            char buf[4096];
            if (get(path, buf, sizeof buf)) snprintf(ans, sizeof ans, "!%s", strerror(errno)); else snprintf(ans, sizeof ans, "%s", buf);
        } else if (line[0] == 'l') {
            DIR *d = opendir(path);
            if (!d) snprintf(ans, sizeof ans, "!%s", strerror(errno));
            else {
                size_t o = 0;
                ans[0] = 0;
                for (struct dirent *e; (e = readdir(d)) && o < sizeof ans - 300;)
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) o += (size_t)snprintf(ans + o, sizeof ans - o, "%s,", e->d_name);
                closedir(d);
            }
        } else if (line[0] == 'm') {
            snprintf(ans, sizeof ans, "%s", mkdir(path, 0700) ? strerror(errno) : "ok");
        } else snprintf(ans, sizeof ans, "?");
        say(out, ans);
    }
}

typedef struct { int to, from, lend; } View;

/* A view of app `id` with the share folders src (NULL: none), lending when lend is set; bwrap: the app runs inside
 * bubblewrap with app/<id> bound as the runtime folder (and the home hidden), as `flatpak run` does. */
static int start_view(const char *id, char src[2][1400], int lend, int bwrap, View *v) {
    int a[2], b[2], s[2] = { -1, -1 }, rep[2];
    if (pipe(a) || pipe(b) || pipe(rep) || (lend && socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, s))) return -1;
    pid_t t = fork();
    if (t == 0) {
        close(a[1]); close(b[0]); close(rep[0]);
        if (s[0] >= 0) close(s[0]);
        static char dst0[256], dst1[256], ld[256];
        snprintf(dst0, sizeof dst0, ".flatpak/%s", id);
        snprintf(dst1, sizeof dst1, "app/%s", id);
        snprintf(ld, sizeof ld, "app/%s/doc", id);
        SandboxFilesCfg cfg = { .home = home, .runtime_dir = rt, .keep_home = 1, .home_ro = 1 };
        if (src) { cfg.share_src[0] = src[0]; cfg.share_src[1] = src[1]; cfg.share_dst[0] = dst0; cfg.share_dst[1] = dst1; }
        if (lend) { cfg.lend_sock = s[1]; cfg.lend_dir = ld; }
        if (sandbox_files_isolate(&cfg)) { if (write(rep[1], "S", 1) < 0) {} _exit(0); }
        if (write(rep[1], "V", 1) < 0) {}
        close(rep[1]);
        sandbox_view_helper(0);
        if (!bwrap) fake_app(a[0], b[1]);
        /* in bubblewrap: the runtime folder is app/<id>, the home hidden, as Flatpak's sandbox has them */
        char appdir[1400], self[64], fa[16], fb[16];
        snprintf(appdir, sizeof appdir, "%s/app/%s", rt, id);
        snprintf(fa, sizeof fa, "%d", a[0]);
        snprintf(fb, sizeof fb, "%d", b[1]);
        snprintf(self, sizeof self, "/proc/%d/exe", (int)getpid());
        char exe[4096];
        ssize_t n = readlink(self, exe, sizeof exe - 1);
        if (n <= 0) _exit(1);
        exe[n] = 0;
        char *argv[] = { "bwrap", "--unshare-user", "--unshare-pid", "--dev-bind", "/", "/", "--tmpfs", home,
                         "--bind", appdir, rt, "--die-with-parent", exe, "--fake-app", fa, fb, NULL };
        execv("/usr/bin/bwrap", argv);
        _exit(1);
    }
    close(a[0]); close(b[1]); close(rep[1]);
    if (s[1] >= 0) close(s[1]);
    int st;
    waitpid(t, &st, 0);
    char c = 0;
    if (read(rep[0], &c, 1) != 1 || c == 'S') { close(rep[0]); return 77; }
    close(rep[0]);
    v->to = a[1]; v->from = b[0]; v->lend = s[0];
    /* PID 1's hello, with its pidfd */
    if (lend) {
        SbLendMsg *m = calloc(1, sizeof *m);
        char cb[256];
        struct iovec iov = { m, sizeof *m };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
        struct pollfd p = { s[0], POLLIN, 0 };
        int ok = poll(&p, 1, 5000) == 1 && recvmsg(s[0], &mh, MSG_CMSG_CLOEXEC) == (ssize_t)sizeof *m && m->type == SB_LEND_HELLO;
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h))
            if (h->cmsg_type == SCM_RIGHTS) { int f; memcpy(&f, CMSG_DATA(h), sizeof f); close(f); }
        free(m);
        CHECK(ok, "PID 1 said hello with its pidfd");
    }
    return 0;
}

static void ask(View *v, const char *cmd, char *ans, size_t n) {
    say(v->to, cmd);
    if (hear(v->from, ans, n, 10000)) snprintf(ans, n, "(no answer)");
}

static void stop(View *v) {
    say(v->to, "q");
    close(v->to); close(v->from);
    if (v->lend >= 0) close(v->lend);
}

/* lord-server's side of one grant: 0 and the grant, or the view's errno */
static int lend(View *v, int kind, const char *path, const char *name, int rw, const char *target, char *grant) {
    SbLendMsg *q = calloc(1, sizeof *q);
    struct stat st, tt;
    q->magic = SB_LEND_MAGIC; q->type = SB_LEND_LEND; q->seq = 7; q->kind = (uint32_t)kind; q->rw = (uint32_t)rw;
    snprintf(q->path, sizeof q->path, "%s", path);
    snprintf(q->name, sizeof q->name, "%s", name);
    if (!lstat(path, &st)) { q->dev = st.st_dev; q->ino = st.st_ino; }
    if (target && !stat(target, &tt)) { q->tdev = tt.st_dev; q->tino = tt.st_ino; }
    int r = EIO;
    if (send(v->lend, q, sizeof *q, 0) == (ssize_t)sizeof *q) {
        struct pollfd p = { v->lend, POLLIN, 0 };
        if (poll(&p, 1, 5000) == 1 && recv(v->lend, q, sizeof *q, 0) == (ssize_t)sizeof *q && q->type == SB_LEND_DONE && q->seq == 7) {
            r = q->status;
            if (!r) snprintf(grant, 33, "%s", q->grant);
        }
    }
    free(q);
    return r;
}

static void reap(void) { while (waitpid(-1, NULL, WNOHANG) > 0) {} }

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--fake-app")) { fake_app(atoi(argv[2]), atoi(argv[3])); return 0; }
    CHECK(sandbox_lend_name_ok("a.txt") && !sandbox_lend_name_ok("") && !sandbox_lend_name_ok("..") &&
          !sandbox_lend_name_ok(".") && !sandbox_lend_name_ok("a/b"), "lent names: one plain path element");
    if (!getcwd(base, sizeof base - 40)) return 1;      /* not under /tmp: the view mounts its own there */
    strcat(base, "/build/sbx-lend-test.XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    snprintf(home, sizeof home, "%s/home", base);
    snprintf(rt, sizeof rt, "%s/rt", base);
    mkdir(home, 0700); mkdir(rt, 0700);
    char p[1400], q[4400], ans[8192], grant[33], src_a[2][1400], src_b[2][1400];
    snprintf(p, sizeof p, "%s/Docs", home); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/Docs/chosen.txt", home); put(p, "chosen");
    snprintf(p, sizeof p, "%s/Docs/sibling.txt", home); put(p, "sibling");
    snprintf(p, sizeof p, "%s/Docs/Sub", home); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/Docs/Sub/in.txt", home); put(p, "in");
    snprintf(p, sizeof p, "%s/link", home); symlink("Docs", p);
    share_dirs("org.example.App", src_a);
    share_dirs("org.example.Other", src_b);
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    signal(SIGPIPE, SIG_IGN);

    /* 1. the shared runtime folders */
    View a1, a2, b1;
    int r = start_view("org.example.App", src_a, 1, 0, &a1);
    if (r == 77) { printf("sandbox-lend-test: skipped: needs unprivileged user namespaces\n"); return 77; }
    CHECK(r == 0, "the first view of the app started");
    CHECK(start_view("org.example.App", src_a, 1, 0, &a2) == 0, "a second view of the same app started");
    CHECK(start_view("org.example.Other", src_b, 0, 0, &b1) == 0, "a view of another app started");
    snprintf(q, sizeof q, "w %s/.flatpak/org.example.App/tmp-sock first", rt); ask(&a1, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "ok"), "1: the first instance writes in its per-app folder");
    snprintf(q, sizeof q, "w %s/app/org.example.App/SingletonLock first", rt); ask(&a1, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "ok"), "1: the first instance writes in its app runtime folder");
    snprintf(q, sizeof q, "r %s/.flatpak/org.example.App/tmp-sock", rt); ask(&a2, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "first"), "1: a second launch sees the first one's per-app /tmp");
    snprintf(q, sizeof q, "r %s/app/org.example.App/SingletonLock", rt); ask(&a2, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "first"), "1: a second launch sees the first one's app runtime folder");
    snprintf(q, sizeof q, "r %s/app/org.example.App/SingletonLock", rt); ask(&b1, q, ans, sizeof ans);
    CHECK(ans[0] == '!', "1: another app doesn't see it");
    snprintf(q, sizeof q, "r %s/lordos/fpshare/org.example.App.0/app/SingletonLock", rt); ask(&b1, q, ans, sizeof ans);
    CHECK(ans[0] == '!', "1: another app can't reach the share folders at all (the view's runtime folder is its own)");
    snprintf(q, sizeof q, "r %s/app/org.example.Other/SingletonLock", rt); ask(&b1, q, ans, sizeof ans);
    CHECK(ans[0] == '!', "1: another app's own folder is empty");

    /* 2. a lent file */
    snprintf(p, sizeof p, "%s/Docs/chosen.txt", home);
    r = lend(&a1, SB_LEND_FILE, p, "chosen.txt", 1, NULL, grant);
    CHECK(r == 0 && strlen(grant) == 32, "2: the chosen file is lent");
    snprintf(q, sizeof q, "r %s/app/org.example.App/doc/%s/chosen.txt", rt, grant); ask(&a1, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "chosen"), "2: the app reads the lent file");
    snprintf(q, sizeof q, "w %s/app/org.example.App/doc/%s/chosen.txt edited", rt, grant); ask(&a1, q, ans, sizeof ans);
    CHECK(!strcmp(ans, "ok"), "2: and writes it");
    get(p, ans, sizeof ans);
    CHECK(!strcmp(ans, "edited"), "2: the write reached the real file");
    snprintf(q, sizeof q, "r %s/app/org.example.App/doc/%s/sibling.txt", rt, grant); ask(&a1, q, ans, sizeof ans);
    CHECK(ans[0] == '!', "2: the file next to it is not lent");
    snprintf(q, sizeof q, "w %s/app/org.example.App/doc/%s/new.txt x", rt, grant); ask(&a1, q, ans, sizeof ans);
    CHECK(strcmp(ans, "ok"), "2: nothing new can be made in the grant");
    snprintf(q, sizeof q, "m %s/app/org.example.App/doc/mine", rt); ask(&a1, q, ans, sizeof ans);
    CHECK(strcmp(ans, "ok"), "2: the app can't make folders where grants go");
    snprintf(q, sizeof q, "r %s/app/org.example.App/doc/%s/chosen.txt", rt, grant); ask(&a2, q, ans, sizeof ans);
    CHECK(ans[0] == '!', "2: another view of the same app doesn't get the grant");
    /* read-only */
    put(p, "chosen");
    r = lend(&a1, SB_LEND_FILE, p, "chosen.txt", 0, NULL, grant);
    snprintf(q, sizeof q, "w %s/app/org.example.App/doc/%s/chosen.txt x", rt, grant); ask(&a1, q, ans, sizeof ans);
    CHECK(r == 0 && strcmp(ans, "ok"), "2: a read-only grant can't be written");
    /* refused */
    char g2[33];
    CHECK(lend(&a1, SB_LEND_FILE, p, "chosen.txt", 1, NULL, g2) == 0, "2: the same file may be lent again");
    snprintf(q, sizeof q, "%s/link/chosen.txt", home);
    CHECK(lend(&a1, SB_LEND_FILE, q, "chosen.txt", 1, NULL, g2) != 0, "4: a path through a link is refused");
    CHECK(lend(&a1, SB_LEND_FILE, p, "../x", 1, NULL, g2) != 0, "4: a name with a slash is refused");
    snprintf(q, sizeof q, "%s/Docs/sibling.txt", home);
    {   /* the inode lord-server checked is not this one */
        SbLendMsg *m = calloc(1, sizeof *m);
        struct stat st;
        stat(q, &st);
        m->magic = SB_LEND_MAGIC; m->type = SB_LEND_LEND; m->seq = 9; m->kind = SB_LEND_FILE; m->rw = 1;
        snprintf(m->path, sizeof m->path, "%.1300s", p);
        snprintf(m->name, sizeof m->name, "chosen.txt");
        m->dev = st.st_dev; m->ino = st.st_ino;
        int ok = send(a1.lend, m, sizeof *m, 0) == (ssize_t)sizeof *m && recv(a1.lend, m, sizeof *m, 0) == (ssize_t)sizeof *m &&
                 m->type == SB_LEND_DONE && m->status == ESTALE;
        CHECK(ok, "4: a file that isn't the one checked is refused (ESTALE)");
        free(m);
    }

    /* 3. a save: the chosen file alone, writable; its folder is never lent (F5) */
    snprintf(q, sizeof q, "%s/Docs/new.txt", home);
    put(q, "");                                         /* lord-server makes the target first */
    r = lend(&a1, SB_LEND_SAVE, q, "new.txt", 1, NULL, grant);
    CHECK(r == 0, "3: a save is lent");
    char c2[8800];
    snprintf(c2, sizeof c2, "w %s/app/org.example.App/doc/%s/new.txt saved", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(!strcmp(ans, "ok"), "3: the app writes the chosen name");
    get(q, ans, sizeof ans);
    CHECK(!strcmp(ans, "saved"), "3: the save reached the real file");
    snprintf(c2, sizeof c2, "l %s/app/org.example.App/doc/%s", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(!strcmp(ans, "new.txt,"), "3: the grant holds the chosen name and nothing else of its folder");
    snprintf(c2, sizeof c2, "r %s/app/org.example.App/doc/%s/sibling.txt", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(ans[0] == '!', "3: a file next to it can't be read");
    snprintf(c2, sizeof c2, "r %s/app/org.example.App/doc/%s/Sub/in.txt", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(ans[0] == '!', "3: nor a folder next to it");
    snprintf(c2, sizeof c2, "w %s/app/org.example.App/doc/%s/other.txt x", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(strcmp(ans, "ok"), "3: nothing else can be written there");
    snprintf(c2, sizeof c2, "w %s/app/org.example.App/doc/%s/sibling.txt x", rt, grant); ask(&a1, c2, ans, sizeof ans);
    snprintf(p, sizeof p, "%s/Docs/sibling.txt", home);
    get(p, ans + 100, 100);
    CHECK(strcmp(ans, "ok") && !strcmp(ans + 100, "sibling"), "3: a file next to it can't be changed");
    snprintf(p, sizeof p, "%s/Docs", home);
    CHECK(lend(&a1, SB_LEND_SAVE, p, "Docs", 1, NULL, grant) != 0, "3: a save naming a folder is refused");
    {   /* the target lord-server checked is not this file */
        SbLendMsg *m = calloc(1, sizeof *m);
        struct stat st;
        snprintf(p, sizeof p, "%s/Docs/sibling.txt", home);
        stat(p, &st);
        m->magic = SB_LEND_MAGIC; m->type = SB_LEND_LEND; m->seq = 10; m->kind = SB_LEND_SAVE; m->rw = 1;
        snprintf(m->path, sizeof m->path, "%.1300s", q);
        snprintf(m->name, sizeof m->name, "new.txt");
        m->dev = st.st_dev; m->ino = st.st_ino;
        int ok = send(a1.lend, m, sizeof *m, 0) == (ssize_t)sizeof *m && recv(a1.lend, m, sizeof *m, 0) == (ssize_t)sizeof *m &&
                 m->type == SB_LEND_DONE && m->status == ESTALE;
        CHECK(ok, "3: a save target that isn't the checked file is refused (ESTALE)");
        free(m);
    }
    CHECK(lend(&a1, SB_LEND_SAVE, q, "other.txt", 1, NULL, grant) == EINVAL, "3: a name that isn't the path's own is refused");

    /* 4. paths not written the canonical way never reach a grant (F1/F2): the view refuses them too */
    snprintf(p, sizeof p, "%s/Docs/./chosen.txt", home);
    CHECK(lend(&a1, SB_LEND_FILE, p, "chosen.txt", 1, NULL, g2) == EINVAL, "4: a file through \"/./\" is refused");
    snprintf(p, sizeof p, "%s//Docs/chosen.txt", home);
    CHECK(lend(&a1, SB_LEND_FILE, p, "chosen.txt", 1, NULL, g2) == EINVAL, "4: a file through \"//\" is refused");
    snprintf(p, sizeof p, "%s/./Docs", home);
    CHECK(lend(&a1, SB_LEND_DIR, p, "Docs", 1, NULL, g2) == EINVAL, "4: a folder through \"/./\" is refused");
    snprintf(p, sizeof p, "%s/Docs/Sub/", home);
    CHECK(lend(&a1, SB_LEND_DIR, p, "Sub", 1, NULL, g2) == EINVAL, "4: a folder with a '/' at the end is refused");
    snprintf(p, sizeof p, "%s/Docs//new.txt", home);
    CHECK(lend(&a1, SB_LEND_SAVE, p, "new.txt", 1, NULL, g2) == EINVAL, "4: a save through \"//\" is refused");

    /* 4. a folder */
    snprintf(p, sizeof p, "%s/Docs/Sub", home);
    r = lend(&a1, SB_LEND_DIR, p, "Sub", 1, NULL, grant);
    snprintf(c2, sizeof c2, "r %s/app/org.example.App/doc/%s/Sub/in.txt", rt, grant); ask(&a1, c2, ans, sizeof ans);
    CHECK(r == 0 && !strcmp(ans, "in"), "4: a lent folder's files are there");
    snprintf(c2, sizeof c2, "w %s/app/org.example.App/doc/%s/Sub/made.txt m", rt, grant); ask(&a1, c2, ans, sizeof ans);
    snprintf(q, sizeof q, "%s/Docs/Sub/made.txt", home);
    get(q, ans + 100, 100);
    CHECK(!strcmp(ans, "ok") && !strcmp(ans + 100, "m"), "4: and a file made in it is in the real folder");
    stop(&a1); stop(&a2); stop(&b1);

    /* 2 again, inside bubblewrap, as `flatpak run` binds app/<id>: the grant is made after the sandbox started */
    struct stat bw;
    if (stat("/usr/bin/bwrap", &bw)) printf("sandbox-lend-test: bubblewrap part skipped: needs bwrap\n");
    else {
        View s1;
        CHECK(start_view("org.example.App", src_a, 1, 1, &s1) == 0, "bwrap: the view started");
        snprintf(q, sizeof q, "r %s/SingletonLock", rt); ask(&s1, q, ans, sizeof ans);
        if (!strcmp(ans, "first")) {
            snprintf(q, sizeof q, "r %s/Docs/sibling.txt", home); ask(&s1, q, ans, sizeof ans);
            CHECK(ans[0] == '!', "bwrap: the home isn't there");
            snprintf(p, sizeof p, "%s/Docs/chosen.txt", home);
            put(p, "chosen");
            r = lend(&s1, SB_LEND_FILE, p, "chosen.txt", 1, NULL, grant);
            snprintf(q, sizeof q, "r %s/doc/%s/chosen.txt", rt, grant); ask(&s1, q, ans, sizeof ans);
            CHECK(r == 0 && !strcmp(ans, "chosen"), "bwrap: a file lent after the sandbox started is readable inside it");
            snprintf(q, sizeof q, "r %s/doc/%s/sibling.txt", rt, grant); ask(&s1, q, ans, sizeof ans);
            CHECK(ans[0] == '!', "bwrap: the file next to it is not");
            snprintf(q, sizeof q, "w %s/doc/%s/chosen.txt fromsandbox", rt, grant); ask(&s1, q, ans, sizeof ans);
            get(p, ans + 100, 100);
            CHECK(!strcmp(ans, "ok") && !strcmp(ans + 100, "fromsandbox"), "bwrap: and writable through it");
        } else printf("sandbox-lend-test: bubblewrap part skipped: bwrap can't run here (%s)\n", ans);
        stop(&s1);
    }
    struct timespec d = { 1, 0 };
    nanosleep(&d, NULL);
    reap();
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd)) {}
    printf("sandbox-lend-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
