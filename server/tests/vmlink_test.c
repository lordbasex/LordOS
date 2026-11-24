/* vmlink_test.c — a machine group's internal link (vm/vmlink.c, docs/VIRTUALIZATION.md 19.3): VM_LINK's message checks
 * (vmproto.h), lord-vm's descriptor check (vm_link_fd_ok) on every kind of descriptor, and lord-server's side
 * (vm_link_pair) against two stand-in lord-vms on runtime sockets: each gets one end of one pair, the two ends talk to
 * each other, nothing stays open in the caller, a refusal is reported, and an absent member is. Under ASan and UBSan. */
#include "../src/vm/vmlink.h"
#include "../src/vm/vmproto.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); fails++; } } while (0)

static const char *GID = "6f1c2b3a-1111-4222-8333-444455556666";
static const char *A = "0b6c1a2e-1111-4222-8333-444455556661", *B = "0b6c1a2e-1111-4222-8333-444455556662";

static void messages(void) {
    VmMsg m = vm_msg(VM_LINK, 1, 1, 0, 0, 0, GID);
    CHECK(vm_msg_valid(&m, 1), "a link with a group id");
    m = vm_msg(VM_LINK, 1, 0, 0, 0, 0, GID);
    CHECK(vm_msg_valid(&m, 1), "a pulled cable");
    m = vm_msg(VM_LINK, 1, 2, 0, 0, 0, GID);
    CHECK(!vm_msg_valid(&m, 1), "a = 2");
    m = vm_msg(VM_LINK, 1, 1, 0, 0, 0, "");
    CHECK(!vm_msg_valid(&m, 1), "no group id");
    m = vm_msg(VM_LINK, 1, 1, 0, 0, 0, "6f1c2b3a");
    CHECK(!vm_msg_valid(&m, 1), "a short id");
    m = vm_msg(VM_LINK, 1, 1, 0, 0, 0, GID);
    CHECK(!vm_msg_valid(&m, 0), "VM_LINK is a request, never an event");
    CHECK(VM_REQ_LAST >= VM_LINK && VM_LINK < VM_REPLY, "VM_LINK is a request (VM_SHELL came after it)");
}

static void descriptors(void) {
    char why[96];
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv), "socketpair");
    CHECK(vm_link_fd_ok(sv[0], why, sizeof why), "a stream pair's end");
    close(sv[0]); close(sv[1]);
    CHECK(!socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), "socketpair dgram");
    CHECK(!vm_link_fd_ok(sv[0], why, sizeof why) && strstr(why, "stream"), "a datagram pair");
    close(sv[0]); close(sv[1]);
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), "socketpair seqpacket");
    CHECK(!vm_link_fd_ok(sv[0], why, sizeof why), "a seqpacket pair");
    close(sv[0]); close(sv[1]);
    int p[2];
    CHECK(!pipe(p), "pipe");
    CHECK(!vm_link_fd_ok(p[0], why, sizeof why) && strstr(why, "socket"), "a pipe");
    close(p[0]); close(p[1]);
    int f = open("/dev/null", O_RDWR);
    CHECK(!vm_link_fd_ok(f, why, sizeof why), "a file");
    close(f);
    int in = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(in < 0 || !vm_link_fd_ok(in, why, sizeof why), "an internet socket");
    if (in >= 0) close(in);
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(!vm_link_fd_ok(u, why, sizeof why), "an unconnected socket (no peer)");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "vmlink-test-%d", (int)getpid());   /* abstract */
    CHECK(!bind(u, (struct sockaddr *)&a, sizeof a) && !listen(u, 1), "listen");
    CHECK(!vm_link_fd_ok(u, why, sizeof why), "a listening socket");
    close(u);
    CHECK(!vm_link_fd_ok(-1, why, sizeof why), "no descriptor");
}

/* a stand-in lord-vm: listens on <rt>/lordos-vm/<id>.sock, takes one VM_LINK, checks its descriptor like lord-vm, answers
 * (refuse: VME_BAD_REQUEST), then writes its name on the end it got and reads the other's */
