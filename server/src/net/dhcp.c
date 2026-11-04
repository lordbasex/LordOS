/* dhcp.c — our own DHCPv4 client (RFC 2131), as a resident process of the privileged helper
 * ("if:<interface>"): DISCOVER → OFFER → REQUEST → ACK, applies address, default route and DNS
 * with netlink, renews at T1 (unicast to the server) and re-requests at T2 (broadcast); if the
 * lease expires or a NAK arrives, it starts over. It follows the link: without carrier it waits,
 * and when it comes back it confirms the same address (INIT-REBOOT). On Wi-Fi it also translates
 * wpa_supplicant's notices (connected/disconnected) into EVENTs. SIGTERM: DHCPRELEASE and cleanup. If the process that
 * started it dies (lord-netd, which lord-sessiond restarts right away) it gets SIGUSR1: it keeps
 * the address and leaves the lease in /run/lordos/dhcp/<if>.lease, and the new client confirms it
 * (INIT-REBOOT) without dropping connections. SIGUSR2 (lord-netd, after the computer wakes from sleep):
 * confirm the lease now (INIT-REBOOT), since the network may have changed and the timers, on
 * CLOCK_MONOTONIC, stood still while asleep.
 *
 * It sends and receives over an AF_PACKET socket (no IP needed yet); a UDP socket on
 * port 68 bound to the interface keeps the kernel from answering "port unreachable" and serves
 * for unicast renewal. IPv6: the kernel does SLAAC by itself; DHCPv6 is still pending. */
#include "netpriv.h"

#include <net/if.h>                 /* before the linux/ ones */
#include "../admin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE };
enum { S_WAIT_LINK, S_SELECTING, S_REQUESTING, S_REBOOTING, S_BOUND, S_RENEWING, S_REBINDING };

#define MAGIC 0x63825363u

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16], sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

typedef struct {
    uint32_t addr, mask, router, server, lease, t1, t2;
    uint32_t dns[4];
    int ndns;
    char domain[128];
} Lease;

static volatile sig_atomic_t stop_flag, keep_flag, renew_flag;
static void on_term(int s) { (void)s; stop_flag = 1; }
static void on_renew(int s) { (void)s; renew_flag = 1; }
static void on_parent_gone(int s) { (void)s; keep_flag = 1; stop_flag = 1; }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint16_t ip_sum(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)b[i] << 8 | b[i + 1];
    if (n & 1) s += (uint32_t)b[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return htons((uint16_t)~s);
}

typedef struct {
    char ifname[IFNAMSIZ];
    int ifindex, wifi, pkt, udp, mon, wpa;
    uint8_t mac[6];
    uint32_t xid;
    int state;
    double start, deadline, next_tx, t1_at, t2_at, end_at;
    int tries;
    Lease offer, lease;
    char host[64];
} Dhcp;

static size_t put_opt(uint8_t *o, size_t at, uint8_t code, const void *data, uint8_t len) {
    if (at + 2u + len > 308) return at;
    o[at] = code; o[at + 1] = len;
    memcpy(o + at + 2, data, len);
    return at + 2u + len;
}

static size_t build(Dhcp *d, struct dhcp_msg *m, int type, uint32_t ciaddr, uint32_t req, uint32_t server) {
    memset(m, 0, sizeof *m);
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = d->xid;
    m->secs = htons((uint16_t)(now_s() - d->start > 65535 ? 65535 : now_s() - d->start));
    m->ciaddr = ciaddr;
    memcpy(m->chaddr, d->mac, 6);
    m->magic = htonl(MAGIC);
    uint8_t t = (uint8_t)type, cid[7] = { 1 };
    memcpy(cid + 1, d->mac, 6);
    size_t at = put_opt(m->options, 0, 53, &t, 1);
    at = put_opt(m->options, at, 61, cid, 7);
    if (req) at = put_opt(m->options, at, 50, &req, 4);
    if (server) at = put_opt(m->options, at, 54, &server, 4);
    if (type != RELEASE) {
        static const uint8_t params[] = { 1, 3, 6, 15, 51, 54, 58, 59, 119 };
        at = put_opt(m->options, at, 55, params, sizeof params);
        uint16_t mx = htons(1500);
        at = put_opt(m->options, at, 57, &mx, 2);
        if (d->host[0]) at = put_opt(m->options, at, 12, d->host, (uint8_t)strlen(d->host));
    }
    m->options[at++] = 255;
    return offsetof(struct dhcp_msg, options) + at;
}

