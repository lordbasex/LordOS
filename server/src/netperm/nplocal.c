/* nplocal.c — this computer's own networks, for np_local (see nplocal.h). */
#include "netperm/nplocal.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* one network, masked to its bits; 0 added or already there, -1 no room */
static int put(NpLocalKey *out, int max, int *n, int fam, const unsigned char *addr, int bits) {
    int all = fam == 4 ? 32 : 128;
    if (bits < 0 || bits > all) return 0;
    NpLocalKey k;
    memset(&k, 0, sizeof k);
    k.prefixlen = (np_u32)(8 + bits);
    k.data[0] = (np_u8)fam;
    memcpy(k.data + 1, addr, fam == 4 ? 4 : 16);
    for (int i = bits; i < all; i++) k.data[1 + i / 8] &= (np_u8)~(0x80u >> (i % 8));
    for (int i = 0; i < *n; i++) if (!memcmp(&out[i], &k, sizeof k)) return 0;
    if (*n >= max) return -1;
    out[(*n)++] = k;
    return 0;
}

int np_local_parse(const void *buf, size_t len, NpLocalKey *out, int max, int *n, int *full) {
    int rest = (int)len;
    for (const struct nlmsghdr *h = buf; NLMSG_OK(h, (unsigned)rest); h = NLMSG_NEXT(h, rest)) {
        if (h->nlmsg_type == NLMSG_DONE) return 1;
        if (h->nlmsg_type == NLMSG_ERROR) return -1;
        if (h->nlmsg_type == RTM_NEWADDR && h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifaddrmsg))) {
            const struct ifaddrmsg *m = NLMSG_DATA(h);
            int fam = m->ifa_family == AF_INET ? 4 : m->ifa_family == AF_INET6 ? 6 : 0;
            if (!fam) continue;
            int alen = (int)IFA_PAYLOAD(h);
            for (const struct rtattr *a = IFA_RTA(m); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type != IFA_LOCAL && a->rta_type != IFA_ADDRESS) continue;
                if (RTA_PAYLOAD(a) != (fam == 4 ? 4u : 16u)) continue;
                /* the address itself and its on-link network (a point-to-point peer's IFA_ADDRESS too) */
                if (put(out, max, n, fam, RTA_DATA(a), fam == 4 ? 32 : 128) ||
                    (m->ifa_prefixlen >= (fam == 4 ? 8 : 16) && put(out, max, n, fam, RTA_DATA(a), m->ifa_prefixlen))) *full = 1;
            }
        } else if (h->nlmsg_type == RTM_NEWROUTE && h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct rtmsg))) {
            const struct rtmsg *m = NLMSG_DATA(h);
            int fam = m->rtm_family == AF_INET ? 4 : m->rtm_family == AF_INET6 ? 6 : 0;
            /* a directly connected network: unicast or local, no gateway. Never a default route, nor a VPN's
             * default-like split (0/1 and 128/1): shorter than /8 (IPv4) or /16 (IPv6) isn't a link */
            if (!fam || (m->rtm_type != RTN_UNICAST && m->rtm_type != RTN_LOCAL) || m->rtm_dst_len < (fam == 4 ? 8 : 16)) continue;
            int alen = (int)RTM_PAYLOAD(h), gw = 0;
            const struct rtattr *dst = NULL;
            for (const struct rtattr *a = RTM_RTA(m); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type == RTA_GATEWAY || a->rta_type == RTA_VIA || a->rta_type == RTA_MULTIPATH) gw = 1;
                if (a->rta_type == RTA_DST && RTA_PAYLOAD(a) == (fam == 4 ? 4u : 16u)) dst = a;
            }
            if (!gw && dst && put(out, max, n, fam, RTA_DATA(dst), m->rtm_dst_len)) *full = 1;
        }
    }
    return 0;
}

/* one dump (RTM_GETADDR or RTM_GETROUTE, every family) into out; 0 or -1 */
static int dump(int type, NpLocalKey *out, int max, int *n, int *full) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct { struct nlmsghdr h; struct rtmsg m; } req;     /* rtmsg and ifaddrmsg both start with the family */
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = type == RTM_GETADDR ? NLMSG_LENGTH(sizeof(struct ifaddrmsg)) : NLMSG_LENGTH(sizeof(struct rtmsg));
    req.h.nlmsg_type = (unsigned short)type;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq = 1;
    req.m.rtm_family = AF_UNSPEC;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, req.h.nlmsg_len, 0, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static char buf[65536];
    int r = 0;
    while (!r) {
        ssize_t len = recv(fd, buf, sizeof buf, 0);
        if (len <= 0) { r = -1; break; }
        r = np_local_parse(buf, (size_t)len, out, max, n, full);
    }
    close(fd);
    return r < 0 ? -1 : 0;
}

int np_host_addrs(NpLocalKey *out, int max, int *full) {
    int n = 0;
    *full = 0;
    if (dump(RTM_GETADDR, out, max, &n, full) || dump(RTM_GETROUTE, out, max, &n, full)) return -1;
    return n;
}
