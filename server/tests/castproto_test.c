/* castproto_test.c — the ring and the channel between lord-server and lord-screencast (src/cast/castproto.c): the
 * ring's size limits and header, the slots a frame may use and who holds them, each side refusing what the other may
 * not say now (a slot it doesn't hold, a node twice, an answer nobody asked for, a frame of another size), the rate,
 * and the channel itself over a real socketpair (one message, at most one fd, nothing truncated). */
#include "cast/castproto.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int ok, bad;
#define CHECK(c) do { if (c) ok++; else { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static CastMsg msg(int type, int stream, uint32_t a) {
    CastMsg m;
    memset(&m, 0, sizeof m);
    m.type = (uint16_t)type;
    m.stream = (uint16_t)stream;
    m.a = a;
    return m;
}

static void t_layout(void) {
    CastLayout l;
    CHECK(cast_layout(1920, 1080, &l) == 0 && l.stride == 7680 && l.slot_bytes == 8294400 && l.slot_bytes % 4096 == 0);
    CHECK(l.total == CAST_RING_HDR + 3 * l.slot_bytes);
    CHECK(cast_layout(17, 17, &l) == 0 && l.slot_bytes == 4096);          /* rounded up to a page */
    CHECK(cast_slot_off(&l, 2) == CAST_RING_HDR + 2 * 4096);
    CHECK(cast_layout(15, 100, &l) == -1 && cast_layout(100, 15, &l) == -1);
    CHECK(cast_layout(CAST_MAX_DIM + 1, 100, &l) == -1 && cast_layout(CAST_MAX_DIM, CAST_MAX_DIM, &l) == 0);
    CHECK(cast_layout(0xffffffffu, 0xffffffffu, &l) == -1);

    /* the header the helper checks */
    CHECK(cast_layout(64, 32, &l) == 0);
    uint8_t *map = calloc(1, l.total);
    CastRingHdr h = { CAST_RING_MAGIC, 64, 32, l.stride, CAST_SLOTS, 0, l.slot_bytes };
    memcpy(map, &h, sizeof h);
    CHECK(cast_ring_ok(map, l.total, 64, 32) == 0);
    CHECK(cast_ring_ok(map, l.total - 1, 64, 32) == -1);                 /* shorter than the slots */
    CHECK(cast_ring_ok(map, l.total, 64, 33) == -1);                     /* not the size CM_STREAM said */
    h.slots = 2;
    memcpy(map, &h, sizeof h);
    CHECK(cast_ring_ok(map, l.total, 64, 32) == -1);
    h.slots = CAST_SLOTS; h.magic = 0;
    memcpy(map, &h, sizeof h);
    CHECK(cast_ring_ok(map, l.total, 64, 32) == -1);
    CHECK(cast_ring_ok(NULL, l.total, 64, 32) == -1);
    free(map);
}

