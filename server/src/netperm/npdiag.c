/* npdiag.c — the sock_diag client of axia-netperm (see npdiag.h). */
#include "netperm/npdiag.h"

#include <errno.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int npdiag_open(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) return -1;
    struct timeval tv = { 0, 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* The next netlink message in buf[0..len) at *off, or NULL (checked without the NLMSG_OK macro,
 * which compares signed and unsigned). */
static const struct nlmsghdr *next_msg(const char *buf, size_t len, size_t *off) {
    if (*off + sizeof(struct nlmsghdr) > len) return NULL;
    const struct nlmsghdr *h = (const struct nlmsghdr *)(const void *)(buf + *off);
    if (h->nlmsg_len < sizeof *h || *off + h->nlmsg_len > len) return NULL;
    *off += NLMSG_ALIGN(h->nlmsg_len);
    return h;
}

typedef struct {
    struct nlmsghdr nh;
    struct inet_diag_req_v2 r;
} DiagReq;

static int send_req(int fd, uint16_t type, uint16_t flags, const struct inet_diag_req_v2 *r, uint32_t seq) {
    DiagReq q;
    memset(&q, 0, sizeof q);
    q.nh.nlmsg_len = sizeof q;
    q.nh.nlmsg_type = type;
    q.nh.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags);
    q.nh.nlmsg_seq = seq;
    q.r = *r;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    ssize_t n = sendto(fd, &q, sizeof q, 0, (struct sockaddr *)&sa, sizeof sa);
    return n == (ssize_t)sizeof q ? 0 : -(errno ? errno : EIO);
}

static void from_msg(const struct inet_diag_msg *m, int proto, NpSock *s) {
    memset(s, 0, sizeof *s);
    s->fam = m->idiag_family;
    s->proto = proto;
    s->state = m->idiag_state;
    size_t al = m->idiag_family == AF_INET ? 4 : 16;
    memcpy(s->src, m->id.idiag_src, al);
    memcpy(s->dst, m->id.idiag_dst, al);
    s->sport = ntohs(m->id.idiag_sport);
    s->dport = ntohs(m->id.idiag_dport);
    s->ifindex = m->id.idiag_if;
    s->cookie[0] = m->id.idiag_cookie[0];
    s->cookie[1] = m->id.idiag_cookie[1];
}

#ifndef INET_DIAG_CGROUP_ID
#define INET_DIAG_CGROUP_ID 21          /* Linux 5.9 */
#endif

static uint64_t cgroup_attr(const struct nlmsghdr *h) {
    size_t off = NLMSG_LENGTH(sizeof(struct inet_diag_msg));
    off = NLMSG_ALIGN(off);
    while (off + sizeof(struct nlattr) <= h->nlmsg_len) {
        const struct nlattr *a = (const struct nlattr *)(const void *)((const char *)h + off);
        if (a->nla_len < sizeof *a || off + a->nla_len > h->nlmsg_len) break;
        if ((a->nla_type & NLA_TYPE_MASK) == INET_DIAG_CGROUP_ID && a->nla_len >= NLA_HDRLEN + 8) {
            uint64_t v;
            memcpy(&v, (const char *)a + NLA_HDRLEN, 8);
            return v;
        }
        off += NLA_ALIGN(a->nla_len);
    }
    return 0;
}

int npdiag_each(int fd, int fam, int proto, uint32_t states, int (*cb)(void *u, const NpSock *s), void *u) {
    static uint32_t seq;
    struct inet_diag_req_v2 r;
    memset(&r, 0, sizeof r);
    r.sdiag_family = (uint8_t)fam;
    r.sdiag_protocol = (uint8_t)proto;
    r.idiag_states = states;
    uint32_t my = ++seq;
    int rc = send_req(fd, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP, &r, my);
    if (rc) return rc;
    static char buf[32768] __attribute__((aligned(8)));
    int seen = 0, stop = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) return errno == EINTR ? -EINTR : -errno;
        if (n == 0) return -EIO;
        size_t off = 0;
        for (const struct nlmsghdr *h; (h = next_msg(buf, (size_t)n, &off));) {
            if (h->nlmsg_seq != my) continue;
            if (h->nlmsg_type == NLMSG_DONE) return seen;
            if (h->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *e = (const struct nlmsgerr *)NLMSG_DATA(h);
                return h->nlmsg_len >= NLMSG_LENGTH(sizeof *e) && e->error ? e->error : -EIO;
            }
            if (h->nlmsg_type != SOCK_DIAG_BY_FAMILY || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg)) || stop) continue;
            const struct inet_diag_msg *m = (const struct inet_diag_msg *)NLMSG_DATA(h);
            if (m->idiag_family != AF_INET && m->idiag_family != AF_INET6) continue;
            NpSock s;
            from_msg(m, proto, &s);
            s.cgid = cgroup_attr(h);
            seen++;
            if (cb(u, &s)) stop = 1;    /* keep reading to the end of the dump */
        }
    }
}

int npdiag_destroy(int fd, const NpSock *s) {
    static uint32_t seq = 1u << 30;
    struct inet_diag_req_v2 r;
    memset(&r, 0, sizeof r);
    r.sdiag_family = (uint8_t)s->fam;
    r.sdiag_protocol = (uint8_t)s->proto;
    r.idiag_states = ~0u;
    size_t al = s->fam == AF_INET ? 4 : 16;
    memcpy(r.id.idiag_src, s->src, al);
    memcpy(r.id.idiag_dst, s->dst, al);
    r.id.idiag_sport = htons(s->sport);
    r.id.idiag_dport = htons(s->dport);
    r.id.idiag_if = s->ifindex;
    r.id.idiag_cookie[0] = s->cookie[0];
    r.id.idiag_cookie[1] = s->cookie[1];
    uint32_t my = ++seq;
    int rc = send_req(fd, SOCK_DESTROY, NLM_F_ACK, &r, my);
    if (rc) return rc;
    char buf[1024] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) return errno == EINTR ? -EINTR : -errno;
        if (n <= 0) return -EIO;
        size_t off = 0;
        for (const struct nlmsghdr *h; (h = next_msg(buf, (size_t)n, &off));) {
            if (h->nlmsg_seq != my || h->nlmsg_type != NLMSG_ERROR) continue;
            const struct nlmsgerr *e = (const struct nlmsgerr *)NLMSG_DATA(h);
            return h->nlmsg_len >= NLMSG_LENGTH(sizeof *e) ? e->error : -EIO;
        }
    }
}