static int send_broadcast(Dhcp *d, struct dhcp_msg *m, size_t len) {
    uint8_t pkt[sizeof(struct iphdr) + sizeof(struct udphdr) + sizeof *m];
    struct iphdr *ip = (struct iphdr *)pkt;
    struct udphdr *udp = (struct udphdr *)(pkt + sizeof *ip);
    memset(pkt, 0, sizeof *ip + sizeof *udp);
    memcpy(pkt + sizeof *ip + sizeof *udp, m, len);
    size_t total = sizeof *ip + sizeof *udp + len;
    ip->version = 4; ip->ihl = 5; ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->tot_len = htons((uint16_t)total);
    ip->saddr = 0; ip->daddr = 0xffffffffu;
    ip->check = ip_sum(ip, sizeof *ip);
    udp->source = htons(68); udp->dest = htons(67);
    udp->len = htons((uint16_t)(sizeof *udp + len));
    udp->check = 0;                         /* optional in IPv4 */
    struct sockaddr_ll sa = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = d->ifindex, .sll_halen = 6 };
    memset(sa.sll_addr, 0xff, 6);
    return sendto(d->pkt, pkt, total, 0, (struct sockaddr *)&sa, sizeof sa) == (ssize_t)total ? 0 : -1;
}

static int send_unicast(Dhcp *d, struct dhcp_msg *m, size_t len, uint32_t server) {
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = server };
    return sendto(d->udp, m, len, 0, (struct sockaddr *)&sa, sizeof sa) == (ssize_t)len ? 0 : -1;
}

/* Reads the options; returns the message type (or 0). */
static int parse(const struct dhcp_msg *m, size_t len, Lease *l) {
    memset(l, 0, sizeof *l);
    l->addr = m->yiaddr;
    int type = 0;
    size_t n = len - offsetof(struct dhcp_msg, options);
    const uint8_t *o = m->options;
    for (size_t i = 0; i < n;) {
        uint8_t c = o[i];
        if (c == 0) { i++; continue; }
        if (c == 255 || i + 1 >= n) break;
        uint8_t l2 = o[i + 1];
        if (i + 2 + l2 > n) break;
        const uint8_t *v = o + i + 2;
        switch (c) {
        case 53: if (l2 >= 1) type = v[0]; break;
        case 1: if (l2 == 4) memcpy(&l->mask, v, 4); break;
        case 3: if (l2 >= 4) memcpy(&l->router, v, 4); break;
        case 6: for (int k = 0; k + 4 <= l2 && l->ndns < 4; k += 4) memcpy(&l->dns[l->ndns++], v + k, 4); break;
        case 15: {
            size_t k = 0;
            for (; k < l2 && k < sizeof l->domain - 1; k++) {
                char ch = (char)v[k];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')) break;
                l->domain[k] = ch;
            }
            l->domain[k] = 0;
            break;
        }
        case 51: if (l2 == 4) { uint32_t t; memcpy(&t, v, 4); l->lease = ntohl(t); } break;
        case 54: if (l2 == 4) memcpy(&l->server, v, 4); break;
        case 58: if (l2 == 4) { uint32_t t; memcpy(&t, v, 4); l->t1 = ntohl(t); } break;
        case 59: if (l2 == 4) { uint32_t t; memcpy(&t, v, 4); l->t2 = ntohl(t); } break;
        }
        i += 2u + l2;
    }
    if (!l->lease) l->lease = 3600;
    if (l->lease < 60) l->lease = 60;
    if (!l->t1 || l->t1 >= l->lease) l->t1 = l->lease / 2;
    if (!l->t2 || l->t2 >= l->lease || l->t2 <= l->t1) l->t2 = l->lease / 8 * 7;
    return type;
}

