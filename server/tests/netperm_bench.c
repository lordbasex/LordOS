/* netperm_bench.c — how much axia-netperm adds to connect() (docs/FIREWALL.md, "Measurements").
 *
 *   cc -O2 -o netperm-bench tests/netperm_bench.c
 *   ./netperm-bench [times]            (runs inside an app's scope, see netperm_measure.sh)
 *
 * Opens a TCP server on 127.0.0.1 and does socket + connect + close N times, and the same with UDP
 * (socket + connect + close, which goes through the connect hook but sends nothing). Prints the
 * median and the 99th percentile of each connect() in microseconds. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, double *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    printf("%s: median %.2f us, p99 %.2f us, mean %.2f us (%d)\n", what, v[n / 2], v[n * 99 / 100], sum / n, n);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 20000;
    if (n < 100 || n > 1000000) n = 20000;
    double *v = malloc((size_t)n * sizeof *v);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (!v || ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4096)) { perror("servidor"); return 1; }
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    for (int i = 0; i < n; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        double t0 = now_us();
        int r = connect(s, (struct sockaddr *)&a, sizeof a);
        v[i] = now_us() - t0;
        if (r) { perror("connect tcp"); return 1; }
        close(s);
        int c = accept(ls, NULL, NULL);
        if (c >= 0) close(c);
    }
    report("connect TCP 127.0.0.1", v, n);
    for (int i = 0; i < n; i++) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        double t0 = now_us();
        int r = connect(s, (struct sockaddr *)&a, sizeof a);
        v[i] = now_us() - t0;
        if (r) { perror("connect udp"); return 1; }
        close(s);
    }
    report("connect UDP 127.0.0.1", v, n);
    return 0;
}
