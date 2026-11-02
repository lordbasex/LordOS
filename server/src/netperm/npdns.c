/* npdns.c — names from the system resolver (see npdns.h). */
#include "netperm/npdns.h"
#include "fw/rules.h"
#include "sec/json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MONITOR_SOCK "/run/systemd/resolve/io.systemd.Resolve.Monitor"
#define VL_MAX       (1u << 20)         /* a varlink message doesn't exceed 1 MB */
#define MAX_SERVERS  8

struct NpDns {
    NpDnsCb cb;
    void *u;
    int vfd;                    /* varlink with resolved */
    char *vbuf;
    size_t vlen;
    int pfd;                    /* AF_PACKET (without resolved) */
    NpAddr servers[MAX_SERVERS];
    int nservers;
    long long last_try, last_conf;
};

static long long mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* ---- building a DNS message from resolved's "raw" records ---------------------------------- */

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* strict base64 (with padding). Returns the bytes or -1. */
static long b64dec(const char *s, size_t n, uint8_t *out, size_t max) {
    if (n % 4) return -1;
    size_t w = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4], pad = 0;
        for (int k = 0; k < 4; k++) {
            if (s[i + k] == '=' && i + 4 == n && k >= 2) { v[k] = 0; pad++; continue; }
            if (pad || (v[k] = b64v((unsigned char)s[i + k])) < 0) return -1;
        }
        uint32_t x = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        uint8_t b[3] = { (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x };
        for (int k = 0; k < 3 - pad; k++) { if (w >= max) return -1; out[w++] = b[k]; }
    }
    return (long)w;
}

static int put_name(uint8_t *p, size_t max, const char *name) {
    char nm[FW_NAME_MAX];
    if (fw_norm_domain(name, nm)) return -1;
    size_t w = 0;
    for (const char *l = nm; *l;) {
        const char *dot = strchr(l, '.');
        size_t ll = dot ? (size_t)(dot - l) : strlen(l);
        if (!ll || ll > 63 || w + 1 + ll + 1 > max) return -1;
        p[w++] = (uint8_t)ll;
        memcpy(p + w, l, ll);
        w += ll;
        l += ll + (dot ? 1 : 0);
        if (!dot) break;
    }
    p[w++] = 0;
    return (int)w;
}

int npdns_from_varlink(const char *json, size_t len, NpDnsCb cb, void *u) {
    JDoc doc;
    if (json_parse(json, len, &doc, NULL, 0)) return -1;
    int made = 0;
    const JVal *par = json_get(doc.root, "parameters");
    const char *state = json_str(par, "state");
    const JVal *qs = json_get(par, "question"), *ans = json_get(par, "answer");
    if (!state || strcmp(state, "success") || !qs || qs->type != J_ARR || !ans || ans->type != J_ARR) goto out;
    uint8_t *pkt = malloc(65535);
    if (!pkt) { made = -1; goto out; }
    char done[4][FW_NAME_MAX];
    int ndone = 0;
    for (const JVal *q = qs->kid; q && ndone < 4; q = q->next) {
        const char *name = json_str(q, "name");
        char nm[FW_NAME_MAX];
        if (!name || fw_norm_domain(name, nm)) continue;
        int dup = 0;
        for (int i = 0; i < ndone; i++) if (!strcmp(done[i], nm)) dup = 1;
        if (dup) continue;
        memcpy(done[ndone++], nm, sizeof nm);
        /* header: response (QR, RD, RA), one question */
        memset(pkt, 0, 12);
        pkt[2] = 0x81; pkt[3] = 0x80; pkt[5] = 1;
        size_t w = 12;
        int nl = put_name(pkt + w, 65535 - w - 4, nm);
        if (nl < 0) continue;
        w += (size_t)nl;
        pkt[w++] = 0; pkt[w++] = 1; pkt[w++] = 0; pkt[w++] = 1;      /* type A, class IN (the type isn't checked) */
        unsigned an = 0;
        for (const JVal *a = ans->kid; a && an < 512; a = a->next) {
            const JVal *raw = json_get(a, "raw");
            if (!raw || raw->type != J_STR || raw->len > 4 * 1024) continue;
            long n = b64dec(raw->str, raw->len, pkt + w, 65535 - w);
            if (n < 11) continue;                       /* not even one record */
            w += (size_t)n;
            an++;
        }
        if (!an) continue;
        pkt[6] = (uint8_t)(an >> 8); pkt[7] = (uint8_t)an;
        cb(u, pkt, w);
        made++;
    }
    free(pkt);
out:
    json_free(&doc);
    return made;
}

