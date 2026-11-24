/* nplocal.c — this computer's own interface addresses, for np_local (see nplocal.h). */
#include "netperm/nplocal.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int np_host_addrs(NpLocalKey *out, int max) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct { struct nlmsghdr h; struct ifaddrmsg m; } req;
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req;
    req.h.nlmsg_type = RTM_GETADDR;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq = 1;
    req.m.ifa_family = AF_UNSPEC;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, sizeof req, 0, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    static char buf[32768];
    int n = 0, done = 0, err = 0;
    while (!done && !err) {
        struct timeval tv = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t len = recv(fd, buf, sizeof buf, 0);
        if (len <= 0) { err = 1; break; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type == NLMSG_ERROR) { err = 1; break; }
            if (h->nlmsg_type != RTM_NEWADDR || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifaddrmsg))) continue;
            struct ifaddrmsg *m = NLMSG_DATA(h);
            int fam = m->ifa_family == AF_INET ? 4 : m->ifa_family == AF_INET6 ? 6 : 0;
            if (!fam) continue;
            int alen = IFA_PAYLOAD(h);
            for (struct rtattr *a = IFA_RTA(m); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type != IFA_LOCAL && a->rta_type != IFA_ADDRESS) continue;
                if (RTA_PAYLOAD(a) != (fam == 4 ? 4u : 16u) || n >= max) continue;
                NpLocalKey k;
                memset(&k, 0, sizeof k);
                k.fam = (np_u8)fam;
                memcpy(k.addr, RTA_DATA(a), fam == 4 ? 4 : 16);
                int dup = 0;
                for (int i = 0; i < n && !dup; i++) dup = !memcmp(&out[i], &k, sizeof k);
                if (!dup) out[n++] = k;
            }
        }
    }
    close(fd);
    return err ? -1 : n;
}

