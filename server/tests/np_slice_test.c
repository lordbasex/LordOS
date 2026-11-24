/* np_slice_test.c — the machines' slice (docs/VIRTUALIZATION.md 3.7) in lord-netperm's programs, in the real kernel:
 * a machine's scope one level below its session's lordos-machines.slice is decided by its own np_apps entry when the
 * slice's entry carries NP_A_SLICE, and by the slice's entry (lordos:machines) while it isn't registered yet; never by
 * nothing. lord-netperm's own programs (build/gen/np_bpf.h) with private maps, attached ONLY to a throwaway cgroup this
 * test makes (/sys/fs/cgroup/lordos-slicetest-<pid>: its "lordos-machines.slice" at level 2, the scopes at level 3),
 * never to lordos.service, user@ or the running lord-netperm's maps; removed at the end.
 *
 *   sudo build/np-slice-test      (without root, `make test` says it skipped)
 *
 * The policies are told apart by what socket() answers: the slice's entry has "no network" (every inet socket refused),
 * a registered machine's has the Tor gateway's built-in restriction (TCP sockets made, UDP refused). */
#include "netperm/bpfsys.h"
#include "netperm/npcompile.h"
#include "netperm/npload.h"
#include "np_bpf.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int maps[24];
static const char *map_names[24];
static int nmaps;
static int map_fd(const char *name) {
    for (int i = 0; i < nmaps; i++) if (!strcmp(map_names[i], name)) return maps[i];
    return -1;
}
static int links[16], nlinks;
static char cgroot[96], cgslice[160], cgvm[256], cgnew[256];
static uint64_t slice_cg, vm_cg;
static NpApp slice_app, vm_app;

static int write_file(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
}

static void rm_cg(const char *dir) {
    char p[300];
    if (!dir[0]) return;
    snprintf(p, sizeof p, "%s/cgroup.kill", dir);
    write_file(p, "1");
    for (int i = 0; i < 50 && rmdir(dir) && errno == EBUSY; i++) usleep(100000);
}
static void cleanup(void) {
    rm_cg(cgvm);
    rm_cg(cgnew);
    if (cgslice[0]) rmdir(cgslice);
    if (cgroot[0]) rmdir(cgroot);
    for (int i = 0; i < nlinks; i++) close(links[i]);
    nlinks = 0;
}

