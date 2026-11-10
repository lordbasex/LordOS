/* kbinhibit_test.c — a Linux app that takes the desktop's shortcuts (src/wl/kbinhibit.c): whose decision it is
 * (who the compositor checks the client is: a Flatpak app's id, or the program it runs; never its app_id), when the
 * person is asked, what holds after allow, deny, revoke and ❖Esc, and which keys reach the app. */
#include "../src/wl/kbinhibit.h"
#include "../src/wl/keygrab.h"
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

/* A fake /proc/<pid>: root (a directory or a link to one), exe, and ns/{mnt,pid,user} (links to ours unless told) */
static int fake_proc(const char *base, const char *root_target, const char *exe_target, const char *other_ns) {
    char p[512];
    snprintf(p, sizeof p, "%s/ns", base);
    if (mkdir(base, 0700) || mkdir(p, 0700)) return -1;
    snprintf(p, sizeof p, "%s/root", base);
    if (root_target ? symlink(root_target, p) : mkdir(p, 0700)) return -1;
    snprintf(p, sizeof p, "%s/exe", base);
    if (exe_target && symlink(exe_target, p)) return -1;
    const char *ns[] = { "mnt", "pid", "user" };
    for (int i = 0; i < 3; i++) {
        char t[64];
        snprintf(p, sizeof p, "%s/ns/%s", base, ns[i]);
        snprintf(t, sizeof t, "/proc/self/ns/%s", ns[i]);
        if (symlink(other_ns && !strcmp(other_ns, ns[i]) ? "/proc/self/exe" : t, p)) return -1;
    }
    return open(base, O_PATH | O_DIRECTORY | O_CLOEXEC);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)ftw;
    return flag == FTW_DP ? rmdir(path) : unlink(path);
}

static int identity_of(int procfd, char *out) {
    int r = kbi_identity(procfd, out, KBI_ID_MAX);
    if (procfd >= 0) close(procfd);
    return r;
}

