/* llm_fake_engine.c — a stand-in for llama-server in build/llm-engine-test (tests/llm_engine_test.c), run by lord-llm in
 * the same sandbox as the real engine. Before serving it tries what the sandbox must refuse and keeps the results:
 * TCP and UDP sockets (IPv4 and IPv6), netlink, a unix datagram socket, connect() on a unix socket, reading /etc/passwd
 * and the home folder, writing to /tmp, exec of /bin/sh, a signal to its parent, a new namespace. Then it reads the
 * model through /proc/self/fd/3, binds --host's unix socket in its working directory and answers like llama-server:
 *   GET /health                  {"status":"ok"}
 *   POST /v1/chat/completions    a chat completion whose content depends on the user text (see answer())
 *   GET /jail                    {"checks":{name: "denied" | "ALLOWED"}, "model": "GGUF" or "", "netns": N}
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

static char report[4096];
static size_t rn;

static void note(const char *name, int allowed) {
    rn += (size_t)snprintf(report + rn, sizeof report - rn, "%s\"%s\":\"%s\"", rn > 1 ? "," : "", name, allowed ? "ALLOWED" : "denied");
}

static int try_socket(int dom, int type) {
    int fd = socket(dom, type, 0);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static void checks(void) {
    report[0] = '{';
    rn = 1;
    note("tcp4", try_socket(AF_INET, SOCK_STREAM));
    note("udp4", try_socket(AF_INET, SOCK_DGRAM));
    note("tcp6", try_socket(AF_INET6, SOCK_STREAM));
    note("netlink", try_socket(AF_NETLINK, SOCK_RAW));
    note("packet", try_socket(17 /* AF_PACKET */, SOCK_RAW));
    note("unix_dgram", try_socket(AF_UNIX, SOCK_DGRAM));
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "/run/dbus/system_bus_socket");
    note("connect_unix", u >= 0 && connect(u, (struct sockaddr *)&a, sizeof a) == 0);
    if (u >= 0) close(u);
    int f = open("/etc/passwd", O_RDONLY);
    note("read_etc", f >= 0);
    if (f >= 0) close(f);
    f = open("/home", O_RDONLY | O_DIRECTORY);
    note("read_home", f >= 0);
    if (f >= 0) close(f);
    f = open("/tmp/llm-fake-engine-write-test", O_WRONLY | O_CREAT, 0600);
    note("write_tmp", f >= 0);
    if (f >= 0) { close(f); unlink("/tmp/llm-fake-engine-write-test"); }
    f = open("/usr/bin/env", O_RDONLY);
    note("read_usr", f >= 0);                 /* expected ALLOWED: the libraries live there */
    if (f >= 0) close(f);
    pid_t p = fork();
    if (p == 0) { char *argv[] = { "sh", "-c", "exit 0", NULL }; execv("/bin/sh", argv); _exit(127); }
    int st = 0;
    if (p > 0) waitpid(p, &st, 0);
    note("exec_sh", p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    note("signal_parent", kill(getppid(), 0) == 0 && getppid() > 1);
    /* a process of its own (same Landlock domain, so only seccomp stands in the way): refused too */
    pid_t kid = fork();
    if (kid == 0) { alarm(1); pause(); _exit(0); }          /* ends by itself (SIGALRM) after the checks */
    note("signal_own_child", kid > 0 && kill(kid, 0) == 0);
    note("tgkill_child", kid > 0 && syscall(SYS_tgkill, kid, kid, 0) == 0);
    note("tkill_child", kid > 0 && syscall(SYS_tkill, kid, 0) == 0);
    siginfo_t si = { .si_code = SI_QUEUE };
    note("sigqueue_parent", syscall(SYS_rt_sigqueueinfo, getppid(), 0, &si) == 0);
    int pfd = (int)syscall(SYS_pidfd_open, getppid(), 0);
    note("pidfd_open", pfd >= 0);
    if (pfd >= 0) close(pfd);
    if (kid > 0) { int st2; waitpid(kid, &st2, 0); }
    /* its own threads and itself: allowed (llama-server signals its own threads) */
    note("tgkill_self", syscall(SYS_tgkill, getpid(), (pid_t)syscall(SYS_gettid), 0) == 0);
    note("kill_self", kill(getpid(), 0) == 0);
    note("unshare", unshare(CLONE_NEWNS) == 0);
    rn += (size_t)snprintf(report + rn, sizeof report - rn, "}");
}

