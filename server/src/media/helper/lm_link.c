/* lm_link.c — lord-media's end of a live link (../live.h). The other end is a sandboxed program of a third party
 * that also reads from the network: every length is checked here, and a message out of bounds ends the link. */
#include "lm_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUT_CAP (2u << 20)              /* what waits for the companion; past it the message is dropped */

void lm_link_init(LmLink *l, int fd) {
    memset(l, 0, sizeof *l);
    l->fd = fd;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);    /* whatever the other side was given */
}

void lm_link_close(LmLink *l) {
    if (l->fd >= 0) close(l->fd);
    free(l->in);
    free(l->out);
    memset(l, 0, sizeof *l);
    l->fd = -1;
}

int lm_link_read(LmLink *l, LmLinkFn fn, void *arg) {
    for (int rounds = 0; rounds < 64; rounds++) {
        size_t want = sizeof(LiveHdr);
        if (l->in_len >= sizeof(LiveHdr)) {
            LiveHdr h;
            memcpy(&h, l->in, sizeof h);
            if (h.len > LIVE_MAX) return -1;
            want = sizeof h + h.len;
        }
        if (l->in_len >= want && l->in_len >= sizeof(LiveHdr)) {
            LiveHdr h;
            memcpy(&h, l->in, sizeof h);
            fn(&h, l->in + sizeof h, arg);
            l->in_len = 0;
            continue;
        }
        if (want > l->in_cap) {
            size_t cap = want < 65536 ? 65536 : want;
            uint8_t *p = realloc(l->in, cap);
            if (!p) return -1;
            l->in = p;
            l->in_cap = cap;
        }
        ssize_t n = read(l->fd, l->in + l->in_len, want - l->in_len);
        if (n == 0) return -1;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        l->in_len += (size_t)n;
    }
    return 0;
}

void lm_link_flush(LmLink *l) {
    while (l->out_len) {
        ssize_t n = write(l->fd, l->out, l->out_len);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return; }
        memmove(l->out, l->out + n, l->out_len - (size_t)n);
        l->out_len -= (size_t)n;
    }
}

int lm_link_send(LmLink *l, int type, int flags, int64_t pts, const void *data, size_t n) {
    LiveHdr h = { .len = (uint32_t)n, .type = (uint8_t)type, .flags = (uint8_t)flags, .pts = pts };
    if (l->fd < 0 || n > LIVE_MAX || l->out_len + sizeof h + n > OUT_CAP) return -1;
    if (!l->out && !(l->out = malloc(OUT_CAP))) return -1;
    memcpy(l->out + l->out_len, &h, sizeof h);
    if (n) memcpy(l->out + l->out_len + sizeof h, data, n);
    l->out_len += sizeof h + n;
    lm_link_flush(l);
    return 0;
}

short lm_link_events(const LmLink *l) { return (short)(0x001 /* POLLIN */ | (l->out_len ? 0x004 /* POLLOUT */ : 0)); }
