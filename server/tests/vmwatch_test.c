/* vmwatch_test.c — the pure checks behind Sentinela's machine facts (src/vm/vmwatch.h): the machine's scope is an exact
 * path beside lord-server's leaf, a cgroup is "in" it only when it is that path or below it (never a path that merely
 * contains the name), and a process's program must be the installed lord-vm. */
#include "../src/vm/vmwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the test lord-vm's request, waited for: vm_recv never blocks (MSG_DONTWAIT), and the asker may not have sent it yet */
static int recv_wait(int c, VmMsg *q, int *qfd) {
    struct pollfd p = { c, POLLIN, 0 };
    if (poll(&p, 1, 5000) <= 0) return -1;
    return vm_recv(c, q, qfd);
}

int main(void) {
    const char *self = "/lordos.slice/lordos-session-1000/server";
    const char *id = "6f1c0d2e-1111-4222-8333-444455556666";
    char sc[256];

    /* where the scope must be: in the machines' slice (3.7), or beside the server leaf for one started before it */
    CHECK(!vmwatch_scope_path(self, id, 0, 0, sc, sizeof sc) && !strcmp(sc, "/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope"), "old place: %s", sc);
    CHECK(!vmwatch_scope_path(self, id, 1, 0, sc, sizeof sc) && !strcmp(sc, "/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-tor-6f1c0d2e-1111-4222-8333-444455556666.scope"), "a Tor gateway's old place: %s", sc);
    CHECK(!vmwatch_scope_path(self, id, 1, 1, sc, sizeof sc) && !strcmp(sc, "/lordos.slice/lordos-session-1000/lordos-machines.slice/app-lordos-lordos-vm-tor-6f1c0d2e-1111-4222-8333-444455556666.scope"), "a Tor gateway in the slice: %s", sc);
    CHECK(vmwatch_scope_path("/user.slice/session-3.scope", id, 0, 1, sc, sizeof sc) && !sc[0], "not a LordOS session: unknown");
    CHECK(vmwatch_scope_path("/x/notasession/server", id, 0, 1, sc, sizeof sc) && !sc[0], "a server leaf outside a session");
    CHECK(vmwatch_scope_path(self, "../../evil", 0, 1, sc, sizeof sc) && !sc[0], "an id with a path in it");
    CHECK(vmwatch_scope_path(self, "", 0, 1, sc, sizeof sc) && !sc[0], "no id");
    char tiny[16];
    CHECK(vmwatch_scope_path(self, id, 0, 1, tiny, sizeof tiny) && !tiny[0], "too small: nothing, not a cut path");
    char mid[110];   /* fits the old place, not the slice's */
    CHECK(vmwatch_scope_path(self, id, 0, 1, mid, sizeof mid) && !mid[0], "the slice's path doesn't fit: nothing");
    vmwatch_scope_path(self, id, 0, 1, sc, sizeof sc);
    CHECK(!strcmp(sc, "/lordos.slice/lordos-session-1000/lordos-machines.slice/app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope"), "in the slice: %s", sc);
    {
        char t0[512];
        snprintf(t0, sizeof t0, "0::/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-%s.scope\n", id);
        CHECK(!vmwatch_cg_under(t0, sc), "the old place isn't the slice's scope");
        snprintf(t0, sizeof t0, "0::/lordos.slice/lordos-session-1000/lordos-machines.slice\n");
        CHECK(!vmwatch_cg_under(t0, sc), "the slice itself isn't the machine's scope");
    }

    /* in it: exactly, or a leaf below */
    char t[512];
    snprintf(t, sizeof t, "0::%s\n", sc);
    CHECK(vmwatch_cg_under(t, sc), "the scope itself");
    snprintf(t, sizeof t, "0::%s/qemu\n", sc);
    CHECK(vmwatch_cg_under(t, sc), "its qemu leaf");
    snprintf(t, sizeof t, "0::%s/supervisor", sc);
    CHECK(vmwatch_cg_under(t, sc), "its supervisor leaf, no newline");
    /* the substring trap: names that contain it but aren't it */
    snprintf(t, sizeof t, "0::/user.slice/user-1000.slice/fake%s\n", sc);
    CHECK(!vmwatch_cg_under(t, sc), "another root that contains the path");
    snprintf(t, sizeof t, "0::/lordos.slice/lordos-session-1000/lordos-machines.slice/app-lordos-lordos-vm-%s.scope2/qemu\n", id);
    CHECK(!vmwatch_cg_under(t, sc), "a longer name with the same start");
    snprintf(t, sizeof t, "0::/elsewhere/app-lordos-lordos-vm-%s.scope/qemu\n", id);
    CHECK(!vmwatch_cg_under(t, sc), "the scope's name under another parent");
    snprintf(t, sizeof t, "0::/lordos.slice/lordos-session-1000/server\n");
    CHECK(!vmwatch_cg_under(t, sc), "lord-server's own leaf");
    snprintf(t, sizeof t, "0::%s/../server\n", sc);
    CHECK(!vmwatch_cg_under(t, sc), "a .. component");
    snprintf(t, sizeof t, "1:name=x:%s\n", sc);
    CHECK(!vmwatch_cg_under(t, sc), "not a cgroup v2 line");
    CHECK(!vmwatch_cg_under("0::/\n", sc), "the root");
    CHECK(!vmwatch_cg_under(t, ""), "no scope known: never in");

    /* the installed program */
    CHECK(vmwatch_exe_same("/usr/lib/lordos/bin/lord-vm", "/usr/lib/lordos/bin/lord-vm"), "the same");
    CHECK(vmwatch_exe_same("/usr/lib/lordos/bin/lord-vm (deleted)", "/usr/lib/lordos/bin/lord-vm"), "upgraded under it");
    CHECK(!vmwatch_exe_same("/home/ana/bin/lord-vm", "/usr/lib/lordos/bin/lord-vm"), "a copy elsewhere");
    CHECK(!vmwatch_exe_same("/usr/lib/lordos/bin/lord-vm2", "/usr/lib/lordos/bin/lord-vm"), "a longer name");
    CHECK(!vmwatch_exe_same("/usr/lib/lordos/bin/lord-vm (deleted)x", "/usr/lib/lordos/bin/lord-vm"), "a lookalike suffix");
    CHECK(!vmwatch_exe_same("/usr/lib/lordos/bin/lord-vmXXXXXXXXXX", "/usr/lib/lordos/bin/lord-vm"), "same length as the suffix");
    CHECK(!vmwatch_exe_same("", "/usr/lib/lordos/bin/lord-vm"), "none");

    /* the live answer: a stand-in lord-vm on a socket (mode: 0 both answers, 1 silent, 2 only its state) */
    for (int mode = 0; mode < 3; mode++) {
        char d[] = "/tmp/vmwatch-XXXXXX", sp[64];
        CHECK(mkdtemp(d) != NULL, "temp folder");
        snprintf(sp, sizeof sp, "%s/m.sock", d);
        int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        memcpy(a.sun_path, sp, strlen(sp) + 1);
        CHECK(ls >= 0 && !bind(ls, (struct sockaddr *)&a, sizeof a) && !listen(ls, 1), "listen");
        pid_t child = fork();
        if (!child) {
            int c = accept(ls, NULL, NULL);
            VmMsg q;
            int qfd = -1;
            if (c < 0 || recv_wait(c, &q, &qfd) <= 0 || q.type != VM_STATUS) _exit(1);
            if (mode == 0) { VmMsg i = vm_msg(VM_INFO, q.seq, 2, 2048, 0x1ff, 2, "id"); vm_send(c, &i, -1); }
            if (mode != 1) { VmMsg s = vm_msg(VM_STATE, q.seq, 2, 0, 0, 4321, ""); vm_send(c, &s, -1); }
            struct timespec ts = { 0, 400 * 1000000 };
            nanosleep(&ts, NULL);
            _exit(0);
        }
        close(ls);
        VmLive lv;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = vmwatch_live(sp, &lv, mode == 0 ? 2000 : 150);   /* both answers come at once: a loaded machine (ASan, make -j) may take long */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        /* the peer is the process that listens (lord-vm itself), as the kernel says */
        if (mode == 0) CHECK(!r && lv.pid == getpid() && lv.uid == getuid() && lv.state == 2 && lv.qemu == 4321 && lv.sandbox == 0x1ff && lv.cg == 2,
                             "both answers: r %d pid %ld/%d qemu %ld sandbox %d cg %d", r, lv.pid, (int)getpid(), lv.qemu, lv.sandbox, lv.cg);
        else CHECK(r == -1 && ms < 400, "mode %d: no full answer, refused in %ld ms", mode, ms);
        int st;
        waitpid(child, &st, 0);
        unlink(sp);
        rmdir(d);
    }
    VmLive none;
    CHECK(vmwatch_live("/nonexistent/lordos-vm/x.sock", &none, 150) == -1 && none.pid == 0, "no socket");

    /* lord-vm's own cgroup: the scope itself or its supervisor leaf; never QEMU's or passt's leaf, a lookalike or a path
     * that climbs out */
    {
        const char *scope = "/s/lordos-session-1000/lordos-machines.slice/app-lordos-lordos-vm-ab.scope";
        char t[512];
        snprintf(t, sizeof t, "0::%s\n", scope);
        CHECK(vmwatch_cg_lordvm(t, scope), "the scope itself");
        snprintf(t, sizeof t, "0::%s/supervisor\n", scope);
        CHECK(vmwatch_cg_lordvm(t, scope), "its supervisor leaf");
        snprintf(t, sizeof t, "0::%s/qemu\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "QEMU's leaf is never lord-vm's");
        snprintf(t, sizeof t, "0::%s/passt\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "passt's leaf");
        snprintf(t, sizeof t, "0::%s/supervisor/x\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "below the supervisor leaf");
        snprintf(t, sizeof t, "0::%s/supervisorx\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "a longer leaf name");
        snprintf(t, sizeof t, "0::%sx\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "a longer scope name");
        snprintf(t, sizeof t, "0::%s/supervisor/../../other\n", scope);
        CHECK(!vmwatch_cg_lordvm(t, scope), "a path that climbs out");
        CHECK(!vmwatch_cg_lordvm("0::/elsewhere\n", scope) && !vmwatch_cg_lordvm(NULL, scope) && !vmwatch_cg_lordvm(t, ""), "elsewhere, nothing");
    }

    /* the lord-vm processes lord-server vouches for: a pidfd per start, matched by machine id and pid, alive and the same
     * process (start time); a dead one, another pid or another machine's never match */
    {
        VmProcRec tab[4];
        memset(tab, 0, sizeof tab);
        const char *ida = "6f1c0d2e-1111-4222-8333-444455556666", *idb = "aaaa0000-1111-4222-8333-444455556666";
        int pa[2];
        CHECK(!pipe(pa), "pipe");
        pid_t ch = fork();
        if (!ch) { char x; close(pa[1]); (void)!read(pa[0], &x, 1); _exit(0); }
        close(pa[0]);
        int pfd = (int)syscall(SYS_pidfd_open, ch, 0);
        CHECK(pfd >= 0 && vmwatch_pidfd_pid(pfd) == ch, "the pidfd names the child: %ld/%d", vmwatch_pidfd_pid(pfd), (int)ch);
        CHECK(vmwatch_pidfd_alive(pfd), "alive");
        CHECK(vmwatch_start_time(ch) > 0, "a start time");
        CHECK(!vmwatch_procs_add(tab, 4, ida, pfd, 0), "kept");
        CHECK(vmwatch_procs_match(tab, 4, ida, ch) != NULL, "its machine and pid match");
        CHECK(!vmwatch_procs_match(tab, 4, idb, ch), "another machine's id never matches");
        CHECK(!vmwatch_procs_match(tab, 4, ida, getpid()), "another pid never matches");
        CHECK(!vmwatch_procs_match(tab, 4, ida, 1) && !vmwatch_procs_match(tab, 4, "", ch), "init, no id");
        int dup1 = (int)syscall(SYS_pidfd_open, ch, 0);
        CHECK(!vmwatch_procs_add(tab, 4, ida, dup1, 0), "the same one again: kept once");
        int used = 0;
        for (int i = 0; i < 4; i++) used += tab[i].pid != 0;
        CHECK(used == 1, "one record, %d", used);
        /* a forged record (the start time of another process) is never a match */
        int pself = (int)syscall(SYS_pidfd_open, getpid(), 0);
        CHECK(!vmwatch_procs_add(tab, 4, idb, pself, 1), "self adopted");
        for (int i = 0; i < 4; i++) if (tab[i].pid == getpid()) tab[i].start++;
        CHECK(!vmwatch_procs_match(tab, 4, idb, getpid()), "a start time that changed: not the same process");
        /* the child dies: its record never matches again, and pruning frees it */
        close(pa[1]);
        int st;
        waitpid(ch, &st, 0);
        CHECK(!vmwatch_procs_match(tab, 4, ida, ch), "a dead lord-vm never matches");
        vmwatch_procs_prune(tab, 4);
        int left = 0;
        for (int i = 0; i < 4; i++) left += tab[i].pid == ch;
        CHECK(!left, "pruned");
        /* refused: not a pidfd, a bad id, a dead process's pidfd, no room */
        int notpid = open("/dev/null", O_RDONLY);
        CHECK(vmwatch_procs_add(tab, 4, ida, notpid, 0) == -1 && fcntl(notpid, F_GETFD) == -1, "a file that isn't a pidfd: refused and closed");
        int p2 = (int)syscall(SYS_pidfd_open, getpid(), 0);
        CHECK(vmwatch_procs_add(tab, 4, "../x", p2, 0) == -1, "an id with a path");
        CHECK(vmwatch_procs_add(tab, 4, "ABCD", (int)syscall(SYS_pidfd_open, getpid(), 0), 0) == -1, "an id in capitals");
        pid_t ch2 = fork();
        if (!ch2) _exit(0);
        int dead = (int)syscall(SYS_pidfd_open, ch2, 0);
        waitpid(ch2, &st, 0);
        CHECK(vmwatch_procs_add(tab, 4, ida, dead, 0) == -1, "a dead process's pidfd");
        VmProcRec one[1];
        memset(one, 0, sizeof one);
        CHECK(!vmwatch_procs_add(one, 1, ida, (int)syscall(SYS_pidfd_open, getpid(), 0), 0), "fills");
        CHECK(vmwatch_procs_add(one, 1, idb, (int)syscall(SYS_pidfd_open, getpid(), 0), 0) == -1, "no room");
        for (int i = 0; i < 4; i++) if (tab[i].pid) close(tab[i].pidfd);
        close(one[0].pidfd);
    }

    /* the socket's peer as a pidfd from the kernel (SO_PEERPIDFD): it names the process that listens */
    {
        char d[] = "/tmp/vmwatch-pfd.XXXXXX", sp[128];
        CHECK(mkdtemp(d) != NULL, "temp folder");
        snprintf(sp, sizeof sp, "%s/m.sock", d);
        int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        memcpy(a.sun_path, sp, strlen(sp) + 1);
        CHECK(ls >= 0 && !bind(ls, (struct sockaddr *)&a, sizeof a) && !listen(ls, 1), "listen");
        pid_t child = fork();
        if (!child) {
            int c = accept(ls, NULL, NULL);
            VmMsg q;
            int qfd = -1;
            if (c < 0 || recv_wait(c, &q, &qfd) <= 0) _exit(1);
            VmMsg i = vm_msg(VM_INFO, q.seq, 2, 2048, 0x1ff, 2, "id"); vm_send(c, &i, -1);
            VmMsg s2 = vm_msg(VM_STATE, q.seq, 2, 0, 0, 4321, ""); vm_send(c, &s2, -1);
            struct timespec ts = { 3, 0 };
            nanosleep(&ts, NULL);
            _exit(0);
        }
        close(ls);
        VmLive lv;
        int r = vmwatch_live(sp, &lv, 2500);
        CHECK(!r && lv.pidfd >= 0 && vmwatch_pidfd_pid(lv.pidfd) == getpid(), "the peer's pidfd: r %d fd %d pid %ld", r, lv.pidfd, lv.pidfd >= 0 ? vmwatch_pidfd_pid(lv.pidfd) : -1L);
        if (lv.pidfd >= 0) close(lv.pidfd);
        int st;
        waitpid(child, &st, 0);
        unlink(sp);
        rmdir(d);
    }

    printf("vmwatch-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
