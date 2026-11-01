/* rdeskhelper.c — lord-rdesk: one remote desktop connection. It runs in a scope of its own (firewall app
 * desktop:lordos-rdesk), connects, drops into the helper sandbox, and only ever draws into the shared picture. The
 * engine shows that picture; there is no window, toolkit or display connection here. */
#include "proto.h"
#include "rfb.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define CH 3

int rdesk_rdp(int sock, const char *host, const char *user, const char *domain, const char *password);   /* rdeskrdp.c */

void rd_send(uint32_t type, int a, int b, int c, const char *text) {
    RdMsg m = { type, a, b, c, "" };
    if (text) snprintf(m.text, sizeof m.text, "%s", text);
    send(CH, &m, sizeof m, MSG_NOSIGNAL);
}

/* the picture for this size: asks lord-server and waits for the memfd */
uint32_t *rd_picture(int w, int h) {
    static uint32_t *map;
    static size_t len;
    rd_send(RD_SIZE, w, h, 0, NULL);
    for (;;) {
        RdMsg m;
        int fd = -1;
        struct iovec io = { &m, sizeof m };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
        if (recvmsg(CH, &mh, 0) != (ssize_t)sizeof m) return NULL;
        if (m.type == RD_CLOSE) return NULL;
        if (m.type != RD_SHM) continue;          /* input from before the picture existed */
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) if (c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
        if (fd < 0) return NULL;
        if (map) munmap(map, len);
        len = (size_t)w * h * 4;
        map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        return map == MAP_FAILED ? NULL : map;
    }
}

/* ---- VNC ---- */

static int rd_all(void *ud, void *buf, size_t n) {
    int fd = *(int *)ud;
    char *p = buf;
    while (n) { ssize_t r = read(fd, p, n); if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; } p += r; n -= (size_t)r; }
    return 0;
}
static int wr_all(void *ud, const void *buf, size_t n) { int fd = *(int *)ud; const char *p = buf; while (n) { ssize_t r = send(fd, p, n, MSG_NOSIGNAL); if (r <= 0) return -1; p += r; n -= (size_t)r; } return 0; }
static uint32_t *vnc_fb(void *ud, int w, int h) { (void)ud; return rd_picture(w, h); }
static void vnc_frame(void *ud) { (void)ud; rd_send(RD_FRAME, 0, 0, 0, NULL); }

static int run_vnc(int sock, char *password) {
    Rfb r = { rd_all, wr_all, vnc_fb, vnc_frame, &sock, NULL, 0, 0 };
    int h = rfb_handshake(&r, password);
    if (h) { rd_send(RD_STATE, h > 0 ? RS_DENIED : RS_FAILED, 0, 0, NULL); return 1; }
    rd_send(RD_STATE, RS_CONNECTED, 0, 0, NULL);
    if (rfb_update_request(&r, 0)) return 1;
    struct pollfd p[2] = { { sock, POLLIN, 0 }, { CH, POLLIN, 0 } };
    for (;;) {
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; return 1; }
        if (p[0].revents && rfb_message(&r)) break;
        if (p[1].revents & (POLLHUP | POLLERR)) return 0;
        if (p[1].revents & POLLIN) {
            RdMsg m;
            if (recv(CH, &m, sizeof m, 0) != (ssize_t)sizeof m || m.type == RD_CLOSE) return 0;
            if (m.type == RD_KEY) rfb_key(&r, (uint32_t)m.a, m.c);
            else if (m.type == RD_PTR) rfb_pointer(&r, m.a, m.b, m.c);
        }
    }
    rd_send(RD_STATE, RS_ENDED, 0, 0, NULL);
    return 0;
}

static int dial(const char *host, int port) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res)) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        struct timeval tv = { 8, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); struct timeval no = { 0, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &no, sizeof no); }
    return fd;
}

int main(void) {
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGPIPE, SIG_IGN);
    static char job[2048];
    ssize_t n = recv(CH, job, sizeof job - 1, 0);
    if (n <= 0) return 2;
    JDoc d;
    if (json_parse(job, (size_t)n, &d, NULL, 0)) return 2;
    const char *proto = json_str(d.root, "proto"), *host = json_str(d.root, "host"), *user = json_str(d.root, "user"), *dom = json_str(d.root, "domain"), *pw = json_str(d.root, "password");
    int port = (int)json_num(d.root, "port", 0);
    if (!proto || !host || !user || !dom || !pw) return 2;
    int rdp = !strcmp(proto, "rdp");
    int sock = dial(host, port ? port : rdp ? 3389 : 5900);
    if (sock < 0) { rd_send(RD_STATE, RS_UNREACHABLE, 0, 0, NULL); return 1; }
    /* it stays in the scope lord-server placed it in, with that scope's memory limits. connected: no new sockets and no files from here on. RDP's library keeps /etc/ssl and its own crypto config */
    static const char *const ro[] = { "/etc/ssl", "/usr/lib", "/usr/share/ca-certificates", NULL };
    SandboxHelperCfg cfg = { .name = NULL, .ro_paths = rdp ? ro : NULL, .extra_syscalls = (int[]){ -1 }, .allow_ioctl_classes = 0, .ro_dir_fds = NULL, .rw_dir_fds = NULL };
    sandbox_apply_helper(&cfg);
    int rc = rdp ? rdesk_rdp(sock, host, user, dom, pw) : run_vnc(sock, (char *)pw);
    explicit_bzero((char *)pw, strlen(pw));
    json_free(&d);
    explicit_bzero(job, sizeof job);
    return rc;
}
