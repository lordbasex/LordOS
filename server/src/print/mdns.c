/* mdns.c — see mdns.h. */
#include "mdns.h"
#ifndef MDNS_PARSE_ONLY
#include "../sec/netconn.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { T_A = MDNS_T_A, T_PTR = MDNS_T_PTR, T_TXT = MDNS_T_TXT, T_SRV = MDNS_T_SRV };

#ifndef MDNS_PARSE_ONLY
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#endif

/* DNS name with compression → "a.b.c" (without the final dot). first = first label as is.
 * Returns the position after the name (in the original place) or -1. */
static int read_name(const uint8_t *p, size_t len, size_t off, char *out, size_t n, char *first, size_t fn) {
    size_t o = 0, pos = off;
    int jumps = 0, end = -1, labels = 0;
    if (first && fn) first[0] = 0;
    for (;;) {
        if (pos >= len) return -1;
        uint8_t l = p[pos];
        if (l == 0) { if (end < 0) end = (int)pos + 1; break; }
        if ((l & 0xc0) == 0xc0) {
            if (pos + 1 >= len || ++jumps > 64) return -1;
            size_t to = (size_t)(l & 0x3f) << 8 | p[pos + 1];
            if (end < 0) end = (int)pos + 2;
            if (to >= len) return -1;
            pos = to;
            continue;
        }
        if (l & 0xc0) return -1;
        if (pos + 1 + l > len || ++labels > 64) return -1;
        if (first && labels == 1) {
            size_t k = l < fn - 1 ? l : fn - 1;
            memcpy(first, p + pos + 1, k);
            first[k] = 0;
            for (size_t i = 0; i < k; i++) if ((unsigned char)first[i] < 0x20) first[i] = ' ';
        }
        if (o + l + 2 > n) return -1;
        if (o) out[o++] = '.';
        for (size_t i = 0; i < l; i++) { char c = (char)p[pos + 1 + i]; out[o++] = (unsigned char)c < 0x20 ? ' ' : c; }
        pos += 1 + l;
    }
    out[o] = 0;
    return end;
}

int mdns_parse(const uint8_t *p, size_t len, MdnsCache *c) {
    if (len < 12) return -1;
    if (!(p[2] & 0x80)) return 0;                    /* a question, not an answer */
    int qd = p[4] << 8 | p[5], an = p[6] << 8 | p[7], ns = p[8] << 8 | p[9], ar = p[10] << 8 | p[11];
    size_t off = 12;
    char name[256], tmp[256];
    for (int i = 0; i < qd; i++) {
        int e = read_name(p, len, off, name, sizeof name, NULL, 0);
        if (e < 0 || (size_t)e + 4 > len) return -1;
        off = (size_t)e + 4;
    }
    int total = an + ns + ar;
    if (total > 400) return -1;
    for (int i = 0; i < total; i++) {
        int e = read_name(p, len, off, name, sizeof name, NULL, 0);
        if (e < 0 || (size_t)e + 10 > len) return -1;
        off = (size_t)e;
        int type = p[off] << 8 | p[off + 1];
        size_t rdlen = (size_t)(p[off + 8] << 8 | p[off + 9]);
        off += 10;
        if (off + rdlen > len) return -1;
        size_t rd = off;
        off += rdlen;
        if (type == T_PTR && c->nptr < MDNS_MAX) {
            char first[128];
            if (read_name(p, len, rd, tmp, sizeof tmp, first, sizeof first) < 0) continue;
            int dup = 0;
            for (int k = 0; k < c->nptr; k++) if (!strcasecmp(c->ptr[k].target, tmp)) dup = 1;
            if (dup) continue;
            snprintf(c->ptr[c->nptr].name, sizeof c->ptr[0].name, "%s", name);
            snprintf(c->ptr[c->nptr].target, sizeof c->ptr[0].target, "%s", tmp);
            snprintf(c->ptr[c->nptr].first, sizeof c->ptr[0].first, "%s", first);
            c->nptr++;
        } else if (type == T_SRV && rdlen >= 7 && c->nsrv < MDNS_MAX) {
            int port = p[rd + 4] << 8 | p[rd + 5];
            if (read_name(p, len, rd + 6, tmp, sizeof tmp, NULL, 0) < 0) continue;
            snprintf(c->srv[c->nsrv].name, sizeof c->srv[0].name, "%s", name);
            snprintf(c->srv[c->nsrv].host, sizeof c->srv[0].host, "%s", tmp);
            c->srv[c->nsrv].port = port;
            c->nsrv++;
        } else if (type == T_TXT && c->ntxt < MDNS_MAX) {
            char *t = c->txt[c->ntxt].txt;
            size_t o = 0, q = rd;
            while (q < rd + rdlen) {
                size_t l = p[q++];
                if (q + l > rd + rdlen) break;
                if (o + l + 2 < sizeof c->txt[0].txt) {
                    for (size_t k = 0; k < l; k++) { char ch = (char)p[q + k]; t[o++] = (unsigned char)ch < 0x20 ? ' ' : ch; }
                    t[o++] = '\n';
                }
                q += l;
            }
            t[o] = 0;
            snprintf(c->txt[c->ntxt].name, sizeof c->txt[0].name, "%s", name);
            c->ntxt++;
        } else if (type == T_A && rdlen == 4 && c->na < MDNS_MAX) {
            snprintf(c->a[c->na].host, sizeof c->a[0].host, "%s", name);
            inet_ntop(AF_INET, p + rd, c->a[c->na].addr, sizeof c->a[0].addr);
            c->na++;
        }
    }
    return 0;
}