/* A packet from the AF_PACKET socket: is it a DHCP reply for us? */
static int recv_reply(Dhcp *d, Lease *l, uint32_t *from_server) {
    uint8_t buf[2048];
    ssize_t r = recv(d->pkt, buf, sizeof buf, 0);
    if (r < (ssize_t)(sizeof(struct iphdr) + sizeof(struct udphdr))) return 0;
    struct iphdr *ip = (struct iphdr *)buf;
    if (ip->version != 4 || ip->protocol != IPPROTO_UDP || ip->ihl < 5) return 0;
    size_t ihl = ip->ihl * 4u;
    if ((size_t)r < ihl + sizeof(struct udphdr)) return 0;
    struct udphdr *udp = (struct udphdr *)(buf + ihl);
    if (ntohs(udp->dest) != 68) return 0;
    size_t len = (size_t)r - ihl - sizeof *udp;
    if (len < offsetof(struct dhcp_msg, options) + 4 || len > sizeof(struct dhcp_msg)) return 0;
    struct dhcp_msg m;
    memset(&m, 0, sizeof m);
    memcpy(&m, buf + ihl + sizeof *udp, len);
    if (m.op != 2 || m.xid != d->xid || ntohl(m.magic) != MAGIC || memcmp(m.chaddr, d->mac, 6)) return 0;
    int type = parse(&m, len, l);
    if (from_server) *from_server = l->server;
    return type;
}

static int mask_prefix(uint32_t mask) {
    uint32_t m = ntohl(mask);
    int p = 0;
    while (m & 0x80000000u) { p++; m <<= 1; }
    return mask ? p : 24;
}

static void apply(Dhcp *d, const Lease *l, int add) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return;
    int prefix = mask_prefix(l->mask);
    uint32_t metric = d->wifi ? 600 : 100;
    if (add) {
        rt_addr(fd, 1, d->ifindex, AF_INET, &l->addr, prefix, l->lease, l->lease);
        if (l->router) {
            uint32_t any = 0;
            rt_route(fd, 1, AF_INET, &any, 0, &l->router, d->ifindex, metric, 0);
        }
        char servers[128] = "", ip[32];
        for (int i = 0; i < l->ndns; i++) {
            fmt_ip(AF_INET, &l->dns[i], ip, sizeof ip);
            snprintf(servers + strlen(servers), sizeof servers - strlen(servers), "%s%s", i ? " " : "", ip);
        }
        if (l->ndns) dns_set(d->ifname, servers, l->domain, DNS_PLAIN);
    } else {
        if (l->router) {
            uint32_t any = 0;
            rt_route(fd, 0, AF_INET, &any, 0, &l->router, d->ifindex, metric, 0);
        }
        rt_addr(fd, 0, d->ifindex, AF_INET, &l->addr, prefix, 0, 0);
        dns_revert(d->ifname);
    }
    close(fd);
}

static int link_carrier(int ifindex) {
    char path[96], name[IFNAMSIZ];
    if (!if_indextoname((unsigned)ifindex, name)) return 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/operstate", name);
    FILE *f = fopen(path, "r");
    char s[32] = "";
    if (f) { if (!fgets(s, sizeof s, f)) s[0] = 0; fclose(f); }
    return !strncmp(s, "up", 2) || !strncmp(s, "unknown", 7);
}

static void new_xid(Dhcp *d) { random_bytes(&d->xid, sizeof d->xid); d->start = now_s(); d->tries = 0; }