static void t_helper_side(void) {
    CastPeer p;
    memset(&p, 0, sizeof p);
    p.nstreams = 2;
    CastMsg m = msg(CM_NODE, 0, 57);
    CHECK(cast_check_helper(&p, &m, 0) == 0 && p.node[0] == 57);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* a node twice */
    m = msg(CM_NODE, 1, 0xffffffffu);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* PW_ID_ANY */
    m = msg(CM_NODE, 1, 0);
    CHECK(cast_check_helper(&p, &m, 0) == -1);
    m = msg(CM_NODE, 2, 60);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* no such stream */
    m = msg(CM_NODE, 1, 58);
    CHECK(cast_check_helper(&p, &m, 1) == -1);                           /* with an fd */
    CHECK(cast_check_helper(&p, &m, 0) == 0);

    m = msg(CM_WANT, 0, 1); m.b = 30; m.c = 1;
    CHECK(cast_check_helper(&p, &m, 0) == 0);
    m.c = 0;
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* 30/0 */
    m = msg(CM_WANT, 0, 2);
    CHECK(cast_check_helper(&p, &m, 0) == -1);
    m = msg(CM_WANT, 0, 0);
    CHECK(cast_check_helper(&p, &m, 0) == 0);

    /* slots: taken in turn, each the helper's until its CM_DONE, which only frees a slot it holds */
    CHECK(cast_slot_take(&p, 0) == 0 && cast_slot_take(&p, 0) == 1 && cast_slot_take(&p, 0) == 2);
    CHECK(cast_slot_take(&p, 0) == -1);                                  /* the helper is behind: the frame waits */
    CHECK(cast_slot_take(&p, 1) == 0);                                   /* each stream has its own */
    CHECK(cast_slot_take(&p, 5) == -1);
    m = msg(CM_DONE, 0, 1);
    CHECK(cast_check_helper(&p, &m, 0) == 0 && !p.busy[0][1]);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* freed twice */
    CHECK(cast_slot_take(&p, 0) == 1);
    m = msg(CM_DONE, 1, 2);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* not held */
    m = msg(CM_DONE, 0, CAST_SLOTS);
    CHECK(cast_check_helper(&p, &m, 0) == -1);

    /* remotes: only an answer to a request that waits, once */
    CHECK(cast_remote_add(&p, 9) == 0 && cast_remote_add(&p, 9) == -1 && cast_remote_add(&p, 0) == -1);
    m = msg(CM_REMOTE, 0, 8);
    CHECK(cast_check_helper(&p, &m, 1) == -1);
    m = msg(CM_REMOTE, 0, 9);
    CHECK(cast_check_helper(&p, &m, 2) == -1);
    CHECK(cast_check_helper(&p, &m, 1) == 0);
    CHECK(cast_check_helper(&p, &m, 1) == -1);
    for (uint32_t i = 1; i <= CAST_MAX_REMOTES; i++) CHECK(cast_remote_add(&p, 100 + i) == 0);
    CHECK(cast_remote_add(&p, 200) == -1);
    m = msg(CM_REMOTE, 0, 101);
    CHECK(cast_check_helper(&p, &m, 0) == 0);                            /* it failed: no fd */

    m = msg(CM_FAIL, 0, CAST_E_STREAM);
    CHECK(cast_check_helper(&p, &m, 0) == 0);
    m = msg(CM_FRAME, 0, 0);
    CHECK(cast_check_helper(&p, &m, 0) == -1);                           /* lord-server's message */
    m = msg(99, 0, 0);
    CHECK(cast_check_helper(&p, &m, 0) == -1);
}

static void t_server_side(void) {
    CastSide x;
    memset(&x, 0, sizeof x);
    CastMsg m = msg(CM_STREAM, 0, 30);
    m.w = 640; m.h = 480; m.b = CAST_SRC_WINDOW;
    CHECK(cast_check_server(&x, &m, 1) == -1);                           /* before CM_HELLO */
    CastMsg hi = msg(CM_HELLO, 0, 2);
    CHECK(cast_check_server(&x, &hi, 0) == -1);                          /* with no connection */
    CHECK(cast_check_server(&x, &hi, 1) == 0 && x.nstreams == 2);
    CHECK(cast_check_server(&x, &hi, 1) == -1);                          /* once */
    CHECK(cast_check_server(&x, &m, 0) == -1);                           /* no ring */
    CHECK(cast_check_server(&x, &m, 1) == 0);
    m.b = 3;
    CHECK(cast_check_server(&x, &m, 1) == -1);
    m.b = CAST_SRC_MONITOR; m.a = CAST_MAX_FPS + 1;
    CHECK(cast_check_server(&x, &m, 1) == -1);
    m.a = 30; m.w = 8;
    CHECK(cast_check_server(&x, &m, 1) == -1);
    m.w = 640; m.stream = 2;
    CHECK(cast_check_server(&x, &m, 1) == -1);

    CastMsg f = msg(CM_FRAME, 0, 1);
    f.w = 640; f.h = 480;
    CHECK(cast_check_server(&x, &f, 0) == 0);
    f.h = 481;
    CHECK(cast_check_server(&x, &f, 0) == -1);                           /* not the stream's size */
    f.h = 480; f.a = CAST_SLOTS;
    CHECK(cast_check_server(&x, &f, 0) == -1);
    f.a = 0; f.stream = 1;
    CHECK(cast_check_server(&x, &f, 0) == -1);                           /* a stream with no ring yet */
    /* a resize: the stream again, the frames then at the new size */
    m = msg(CM_STREAM, 0, 30); m.w = 800; m.h = 600; m.b = CAST_SRC_WINDOW;
    CHECK(cast_check_server(&x, &m, 1) == 0);
    f = msg(CM_FRAME, 0, 0); f.w = 640; f.h = 480;
    CHECK(cast_check_server(&x, &f, 0) == -1);
    f.w = 800; f.h = 600;
    CHECK(cast_check_server(&x, &f, 0) == 0);

    CastMsg c = msg(CM_CLIENT, 0, 4);
    CHECK(cast_check_server(&x, &c, 0) == -1 && cast_check_server(&x, &c, 1) == 0);
    c.a = 0;
    CHECK(cast_check_server(&x, &c, 1) == -1);
    CastMsg st = msg(CM_STOP, 0, 0);
    CHECK(cast_check_server(&x, &st, 0) == 0 && cast_check_server(&x, &st, 1) == -1);
    CastMsg n = msg(CM_NODE, 0, 5);
    CHECK(cast_check_server(&x, &n, 0) == -1);                           /* the helper's message */
}