static int put_name(uint8_t *q, size_t cap, size_t *o, const char *name) {
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (!l || l > 63 || *o + l + 2 > cap) return -1;
        q[(*o)++] = (uint8_t)l;
        memcpy(q + *o, s, l);
        *o += l;
        s += l + (dot ? 1 : 0);
    }
    if (*o + 1 > cap) return -1;
    q[(*o)++] = 0;
    return 0;
}

/* question (several questions in one packet) */
size_t mdns_query(uint8_t *q, size_t cap, const char *const *names, const int *types, int n, uint16_t id) {
    if (cap < 12) return 0;
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[5] = (uint8_t)n;
    size_t o = 12;
    for (int i = 0; i < n; i++) {
        if (put_name(q, cap, &o, names[i]) || o + 4 > cap) return 0;
        q[o++] = 0; q[o++] = (uint8_t)types[i];
        q[o++] = 0; q[o++] = 1;                     /* IN */
    }
    return o;
}

static const char *find_a(const MdnsCache *c, const char *host) {
    for (int i = 0; i < c->na; i++) if (!strcasecmp(c->a[i].host, host)) return c->a[i].addr;
    return NULL;
}

int mdns_collect(const MdnsCache *c, const char *const *types, int ntypes, MdnsSvc *out, int max) {
    int n = 0;
    for (int i = 0; i < c->nptr && n < max; i++) {
        const char *t = NULL;
        for (int k = 0; k < ntypes; k++) {
            char full[64];
            snprintf(full, sizeof full, "%s.local", types[k]);
            if (!strcasecmp(c->ptr[i].name, full)) t = types[k];
        }
        if (!t) continue;
        MdnsSvc *s = &out[n];
        memset(s, 0, sizeof *s);
        snprintf(s->type, sizeof s->type, "%s", t);
        snprintf(s->name, sizeof s->name, "%s", c->ptr[i].first);
        for (int k = 0; k < c->nsrv; k++)
            if (!strcasecmp(c->srv[k].name, c->ptr[i].target)) {
                snprintf(s->host, sizeof s->host, "%s", c->srv[k].host);
                s->port = c->srv[k].port;
            }
        for (int k = 0; k < c->ntxt; k++)
            if (!strcasecmp(c->txt[k].name, c->ptr[i].target)) snprintf(s->txt, sizeof s->txt, "%s", c->txt[k].txt);
        const char *a = s->host[0] ? find_a(c, s->host) : NULL;
        if (a) snprintf(s->addr, sizeof s->addr, "%s", a);
        if (s->port) n++;
    }
    return n;
}

