/* rdesk_rdp_test.c — lord-rdesk with a server that offers only RDP's own security: no certificate, so no connection, and the
 * password is never sent. A stub answers the X.224 request; anything after that would be the session. */
#include "../src/rdesk/proto.h"

#include <freerdp/freerdp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int rdesk_rdp(int sock, const char *host, const char *user, const char *domain, const char *password, const RdView *view);
void rd_send(uint32_t type, int a, int b, int c, const char *text) { (void)type; (void)a; (void)b; (void)c; (void)text; }
uint32_t *rd_picture(int w, int h) { (void)w; (void)h; return NULL; }

static size_t after;
static unsigned requested;
static void *stub(void *arg) {
    int fd = *(int *)arg;
    uint8_t b[2048];
    ssize_t n = read(fd, b, sizeof b);
    /* the request's RDP_NEG_REQ, last 8 bytes: type 1, flags, length 8, requestedProtocols */
    if (n >= 8 && b[n - 8] == 1) requested = b[n - 4];
    static const uint8_t cc[] = { 3, 0, 0, 19, 14, 0xd0, 0, 0, 0x12, 0x34, 0, 2, 0, 8, 0, 0, 0, 0, 0 };   /* selected: PROTOCOL_RDP */
    if (write(fd, cc, sizeof cc) < 0) return NULL;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) { ssize_t r = read(fd, b, sizeof b); if (r <= 0) break; after += (size_t)r; }
    close(fd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* TCP on the loopback, as the helper's socket is */
    int sv[2], ch[2], l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1) || getsockname(l, (struct sockaddr *)&a, &al)) return 2;
    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sv[0], (struct sockaddr *)&a, sizeof a) || (sv[1] = accept(l, NULL, NULL)) < 0) return 2;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ch)) return 2;
    dup2(ch[0], 3);
    pthread_t th;
    pthread_create(&th, NULL, stub, &sv[1]);
    RdView view = { 0 };
    int rc = rdesk_rdp(sv[0], "h", "user", "", "hunter2", &view);
    close(sv[0]);
    pthread_join(th, NULL);
    int fails = 0;
    if (!rc) { printf("FAIL it connected\n"); fails++; }
    if (after) { printf("FAIL %zu bytes sent after the server chose PROTOCOL_RDP\n", after); fails++; }
    if (!(requested & 1) || !(requested & 2)) { printf("FAIL asked for 0x%x, not TLS and NLA\n", requested); fails++; }
    printf("rdesk-rdp-test: %d failed (requested 0x%x, rc %d)\n", fails, requested, rc);
    return fails != 0;
}