static void t_rate(void) {
    CHECK(cast_period_ms(30, 1) > 33.3 && cast_period_ms(30, 1) < 33.4);
    CHECK(cast_period_ms(0, 1) == 1000.0 / CAST_MAX_FPS);                /* variable: as fast as allowed */
    CHECK(cast_period_ms(240, 1) == 1000.0 / CAST_MAX_FPS);              /* capped */
    CHECK(cast_period_ms(1, 10) == 1000.0);                              /* at least one a second */
    CHECK(cast_period_ms(30000, 1001) > 33.3 && cast_period_ms(30000, 1001) < 33.4);
}

static void t_channel(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { CHECK(0); return; }
    CastMsg m = msg(CM_FRAME, 1, 2), got;
    m.w = 640; m.h = 480; m.seq = 77;
    int fd = -2;
    CHECK(cast_recv(sv[1], &got, &fd) == 0 && fd == -1);                 /* nothing yet */
    CHECK(cast_send(sv[0], &m, -1) == 0);
    CHECK(cast_recv(sv[1], &got, &fd) == 1 && fd == -1 && !memcmp(&got, &m, sizeof m));
    int pp[2];
    CHECK(pipe(pp) == 0);
    CHECK(cast_send(sv[0], &m, pp[1]) == 0);
    close(pp[1]);
    CHECK(cast_recv(sv[1], &got, &fd) == 1 && fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    if (fd >= 0) { CHECK(write(fd, "x", 1) == 1); close(fd); }
    char ch;
    CHECK(read(pp[0], &ch, 1) == 1 && read(pp[0], &ch, 1) == 0);
    close(pp[0]);
    /* a short packet, a long one, two fds: refused, and the fds closed */
    CHECK(send(sv[0], "abc", 3, 0) == 3);
    CHECK(cast_recv(sv[1], &got, &fd) == -1 && fd == -1);
    char big[sizeof m + 8];
    memset(big, 0, sizeof big);
    CHECK(send(sv[0], big, sizeof big, 0) == (ssize_t)sizeof big);
    CHECK(cast_recv(sv[1], &got, &fd) == -1);
    int p2[2], p3[2];
    CHECK(pipe(p2) == 0 && pipe(p3) == 0);
    {
        struct iovec io = { &m, sizeof m };
        union { struct cmsghdr h; char b[CMSG_SPACE(2 * sizeof(int))]; } cb;
        memset(&cb, 0, sizeof cb);
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb.b, .msg_controllen = sizeof cb.b };
        struct cmsghdr *h = CMSG_FIRSTHDR(&mh);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(2 * sizeof(int));
        int two[2] = { p2[1], p3[1] };
        memcpy(CMSG_DATA(h), two, sizeof two);
        CHECK(sendmsg(sv[0], &mh, 0) == (ssize_t)sizeof m);
    }
    close(p2[1]);
    close(p3[1]);
    CHECK(cast_recv(sv[1], &got, &fd) == -1 && fd == -1);
    CHECK(read(p2[0], &ch, 1) == 0 && read(p3[0], &ch, 1) == 0);         /* both closed by cast_recv */
    close(p2[0]);
    close(p3[0]);
    close(sv[0]);
    CHECK(cast_recv(sv[1], &got, &fd) == -1);                            /* the channel ended */
    close(sv[1]);
}

int main(void) {
    t_layout();
    t_helper_side();
    t_server_side();
    t_rate();
    t_channel();
    printf("castproto-test: %d/%d\n", ok, ok + bad);
    return bad ? 1 : 0;
}
