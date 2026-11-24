/* vmwatch_test.c — the pure checks behind Sentinela's machine facts (src/vm/vmwatch.h): the machine's scope is an exact
 * path beside lord-server's leaf, a cgroup is "in" it only when it is that path or below it (never a path that merely
 * contains the name), and a process's program must be the installed lord-vm. */
#include "../src/vm/vmwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *self = "/lordos.slice/lordos-session-1000/server";
    const char *id = "6f1c0d2e-1111-4222-8333-444455556666";
    char sc[256];

    /* where the scope must be */
    CHECK(!vmwatch_scope_path(self, id, 0, sc, sizeof sc) && !strcmp(sc, "/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-6f1c0d2e-1111-4222-8333-444455556666.scope"), "scope: %s", sc);
    CHECK(!vmwatch_scope_path(self, id, 1, sc, sizeof sc) && !strcmp(sc, "/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-tor-6f1c0d2e-1111-4222-8333-444455556666.scope"), "a Tor gateway's scope: %s", sc);
    CHECK(vmwatch_scope_path("/user.slice/session-3.scope", id, 0, sc, sizeof sc) && !sc[0], "not a LordOS session: unknown");
    CHECK(vmwatch_scope_path("/x/notasession/server", id, 0, sc, sizeof sc) && !sc[0], "a server leaf outside a session");
    CHECK(vmwatch_scope_path(self, "../../evil", 0, sc, sizeof sc) && !sc[0], "an id with a path in it");
    CHECK(vmwatch_scope_path(self, "", 0, sc, sizeof sc) && !sc[0], "no id");
    char tiny[16];
    CHECK(vmwatch_scope_path(self, id, 0, tiny, sizeof tiny) && !tiny[0], "too small: nothing, not a cut path");
    vmwatch_scope_path(self, id, 0, sc, sizeof sc);

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
    snprintf(t, sizeof t, "0::/lordos.slice/lordos-session-1000/app-lordos-lordos-vm-%s.scope2/qemu\n", id);
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
            if (c < 0 || vm_recv(c, &q, &qfd) <= 0 || q.type != VM_STATUS) _exit(1);
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
        int r = vmwatch_live(sp, &lv, 150);
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

    printf("vmwatch-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
