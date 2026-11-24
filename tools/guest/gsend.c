/* gsend.c — "Send to LordOS": files from this guest to the host (see gsend.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gsend.h"
#include "gshell.h"
#include "../../server/src/vm/vmagent.h"
#include "../../server/src/pkg/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { GS_IDLE = 0, GS_OFFERED, GS_HASHING, GS_SENDING, GS_ENDED };
#define GS_RETRY_MS 5000
#define GS_GIVE_UP_MS 600000
#define GS_HASH_STEP (8u << 20)             /* a resume's prefix hashed this much per turn: the loop stays responsive */
#define GS_SEND_STEP 16                     /* data frames per turn */

static char *q[GSEND_QUEUE];
static int qn;
static int st, host_takes, fd = -1;
static uint32_t id, next_id;
static uint64_t size, sent, acked, prefix, hashed;
static uint8_t key[32], psha[32];
static Sha256 hs;
static long long retry_at, first_at;

static long long mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int send_frame(int port, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, n);
    struct iovec io[2] = { { h, AG_HEAD }, { (void *)p, n } };
    return writev(port, io, n ? 2 : 1) == (ssize_t)(AG_HEAD + n) ? 0 : -1;
}

static void close_cur(void) {
    if (fd >= 0) close(fd);
    fd = -1;
    st = GS_IDLE;
    id = 0;
}

/* the head of the queue goes (sent, refused, given up) */
static void drop_head(const char *why) {
    if (!qn) return;
    if (why) fprintf(stderr, "lordos-guest: %s: %s\n", q[0], why);
    free(q[0]);
    memmove(q, q + 1, (size_t)(qn - 1) * sizeof *q);
    qn--;
    close_cur();
    first_at = 0;
    retry_at = 0;
}

int gsend_queue(const char *path) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return 1;
    struct stat s;
    const char *base = strrchr(path, '/') + 1;
    if (lstat(path, &s) || !S_ISREG(s.st_mode) || s.st_uid != getuid() || !ag_send_name_ok((const uint8_t *)base, strlen(base))) return 1;
    for (int i = 0; i < qn; i++) if (!strcmp(q[i], path)) return 0;          /* already on its way */
    if (qn >= GSEND_QUEUE) return 2;
    if (!(q[qn] = strdup(path))) return 2;
    qn++;
    return 0;
}