int main(void) {
    char k[KBI_ID_MAX], p[512];

    /* the names kept in permisos.json: what kbi_identity makes, never the first format's nor an .lpk id */
    CHECK(kbi_key_valid("flatpak:org.virt_manager.virt-viewer"));
    CHECK(kbi_key_valid("program:/usr/bin/remote-viewer"));
    CHECK(kbi_key_valid("program:/opt/App+1/bin/x@y~z"));
    CHECK(!kbi_key_valid("desktop:org.gnome.Boxes"));                /* the first format: ignored */
    CHECK(!kbi_key_valid("exe:/usr/bin/remmina"));
    CHECK(!kbi_key_valid("org.gnome.Boxes"));
    CHECK(!kbi_key_valid("machines"));                               /* an .lpk id never passes as a Linux app's */
    CHECK(!kbi_key_valid("flatpak:"));
    CHECK(!kbi_key_valid("flatpak:a\"b"));
    CHECK(!kbi_key_valid("flatpak:../x"));
    CHECK(!kbi_key_valid("flatpak:.x"));
    CHECK(!kbi_key_valid("flatpak:a/b"));
    CHECK(!kbi_key_valid("program:"));
    CHECK(!kbi_key_valid("program:/"));
    CHECK(!kbi_key_valid("program:rel"));
    CHECK(!kbi_key_valid("program:/a/../b"));
    CHECK(!kbi_key_valid("program:/a/./b"));
    CHECK(!kbi_key_valid("program:/a//b"));
    CHECK(!kbi_key_valid("program:/a/"));
    CHECK(!kbi_key_valid("program:/a/.."));
    CHECK(!kbi_key_valid("program:/a b"));
    CHECK(!kbi_key_valid("program:/a\nb"));
    CHECK(!kbi_key_valid(NULL));
    const char *rest = NULL;
    CHECK(kbi_key_kind("flatpak:org.a.B", &rest) == 'f' && !strcmp(rest, "org.a.B"));
    CHECK(kbi_key_kind("program:/usr/bin/x", &rest) == 'p' && !strcmp(rest, "/usr/bin/x"));
    CHECK(kbi_key_kind("exe:/usr/bin/x", &rest) == 0);
    char longp[KBI_ID_MAX + 8] = "program:/";
    memset(longp + 9, 'a', KBI_ID_MAX - 9);
    longp[KBI_ID_MAX] = 0;
    CHECK(!kbi_key_valid(longp));                                    /* it wouldn't fit permisos.json's names */
    longp[KBI_ID_MAX - 1] = 0;
    CHECK(kbi_key_valid(longp));

    /* .flatpak-info: the [Application] group's name, nothing else */
    static const char info[] = "[Application]\nname=org.example.Viewer\nruntime=runtime/org.x/1\n\n[Instance]\nname=other\n";
    CHECK(kbi_flatpak_name(info, sizeof info - 1, k, sizeof k) && !strcmp(k, "org.example.Viewer"));
    static const char crlf[] = "[Application]\r\nname=org.example.Viewer\r\n";
    CHECK(kbi_flatpak_name(crlf, sizeof crlf - 1, k, sizeof k) && !strcmp(k, "org.example.Viewer"));
    static const char other[] = "[Instance]\nname=org.example.Viewer\n";
    CHECK(!kbi_flatpak_name(other, sizeof other - 1, k, sizeof k) && !k[0]);
    static const char twice[] = "[Application]\nname=org.a.A\nname=org.b.B\n";
    CHECK(!kbi_flatpak_name(twice, sizeof twice - 1, k, sizeof k));
    static const char again[] = "[Application]\nname=org.a.A\n[Application]\nname=org.b.B\n";
    CHECK(!kbi_flatpak_name(again, sizeof again - 1, k, sizeof k));
    static const char bad[] = "[Application]\nname=org.a.A\"x\n";
    CHECK(!kbi_flatpak_name(bad, sizeof bad - 1, k, sizeof k));
    static const char dots[] = "[Application]\nname=../../x\n";
    CHECK(!kbi_flatpak_name(dots, sizeof dots - 1, k, sizeof k));
    static const char nul[] = "[Application]\nname=org.a\0.B\n";
    CHECK(!kbi_flatpak_name(nul, sizeof nul - 1, k, sizeof k));
    static const char empty[] = "[Application]\nname=\n";
    CHECK(!kbi_flatpak_name(empty, sizeof empty - 1, k, sizeof k));
    CHECK(!kbi_flatpak_name(info, sizeof info - 1, k, 5));          /* too small a buffer */

    /* who a process is: a fake /proc/<pid> for each case */
    char dir[] = "/tmp/kbinhibit-XXXXXX", self[PATH_MAX], real[PATH_MAX], tmp[100];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    CHECK(realpath("/proc/self/exe", self) != NULL);
    CHECK(realpath(dir, real) != NULL && strlen(real) < sizeof tmp);    /* /tmp may be a link: the names are compared */
    snprintf(tmp, sizeof tmp, "%.99s", real);
    char want[PATH_MAX + 16];
    snprintf(want, sizeof want, "program:%s", self);
    struct stat st;
    int host_flatpak = stat("/.flatpak-info", &st) == 0;            /* tests run inside a Flatpak: no program case */

    /* a Flatpak app: its .flatpak-info's name, whatever program it runs */
    snprintf(p, sizeof p, "%s/fp", tmp);
    int fd = fake_proc(p, NULL, self, NULL);
    snprintf(p, sizeof p, "%s/fp/root/.flatpak-info", tmp);
    write_file(p, info);
    CHECK(identity_of(fd, k) && !strcmp(k, "flatpak:org.example.Viewer"));
    /* …a damaged one names no one (and never falls back to its program) */
    snprintf(p, sizeof p, "%s/fpbad", tmp);
    fd = fake_proc(p, NULL, self, NULL);
    snprintf(p, sizeof p, "%s/fpbad/root/.flatpak-info", tmp);
    write_file(p, "[Application]\nname=a b\n");
    CHECK(!identity_of(fd, k) && !k[0]);
    /* …a link is not followed, even to a good file */
    snprintf(p, sizeof p, "%s/fplink", tmp);
    fd = fake_proc(p, NULL, self, NULL);
    char good[600];
    snprintf(good, sizeof good, "%s/fp/root/.flatpak-info", tmp);
    snprintf(p, sizeof p, "%s/fplink/root/.flatpak-info", tmp);
    CHECK(symlink(good, p) == 0);
    CHECK(!identity_of(fd, k));
    /* …nor is a directory read */
    snprintf(p, sizeof p, "%s/fpdir", tmp);
    fd = fake_proc(p, NULL, self, NULL);
    snprintf(p, sizeof p, "%s/fpdir/root/.flatpak-info", tmp);
    CHECK(mkdir(p, 0700) == 0);
    CHECK(!identity_of(fd, k));
    /* …nor an empty file */
    snprintf(p, sizeof p, "%s/fpempty", tmp);
    fd = fake_proc(p, NULL, self, NULL);
    snprintf(p, sizeof p, "%s/fpempty/root/.flatpak-info", tmp);
    write_file(p, "");
    CHECK(!identity_of(fd, k));

    if (!host_flatpak) {
        /* a process in our root and namespaces: the program it runs, resolved */
        snprintf(p, sizeof p, "%s/prog", tmp);
        CHECK(identity_of(fake_proc(p, "/", self, NULL), k) && !strcmp(k, want));
        /* …but not by a name that isn't the program's own path (a link on the way) */
        char via[PATH_MAX * 2 + 32], linkdir[PATH_MAX + 16];
        snprintf(linkdir, sizeof linkdir, "%s/via", tmp);
        char selfdir[PATH_MAX];
        snprintf(selfdir, sizeof selfdir, "%s", self);
        *strrchr(selfdir, '/') = 0;
        CHECK(symlink(selfdir[0] ? selfdir : "/", linkdir) == 0);
        snprintf(via, sizeof via, "%s/%s", linkdir, strrchr(self, '/') + 1);
        snprintf(p, sizeof p, "%s/progvia", tmp);
        CHECK(!identity_of(fake_proc(p, "/", via, NULL), k));
        /* …nor a program that is gone */
        snprintf(via, sizeof via, "%s/gone", tmp);
        snprintf(p, sizeof p, "%s/proggone", tmp);
        CHECK(!identity_of(fake_proc(p, "/", via, NULL), k));
        /* …nor a path with other characters */
        snprintf(via, sizeof via, "%s/a b", tmp);
        write_file(via, "x");
        snprintf(p, sizeof p, "%s/progspace", tmp);
        CHECK(!identity_of(fake_proc(p, "/", via, NULL), k));
        /* …nor no program at all */
        snprintf(p, sizeof p, "%s/progno", tmp);
        CHECK(!identity_of(fake_proc(p, "/", NULL, NULL), k));
        /* another root, or another mount, pid or user namespace: a sandbox or container, no one */
        snprintf(p, sizeof p, "%s/otherroot", tmp);
        CHECK(!identity_of(fake_proc(p, NULL, self, NULL), k));
        const char *ns[] = { "mnt", "pid", "user" };
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "%s/ns-%s", tmp, ns[i]);
            CHECK(!identity_of(fake_proc(p, "/", self, ns[i]), k));
        }
        /* the real thing: this process */
        CHECK(identity_of(open("/proc/self", O_PATH | O_DIRECTORY | O_CLOEXEC), k) && !strcmp(k, want));
    }
    CHECK(!kbi_identity(-1, k, sizeof k) && !k[0]);
    fd = open("/proc/self", O_PATH | O_DIRECTORY | O_CLOEXEC);
    CHECK(!kbi_identity(fd, k, 10));                                 /* too small a buffer */
    if (fd >= 0) close(fd);

    /* the peer of a socket: the process that connected, while it is still that process */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    int pf = kbi_peer_proc(sv[0], (int)getpid());
    CHECK(pf >= 0);
    if (!host_flatpak) CHECK(identity_of(pf, k) && !strcmp(k, want));
    else if (pf >= 0) close(pf);
    int pidfd = -1;
    socklen_t len = sizeof pidfd;
    int has_pidfd = getsockopt(sv[0], SOL_SOCKET, 77 /* SO_PEERPIDFD */, &pidfd, &len) == 0 && pidfd >= 0;
    if (pidfd >= 0) close(pidfd);
    if (has_pidfd) CHECK(kbi_peer_proc(sv[0], (int)getpid() + 1) < 0);   /* the socket says another: no one */
    close(sv[0]);
    close(sv[1]);
    CHECK(kbi_peer_proc(-1, 0) < 0);
    /* a client that connected and left: its /proc is gone, whatever pid it had */
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%.90s/sock", tmp);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 1) == 0);
    pid_t kid = fork();
    if (kid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        _exit(c >= 0 && connect(c, (struct sockaddr *)&sa, sizeof sa) == 0 ? 0 : 1);
    }
    int ac = accept(ls, NULL, NULL);
    int wst = 0;
    CHECK(kid > 0 && waitpid(kid, &wst, 0) == kid && WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    CHECK(ac >= 0 && kbi_peer_proc(ac, (int)kid) < 0);
    if (ac >= 0) close(ac);
    if (ls >= 0) close(ls);
    nftw(tmp, rm_one, 16, FTW_DEPTH | FTW_PHYS);

    /* asked once; allowed holds right away; denied never asks again; never for isolated apps or the login screen */
    CHECK(kbi_decide(0, 0, 0, 1) == KBI_PROMPT);
    CHECK(kbi_decide(1, 0, 0, 1) == KBI_GRANT);
    CHECK(kbi_decide(2, 0, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(1, 1, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(0, 1, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(1, 0, 1, 1) == KBI_NEVER);
    CHECK(kbi_decide(0, 0, 0, 0) == KBI_NEVER);

    /* allowed: active once; ❖Esc off and back on; revoked: inactive, and ❖Esc does nothing */
    KbiState s = { 0 };
    CHECK(kbi_toggle(&s) == KBI_SAME && !s.active);                  /* not allowed: ❖Esc is LordOS's */
    CHECK(kbi_set_allowed(&s, 1) == KBI_ACTIVE && s.active);
    CHECK(kbi_set_allowed(&s, 1) == KBI_SAME);
    CHECK(kbi_toggle(&s) == KBI_INACTIVE && !s.active && s.off);
    CHECK(kbi_set_allowed(&s, 1) == KBI_SAME && !s.active);          /* still off: allowing again doesn't undo ❖Esc */
    CHECK(kbi_toggle(&s) == KBI_ACTIVE && s.active);
    CHECK(kbi_set_allowed(&s, 0) == KBI_INACTIVE && !s.active);
    CHECK(kbi_toggle(&s) == KBI_SAME && !s.active);
    CHECK(kbi_toggle(&s) == KBI_SAME);
    CHECK(kbi_toggle(&s) == KBI_SAME);
    s.off = 1;                                                       /* revoked while off, allowed again: it holds */
    CHECK(kbi_set_allowed(&s, 0) == KBI_SAME && !s.off);
    CHECK(kbi_set_allowed(&s, 1) == KBI_ACTIVE);

    /* active: the desktop's shortcuts reach the app */
    CHECK(kbi_route(KEY_TAB, 0, KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_TAB, 0, KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_TAB, 0, KG_ALT | KG_SHIFT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_LEFTMETA, 0, KG_META, 1) == KBI_KEY_APP);    /* ❖ alone: no launcher */
    CHECK(kbi_route(KEY_SPACE, 0, KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_Q, 'q', KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_M, 'm', KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_F4, 0, KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_DELETE, 0, KG_CTRL | KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_ESC, 0, KG_META | KG_SHIFT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_ESC, 0, 0, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_A, 'a', 0, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_F2, 0, KG_META | KG_CTRL | KG_ALT, 1) == KBI_KEY_APP);
    /* …but ❖Esc, the lock, the console switch and the media row stay LordOS's */
    CHECK(kbi_route(KEY_ESC, 0, KG_META, 1) == KBI_KEY_TOGGLE);
    CHECK(kbi_route(KEY_L, 'l', KG_META, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_Q, 'q', KG_META | KG_CTRL, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_F1, 0, KG_CTRL | KG_ALT, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_F12, 0, KG_CTRL | KG_ALT | KG_SHIFT, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_VOLUMEUP, 0, 0, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_SYSRQ, 0, 0, 1) == KBI_KEY_LORD);
    /* off after ❖Esc: every key goes as it would without an inhibitor, and ❖Esc turns it back on */
    CHECK(kbi_route(KEY_TAB, 0, KG_META, 0) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_A, 'a', 0, 0) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_ESC, 0, KG_META, 0) == KBI_KEY_TOGGLE);

    if (fails) { printf("kbinhibit-test: %d failures\n", fails); return 1; }
    printf("kbinhibit-test: ok\n");
    return 0;
}