/* ---- packets (without resolved) -------------------------------------------------------------- */

int npdns_from_packet(const uint8_t *p, size_t len, const NpAddr *servers, int nservers, const uint8_t **dns, size_t *dnslen) {
    if (len < 20) return -1;
    const uint8_t *src, *udp;
    size_t rest;
    int fam;
    if (p[0] >> 4 == 4) {
        size_t ihl = (size_t)(p[0] & 0xf) * 4, tot = (size_t)p[2] << 8 | p[3];
        if (ihl < 20 || tot < ihl || tot > len || p[9] != 17) return -1;
        if (((p[6] << 8 | p[7]) & 0x3fff) != 0) return -1;      /* fragments: no */
        fam = 4; src = p + 12; udp = p + ihl; rest = tot - ihl;
    } else if (p[0] >> 4 == 6) {
        if (len < 40) return -1;
        size_t pl = (size_t)p[4] << 8 | p[5];
        if (p[6] != 17 || 40 + pl > len) return -1;              /* no extension headers */
        fam = 6; src = p + 8; udp = p + 40; rest = pl;
    } else return -1;
    if (rest < 8 || (udp[0] << 8 | udp[1]) != 53) return -1;
    size_t ul = (size_t)udp[4] << 8 | udp[5];
    if (ul < 8 || ul > rest) return -1;
    int ok = 0;
    for (int i = 0; i < nservers && !ok; i++)
        ok = servers[i].fam == fam && !memcmp(servers[i].addr, src, fam == 4 ? 4 : 16);
    if (!ok) return -1;
    *dns = udp + 8;
    *dnslen = ul - 8;
    return 0;
}

static void read_resolv(NpDns *d) {
    d->nservers = 0;
    FILE *f = fopen("/etc/resolv.conf", "re");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && d->nservers < MAX_SERVERS) {
        char ip[128];
        if (sscanf(line, "nameserver %127s", ip) != 1) continue;
        char *pct = strchr(ip, '%');                    /* fe80::1%eth0 */
        if (pct) *pct = 0;
        NpAddr *a = &d->servers[d->nservers];
        memset(a, 0, sizeof *a);
        if (inet_pton(AF_INET, ip, a->addr) == 1) a->fam = 4;
        else if (inet_pton(AF_INET6, ip, a->addr) == 1) a->fam = 6;
        else continue;
        d->nservers++;
    }
    fclose(f);
}

static int open_packet(void) {
    int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_ALL));
    if (fd < 0) return -1;
    /* from the IP header: unfragmented IPv4 UDP with source port 53, or IPv6 UDP with 53 */
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 4, 0, 7),           /* → IPv6 */
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 9),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 17, 0, 11),         /* → no */
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 6),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x3fff, 9, 0),     /* fragment → no */
        BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 0),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 53, 5, 6),          /* → yes / no */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 6, 0, 5),           /* IPv6? */
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 6),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 17, 0, 3),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 40),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 53, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0xffff),                      /* yes */
        BPF_STMT(BPF_RET | BPF_K, 0),                           /* no */
    };
    struct sock_fprog prog = { (unsigned short)(sizeof code / sizeof code[0]), code };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog)) { close(fd); return -1; }
    return fd;
}