static pid_t fake_vm(const char *rt, const char *id, const char *name, int refuse, int out) {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    snprintf(path, sizeof path, "%s/lordos-vm/%s.sock", rt, id);
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 2)) { perror("bind"); exit(1); }
    pid_t p = fork();
    if (p) { close(l); return p; }
    alarm(20);
    int c = accept(l, NULL, NULL);
    VmMsg m;
    int fd = -1, got = 0;
    for (int i = 0; i < 100 && got == 0; i++) { struct pollfd pf = { c, POLLIN, 0 }; poll(&pf, 1, 100); got = vm_recv(c, &m, &fd); }
    char why[96] = "", res[128] = "";
    int ok = got == 1 && m.type == VM_LINK && vm_msg_valid(&m, 1) && !strcmp(m.text, GID) && m.a == 1 && vm_link_fd_ok(fd, why, sizeof why);
    VmMsg r = vm_msg(VM_REPLY, m.seq, ok && !refuse ? VME_OK : VME_BAD_REQUEST, 0, 0, 0, refuse ? "refused here" : why);
    vm_send(c, &r, -1);
    if (ok && !refuse) {
        if (write(fd, name, strlen(name)) < 0) {}
        char b[64] = "";
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 5000) == 1) { ssize_t k = read(fd, b, sizeof b - 1); if (k > 0) b[k] = 0; }
        snprintf(res, sizeof res, "%s heard %s\n", name, b);
    } else snprintf(res, sizeof res, "%s %s\n", name, ok ? "refused" : "bad");
    if (write(out, res, strlen(res)) < 0) {}
    _exit(0);
}

static int open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

static void pairing(void) {
    char rt[] = "/tmp/vmlink-test.XXXXXX";
    if (!mkdtemp(rt)) { fails++; return; }
    char d[96], err[200], buf[512];
    snprintf(d, sizeof d, "%s/lordos-vm", rt);
    mkdir(d, 0700);
    for (int round = 0; round < 2; round++) {
        int pp[2];
        if (pipe(pp)) { fails++; return; }
        pid_t a = fake_vm(rt, A, "gateway", 0, pp[1]), b = fake_vm(rt, B, "workstation", round == 1, pp[1]);
        close(pp[1]);
        int before = open_fds();
        int r = vm_link_pair(rt, A, B, GID, 1, 5000, err, sizeof err);
        CHECK(open_fds() == before, "no descriptor stays in the caller");
        waitpid(a, NULL, 0);
        waitpid(b, NULL, 0);
        ssize_t k = read(pp[0], buf, sizeof buf - 1);
        buf[k > 0 ? k : 0] = 0;
        close(pp[0]);
        if (round == 0) {
            CHECK(r == 0, err);
            CHECK(strstr(buf, "gateway heard workstation") && strstr(buf, "workstation heard gateway"), "the two ends are one pair");
        } else {
            CHECK(r != 0 && strstr(err, "second member") && strstr(err, "refused here"), "a refusal is reported");
        }
        char p[160];
        snprintf(p, sizeof p, "%s/%s.sock", d, A); unlink(p);
        snprintf(p, sizeof p, "%s/%s.sock", d, B); unlink(p);
    }
    CHECK(vm_link_pair(rt, A, B, GID, 1, 1000, err, sizeof err) != 0 && strstr(err, "isn't running"), "a member not running");
    CHECK(vm_link_pair(rt, A, A, GID, 1, 1000, err, sizeof err) != 0, "a machine with itself");
    CHECK(vm_link_pair(rt, A, B, "not-a-uuid", 1, 1000, err, sizeof err) != 0, "a bad group id");
    rmdir(d);
    rmdir(rt);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    messages();
    descriptors();
    pairing();
    if (fails) { fprintf(stderr, "vmlink-test: %d failures\n", fails); return 1; }
    printf("vmlink-test: ok\n");
    return 0;
}