static void transmit(Dhcp *d) {
    struct dhcp_msg m;
    size_t len;
    switch (d->state) {
    case S_SELECTING: len = build(d, &m, DISCOVER, 0, d->lease.addr, 0); send_broadcast(d, &m, len); break;
    case S_REQUESTING: len = build(d, &m, REQUEST, 0, d->offer.addr, d->offer.server); send_broadcast(d, &m, len); break;
    case S_REBOOTING: len = build(d, &m, REQUEST, 0, d->lease.addr, 0); send_broadcast(d, &m, len); break;
    case S_RENEWING: len = build(d, &m, REQUEST, d->lease.addr, 0, 0); send_unicast(d, &m, len, d->lease.server); break;
    case S_REBINDING: len = build(d, &m, REQUEST, d->lease.addr, 0, 0); send_broadcast(d, &m, len); break;
    default: return;
    }
    /* retries: 4, 8, 16, 32, 64 s (RFC 2131 4.1), with ±1 s */
    int wait = 4 << (d->tries < 4 ? d->tries : 4);
    uint8_t j;
    random_bytes(&j, 1);
    d->next_tx = now_s() + wait + (j % 3) - 1;
    d->tries++;
}

static void enter(Dhcp *d, int st) {
    d->state = st;
    if (st == S_SELECTING || st == S_REBOOTING) new_xid(d);
    else d->tries = 0;
    d->next_tx = now_s();
    if (st == S_REBOOTING) d->deadline = now_s() + 10;
}

/* ---- the saved lease (so a new lord-netd can resume it) ---- */

#define LEASE_DIR NET_RUN_DIR "/dhcp"
#define LEASE_MAGIC 0x4c444c31u           /* "LDL1" */
typedef struct { uint32_t magic; char ifname[IFNAMSIZ]; int64_t end; Lease lease; } LeaseFile;

static void lease_path(const Dhcp *d, char *p, size_t n) { snprintf(p, n, LEASE_DIR "/%s.lease", d->ifname); }

static void lease_save(const Dhcp *d) {
    char p[96], tmp[100];
    mkdir(LEASE_DIR, 0700);
    lease_path(d, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    LeaseFile f = { .magic = LEASE_MAGIC, .end = (int64_t)time(NULL) + (int64_t)(d->end_at - now_s()), .lease = d->lease };
    snprintf(f.ifname, sizeof f.ifname, "%s", d->ifname);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    int ok = write(fd, &f, sizeof f) == (ssize_t)sizeof f;
    if (close(fd) || !ok || rename(tmp, p)) unlink(tmp);
}

static void lease_forget(const Dhcp *d) { char p[96]; lease_path(d, p, sizeof p); unlink(p); }

int dhcp_lease_router(const char *ifname, uint32_t *router) {
    char p[96];
    snprintf(p, sizeof p, LEASE_DIR "/%s.lease", ifname);
    LeaseFile f;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, &f, sizeof f);
    close(fd);
    if (r != (ssize_t)sizeof f || f.magic != LEASE_MAGIC || strncmp(f.ifname, ifname, sizeof f.ifname)) return -1;
    *router = f.lease.router;
    return 0;
}

/* The lease of an earlier client on this same interface, if still valid. */
static void lease_load(Dhcp *d) {
    char p[96];
    lease_path(d, p, sizeof p);
    LeaseFile f;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    ssize_t r = read(fd, &f, sizeof f);
    close(fd);
    if (r != (ssize_t)sizeof f || f.magic != LEASE_MAGIC || strncmp(f.ifname, d->ifname, sizeof f.ifname) || f.end <= (int64_t)time(NULL) + 5) {
        unlink(p);
        return;
    }
    if (f.lease.ndns < 0 || f.lease.ndns > 4) return;
    f.lease.domain[sizeof f.lease.domain - 1] = 0;
    d->lease = f.lease;
    double left = (double)(f.end - (int64_t)time(NULL));
    d->end_at = now_s() + left;
    admin_event("net dhcp %s resume", d->ifname);
}