static int setup(void) {
#define NP_MKMAP(n, t, k, v, m, f) map_names[nmaps] = #n; maps[nmaps] = bpfs_map_create(t, k, v, m, f, #n); if (maps[nmaps] < 0) { perror(#n); return -1; } nmaps++;
    NP_MAPS(NP_MKMAP)
#undef NP_MKMAP
    snprintf(cgroot, sizeof cgroot, "/sys/fs/cgroup/lordos-slicetest-%d", (int)getpid());
    snprintf(cgslice, sizeof cgslice, "%s/" FW_VM_SLICE, cgroot);
    snprintf(cgvm, sizeof cgvm, "%s/" FW_VM_SCOPE_PREFIX "6f1c0d2e-1111-4222-8333-444455556666.scope", cgslice);
    snprintf(cgnew, sizeof cgnew, "%s/" FW_VM_SCOPE_PREFIX "7f1c0d2e-1111-4222-8333-444455556666.scope", cgslice);
    if (mkdir(cgroot, 0755) || mkdir(cgslice, 0755) || mkdir(cgvm, 0755) || mkdir(cgnew, 0755)) { perror("the test's cgroups"); return -1; }
    int rfd = open(cgroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int sfd = open(cgslice, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    slice_cg = rfd >= 0 ? bpfs_cgroup_id(rfd, FW_VM_SLICE) : 0;
    vm_cg = sfd >= 0 ? bpfs_cgroup_id(sfd, cgvm + strlen(cgslice) + 1) : 0;
    if (sfd >= 0) close(sfd);
    if (rfd < 0 || !slice_cg || !vm_cg) { perror("cgroup ids"); if (rfd >= 0) close(rfd); return -1; }
    static char log[1 << 20];
    for (int w = 0; w < NP_NSPECS; w++) {
        const NpProgSpec *sp = &np_prog_specs[w];
        if (sp->opt && sp->attach != BPF_CGROUP_INET_INGRESS && sp->attach != BPF_CGROUP_INET_EGRESS) continue;
        const NpProgDesc *d = NULL;
        for (int i = 0; i < NP_NPROGS; i++) if (!strcmp(np_progs[i].section, sp->sec)) d = &np_progs[i];
        if (!d) { close(rfd); return -1; }
        int fd = bpfs_prog_load_btf(sp->type, sp->attach, 0, np_prog_license(sp), d, map_fd, log, sizeof log);
        if (fd < 0) { fprintf(stderr, "%s refused: %s\n%s\n", sp->sec, strerror(errno), log); close(rfd); return -1; }
        int l = bpfs_link_create(fd, rfd, sp->attach);
        close(fd);
        if (l < 0) { fprintf(stderr, "%s: attach: %s\n", sp->sec, strerror(errno)); close(rfd); return -1; }
        links[nlinks++] = l;
    }
    close(rfd);
    /* the slice: what lord-netperm writes for it (lordos:machines, NP_A_SLICE), with "no network" to be told apart */
    FwStore *s = fw_store_new();
    NpCompiled c;
    if (!s || np_compile_app(s, "lordos:machines", 1, NULL, NULL, 0, &c)) return -1;
    slice_app = c.app;
    slice_app.flags |= NP_R_NO_NETWORK | NP_A_SLICE;
    slice_app.can_deny = 1;
    np_compiled_free(&c);
    /* a registered machine: the Tor gateway's built-in restriction (TCP sockets only) */
    if (np_compile_app(s, FW_APP_TOR_GATEWAY, 2, NULL, NULL, 0, &c)) return -1;
    vm_app = c.app;
    np_compiled_free(&c);
    fw_store_free(s);
    uint32_t zero = 0;
    NpCfg cfg = { 2, 1, 0, 0, 0 };                  /* lordos-slicetest-<pid>/lordos-machines.slice */
    NpCount z = { 0, 0, 0, 0, 0 };
    if (bpfs_map_update(map_fd("np_cfg"), &zero, &cfg, BPF_ANY) || bpfs_map_update(map_fd("np_apps"), &slice_cg, &slice_app, BPF_ANY) ||
        bpfs_map_update(map_fd("np_counts"), &slice_cg, &z, BPF_ANY) || bpfs_map_update(map_fd("np_apps"), &vm_cg, &vm_app, BPF_ANY) ||
        bpfs_map_update(map_fd("np_counts"), &vm_cg, &z, BPF_ANY)) { perror("maps"); return -1; }
    printf("np-slice-test: %d programs on %s\n", nlinks, cgroot);
    return 0;
}

/* in a child placed in dir: 1 TCP socket made, 2 UDP socket made (bits), or -1 */
static int probe(const char *dir) {
    pid_t c = fork();
    if (c == 0) {
        char p[300], v[32];
        snprintf(p, sizeof p, "%s/cgroup.procs", dir);
        snprintf(v, sizeof v, "%d", (int)getpid());
        if (write_file(p, v)) _exit(100);
        int r = 0, fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) { r |= 1; close(fd); }
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) { r |= 2; close(fd); }
        _exit(r);
    }
    int st = 0;
    if (c < 0 || waitpid(c, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) == 100) return -1;
    return WEXITSTATUS(st);
}

static uint64_t denied_of(uint64_t cg) {
    NpCount n;
    return bpfs_map_lookup(map_fd("np_counts"), &cg, &n) ? (uint64_t)-1 : n.denied;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() != 0) { printf("np-slice-test: skipped: needs root (sudo build/np-slice-test)\n"); return 0; }
    if (setup()) { cleanup(); return 1; }
    int r = probe(cgvm);
    printf("np-slice-test: a registered machine's scope in the slice: TCP %s, UDP %s\n", r >= 0 && r & 1 ? "made" : "refused", r >= 0 && r & 2 ? "made" : "refused");
    CHECK(r == 1);                                  /* its own policy (TCP only), not the slice's (nothing) */
    CHECK(denied_of(vm_cg) >= 1 && denied_of(vm_cg) != (uint64_t)-1);    /* counted under the machine's own scope */
    uint64_t before = denied_of(slice_cg);
    r = probe(cgnew);
    printf("np-slice-test: a machine's scope not registered yet: TCP %s, UDP %s\n", r >= 0 && r & 1 ? "made" : "refused", r >= 0 && r & 2 ? "made" : "refused");
    CHECK(r == 0);                                  /* the slice's policy: never no policy at all */
    CHECK(denied_of(slice_cg) >= before + 2);
    /* without NP_A_SLICE the slice's entry is an ordinary app: its machines get its policy, not their own */
    NpApp plain = slice_app;
    plain.flags &= ~NP_A_SLICE;
    bpfs_map_update(map_fd("np_apps"), &slice_cg, &plain, BPF_ANY);
    r = probe(cgvm);
    CHECK(r == 0);
    /* and the slice's flag never comes from a machine's own entry: a scope whose entry carries it doesn't look further down */
    bpfs_map_update(map_fd("np_apps"), &slice_cg, &slice_app, BPF_ANY);
    NpApp sneaky = vm_app;
    sneaky.flags |= NP_A_SLICE;
    bpfs_map_update(map_fd("np_apps"), &vm_cg, &sneaky, BPF_ANY);
    r = probe(cgvm);
    CHECK(r == 1);
    cleanup();
    struct stat sb;
    CHECK(stat(cgroot, &sb) && errno == ENOENT);    /* nothing left behind */
    printf("np-slice-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