/* the head of the queue opened and offered */
static void offer(int port) {
    while (qn && st == GS_IDLE) {
        int f = open(q[0], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        struct stat s;
        const char *base = strrchr(q[0], '/') + 1;
        if (f < 0 || fstat(f, &s) || !S_ISREG(s.st_mode) || s.st_uid != getuid() || (uint64_t)s.st_size > AG_SEND_SIZE_MAX) {
            if (f >= 0) close(f);
            drop_head("not a file this computer sends (a link, a folder, someone else's, too large)");
            continue;
        }
        fcntl(f, F_SETFL, 0);
        AgSendOffer o;
        memset(&o, 0, sizeof o);
        o.id = ++next_id ? next_id : ++next_id;
        o.size = (uint64_t)s.st_size;
        o.flags = s.st_mode & 0111 ? AGSF_EXEC : 0;
        snprintf(o.name, sizeof o.name, "%s", base);
        /* the resume key: this very file, as long as it doesn't change */
        Sha256 k;
        sha256_init(&k);
        sha256_update(&k, q[0], strlen(q[0]) + 1);
        uint64_t v[5] = { (uint64_t)s.st_dev, (uint64_t)s.st_ino, (uint64_t)s.st_size, (uint64_t)s.st_ctim.tv_sec, (uint64_t)s.st_ctim.tv_nsec };
        sha256_update(&k, v, sizeof v);
        sha256_final(&k, o.key);
        uint8_t b[AG_SEND_OFFER_BYTES];
        size_t n = ag_send_offer_write(&o, b, sizeof b);
        if (!n) { close(f); drop_head("its name can't go to LordOS"); continue; }
        if (send_frame(port, AGK_SEND_OFFER, b, (uint32_t)n)) { close(f); return; }
        fd = f; id = o.id; size = o.size; memcpy(key, o.key, 32);
        sent = acked = prefix = hashed = 0;
        st = GS_OFFERED;
        if (!first_at) first_at = mono();
    }
}

static void begin_from(int port, uint64_t off) {
    uint8_t b[12];
    ag_send_pos_write(b, id, off);
    if (send_frame(port, AGK_SEND_FROM, b, 12)) return;
    sent = acked = off;
    st = GS_SENDING;
}

int gsend_frame(int port, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint32_t i;
    if (kind == AGK_SEND_ACCEPT) {
        uint8_t status, sha[32];
        uint64_t off;
        if (ag_send_accept_parse(p, n, &i, &status, &off, sha) || st != GS_OFFERED || i != id) return 1;
        if (status == AGSA_BUSY) {
            close_cur();
            if (mono() - first_at > GS_GIVE_UP_MS) drop_head("LordOS didn't take it for 10 minutes (is the machine's window open?)");
            else retry_at = mono() + GS_RETRY_MS;
            return 1;
        }
        if (status != AGSA_OK) {
            drop_head(status == AGSA_OFF ? "this machine doesn't take files (Machines: Configure, Sharing)" : status == AGSA_SPACE ? "no room on LordOS" : "refused by LordOS");
            return 1;
        }
        sha256_init(&hs);
        if (!off || off > size) { begin_from(port, 0); return 1; }
        prefix = off; hashed = 0;
        memcpy(psha, sha, 32);
        st = GS_HASHING;
        return 1;
    }
    if (kind == AGK_SEND_ACK) {
        uint64_t off;
        if (ag_send_pos_parse(p, n, &i, &off) || i != id || (st != GS_SENDING && st != GS_ENDED)) return 1;
        if (off >= acked && off <= sent) acked = off;
        return 1;
    }
    if (kind == AGK_SEND_DONE) {
        uint8_t code;
        if (ag_send_done_parse(p, n, &i, &code) || !id || i != id) return 1;
        if (code == AGSD_OK) { fprintf(stderr, "lordos-guest: sent to LordOS: %s\n", qn ? q[0] : "?"); drop_head(NULL); return 1; }
        if (code == AGSD_LATER) { close_cur(); retry_at = mono() + GS_RETRY_MS; return 1; }
        drop_head(code == AGSD_CANCELLED ? "cancelled on LordOS" : code == AGSD_BADHASH ? "it changed while it was sent" : code == AGSD_SPACE ? "no room on LordOS" : "LordOS couldn't keep it");
        return 1;
    }
    return 0;
}

void gsend_host(int port, int takes) {
    (void)port;
    host_takes = takes;
    if (!takes && st != GS_IDLE) close_cur();
}

void gsend_port_lost(void) {
    if (st != GS_IDLE) close_cur();          /* still first in the queue: offered again with the same key, a resume */
    host_takes = 0;
}

int gsend_pump(int port) {
    if (port < 0 || !host_takes) return -1;
    long long now = mono();
    if (st == GS_IDLE) {
        if (!qn) return -1;
        if (now < retry_at) return (int)(retry_at - now);
        offer(port);
        return -1;
    }
    static uint8_t buf[12 + AG_SEND_CHUNK];
    if (st == GS_HASHING) {
        /* this file's first bytes, against the host's: the same, go on from there; else from the start */
        uint64_t stop = hashed + GS_HASH_STEP < prefix ? hashed + GS_HASH_STEP : prefix;
        while (hashed < stop) {
            size_t want = stop - hashed < AG_SEND_CHUNK ? (size_t)(stop - hashed) : AG_SEND_CHUNK;
            ssize_t r = pread(fd, buf, want, (off_t)hashed);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { sha256_init(&hs); begin_from(port, 0); return 0; }
            sha256_update(&hs, buf, (size_t)r);
            hashed += (uint64_t)r;
        }
        if (hashed < prefix) return 0;
        Sha256 c = hs;
        uint8_t d[32];
        sha256_final(&c, d);
        if (memcmp(d, psha, 32)) { sha256_init(&hs); begin_from(port, 0); }
        else begin_from(port, prefix);
        return 0;
    }
    if (st == GS_SENDING) {
        for (int k = 0; k < GS_SEND_STEP && sent < size; k++) {
            uint32_t want = size - sent < AG_SEND_CHUNK ? (uint32_t)(size - sent) : AG_SEND_CHUNK;
            if (!ag_send_room(sent, acked, want)) return -1;                 /* the host's ACK opens it again */
            ssize_t r = pread(fd, buf + 12, want, (off_t)sent);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                uint8_t d[5];
                ag_send_done_write(d, id, AGSD_FAILED);
                send_frame(port, AGK_SEND_DONE, d, 5);
                drop_head("it couldn't be read (did it get shorter?)");
                return 0;
            }
            ag_send_data_head(buf, id, sent);
            if (send_frame(port, AGK_SEND_DATA, buf, (uint32_t)(12 + r))) return 100;
            sha256_update(&hs, buf + 12, (size_t)r);
            sent += (uint64_t)r;
        }
        if (sent < size) return 0;
        uint8_t e[36], d[32];
        sha256_final(&hs, d);
        ag_send_end_write(e, id, d);
        if (!send_frame(port, AGK_SEND_END, e, 36)) st = GS_ENDED;
        return -1;
    }
    return -1;
}

/* ---- the command line ---- */

int gsend_cli(int argc, char **argv) {
    char sp[256];
    if (gshell_sock_path(sp, sizeof sp)) { fprintf(stderr, "lordos-guest: no XDG_RUNTIME_DIR\n"); return 1; }
    int worst = 0;
    for (int i = 0; i < argc; i++) {
        char real[PATH_MAX];
        if (!realpath(argv[i], real)) { fprintf(stderr, "lordos-guest: %s: %s\n", argv[i], strerror(errno)); worst = worst ? worst : 2; continue; }
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        if (strlen(sp) >= sizeof sa.sun_path) return 1;
        memcpy(sa.sun_path, sp, strlen(sp) + 1);
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa)) { fprintf(stderr, "lordos-guest: the LordOS tools aren't running in this session\n"); if (s >= 0) close(s); return 1; }
        uint8_t ans = GSA_NOHOST;
        if (!send_frame(s, AGK_SEND_OFFER, (const uint8_t *)real, (uint32_t)strlen(real))) {
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, 3000) <= 0 || read(s, &ans, 1) != 1) ans = GSA_NOHOST;
        }
        close(s);
        switch (ans) {
        case GSA_SENT: break;
        case GSA_REFUSED: fprintf(stderr, "lordos-guest: this machine doesn't take files (Machines: Configure, Sharing)\n"); return 3;
        case GSA_MALFORMED: fprintf(stderr, "lordos-guest: %s: only your own files (not folders or links) go to LordOS\n", argv[i]); worst = 2; break;
        case GSA_FULL: fprintf(stderr, "lordos-guest: too many files wait to go: try again later\n"); return 1;
        default: fprintf(stderr, "lordos-guest: not connected to LordOS\n"); return 1;
        }
    }
    return worst;
}