static void bound(Dhcp *d, const Lease *l) {
    int same = d->lease.addr == l->addr && d->lease.router == l->router;
    if (d->lease.addr && !same) apply(d, &d->lease, 0);
    d->lease = *l;
    apply(d, l, 1);
    double t = now_s();
    d->t1_at = t + l->t1;
    d->t2_at = t + l->t2;
    d->end_at = t + l->lease;
    d->state = S_BOUND;
    lease_save(d);
    char ip[32], gw[32] = "-";
    fmt_ip(AF_INET, &l->addr, ip, sizeof ip);
    if (l->router) fmt_ip(AF_INET, &l->router, gw, sizeof gw);
    admin_event("net dhcp %s bound %s/%d %s %u", d->ifname, ip, mask_prefix(l->mask), gw, l->lease);
}

static void drop_lease(Dhcp *d, const char *why) {
    if (d->lease.addr) apply(d, &d->lease, 0);
    memset(&d->lease, 0, sizeof d->lease);
    lease_forget(d);
    admin_event("net dhcp %s %s", d->ifname, why);
}

/* ---- does the gateway answer? (lord-netd, after waking from sleep) ---- */

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

int arp_probe(const char *ifname, uint32_t src, uint32_t dst, int tries, int wait_ms) {
    int ifindex = rt_ifindex(ifname);
    if (!ifindex) return -1;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (s < 0 || ioctl(s, SIOCGIFHWADDR, &ifr)) { if (s >= 0) close(s); return -1; }
    close(s);
    int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_ARP));
    struct sockaddr_ll ll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ARP), .sll_ifindex = ifindex };
    if (fd < 0 || bind(fd, (struct sockaddr *)&ll, sizeof ll)) { if (fd >= 0) close(fd); return -1; }
    struct arp_pkt q = { .htype = htons(1), .ptype = htons(ETH_P_IP), .hlen = 6, .plen = 4, .op = htons(1), .spa = src, .tpa = dst };
    memcpy(q.sha, ifr.ifr_hwaddr.sa_data, 6);
    struct sockaddr_ll to = ll;
    to.sll_halen = 6;
    memset(to.sll_addr, 0xff, 6);
    int ok = 0;
    for (int i = 0; i < tries && !ok; i++) {
        if (sendto(fd, &q, sizeof q, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)sizeof q) break;
        double end = now_s() + wait_ms / 1000.0;
        for (double t = now_s(); !ok && t < end; t = now_s()) {
            struct pollfd pf = { fd, POLLIN, 0 };
            if (poll(&pf, 1, (int)((end - t) * 1000) + 1) <= 0) break;
            struct arp_pkt r;
            if (recv(fd, &r, sizeof r, 0) == (ssize_t)sizeof r && r.op == htons(2) && r.spa == dst) ok = 1;
        }
    }
    close(fd);
    return ok ? 0 : -1;
}

/* ---- Wi-Fi: wpa_supplicant notices ---- */

static int wpa_attach(Dhcp *d) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un me = { .sun_family = AF_UNIX }, srv = { .sun_family = AF_UNIX };
    snprintf(me.sun_path, sizeof me.sun_path, NET_RUN_DIR "/wpa/ev-%s", d->ifname);
    unlink(me.sun_path);
    snprintf(srv.sun_path, sizeof srv.sun_path, NET_RUN_DIR "/wpa/%s", d->ifname);
    if (bind(fd, (struct sockaddr *)&me, sizeof me) || connect(fd, (struct sockaddr *)&srv, sizeof srv) ||
        send(fd, "ATTACH", 6, 0) != 6) { close(fd); unlink(me.sun_path); return -1; }
    return fd;
}

