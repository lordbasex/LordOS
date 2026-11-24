/* np_torgw_test.c — the Tor gateway's built-in restriction (FW_R_TOR_UPLINK, docs/FIREWALL.md "The Tor gateway") in the
 * real kernel: lord-netperm's own programs (build/gen/np_bpf.h) with private maps, attached ONLY to a throwaway cgroup
 * this test makes (/sys/fs/cgroup/lordos-fwtest-<pid>, its app leaf at level 2), never to lordos.service, user@ or the
 * running lord-netperm's maps. Removed again at the end, with whatever still runs in it.
 *
 *   sudo build/np-torgw-test                      probes from a process in the gateway's policy: UDP and ICMP sockets,
 *                                                 TCP to port 53 and
 *                                                 853, to the LAN, loopback and this computer's own addresses, a bind;
 *                                                 then TCP 1.1.1.1:443 must pass the firewall
 *   sudo build/np-torgw-test run [--uid U --gid G] -- cmd args...
 *                                                 holds the same policy around a command run in that cgroup as U:G
 *                                                 (tests/vm_gateway_fw.sh), and prints the connections it allowed and
 *                                                 denied at the end
 *
 * Without root (`make test`) it says it skipped. */
#include "netperm/bpfsys.h"
#include "netperm/npcompile.h"
#include "netperm/nplocal.h"
#include "netperm/npload.h"
#include "np_bpf.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
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
static char cgroot[96], cgapp[128];
static uint64_t app_cg;
static NpLocalKey local[NP_MAX_LOCAL];
static int nlocal;

static int write_file(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
}

static void cleanup(void) {
    char p[200];
    if (cgapp[0]) {
        snprintf(p, sizeof p, "%s/cgroup.kill", cgapp);
        write_file(p, "1");
        for (int i = 0; i < 50 && rmdir(cgapp) && errno == EBUSY; i++) usleep(100000);
    }
    if (cgroot[0]) rmdir(cgroot);
    for (int i = 0; i < nlinks; i++) close(links[i]);
    nlinks = 0;
}
static void on_sig(int s) { (void)s; }