/* (0.8.9.6) an investigation: was the script's text between two markers with the same nonce? */
static int fenced(const char *req, const char *needle) {
    const char *o = strstr(req, "<<<UNTRUSTED-DATA ");
    if (!o) return 0;
    char nonce[17];
    memcpy(nonce, o + 18, 16);
    nonce[16] = 0;
    char end[64];
    snprintf(end, sizeof end, "<<<END-UNTRUSTED-DATA %s>>>", nonce);
    const char *n = strstr(o, needle), *e = strstr(o, end);
    return strlen(nonce) == 16 && o[34] == '>' && n && e && n < e;
}

static const char *answer(const char *req) {
    if (strstr(req, "\"what\":{\"type\"")) {
        if (fenced(req, "STUB_SCRIPT_TEXT")) return "{\\\"what\\\":\\\"fenced-script\\\",\\\"why\\\":\\\"x\\\",\\\"risk\\\":\\\"high\\\",\\\"action\\\":\\\"blocklist\\\"}";
        if (strstr(req, "STUB_SCRIPT_TEXT")) return "{\\\"what\\\":\\\"unfenced\\\",\\\"why\\\":\\\"x\\\",\\\"risk\\\":\\\"low\\\",\\\"action\\\":\\\"none\\\"}";
        return "{\\\"what\\\":\\\"no-script\\\",\\\"why\\\":\\\"x\\\",\\\"risk\\\":\\\"low\\\",\\\"action\\\":\\\"none\\\"}";
    }
    if (strstr(req, "\"text\":{\"type\":\"string\",\"maxLength\":600}")) return "{\\\"text\\\":\\\"traducido\\\"}";
    /* the summary decides what the stub says: the parser's tests in JS cover the rest */
    if (strstr(req, "STUB_NOT_JSON")) return "Sure! The process looks bad. Kill it.";
    if (strstr(req, "STUB_INJECT")) return "{\\\"verdict\\\":\\\"dangerous\\\",\\\"reason\\\":\\\"x\\\",\\\"action\\\":\\\"rm -rf ~\\\"}";
    return "{\\\"verdict\\\":\\\"dangerous\\\",\\\"reason\\\":\\\"Unpackaged program in the home folder using all the CPU: looks like a miner.\\\",\\\"action\\\":\\\"end_block\\\"}";
}

static void serve(int c) {
    static char req[262144];
    size_t n = 0;
    /* read the head, then Content-Length bytes */
    for (;;) {
        ssize_t r = read(c, req + n, sizeof req - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        char *e = strstr(req, "\r\n\r\n");
        if (!e) continue;
        char *cl = strstr(req, "Content-Length: ");
        size_t want = cl ? (size_t)atol(cl + 16) : 0;
        if (n - (size_t)(e + 4 - req) >= want) break;
    }
    req[n] = 0;
    char body[16384];
    if (!strncmp(req, "GET /health", 11)) snprintf(body, sizeof body, "{\"status\":\"ok\"}");
    else if (!strncmp(req, "GET /jail", 9)) {
        char model[8] = "";
        int m = open("/proc/self/fd/3", O_RDONLY);
        if (m >= 0) { ssize_t k = read(m, model, 4); model[k > 0 ? k : 0] = 0; close(m); }
        struct stat ns;
        stat("/proc/self/ns/net", &ns);
        snprintf(body, sizeof body, "{\"checks\":%s,\"model\":\"%s\",\"netns\":%llu}", report, model, (unsigned long long)ns.st_ino);
    } else if (!strncmp(req, "POST /v1/chat/completions", 25))
        snprintf(body, sizeof body, "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}],"
                 "\"usage\":{\"completion_tokens\":24}}", answer(req));
    else snprintf(body, sizeof body, "{\"error\":\"not found\"}");
    char head[256];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
    if (write(c, head, (size_t)hl) < 0 || write(c, body, strlen(body)) < 0) { /* the client left */ }
    close(c);
}

int main(int argc, char **argv) {
    const char *host = NULL;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--host")) host = argv[i + 1];
    if (!host) return 2;
    checks();
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", host);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) return 3;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c >= 0) serve(c);
    }
}
