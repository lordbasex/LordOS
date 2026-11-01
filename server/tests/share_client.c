/* share_client.c — a test client of the screen-sharing API and, as root, of remote.sock; tests/share_e2e.sh uses it.
 * A tool, not part of make test.
 *   share-client api    <sock> <script>   client of share.sock
 *   share-client mirror <sock> <script>   client of remote.sock
 *   share-client bridge <sock> <uid>      as root: the order lord-sessiond's bridge uses
 * A script is words, run in order; what arrives is counted and printed as "type count" at the end:
 *   start F C K | query | stop | lock | awake N | hear N | key CODE V | ptr X Y | btn B D | keyframe | rate K | resize W H
 *   text S | clip S | raw TYPE N (a message of N zero bytes) | junk (a wrong length)
 *   wait MS (reads for that long) | until TYPE MS (reads until that type arrives; exit 3 if it doesn't)
 *   none TYPE MS (exit 4 if that type arrives within MS) | reset (counters) | expect-end REASON MS | expect-ref REASON MS
 *   mirror only: seat | hello S | video C K | undisplay | display
 * The count of type 255 is the keyframes among RP_VFRAME.
 * "gone" as a word means: exit 0 only if the server closed the connection within 2 s. */
#include "remote/proto.h"
#include "remote/sharewire.h"

#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fd;
static long seen[256];
static long seen_bytes[256];
static int closed;
static uint8_t *in;
static size_t in_len;
static int last_reason = -1, last_type = -1;
static int cur_x = -1, cur_y = -1;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void out(int type, const void *d, size_t n) {
    uint8_t *b = malloc(5 + n);
    uint32_t l = (uint32_t)(1 + n);
    memcpy(b, &l, 4);
    b[4] = (uint8_t)type;
    if (n) memcpy(b + 5, d, n);
    if (send(fd, b, 5 + n, MSG_NOSIGNAL) < 0) closed = 1;
    free(b);
}

static void pump(int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    if (closed || poll(&p, 1, ms) <= 0) return;
    if (in_len > (64u << 20) - 65536) in_len = 0;
    ssize_t r = recv(fd, in + in_len, 65536, MSG_DONTWAIT);
    if (r < 0 && errno == EAGAIN) return;
    if (r <= 0) { closed = 1; return; }
    in_len += (size_t)r;
    size_t off = 0;
    while (in_len - off >= 5) {
        uint32_t l;
        memcpy(&l, in + off, 4);
        if (in_len - off < 4 + (size_t)l) break;
        int t = in[off + 4];
        seen[t]++;
        seen_bytes[t] += l - 1;
        if (t == SHARE_END || t == SHARE_REFUSED) { last_type = t; last_reason = in[off + 5]; }
        if (t == SHARE_STATE) { last_type = t; last_reason = in[off + 5]; }
        if (t == RP_VFRAME && l >= 3 && in[off + 6]) seen[255]++;       /* keyframes, counted apart */
        if (t == RP_CURSOR && l >= 6) { int16_t x, y; memcpy(&x, in + off + 5, 2); memcpy(&y, in + off + 7, 2); cur_x = x; cur_y = y; }
        off += 4 + (size_t)l;
    }
    memmove(in, in + off, in_len - off);
    in_len -= off;
}

static void wait_ms(int ms) {
    double end = now_ms() + ms;
    while (!closed) {
        double l = end - now_ms();
        if (l <= 0) break;
        pump(l > 50 ? 50 : (int)l + 1);
    }
}

static int until(int type, int ms) {
    double end = now_ms() + ms;
    long was = seen[type];
    while (seen[type] == was && now_ms() < end && !closed) pump(20);
    return seen[type] != was;
}

static int reason_of(const char *s) { for (int i = 0; i < SW_REASONS; i++) if (!strcmp(sw_reason_name(i), s)) return i; return -1; }

