/* cgslice_test.c — the machines' slice (docs/VIRTUALIZATION.md 3.7) in the real cgroupfs, through lord-sessiond's own
 * placement code (cgroup_place_serve): a machine's scope lands in lordos-session-<uid>/lordos-machines.slice with the
 * slice's fixed weights and its own memory.max and cpu.max; any other scope stays beside the «server» leaf; the slice is
 * root-owned, so the session's user can't make a folder in it or move a process into it.
 *
 * All of it on a throwaway tree, /sys/fs/cgroup/lordos-slicetest-<pid> standing in for lordos.service (this process
 * moves itself into its "supervisor" leaf, as lord-sessiond does, and back out at the end); never lordos.service, the
 * running lord-sessiond or a session. The «client» is a child of this test that moves into the stand-in session's
 * server leaf, drops to the user and asks over a Unix socket in /var/tmp/lordos-slice/, as lord-server's lord-vm does.
 *
 *   sudo build/cgslice-test [uid]      (uid: the user to drop to, default SUDO_UID or 1000; without root it skips) */
#include "../src/sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char root[96], home_cg[PATH_MAX], sockdir[] = "/var/tmp/lordos-slice", sockpath[100];
static const char *VMID = "app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope";

static int put(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
}
static const char *get(const char *path) {
    static char b[256];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    b[n > 0 ? n : 0] = 0;
    b[strcspn(b, "\n")] = 0;
    return b;
}
static int move_self(const char *dir) {
    char p[PATH_MAX + 16], v[32];
    snprintf(p, sizeof p, "%s/cgroup.procs", dir);
    snprintf(v, sizeof v, "%d", (int)getpid());
    return put(p, v);
}
static void rm_tree(const char *dir) {        /* depth-first; cgroupfs folders only */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "find '%s' -depth -type d -exec rmdir {} + 2>/dev/null", dir);
    if (system(cmd)) { /* checked below */ }
}