const char *mdns_txt(const MdnsSvc *s, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = s->txt; *p;) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l > kl && p[kl] == '=' && !strncasecmp(p, key, kl)) {
            size_t v = l - kl - 1 < n - 1 ? l - kl - 1 : n - 1;
            memcpy(out, p + kl + 1, v);
            out[v] = 0;
            return out;
        }
        p += l + (nl ? 1 : 0);
    }
    return out;
}

#ifndef MDNS_PARSE_ONLY
int mdns_browse(const char *const *types, int ntypes, int ms, MdnsSvc *out, int max) {
    int fd = netconn_socket(NC_PRINT, AF_INET, SOCK_DGRAM, NULL, 0);   /* the printers' own (sec/netconn.h) */
    if (fd < 0) return 0;
    unsigned char ttl = 255, loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    MdnsCache *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return 0; }
    const char *names[8];
    int qt[8], nq = 0;
    char full[8][64];
    for (int i = 0; i < ntypes && i < 8; i++) {
        snprintf(full[i], sizeof full[i], "%s.local", types[i]);
        names[nq] = full[i];
        qt[nq++] = T_PTR;
    }
    uint8_t pkt[1500];
    uint16_t id = (uint16_t)(getpid() ^ (unsigned)time(NULL));
    double start = now_ms(), next_send = start;
    int sends = 0, followed = 0;
    while (now_ms() - start < ms) {
        if (now_ms() >= next_send && sends < 3) {
            size_t qn = mdns_query(pkt, sizeof pkt, names, qt, nq, id);
            if (qn) sendto(fd, pkt, qn, 0, (struct sockaddr *)&to, sizeof to);
            sends++;
            next_send = now_ms() + (sends == 1 ? 350 : 700);
        }
        /* halfway: the ones that still have no SRV or address are asked one by one */
        if (!followed && now_ms() - start > ms / 2) {
            followed = 1;
            const char *fn[16];
            int ft[16], nf = 0;
            static char hosts[16][256];
            for (int i = 0; i < c->nptr && nf < 14; i++) {
                int has = 0;
                for (int k = 0; k < c->nsrv; k++) if (!strcasecmp(c->srv[k].name, c->ptr[i].target)) {
                    has = 1;
                    if (!find_a(c, c->srv[k].host) && nf < 14) { snprintf(hosts[nf], sizeof hosts[0], "%s", c->srv[k].host); fn[nf] = hosts[nf]; ft[nf++] = T_A; }
                }
                int txt = 0;
                for (int k = 0; k < c->ntxt; k++) if (!strcasecmp(c->txt[k].name, c->ptr[i].target)) txt = 1;
                if (!has && nf < 14) { fn[nf] = c->ptr[i].target; ft[nf++] = T_SRV; }
                if (!txt && nf < 14) { fn[nf] = c->ptr[i].target; ft[nf++] = T_TXT; }
            }
            if (nf) { size_t qn = mdns_query(pkt, sizeof pkt, fn, ft, nf, id); if (qn) sendto(fd, pkt, qn, 0, (struct sockaddr *)&to, sizeof to); }
        }
        int left = (int)(ms - (now_ms() - start));
        int w = (int)(next_send - now_ms());
        if (sends >= 3 || w > left) w = left;
        if (!followed && w > (int)(start + ms / 2 - now_ms())) w = (int)(start + ms / 2 - now_ms()) + 1;
        if (w < 1) w = 1;
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, w) <= 0) continue;
        for (;;) {
            ssize_t r = recv(fd, pkt, sizeof pkt, 0);
            if (r <= 0) break;
            mdns_parse(pkt, (size_t)r, c);
        }
    }
    close(fd);
    int n = mdns_collect(c, types, ntypes, out, max);
    free(c);
    return n;
}
#endif