/* the maps, the programs, the cgroup, the gateway's policy: 0 ready */
static int setup(void) {
#define NP_MKMAP(n, t, k, v, m, f) map_names[nmaps] = #n; maps[nmaps] = bpfs_map_create(t, k, v, m, f, #n); if (maps[nmaps] < 0) { perror(#n); return -1; } nmaps++;
    NP_MAPS(NP_MKMAP)
#undef NP_MKMAP
    snprintf(cgroot, sizeof cgroot, "/sys/fs/cgroup/lordos-fwtest-%d", (int)getpid());
    snprintf(cgapp, sizeof cgapp, "%s/app", cgroot);
    if (mkdir(cgroot, 0755) || mkdir(cgapp, 0755)) { perror("the test's cgroup"); return -1; }
    int rfd = open(cgroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    app_cg = rfd >= 0 ? bpfs_cgroup_id(rfd, "app") : 0;
    if (rfd < 0 || !app_cg) { perror("cgroup id"); return -1; }
    static char log[1 << 20];
    for (int w = 0; w < NP_NSPECS; w++) {
        const NpProgSpec *sp = &np_prog_specs[w];
        if (sp->opt && sp->attach != BPF_CGROUP_INET_INGRESS && sp->attach != BPF_CGROUP_INET_EGRESS) continue;   /* devices, Bluetooth */
        const NpProgDesc *d = NULL;
        for (int i = 0; i < NP_NPROGS; i++) if (!strcmp(np_progs[i].section, sp->sec)) d = &np_progs[i];
        if (!d) return -1;
        int fd = bpfs_prog_load_btf(sp->type, sp->attach, 0, np_prog_license(sp), d, map_fd, log, sizeof log);
        if (fd < 0) { fprintf(stderr, "%s refused: %s\n%s\n", sp->sec, strerror(errno), log); close(rfd); return -1; }
        int l = bpfs_link_create(fd, rfd, sp->attach);
        close(fd);
        if (l < 0) { fprintf(stderr, "%s: attach: %s\n", sp->sec, strerror(errno)); close(rfd); return -1; }
        links[nlinks++] = l;
    }
    close(rfd);
    /* exactly what lord-netperm writes for a scope of lordos:tor-gateway with no rules of the owner */
    FwStore *s = fw_store_new();
    NpCompiled c;
    if (!s || np_compile_app(s, FW_APP_TOR_GATEWAY, 1, NULL, NULL, 0, &c)) return -1;
    if (!(c.app.flags & NP_R_TOR_UPLINK) || !c.app.can_deny) { fprintf(stderr, "the gateway's app has no built-in restriction\n"); return -1; }
    uint32_t zero = 0;
    NpCfg cfg = { 2, 1, 0, 0 };                     /* lordos-fwtest-<pid>/app */
    NpCount z = { 0, 0, 0, 0, 0 };
    if (bpfs_map_update(map_fd("np_apps"), &app_cg, &c.app, BPF_ANY) || bpfs_map_update(map_fd("np_counts"), &app_cg, &z, BPF_ANY) ||
        bpfs_map_update(map_fd("np_cfg"), &zero, &cfg, BPF_ANY)) { perror("maps"); return -1; }
    np_compiled_free(&c);
    fw_store_free(s);
    nlocal = np_host_addrs(local, NP_MAX_LOCAL);
    uint8_t one = 1;
    for (int i = 0; i < nlocal; i++) bpfs_map_update(map_fd("np_local"), &local[i], &one, BPF_ANY);
    NpLocalKey fake = { .fam = 4, .addr = { 198, 51, 100, 77 } };
    bpfs_map_update(map_fd("np_local"), &fake, &one, BPF_ANY);
    printf("np-torgw-test: policy of %s on %s (%d programs, %d of this computer's addresses)\n", FW_APP_TOR_GATEWAY, cgapp, nlinks, nlocal);
    return 0;
}

static int join(void) {
    char p[200], v[32];
    snprintf(p, sizeof p, "%s/cgroup.procs", cgapp);
    snprintf(v, sizeof v, "%d", (int)getpid());
    return write_file(p, v);
}

static void counts(void) {
    NpCount n;
    if (!bpfs_map_lookup(map_fd("np_counts"), &app_cg, &n))
        printf("np-torgw-test: in the gateway's policy: %llu connections allowed, %llu denied, %llu bytes blocked\n",
               (unsigned long long)n.allowed, (unsigned long long)n.denied, (unsigned long long)n.blocked);
}

/* ---- the probes (in a child inside the cgroup); each one's errno, 0 = it went through ---- */
static int tcp(int fam, const char *ip, int port, int wait_ms) {
    int fd = socket(fam == 6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return errno;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (fam == 6) {
        struct sockaddr_in6 *a = (void *)&ss;
        a->sin6_family = AF_INET6; a->sin6_port = htons((uint16_t)port); inet_pton(AF_INET6, ip, &a->sin6_addr); sl = sizeof *a;
    } else {
        struct sockaddr_in *a = (void *)&ss;
        a->sin_family = AF_INET; a->sin_port = htons((uint16_t)port); inet_pton(AF_INET, ip, &a->sin_addr); sl = sizeof *a;
    }
    int r = connect(fd, (struct sockaddr *)&ss, sl) ? errno : 0;
    if (r == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        r = poll(&p, 1, wait_ms) == 1 ? 0 : ETIMEDOUT;
        int e = 0;
        socklen_t el = sizeof e;
        if (!r && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el)) r = e;
    }
    close(fd);
    return r;
}

static int expect_denied(const char *what, int err) {
    printf("np-torgw-test: %-46s %s\n", what, err == EPERM ? "denied (EPERM)" : err ? strerror(err) : "WENT THROUGH");
    return err == EPERM;
}

static int probes(void) {
    int ok = 1;
    /* (a socket's cgroup programs are the ones of the cgroup it was made in, so every socket of the gateway's passt is
     * decided here: no UDP or ICMP socket at all; sendmsg4/6 and the packet filter deny UDP too, as npcompile-test
     * shows against fw_decide) */
    if (join()) { perror("join"); return 0; }
    int e = socket(AF_INET, SOCK_DGRAM, 0) < 0 ? errno : 0;
    ok &= expect_denied("socket(AF_INET, SOCK_DGRAM)", e);
    e = socket(AF_INET6, SOCK_DGRAM, 0) < 0 ? errno : 0;
    ok &= expect_denied("socket(AF_INET6, SOCK_DGRAM)", e);
    e = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) < 0 ? errno : 0;
    ok &= expect_denied("socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)", e);
    e = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) < 0 ? errno : 0;
    ok &= expect_denied("socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)", e);
    ok &= expect_denied("TCP 1.1.1.1:53 (DNS over TCP)", tcp(4, "1.1.1.1", 53, 3000));
    ok &= expect_denied("TCP 1.1.1.1:853 (DNS over TLS)", tcp(4, "1.1.1.1", 853, 3000));
    ok &= expect_denied("TCP 192.168.1.1:443 (LAN)", tcp(4, "192.168.1.1", 443, 3000));
    ok &= expect_denied("TCP 10.0.0.1:443 (LAN)", tcp(4, "10.0.0.1", 443, 3000));
    ok &= expect_denied("TCP 172.16.0.1:9001 (LAN)", tcp(4, "172.16.0.1", 9001, 3000));
    ok &= expect_denied("TCP 169.254.169.254:80 (link-local)", tcp(4, "169.254.169.254", 80, 3000));
    ok &= expect_denied("TCP 127.0.0.1:631 (loopback)", tcp(4, "127.0.0.1", 631, 3000));
    ok &= expect_denied("TCP [::1]:631 (loopback)", tcp(6, "::1", 631, 3000));
    ok &= expect_denied("TCP [fd00::1]:443 (ULA)", tcp(6, "fd00::1", 443, 3000));
    /* this computer's own addresses, a global one first if it has any */
    int shown = 0;
    for (int pass = 0; pass < 2 && shown < 2; pass++)
        for (int i = 0; i < nlocal && shown < 2; i++) {
            int lan = local[i].fam == 4 ? np_is_lan4(local[i].addr) : np_is_lan6(local[i].addr);
            if (pass == 0 ? lan : !lan) continue;
            char ip[64], what[120];
            inet_ntop(local[i].fam == 4 ? AF_INET : AF_INET6, local[i].addr, ip, sizeof ip);
            snprintf(what, sizeof what, "TCP %s:22 (this computer%s)", ip, lan ? "" : ", a public address");
            ok &= expect_denied(what, tcp(local[i].fam, ip, 22, 3000));
            shown++;
        }
    /* np_local itself: a public address put in the map as if an interface had it (198.51.100.77, documentation range)
     * is this computer's, so denied; its neighbour, not in the map, passes the firewall (and then times out) */
    ok &= expect_denied("TCP 198.51.100.77:443 (in np_local)", tcp(4, "198.51.100.77", 443, 1500));
    e = tcp(4, "198.51.100.78", 443, 1500);
    printf("np-torgw-test: %-46s %s\n", "TCP 198.51.100.78:443 (not in np_local)", e == EPERM ? "DENIED" : "passes the firewall");
    ok &= e != EPERM;
    int b = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(40999) };
    e = b < 0 ? errno : bind(b, (struct sockaddr *)&a, sizeof a) ? errno : 0;
    if (b >= 0) close(b);
    ok &= expect_denied("bind 0.0.0.0:40999 (incoming)", e);
    /* what Tor needs: TCP to a public address */
    e = tcp(4, "1.1.1.1", 443, 6000);
    printf("np-torgw-test: %-46s %s\n", "TCP 1.1.1.1:443 (the internet)", !e ? "allowed (connected)" : e == EPERM ? "DENIED" : strerror(e));
    ok &= e != EPERM;
    return ok;
}

