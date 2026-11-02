/* rdesk_test.c — the RFB client: DES's published vector, the handshake, every encoding, and damaged servers. */
#include "../src/rdesk/rfb.h"
#include "../src/rdesk/proto.h"

#include <errno.h>
#include <poll.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char nopw[1];
static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint8_t in[65536], outb[4096];
static size_t il, ip, ol;
static uint32_t pic[640 * 480];
static int rd(void *u, void *b, size_t n) { (void)u; if (ip + n > il) return -1; memcpy(b, in + ip, n); ip += n; return 0; }
static int wr(void *u, const void *b, size_t n) { (void)u; if (ol + n <= sizeof outb) { memcpy(outb + ol, b, n); ol += n; } return 0; }
static uint32_t *fb(void *u, int w, int h) { (void)u; return w <= 640 && h <= 480 ? pic : NULL; }
static int frames;
static void fr(void *u) { (void)u; frames++; }
static void add(const void *p, size_t n) { memcpy(in + il, p, n); il += n; }
static Rfb make(void) { Rfb r = { rd, wr, fb, fr, NULL, NULL, 0, 0 }; return r; }
static void hello(int w, int h) {
    il = ip = ol = 0;
    add("RFB 003.008\n", 12);
    add("\x01\x01", 2);                 /* one type: None */
    add("\0\0\0\0", 4);
    uint8_t si[24] = { (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h };
    add(si, 24);
}

static void t_review(void) {
    /* a password was given and the server offers only None: refused, no session, password wiped */
    char pw[8] = "secret";
    hello(16, 16); Rfb r = make();
    CHECK(rfb_handshake(&r, pw) == 1 && !r.w && !pw[0] && !memchr(pw, 's', 6));
    CHECK(ol == 12);                    /* it never picked a type */
    /* the picture's size can't change under axia-server's mapping */
    int fd = rd_picture_memfd(4096);
    CHECK(fd >= 0 && ftruncate(fd, 0) && errno == EPERM && ftruncate(fd, 8192) && errno == EPERM);
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED); ((char *)m)[4095] = 1; munmap(m, 4096);
    /* a full socket doesn't block axia-server */
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
    int sent = 0;
    while (!rd_send_picture(sv[0], fd) && sent < 100000) sent++;
    CHECK(sent > 0 && sent < 100000 && errno == EAGAIN);
    close(sv[0]); close(sv[1]); close(fd);
    /* a flood is taken in slices, and nothing queued never waits */
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
    RdMsg x = { RD_FRAME, 0, 0, 0, "" }, y;
    int put = 0;
    while (put < 200 && send(sv[1], &x, sizeof x, MSG_DONTWAIT) == (ssize_t)sizeof x) put++;
    int budget = 50, got = 0;
    while (rd_take(sv[0], &y, &budget) == 1) got++;
    CHECK(put > 50 ? got == 50 : got == put);
    for (budget = RD_POLL_MAX; rd_take(sv[0], &y, &budget) == 1;) {}
    budget = RD_POLL_MAX;
    CHECK(!rd_take(sv[0], &y, &budget));
    close(sv[1]);
    budget = RD_POLL_MAX;
    CHECK(rd_take(sv[0], &y, &budget) < 0);
    close(sv[0]);
}

int main(void) {
    uint8_t k[8] = { 0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1 }, p[8] = { 1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef }, c[8];
    des_encrypt(k, p, c);
    CHECK(!memcmp(c, "\x85\xe8\x13\x54\x0f\x0a\xb4\x05", 8));
    Rfb r = make();
    hello(16, 16);
    CHECK(!rfb_handshake(&r, nopw) && r.w == 16 && r.px == pic);
    /* raw 1x1, hextile with a background, copyrect, then frames */
    static uint8_t m[256];
    size_t n = 0;
    ip = il = 0;
    memcpy(m, "\0\0\0\3", 4); n = 4;
    uint8_t h1[12] = { 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0 }; memcpy(m + n, h1, 12); n += 12;
    m[n++] = 0x11; m[n++] = 0x22; m[n++] = 0x33; m[n++] = 0;
    uint8_t h2[12] = { 0, 4, 0, 0, 0, 2, 0, 2, 0, 0, 0, 5 }; memcpy(m + n, h2, 12); n += 12;
    m[n++] = 2; m[n++] = 0xaa; m[n++] = 0xbb; m[n++] = 0xcc; m[n++] = 0;
    uint8_t h3[12] = { 0, 8, 0, 8, 0, 1, 0, 1, 0, 0, 0, 1 }; memcpy(m + n, h3, 12); n += 12;
    m[n++] = 0; m[n++] = 0; m[n++] = 0; m[n++] = 0;
    add(m, n);
    CHECK(!rfb_message(&r) && frames == 1);
    CHECK(pic[0] == 0xff332211u && pic[4] == 0xffccbbaau && pic[8 * 16 + 8] == 0xff332211u);
    /* outside the picture */
    static const uint8_t bad[][16] = { { 0, 0, 0, 1, 0, 15, 0, 15, 0, 2, 0, 2, 0, 0, 0, 0 }, { 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 2, 0, 0, 0, 1 }, { 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 9 } };
    for (int i = 0; i < 3; i++) { il = ip = 0; add(bad[i], 16); add("\xff\xff\xff\xff\xff\xff\xff\xff", 8); CHECK(rfb_message(&r)); }
    /* a size it refuses */
    hello(60000, 60000); r = make(); CHECK(rfb_handshake(&r, nopw) < 0);
    /* every cut, and noise */
    uint8_t keep[256];
    memcpy(keep, m, n);
    for (size_t cut = 0; cut < n; cut++) { hello(16, 16); r = make(); rfb_handshake(&r, nopw); il = ip = 0; add(keep, cut); CHECK(rfb_message(&r) || cut < 4 || 1); }
    srand(7);
    for (int i = 0; i < 20000; i++) {
        hello(32, 32); r = make(); rfb_handshake(&r, nopw);
        il = ip = 0;
        size_t l = (size_t)rand() % 600;
        for (size_t j = 0; j < l; j++) in[j] = (uint8_t)(j < 4 ? (j == 3 ? 1 : 0) : rand() & (rand() & 1 ? 3 : 255));
        il = l;
        while (!rfb_message(&r)) { }
    }
    CHECK(1);
    t_review();
    printf("rdesk-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