static void wpa_event(Dhcp *d) {
    char buf[1024];
    ssize_t r = recv(d->wpa, buf, sizeof buf - 1, 0);
    if (r <= 0) return;
    buf[r] = 0;
    char *msg = buf[0] == '<' && strchr(buf, '>') ? strchr(buf, '>') + 1 : buf;
    if (!strncmp(msg, "CTRL-EVENT-CONNECTED", 20)) {
        char st[2048], ssid[128] = "";
        if (wifi_ctrl(d->ifname, "STATUS", st, sizeof st) > 0) {
            char *s = strstr(st, "\nssid=");
            if (s) { s += 6; size_t k = strcspn(s, "\n"); if (k >= sizeof ssid) k = sizeof ssid - 1; memcpy(ssid, s, k); ssid[k] = 0; }
        }
        admin_event("net wifi %s connected %s", d->ifname, ssid);
    } else if (!strncmp(msg, "CTRL-EVENT-DISCONNECTED", 23)) admin_event("net wifi %s disconnected", d->ifname);
    else if (!strncmp(msg, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) && strstr(msg, "WRONG_KEY")) admin_event("net wifi %s wrong-key", d->ifname);
    else if (!strncmp(msg, "CTRL-EVENT-SCAN-RESULTS", 23)) admin_event("net wifi %s scan-results", d->ifname);
}