static int open_varlink(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", MONITOR_SOCK);
    static const char req[] = "{\"method\":\"io.systemd.Resolve.Monitor.SubscribeQueryResults\",\"more\":true}";
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) || send(fd, req, sizeof req, MSG_NOSIGNAL) != (ssize_t)sizeof req) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

NpDns *npdns_new(NpDnsCb cb, void *u) {
    NpDns *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cb = cb;
    d->u = u;
    d->vfd = d->pfd = -1;
    d->last_try = -100;
    npdns_tick(d);
    return d;
}

void npdns_free(NpDns *d) {
    if (!d) return;
    if (d->vfd >= 0) close(d->vfd);
    if (d->pfd >= 0) close(d->pfd);
    free(d->vbuf);
    free(d);
}

int npdns_fd(const NpDns *d) { return d->vfd >= 0 ? d->vfd : d->pfd; }

const char *npdns_source(const NpDns *d) { return d->vfd >= 0 ? "resolved" : d->pfd >= 0 ? "packet" : "none"; }

void npdns_tick(NpDns *d) {
    long long now = mono();
    struct stat st;
    int have_resolved = stat(MONITOR_SOCK, &st) == 0 && S_ISSOCK(st.st_mode);
    if (have_resolved) {
        if (d->vfd < 0 && now - d->last_try >= 5) {
            d->last_try = now;
            d->vfd = open_varlink();
            d->vlen = 0;
            if (d->vfd >= 0) fprintf(stderr, "axia-netperm: names from systemd-resolved (varlink monitor)\n");
        }
        if (d->vfd >= 0 && d->pfd >= 0) { close(d->pfd); d->pfd = -1; }
    } else {
        if (d->vfd >= 0) { close(d->vfd); d->vfd = -1; }
        if (d->pfd < 0 && now - d->last_try >= 5) {
            d->last_try = now;
            d->pfd = open_packet();
            if (d->pfd >= 0) fprintf(stderr, "axia-netperm: no systemd-resolved; names from the responses of the servers in /etc/resolv.conf\n");
            d->last_conf = -100;
        }
    }
    if (d->pfd >= 0 && now - d->last_conf >= 30) { d->last_conf = now; read_resolv(d); }
}

static void varlink_read(NpDns *d) {
    if (!d->vbuf && !(d->vbuf = malloc(VL_MAX))) return;
    for (;;) {
        if (d->vlen == VL_MAX) { d->vlen = 0; break; }         /* giant message: discarded */
        ssize_t n = recv(d->vfd, d->vbuf + d->vlen, VL_MAX - d->vlen, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            fprintf(stderr, "axia-netperm: the systemd-resolved monitor was cut off\n");
            close(d->vfd);
            d->vfd = -1;
            d->vlen = 0;
            return;
        }
        if (n < 0) return;
        d->vlen += (size_t)n;
        size_t start = 0;
        for (size_t i = d->vlen - (size_t)n; i < d->vlen; i++) {
            if (d->vbuf[i]) continue;
            if (i > start) npdns_from_varlink(d->vbuf + start, i - start, d->cb, d->u);
            start = i + 1;
        }
        memmove(d->vbuf, d->vbuf + start, d->vlen - start);
        d->vlen -= start;
    }
}

static void packet_read(NpDns *d) {
    uint8_t buf[65536];
    for (int k = 0; k < 256; k++) {
        struct sockaddr_ll sll;
        socklen_t sl = sizeof sll;
        ssize_t n = recvfrom(d->pfd, buf, sizeof buf, 0, (struct sockaddr *)&sll, &sl);
        if (n <= 0) return;
        if (sll.sll_pkttype == PACKET_OUTGOING) continue;       /* on lo every packet is seen twice */
        const uint8_t *dns;
        size_t dl;
        if (!npdns_from_packet(buf, (size_t)n, d->servers, d->nservers, &dns, &dl)) d->cb(d->u, dns, dl);
    }
}

void npdns_ready(NpDns *d) {
    if (d->vfd >= 0) varlink_read(d);
    else if (d->pfd >= 0) packet_read(d);
}
