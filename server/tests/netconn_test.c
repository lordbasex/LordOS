/* netconn_test.c — the components' network connectors (src/sec/netconn.c, 0.8.5):
 * request validation on both sides, the sockets the connector makes, the real connector process
 * (this binary re-executed with --net-connector, as lord-server is) and its fail-closed start. */
#include "sec/netconn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static NcReq good(void) {
    NcReq r;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(443) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    netconn_req_make(&r, AF_INET, SOCK_STREAM, (struct sockaddr *)&a, sizeof a);
    return r;
}

static void test_requests(void) {
    NcReq r = good();
    CHECK(!netconn_req_ok(&r) && r.fam == 4 && r.type == 1 && r.connect == 1 && r.port == 443);
    NcReq x = r; x.magic ^= 1; CHECK(netconn_req_ok(&x));
    x = r; x.fam = 5; CHECK(netconn_req_ok(&x));
    x = r; x.type = 3; CHECK(netconn_req_ok(&x));
    x = r; x.connect = 2; CHECK(netconn_req_ok(&x));
    x = r; x.port = 0; CHECK(netconn_req_ok(&x));                    /* connect needs a port */
    x = r; x.addr[9] = 1; CHECK(netconn_req_ok(&x));                 /* IPv4 with bytes past the 4th */
    x = r; x.scope_id = 2; CHECK(netconn_req_ok(&x));
    x = r; x.pad = 1; CHECK(netconn_req_ok(&x));
    x = r; x.connect = 0; CHECK(netconn_req_ok(&x));                 /* no connect: no port either */
    x.port = 0; CHECK(!netconn_req_ok(&x));
    CHECK(netconn_req_ok(NULL));
    NcReq y;
    CHECK(netconn_req_make(&y, AF_UNIX, SOCK_STREAM, NULL, 0));     /* only internet sockets */
    CHECK(netconn_req_make(&y, AF_INET, SOCK_RAW, NULL, 0));         /* no raw sockets */
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons(80) };
    CHECK(netconn_req_make(&y, AF_INET, SOCK_STREAM, (struct sockaddr *)&a6, sizeof a6));   /* family mismatch */
    CHECK(netconn_req_make(&y, AF_INET6, SOCK_STREAM, (struct sockaddr *)&a6, 4));         /* short address */
    CHECK(!netconn_req_make(&y, AF_INET6, SOCK_DGRAM, (struct sockaddr *)&a6, sizeof a6) && y.fam == 6 && y.type == 2);
    x = r; x.magic = 0;
    CHECK(netconn_serve_one(&x) == -EINVAL);                         /* the connector checks again */
    CHECK(!strcmp(netconn_name(NC_MAIL), "mail") && !strcmp(netconn_name(NC_LISTS), "lists") && !netconn_name(NC_COUNT));
}

static int listener(int *port) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    socklen_t al = sizeof a;
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 4) || getsockname(l, (struct sockaddr *)&a, &al)) return -1;
    *port = ntohs(a.sin_port);
    return l;
}

static int connected(int fd) {
    struct pollfd p = { fd, POLLOUT, 0 };
    int err = 1;
    socklen_t el = sizeof err;
    return poll(&p, 1, 3000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && !err;
}

static void test_sockets(int via_process) {
    int port = 0, l = listener(&port);
    CHECK(l >= 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int fd = netconn_socket(NC_MAIL, AF_INET, SOCK_STREAM, (struct sockaddr *)&a, sizeof a);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(connected(fd));
        CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
        CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        int c = accept(l, NULL, NULL);
        CHECK(c >= 0);
        CHECK(write(fd, "x", 1) == 1);
        char b;
        CHECK(c >= 0 && read(c, &b, 1) == 1 && b == 'x');
        if (c >= 0) close(c);
        close(fd);
    }
    int u = netconn_socket(NC_PRINT, AF_INET, SOCK_DGRAM, NULL, 0);
    int t = 0;
    socklen_t tl = sizeof t;
    CHECK(u >= 0 && !getsockopt(u, SOL_SOCKET, SO_TYPE, &t, &tl) && t == SOCK_DGRAM);
    if (u >= 0) close(u);
    /* a refused connection comes back as a socket whose SO_ERROR says so */
    close(l);
    fd = netconn_socket(NC_LISTS, AF_INET, SOCK_STREAM, (struct sockaddr *)&a, sizeof a);
    CHECK(fd < 0 || !connected(fd));
    if (fd >= 0) close(fd);
    errno = 0;
    CHECK(netconn_socket(NC_COUNT, AF_INET, SOCK_STREAM, NULL, 0) == -1 && errno == EINVAL);
    printf("netconn-test: sockets %s: ok\n", via_process ? "from the connector processes" : "made in place (no LordOS session)");
}

/* the connector's own checks: only a known component, only with a SEQPACKET channel on fd 3 */
static void test_connector_start(void) {
    const char *bad[] = { "evil", "terminal", "store", "", NULL };   /* terminal and store have no connector */
    for (int i = 0; bad[i]; i++) {
        pid_t p = fork();
        if (p == 0) {
            char *argv[] = { "lord-net-x", "--net-connector", (char *)bad[i], NULL };
            _exit(netconn_main(3, argv));
        }
        int st;
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
    }
    pid_t p = fork();
    if (p == 0) {
        close(3);
        int f = open("/dev/null", O_RDONLY);
        if (f != 3) dup2(f, 3);
        char *argv[] = { "lord-net-mail", "--net-connector", "mail", NULL };
        _exit(netconn_main(3, argv));
    }
    int st;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);                   /* fd 3 isn't a channel */
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--net-connector")) return netconn_main(argc, argv);
    test_requests();
    test_connector_start();
    unsetenv("LORDOS_SESSIOND");
    test_sockets(0);
    /* in a "session" the sockets come from real connector processes (this binary re-executed);
     * they can't be placed in a scope from here (no lord-sessiond for this process): with
     * LORD_NETCONN_TEST_PLACED unset the test checks that the request fails closed */
    setenv("LORDOS_SESSIOND", "1", 1);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int fd = netconn_socket(NC_MAIL, AF_INET, SOCK_STREAM, (struct sockaddr *)&a, sizeof a);
    if (fd >= 0) {
        /* placed after all (development cgroup delegation): the full path works */
        close(fd);
        test_sockets(1);
    } else {
        CHECK(errno == EPIPE || errno == ETIMEDOUT);
        printf("netconn-test: a connector that can't enter its scope refuses (fail closed): ok\n");
    }
    printf("netconn-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