/* What lord-sessiond's bridge does: connect as root, then give root up and go on with the same socket. */
static int bridge(uid_t uid) {
    out(RP_HELLO, "127.0.0.1 test", 14);
    if (setgroups(0, NULL) || setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) return 6;
    uint8_t v = 0;
    out(RP_VMODE, &v, 1);
    wait_ms(300);
    if (closed || !seen[RP_SIZE]) { printf("the bridge's connection failed\n"); return 3; }
    printf("bridge: uid %u, connected\n", (unsigned)getuid());
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    in = malloc(64u << 20);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", argv[2]);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { printf("connect: %s\n", strerror(errno)); return 5; }
    if (!strcmp(argv[1], "bridge") && argc > 3) return bridge((uid_t)atoi(argv[3]));
    int i = 3;
#define W(n) (i + (n) < argc ? argv[i + (n)] : "0")
#define N(n) atoi(W(n))
    while (i < argc) {
        const char *w = argv[i];
        pump(0);
        if (!strcmp(w, "start")) {
            uint8_t m[8] = { (uint8_t)N(1), (uint8_t)N(2), 60, 0 };
            uint32_t k = (uint32_t)N(3);
            memcpy(m + 4, &k, 4);
            out(SHARE_START, m, 8);
            i += 4;
        } else if (!strcmp(w, "query")) { out(SHARE_QUERY, NULL, 0); i++; }
        else if (!strcmp(w, "stop")) { out(SHARE_STOP, NULL, 0); i++; }
        else if (!strcmp(w, "lock")) { out(SHARE_LOCK, NULL, 0); i++; }
        else if (!strcmp(w, "keyframe")) { out(RP_KEYFRAME, NULL, 0); i++; }
        else if (!strcmp(w, "awake") || !strcmp(w, "hear")) { uint8_t v = (uint8_t)N(1); out(w[0] == 'a' ? SHARE_AWAKE : SHARE_HEAR, &v, 1); i += 2; }
        else if (!strcmp(w, "key")) { uint8_t m[3] = { (uint8_t)N(1), (uint8_t)(N(1) >> 8), (uint8_t)N(2) }; out(RP_KEY, m, 3); i += 3; }
        else if (!strcmp(w, "ptr")) { int16_t p[2] = { (int16_t)N(1), (int16_t)N(2) }; out(RP_PTR, p, 4); i += 3; }
        else if (!strcmp(w, "btn")) { uint8_t m[2] = { (uint8_t)N(1), (uint8_t)N(2) }; out(RP_BTN, m, 2); i += 3; }
        else if (!strcmp(w, "rate")) { uint32_t k = (uint32_t)N(1); out(RP_RATE, &k, 4); i += 2; }
        else if (!strcmp(w, "resize")) { uint16_t s[2] = { (uint16_t)N(1), (uint16_t)N(2) }; out(RP_RESIZE, s, 4); i += 3; }
        else if (!strcmp(w, "text")) { out(SHARE_TEXT, W(1), strlen(W(1))); i += 2; }
        else if (!strcmp(w, "clip")) { out(RP_SETCLIP, W(1), strlen(W(1))); i += 2; }
        else if (!strcmp(w, "raw")) { static uint8_t z[64]; out(N(1), z, (size_t)N(2)); i += 3; }
        else if (!strcmp(w, "junk")) { uint8_t b[5] = { 0, 0, 0, 0, 0 }; if (send(fd, b, 5, MSG_NOSIGNAL) < 0) closed = 1; i++; }
        else if (!strcmp(w, "wait")) { wait_ms(N(1)); i += 2; }
        else if (!strcmp(w, "until")) { if (!until(N(1), N(2))) { printf("no message of type %d in %d ms\n", N(1), N(2)); return 3; } i += 3; }
        else if (!strcmp(w, "none")) {
            long was = seen[N(1)];
            wait_ms(N(2));
            if (seen[N(1)] != was) { printf("%ld messages of type %d arrived\n", seen[N(1)] - was, N(1)); return 4; }
            i += 3;
        } else if (!strcmp(w, "reset")) { memset(seen, 0, sizeof seen); memset(seen_bytes, 0, sizeof seen_bytes); i++; }
        else if (!strcmp(w, "expect-end") || !strcmp(w, "expect-ref")) {
            int t = w[7] == 'e' ? SHARE_END : SHARE_REFUSED, r = reason_of(W(1));
            double end = now_ms() + N(2);
            while (!(last_type == t && last_reason == r) && now_ms() < end && !closed) pump(20);
            if (last_type != t || last_reason != r) { printf("wanted %s %s; last message %d reason %s%s\n", w, W(1), last_type, sw_reason_name(last_reason), closed ? ", closed" : ""); return 3; }
            printf("%s %s\n", t == SHARE_END ? "end" : "refused", W(1));
            i += 3;
        } else if (!strcmp(w, "gone")) {
            double end = now_ms() + 2000;
            while (!closed && now_ms() < end) pump(20);
            printf(closed ? "closed\n" : "still open\n");
            return closed ? 0 : 4;
        } else if (!strcmp(w, "seat")) { out(RP_SEAT, NULL, 0); i++; }
        else if (!strcmp(w, "hello")) { out(RP_HELLO, W(1), strlen(W(1))); i += 2; }
        else if (!strcmp(w, "undisplay")) { out(RP_UNDISPLAY, NULL, 0); i++; }
        else if (!strcmp(w, "video")) {
            uint8_t m[8] = { (uint8_t)N(1), 0, 60, 0 };
            uint32_t k = (uint32_t)N(2);
            memcpy(m + 4, &k, 4);
            out(RP_VIDEO, m, 8);
            i += 3;
        } else if (!strcmp(w, "display")) {
            /* the seat lends a device: a dummy one, the card can't be used, and the session stays in memory */
            int d = open("/dev/null", O_RDWR);
            uint8_t hd[6] = { 2, 0, 0, 0, RP_DISPLAY, 1 };
            struct iovec iov = { hd, 6 };
            char cb[CMSG_SPACE(sizeof(int))];
            struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &d, sizeof d);
            if (sendmsg(fd, &mh, 0) < 0) perror("sendmsg");
            close(d);
            i++;
        } else if (!strcmp(w, "cursor")) { printf("cursor %d %d\n", cur_x, cur_y); i++; }
        else { printf("unknown word %s\n", w); return 2; }
    }
    wait_ms(50);
    for (int t = 0; t < 256; t++) if (seen[t]) printf("type %d: %ld (%ld bytes)\n", t, seen[t], seen_bytes[t]);
    if (closed) printf("closed by the server\n");
    return 0;
}