/* the client: into the session's server leaf, then the user, then one request; its answer's first byte ('o' ok) */
static int ask(unsigned uid, const char *name, char *cg_out, size_t cn) {
    int pfd[2];
    if (pipe(pfd)) return -1;
    pid_t c = fork();
    if (c == 0) {
        char leaf[256];
        snprintf(leaf, sizeof leaf, "%s/lordos-session-%u/server", root, uid);
        close(pfd[0]);
        if (move_self(leaf) || setgid(uid) || setuid(uid)) _exit(100);
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) _exit(101);
        char req[160], rep[64] = "";
        int n = snprintf(req, sizeof req, "enter %s 0 4224 450", name);
        if (send(fd, req, (size_t)n, 0) != n) _exit(102);
        ssize_t r = recv(fd, rep, sizeof rep - 1, 0);
        rep[r > 0 ? r : 0] = 0;
        /* where it is now, and whether it can make a folder in the slice or leave its scope (it must not) */
        char cg[PATH_MAX] = "", line[PATH_MAX];
        FILE *f = fopen("/proc/self/cgroup", "re");
        if (f && fgets(line, sizeof line, f) && !strncmp(line, "0::", 3)) snprintf(cg, sizeof cg, "%s", line + 3);
        if (f) fclose(f);
        cg[strcspn(cg, "\n")] = 0;
        char sl[256], other[300];
        snprintf(sl, sizeof sl, "%s/lordos-session-%u/" CG_MACHINES_SLICE, root, uid);
        snprintf(other, sizeof other, "%s/intruder", sl);
        int made = !mkdir(other, 0755);
        int moved = !move_self(sl) || !move_self(leaf);
        char msg[PATH_MAX + 8];
        int m = snprintf(msg, sizeof msg, "%c%c%c%s", rep[0] ? rep[0] : '?', made ? 'M' : '-', moved ? 'V' : '-', cg);
        if (write(pfd[1], msg, (size_t)m) != m) _exit(103);
        _exit(0);
    }
    close(pfd[1]);
    char buf[PATH_MAX];
    ssize_t n = read(pfd[0], buf, sizeof buf - 1);
    close(pfd[0]);
    int st = 0;
    waitpid(c, &st, 0);
    if (n < 3) { fprintf(stderr, "client exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return -1; }
    buf[n] = 0;
    if (cn) { size_t k = strnlen(buf + 3, cn - 1); memcpy(cg_out, buf + 3, k); cg_out[k] = 0; }
    CHECK(buf[1] == '-', "the user made a folder in the slice");
    CHECK(buf[2] == '-', "the user moved itself out of its scope");
    return buf[0];
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() != 0) { printf("cgslice-test: skipped: needs root (sudo build/cgslice-test)\n"); return 0; }
    unsigned uid = argc > 1 ? (unsigned)atoi(argv[1]) : getenv("SUDO_UID") ? (unsigned)atoi(getenv("SUDO_UID")) : 1000;
    if (uid < 1000 || uid >= 60000) { fprintf(stderr, "cgslice-test: uid %u isn't a person's\n", uid); return 2; }
    {   /* where this process came from (read here: cgroup_self() caches its first answer, and lord-sessiond's code below must see supervisor) */
        char l[PATH_MAX - 32] = "";
        FILE *f = fopen("/proc/self/cgroup", "re");
        if (f && fgets(l, sizeof l, f) && !strncmp(l, "0::", 3)) { l[strcspn(l, "\n")] = 0; snprintf(home_cg, sizeof home_cg, "/sys/fs/cgroup%s", l + 3); }
        if (f) fclose(f);
    }
    snprintf(root, sizeof root, "/sys/fs/cgroup/lordos-slicetest-%d", (int)getpid());
    char sup[128], sess[160], server[200], p[600];
    snprintf(sup, sizeof sup, "%s/supervisor", root);
    snprintf(sess, sizeof sess, "%s/lordos-session-%u", root, uid);
    snprintf(server, sizeof server, "%s/server", sess);
    mkdir(sockdir, 0755);
    snprintf(sockpath, sizeof sockpath, "%s/cg-%d.sock", sockdir, (int)getpid());
    if (mkdir(root, 0755) || mkdir(sup, 0755) || move_self(sup)) { perror("the stand-in service"); rmdir(sup); rmdir(root); return 1; }
    /* as lord-sessiond: controllers on the service root, the session's subtree, root-owned (cgroup_delegate_session's steps) */
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", root);
    put(p, "+memory +cpu +pids");
    put(p, "+io");
    mkdir(sess, 0755);
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", sess);
    put(p, "+memory +cpu +pids");
    mkdir(server, 0755);
    int lis = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
    unlink(sockpath);
    if (lis < 0 || bind(lis, (struct sockaddr *)&a, sizeof a) || chmod(sockpath, 0666) || listen(lis, 4)) { perror("socket"); goto out; }
    /* the client's executable is this test: its folder stands in for /usr/lib/lordos/bin (insecure: not root-owned) */
    char bindir[PATH_MAX];
    ssize_t bl = readlink("/proc/self/exe", bindir, sizeof bindir - 1);
    bindir[bl > 0 ? bl : 0] = 0;
    char *slash = strrchr(bindir, '/');
    if (slash) *slash = 0;

    /* which lord-netperm runs (cgroup_netperm_current): a stand-in procs file in the scratch folder */
    {
        char pf[160], copy[160];
        snprintf(pf, sizeof pf, "%s/procs-%d", sockdir, (int)getpid());
        snprintf(copy, sizeof copy, "%s/lord-netperm", sockdir);
        CHECK(cgroup_netperm_current("/nonexistent/cgroup.procs") == 1, "not running: the installed one starts next");
        int fd = open(pf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        close(fd);
        CHECK(cgroup_netperm_current(pf) == 1, "empty: restarting");
        put(pf, "garbage\n");
        CHECK(cgroup_netperm_current(pf) == 0, "garbage: not sure, the old place");
        put(pf, "1\n");
        CHECK(cgroup_netperm_current(pf) == 0, "pid 1");
        char me[32];
        snprintf(me, sizeof me, "%d\n", (int)getpid());
        put(pf, me);
        CHECK(cgroup_netperm_current(pf) == 0, "another program");
        /* a program named lord-netperm: running from its file it is current; with the file replaced, older */
        char cmd[400];
        snprintf(cmd, sizeof cmd, "cp /bin/sleep '%s'", copy);
        if (system(cmd)) { /* checked below */ }
        pid_t sl = fork();
        if (sl == 0) { execl(copy, "lord-netperm", "30", (char *)NULL); _exit(127); }
        usleep(200000);
        snprintf(me, sizeof me, "%d\n", (int)sl);
        put(pf, me);
        CHECK(cgroup_netperm_current(pf) == 1, "the installed lord-netperm runs");
        unlink(copy);
        CHECK(cgroup_netperm_current(pf) == 0, "an older lord-netperm runs (its file replaced)");
        kill(sl, SIGKILL);
        waitpid(sl, NULL, 0);
        unlink(pf);
    }
    int current = cgroup_netperm_current(CG_NETPERM_PROCS);
    printf("cgslice-test: this computer's lord-netperm %s\n", current ? "knows the slice (or isn't running)" : "is older than its installed file: machines go beside the server leaf");
    for (int round = 0; round < 2; round++) {
        const char *name = round == 0 ? VMID : "app-lordos-featherpad.scope";
        pid_t srv = fork();
        if (srv == 0) {
            int cfd = accept(lis, NULL, NULL);
            _exit(cfd >= 0 && !cgroup_place_serve(cfd, bindir, 1) ? 0 : 1);
        }
        char cg[PATH_MAX] = "";
        int rep = ask(uid, name, cg, sizeof cg);
        int st = 0;
        waitpid(srv, &st, 0);
        CHECK(rep == 'o' && WIFEXITED(st) && WEXITSTATUS(st) == 0, "%s: lord-sessiond's answer %c", name, rep > 0 ? rep : '?');
        char want[400];
        if (round == 0 && current) snprintf(want, sizeof want, "%s/" CG_MACHINES_SLICE "/%s", sess + strlen("/sys/fs/cgroup"), name);
        else snprintf(want, sizeof want, "%s/%s", sess + strlen("/sys/fs/cgroup"), name);
        printf("cgslice-test: %s → %s\n", name, cg);
        CHECK(!strcmp(cg, want), "placed in %s, wanted %s", cg, want);
    }
    if (!current) { printf("cgslice-test: the slice's own checks need this computer's lord-netperm current: skipped\n"); goto out; }
    /* the slice: root-owned, its weights; the machine's scope: its own limits inside it */
    char sl[256], sc[400];
    snprintf(sl, sizeof sl, "%s/" CG_MACHINES_SLICE, sess);
    snprintf(sc, sizeof sc, "%s/%s", sl, VMID);
    struct stat st;
    CHECK(!stat(sl, &st) && st.st_uid == 0 && !(st.st_mode & 022), "the slice isn't root's alone");
    snprintf(p, sizeof p, "%s/cgroup.procs", sl);
    CHECK(!stat(p, &st) && st.st_uid == 0, "the slice's cgroup.procs isn't root's");
    snprintf(p, sizeof p, "%s/cpu.weight", sl);
    printf("cgslice-test: slice cpu.weight %s", get(p));
    CHECK(!strcmp(get(p), CG_MACHINES_CPU_WEIGHT), "cpu.weight %s", get(p));
    snprintf(p, sizeof p, "%s/cgroup.controllers", sess);
    int has_io = strstr(get(p), "io") != NULL;
    snprintf(p, sizeof p, "%s/io.weight", sl);
    printf(", io.weight %s\n", has_io ? get(p) : "(no io controller here)");
    if (has_io) CHECK(!strcmp(get(p), CG_MACHINES_IO_WEIGHT), "io.weight %s", get(p));
    snprintf(p, sizeof p, "%s/memory.max", sc);
    CHECK(!strcmp(get(p), "4429185024"), "the machine's memory.max %s", get(p));
    printf("cgslice-test: the machine's memory.max %s", get(p));
    snprintf(p, sizeof p, "%s/cpu.max", sc);
    printf(", cpu.max %s\n", get(p));
    CHECK(!strcmp(get(p), "450000 100000"), "the machine's cpu.max %s", get(p));
    snprintf(p, sizeof p, "%s/memory.max", sl);
    CHECK(!strcmp(get(p), "max"), "the slice itself has no memory limit: %s", get(p));
    /* #140's release and restore on the real files: the slice's controllers off before its session's, the machine's
     * limits noted and put back */
    int svc = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char notes_path[200];
    snprintf(notes_path, sizeof notes_path, "%s/notes-%d", sockdir, (int)getpid());
    int nf = open(notes_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    /* the test itself sits in supervisor: the stand-in root keeps its controllers (as on a live lordos.service whose
     * ExecStartPre isn't in the root), so only the sessions' part is checked */
    cgroup_release_at(svc, nf);
    char notes[4096] = "";
    ssize_t nn = pread(nf, notes, sizeof notes - 1, 0);
    notes[nn > 0 ? nn : 0] = 0;
    close(nf);
    unlink(notes_path);
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", sl);
    CHECK(get(p)[0] == 0, "the slice keeps controllers: %s", get(p));
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", sess);
    CHECK(get(p)[0] == 0, "the session keeps controllers: %s", get(p));
    CHECK(strstr(notes, "lordos-machines.slice/" "app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope memory.max 4429185024"), "not noted:\n%s", notes);
    /* back as cgroup_service_init does it: the root's controllers, the session's, then the slice and the noted limits */
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", root);
    put(p, "+memory +cpu +pids");
    put(p, "+io");
    snprintf(p, sizeof p, "%s/cgroup.subtree_control", sess);
    put(p, "+memory +cpu +pids");
    int sfd = open(sess, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int vfd = cgroup_machines_slice(sfd);
    if (vfd >= 0) close(vfd);
    close(sfd);
    int failed = -1, back = cgroup_restore_at(svc, notes, &failed);
    close(svc);
    snprintf(p, sizeof p, "%s/memory.max", sc);
    CHECK(back >= 2 && failed == 0 && !strcmp(get(p), "4429185024"), "put back %d (%d failed): memory.max %s", back, failed, get(p));
    snprintf(p, sizeof p, "%s/cpu.weight", sl);
    CHECK(!strcmp(get(p), CG_MACHINES_CPU_WEIGHT), "the slice's weight after the restart: %s", get(p));
out:
    if (lis >= 0) close(lis);
    unlink(sockpath);
    rmdir(sockdir);
    move_self(home_cg);
    rm_tree(root);
    CHECK(access(root, F_OK) && errno == ENOENT, "the test's tree is left behind");
    printf("cgslice-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