static int run_cmd(int argc, char **argv) {
    uid_t uid = 0; gid_t gid = 0;
    int i = 2, have = 0;
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--uid") && i + 1 < argc) { uid = (uid_t)atoi(argv[++i]); have |= 1; }
        else if (!strcmp(argv[i], "--gid") && i + 1 < argc) { gid = (gid_t)atoi(argv[++i]); have |= 2; }
        else { fprintf(stderr, "usage: np-torgw-test run [--uid U --gid G] -- cmd args...\n"); return 2; }
    }
    if (i + 1 >= argc) { fprintf(stderr, "usage: np-torgw-test run [--uid U --gid G] -- cmd args...\n"); return 2; }
    if (setup()) { cleanup(); return 1; }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    pid_t c = fork();
    if (c == 0) {
        if (join()) { perror("join"); _exit(126); }
        /* the account's own groups too (kvm, for /dev/kvm) */
        struct passwd *pw = have == 3 ? getpwuid(uid) : NULL;
        if (have == 3 && ((pw ? initgroups(pw->pw_name, gid) : setgroups(1, &gid)) || setgid(gid) || setuid(uid))) { perror("drop to the user"); _exit(126); }
        execvp(argv[i + 1], argv + i + 1);
        perror(argv[i + 1]);
        _exit(127);
    }
    int st = 0;
    while (c > 0 && waitpid(c, &st, 0) < 0 && errno == EINTR) {}
    counts();
    cleanup();
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() != 0) {
        if (argc > 1) { fprintf(stderr, "np-torgw-test: needs root\n"); return 77; }
        printf("np-torgw-test: skipped: needs root (sudo build/np-torgw-test)\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "run")) return run_cmd(argc, argv);
    if (setup()) { cleanup(); return 1; }
    pid_t c = fork();
    if (c == 0) _exit(probes() ? 0 : 1);
    int st = 0;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    counts();
    cleanup();
    struct stat sb;
    CHECK(stat(cgroot, &sb) && errno == ENOENT);    /* nothing left behind */
    printf("np-torgw-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