void dhcp_run(const char *ifname, int wifi) {
    Dhcp d = { .wifi = wifi, .pkt = -1, .udp = -1, .mon = -1, .wpa = -1 };
    snprintf(d.ifname, sizeof d.ifname, "%s", ifname);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    struct sigaction sk = { .sa_handler = on_parent_gone };
    sigaction(SIGUSR1, &sk, NULL);
    struct sigaction sr = { .sa_handler = on_renew };
    sigaction(SIGUSR2, &sr, NULL);
    prctl(PR_SET_PDEATHSIG, SIGUSR1);           /* lord-netd dies: exit without releasing the address */
    d.ifindex = rt_ifindex(ifname);
    gethostname(d.host, sizeof d.host - 1);
    for (char *p = d.host; *p; p++) if (*p == '.') { *p = 0; break; }
    if (!d.ifindex) { admin_event("net dhcp %s error no-interface", ifname); return; }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (s < 0 || ioctl(s, SIOCGIFHWADDR, &ifr)) { admin_event("net dhcp %s error no-mac", ifname); if (s >= 0) close(s); return; }
    memcpy(d.mac, ifr.ifr_hwaddr.sa_data, 6);
    close(s);

    d.pkt = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
    struct sockaddr_ll ll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = d.ifindex };
    if (d.pkt < 0 || bind(d.pkt, (struct sockaddr *)&ll, sizeof ll)) { admin_event("net dhcp %s error socket", ifname); return; }
    d.udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(d.udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(d.udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(d.udp, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname) + 1);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(68) };
    if (bind(d.udp, (struct sockaddr *)&me, sizeof me)) { close(d.udp); d.udp = -1; }
    d.mon = nl_open(NETLINK_ROUTE, RTMGRP_LINK);
    {
        int fd = nl_open(NETLINK_ROUTE, 0);
        if (fd >= 0) { rt_link_set(fd, d.ifindex, 1); close(fd); }
    }
    double wpa_retry = 0;
    lease_load(&d);
    enter(&d, S_WAIT_LINK);
    admin_event("net dhcp %s started", ifname);

    while (!stop_flag) {
        if (renew_flag) {
            renew_flag = 0;
            admin_event("net dhcp %s renew", ifname);
            if (d.state != S_WAIT_LINK) enter(&d, d.lease.addr ? S_REBOOTING : S_SELECTING);
        }
        double t = now_s();
        if (wifi && d.wpa < 0 && t >= wpa_retry) { d.wpa = wpa_attach(&d); wpa_retry = t + 2; }
        if (d.state == S_WAIT_LINK && link_carrier(d.ifindex)) enter(&d, d.lease.addr ? S_REBOOTING : S_SELECTING);
        if (d.state != S_WAIT_LINK && d.state != S_BOUND && t >= d.next_tx) transmit(&d);
        if (d.state == S_REQUESTING && d.tries > 4) enter(&d, S_SELECTING);
        if (d.state == S_REBOOTING && t > d.deadline) { drop_lease(&d, "reboot-timeout"); enter(&d, S_SELECTING); }
        if (d.state == S_BOUND && t >= d.t1_at) { d.state = S_RENEWING; new_xid(&d); d.next_tx = t; }
        if (d.state == S_RENEWING && t >= d.t2_at) { d.state = S_REBINDING; new_xid(&d); d.next_tx = t; }
        if ((d.state == S_RENEWING || d.state == S_REBINDING) && t >= d.end_at) { drop_lease(&d, "expired"); enter(&d, S_SELECTING); }
        /* until the next event */
        double next = t + 30;
        if (d.state != S_WAIT_LINK && d.state != S_BOUND && d.next_tx < next) next = d.next_tx;
        if (d.state == S_BOUND && d.t1_at < next) next = d.t1_at;
        if (d.state == S_REBOOTING && d.deadline < next) next = d.deadline;
        if ((d.state == S_RENEWING || d.state == S_REBINDING) && d.end_at < next) next = d.end_at;
        if (wifi && d.wpa < 0 && wpa_retry < next) next = wpa_retry;
        int ms = (int)((next - t) * 1000) + 1;
        if (ms < 1) ms = 1;
        struct pollfd pf[4] = { { d.pkt, POLLIN, 0 }, { d.mon, POLLIN, 0 }, { d.udp, POLLIN, 0 }, { d.wpa, POLLIN, 0 } };
        if (poll(pf, 4, ms) < 0) continue;
        if (pf[2].revents) { char junk[1500]; while (recv(d.udp, junk, sizeof junk, MSG_DONTWAIT) > 0) {} }
        if (pf[3].revents) { if (pf[3].revents & (POLLERR | POLLHUP)) { close(d.wpa); d.wpa = -1; } else wpa_event(&d); }
        if (pf[1].revents) {
            char junk[8192];
            while (recv(d.mon, junk, sizeof junk, MSG_DONTWAIT) > 0) {}
            int up = link_carrier(d.ifindex);
            if (!up && d.state != S_WAIT_LINK) {
                if (wifi) drop_lease(&d, "no-carrier");        /* another network: another address */
                else admin_event("net dhcp %s no-carrier", ifname);
                enter(&d, S_WAIT_LINK);
            }
        }
        if (pf[0].revents) {
            Lease l;
            uint32_t srv = 0;
            int type = recv_reply(&d, &l, &srv);
            if (!type) continue;
            if (d.state == S_SELECTING && type == OFFER && l.addr) { d.offer = l; enter(&d, S_REQUESTING); }
            else if ((d.state == S_REQUESTING || d.state == S_REBOOTING || d.state == S_RENEWING || d.state == S_REBINDING) && type == ACK && l.addr) {
                if (!l.server) l.server = srv;
                bound(&d, &l);
            } else if (type == NAK && d.state != S_SELECTING && d.state != S_BOUND) {
                drop_lease(&d, "nak");
                enter(&d, S_SELECTING);
            }
        }
    }
    if (keep_flag) {                            /* the next lord-netd resumes it */
        if (d.lease.addr && d.state == S_BOUND) lease_save(&d);
        _exit(0);
    }
    lease_forget(&d);
    if (d.lease.addr) {
        struct dhcp_msg m;
        new_xid(&d);
        size_t len = build(&d, &m, RELEASE, d.lease.addr, 0, d.lease.server);
        if (d.udp >= 0) send_unicast(&d, &m, len, d.lease.server);
        apply(&d, &d.lease, 0);
    }
    if (d.wpa >= 0) { char p[108]; snprintf(p, sizeof p, NET_RUN_DIR "/wpa/ev-%s", ifname); unlink(p); }
    admin_event("net dhcp %s stopped", ifname);
}
