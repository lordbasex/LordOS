/* nl.c — our own netlink (no libnl or libmnl): messages, attributes, ACK and dumps; the
 * rtnetlink operations the network module uses; and utilities (JSON, addresses, files). */
#include "netpriv.h"

#include <net/if.h>                 /* before the linux/ ones */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fib_rules.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Buf ---- */

static void buf_grow(Buf *b, size_t need) {
    if (b->oom || b->len + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = 1; return; }
    b->p = p;
    b->cap = cap;
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_grow(b, (size_t)n);
    if (b->oom) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void buf_json_str(Buf *b, const char *s) {
    buf_printf(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') buf_printf(b, "\\%c", *p);
        else if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
        else buf_printf(b, "%c", *p);
    }
    buf_printf(b, "\"");
}

void buf_free(Buf *b) { free(b->p); *b = (Buf){0}; }

/* ---- messages ---- */

static uint32_t seq_counter;

int nl_open(int proto, unsigned groups) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, proto);
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = groups };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, SOL_NETLINK, NETLINK_EXT_ACK, &one, sizeof one);
    return fd;
}

static void nlm_grow(NlMsg *m, size_t n) {
    if (m->len + n <= m->cap) return;
    size_t cap = m->cap ? m->cap * 2 : 512;
    while (cap < m->len + n) cap *= 2;
    char *p = realloc(m->p, cap);
    if (!p) abort();
    memset(p + m->cap, 0, cap - m->cap);
    m->p = p;
    m->cap = cap;
}

void nlm_init(NlMsg *m, uint16_t type, uint16_t flags) {
    *m = (NlMsg){0};
    nlm_grow(m, NLMSG_HDRLEN);
    struct nlmsghdr *h = (struct nlmsghdr *)m->p;
    h->nlmsg_type = type;
    h->nlmsg_flags = (uint16_t)(flags | NLM_F_REQUEST);
    h->nlmsg_seq = ++seq_counter;
    m->len = NLMSG_HDRLEN;
}

void nlm_put(NlMsg *m, const void *data, size_t len) {
    size_t al = NLMSG_ALIGN(len);
    nlm_grow(m, al);
    memcpy(m->p + m->len, data, len);
    memset(m->p + m->len + len, 0, al - len);
    m->len += al;
}

void nlm_attr(NlMsg *m, uint16_t type, const void *data, size_t len) {
    size_t total = RTA_ALIGN(RTA_LENGTH(len));
    nlm_grow(m, total);
    struct rtattr *a = (struct rtattr *)(m->p + m->len);
    a->rta_type = type;
    a->rta_len = (unsigned short)RTA_LENGTH(len);
    if (len) memcpy(RTA_DATA(a), data, len);
    memset((char *)a + RTA_LENGTH(len), 0, total - RTA_LENGTH(len));
    m->len += total;
}

void nlm_attr_u8(NlMsg *m, uint16_t type, uint8_t v) { nlm_attr(m, type, &v, 1); }
void nlm_attr_u16(NlMsg *m, uint16_t type, uint16_t v) { nlm_attr(m, type, &v, 2); }
void nlm_attr_u32(NlMsg *m, uint16_t type, uint32_t v) { nlm_attr(m, type, &v, 4); }
void nlm_attr_str(NlMsg *m, uint16_t type, const char *s) { nlm_attr(m, type, s, strlen(s) + 1); }

size_t nlm_nest_begin(NlMsg *m, uint16_t type) {
    size_t at = m->len;
    nlm_attr(m, type | NLA_F_NESTED, NULL, 0);
    return at;
}

void nlm_nest_end(NlMsg *m, size_t at) {
    struct rtattr *a = (struct rtattr *)(m->p + at);
    a->rta_len = (unsigned short)(m->len - at);
}

void nlm_free(NlMsg *m) { free(m->p); *m = (NlMsg){0}; }

static int nl_send(int fd, NlMsg *m) {
    struct nlmsghdr *h = (struct nlmsghdr *)m->p;
    h->nlmsg_len = (uint32_t)m->len;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    return sendto(fd, m->p, m->len, 0, (struct sockaddr *)&sa, sizeof sa) == (ssize_t)m->len ? 0 : -errno;
}

/* Reads replies until the ACK/DONE of request seq. fn receives the data messages. */
static int nl_wait(int fd, uint32_t seq, NlFn fn, void *ud) {
    char buf[32768];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0) { if (errno == EINTR) continue; return -errno; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)r); h = NLMSG_NEXT(h, r)) {
            if (h->nlmsg_seq != seq) continue;
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(h);
                return e->error;                    /* 0 = ACK */
            }
            if (h->nlmsg_type == NLMSG_DONE) return 0;
            if (fn) fn(h, ud);
            if (!(h->nlmsg_flags & NLM_F_MULTI)) return 0;
        }
    }
}

int nl_talk(int fd, NlMsg *m) {
    ((struct nlmsghdr *)m->p)->nlmsg_flags |= NLM_F_ACK;
    int r = nl_send(fd, m);
    if (!r) r = nl_wait(fd, ((struct nlmsghdr *)m->p)->nlmsg_seq, NULL, NULL);
    nlm_free(m);
    return r;
}

int nl_dump(int fd, NlMsg *m, NlFn fn, void *ud) {
    int r = nl_send(fd, m);
    if (!r) r = nl_wait(fd, ((struct nlmsghdr *)m->p)->nlmsg_seq, fn, ud);
    nlm_free(m);
    return r;
}

void nl_parse(struct rtattr **tb, int max, const void *attrs, size_t len) {
    memset(tb, 0, sizeof(*tb) * (size_t)(max + 1));
    int l = (int)len;
    for (struct rtattr *a = (struct rtattr *)attrs; RTA_OK(a, l); a = RTA_NEXT(a, l)) {
        int t = a->rta_type & ~(NLA_F_NESTED | NLA_F_NET_BYTEORDER);
        if (t <= max) tb[t] = a;
    }
}

/* ---- generic netlink ---- */

static void family_cb(const struct nlmsghdr *h, void *ud) {
    if (h->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) return;
    struct rtattr *tb[CTRL_ATTR_MAX + 1];
    nl_parse(tb, CTRL_ATTR_MAX, (char *)NLMSG_DATA(h) + GENL_HDRLEN, h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN);
    if (tb[CTRL_ATTR_FAMILY_ID]) *(int *)ud = *(uint16_t *)RTA_DATA(tb[CTRL_ATTR_FAMILY_ID]);
}

int genl_family(int fd, const char *name) {
    NlMsg m;
    nlm_init(&m, GENL_ID_CTRL, 0);
    struct genlmsghdr g = { .cmd = CTRL_CMD_GETFAMILY, .version = 1 };
    nlm_put(&m, &g, sizeof g);
    nlm_attr_str(&m, CTRL_ATTR_FAMILY_NAME, name);
    int id = -1;
    return nl_dump(fd, &m, family_cb, &id) < 0 ? -1 : id;
}

/* ---- rtnetlink ---- */

int rt_ifindex(const char *name) { return (int)if_nametoindex(name); }

int rt_link_set(int fd, int ifindex, int up) {
    NlMsg m;
    nlm_init(&m, RTM_NEWLINK, 0);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_index = ifindex, .ifi_change = IFF_UP, .ifi_flags = up ? IFF_UP : 0 };
    nlm_put(&m, &ifi, sizeof ifi);
    return nl_talk(fd, &m);
}

int rt_link_mtu(int fd, int ifindex, unsigned mtu) {
    NlMsg m;
    nlm_init(&m, RTM_NEWLINK, 0);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_index = ifindex };
    nlm_put(&m, &ifi, sizeof ifi);
    nlm_attr_u32(&m, IFLA_MTU, mtu);
    return nl_talk(fd, &m);
}

int rt_link_new(int fd, const char *name, const char *kind) {
    NlMsg m;
    nlm_init(&m, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
    nlm_put(&m, &ifi, sizeof ifi);
    nlm_attr_str(&m, IFLA_IFNAME, name);
    size_t li = nlm_nest_begin(&m, IFLA_LINKINFO);
    nlm_attr(&m, IFLA_INFO_KIND, kind, strlen(kind));
    nlm_nest_end(&m, li);
    return nl_talk(fd, &m);
}

int rt_link_del(int fd, int ifindex) {
    NlMsg m;
    nlm_init(&m, RTM_DELLINK, 0);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_index = ifindex };
    nlm_put(&m, &ifi, sizeof ifi);
    return nl_talk(fd, &m);
}

int rt_addr(int fd, int add, int ifindex, int family, const void *addr, int prefix, uint32_t valid, uint32_t pref) {
    NlMsg m;
    nlm_init(&m, add ? RTM_NEWADDR : RTM_DELADDR, add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
    struct ifaddrmsg ifa = { .ifa_family = (uint8_t)family, .ifa_prefixlen = (uint8_t)prefix, .ifa_index = (unsigned)ifindex,
                             .ifa_scope = RT_SCOPE_UNIVERSE };
    nlm_put(&m, &ifa, sizeof ifa);
    size_t alen = family == AF_INET ? 4 : 16;
    nlm_attr(&m, IFA_LOCAL, addr, alen);
    nlm_attr(&m, IFA_ADDRESS, addr, alen);
    if (family == AF_INET && prefix < 31) {
        uint32_t a;
        memcpy(&a, addr, 4);
        uint32_t mask = prefix ? htonl(~0u << (32 - prefix)) : 0;
        uint32_t bc = a | ~mask;
        nlm_attr(&m, IFA_BROADCAST, &bc, 4);
    }
    if (add && valid) {
        struct ifa_cacheinfo ci = { .ifa_prefered = pref ? pref : valid, .ifa_valid = valid };
        nlm_attr(&m, IFA_CACHEINFO, &ci, sizeof ci);
    }
    return nl_talk(fd, &m);
}

typedef struct { int ifindex, family; struct { int family, prefix; uint8_t a[16]; } list[64]; int n; } AddrList;

static void addr_collect(const struct nlmsghdr *h, void *ud) {
    AddrList *l = ud;
    if (h->nlmsg_type != RTM_NEWADDR) return;
    struct ifaddrmsg *ifa = NLMSG_DATA(h);
    if ((int)ifa->ifa_index != l->ifindex || (l->family && ifa->ifa_family != l->family) || l->n >= 64) return;
    if (ifa->ifa_family == AF_INET6 && ifa->ifa_scope == RT_SCOPE_LINK) return;   /* fe80:: stays */
    struct rtattr *tb[IFA_MAX + 1];
    nl_parse(tb, IFA_MAX, IFA_RTA(ifa), IFA_PAYLOAD(h));
    struct rtattr *a = tb[IFA_LOCAL] ? tb[IFA_LOCAL] : tb[IFA_ADDRESS];
    if (!a) return;
    l->list[l->n].family = ifa->ifa_family;
    l->list[l->n].prefix = ifa->ifa_prefixlen;
    memcpy(l->list[l->n].a, RTA_DATA(a), ifa->ifa_family == AF_INET ? 4 : 16);
    l->n++;
}

int rt_addr_flush(int fd, int ifindex, int family) {
    AddrList l = { .ifindex = ifindex, .family = family };
    NlMsg m;
    nlm_init(&m, RTM_GETADDR, NLM_F_DUMP);
    struct ifaddrmsg ifa = { .ifa_family = (uint8_t)family };
    nlm_put(&m, &ifa, sizeof ifa);
    int r = nl_dump(fd, &m, addr_collect, &l);
    for (int i = 0; i < l.n; i++) rt_addr(fd, 0, ifindex, l.list[i].family, l.list[i].a, l.list[i].prefix, 0, 0);
    return r;
}

int rt_route(int fd, int add, int family, const void *dst, int prefix, const void *gw, int ifindex, uint32_t metric, uint32_t table) {
    return rt_route_proto(fd, add, family, dst, prefix, gw, ifindex, metric, table, RTPROT_STATIC);
}

int rt_route_proto(int fd, int add, int family, const void *dst, int prefix, const void *gw, int ifindex, uint32_t metric, uint32_t table, int proto) {
    NlMsg m;
    nlm_init(&m, add ? RTM_NEWROUTE : RTM_DELROUTE, add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
    struct rtmsg rt = { .rtm_family = (uint8_t)family, .rtm_dst_len = (uint8_t)prefix,
                        .rtm_table = table < 256 ? (uint8_t)(table ? table : RT_TABLE_MAIN) : RT_TABLE_UNSPEC,
                        .rtm_protocol = (uint8_t)proto, .rtm_scope = gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK,
                        .rtm_type = RTN_UNICAST };
    if (!add) rt.rtm_scope = RT_SCOPE_NOWHERE;
    nlm_put(&m, &rt, sizeof rt);
    size_t alen = family == AF_INET ? 4 : 16;
    if (prefix) nlm_attr(&m, RTA_DST, dst, alen);
    if (gw) nlm_attr(&m, RTA_GATEWAY, gw, alen);
    if (ifindex) nlm_attr_u32(&m, RTA_OIF, (uint32_t)ifindex);
    if (metric) nlm_attr_u32(&m, RTA_PRIORITY, metric);
    if (table >= 256) nlm_attr_u32(&m, RTA_TABLE, table);
    return nl_talk(fd, &m);
}

typedef struct { int family; uint8_t gw[16]; int has_gw, oif; } RtGet;

static void get_cb(const struct nlmsghdr *h, void *ud) {
    RtGet *g = ud;
    if (h->nlmsg_type != RTM_NEWROUTE) return;
    struct rtmsg *rt = NLMSG_DATA(h);
    struct rtattr *tb[RTA_MAX + 1];
    nl_parse(tb, RTA_MAX, RTM_RTA(rt), RTM_PAYLOAD(h));
    if (tb[RTA_GATEWAY]) { memcpy(g->gw, RTA_DATA(tb[RTA_GATEWAY]), g->family == AF_INET ? 4 : 16); g->has_gw = 1; }
    if (tb[RTA_OIF]) g->oif = (int)*(uint32_t *)RTA_DATA(tb[RTA_OIF]);
}

int rt_get(int fd, int family, const void *dst, uint8_t gw[16], int *has_gw, int *oif) {
    NlMsg m;
    nlm_init(&m, RTM_GETROUTE, 0);
    struct rtmsg rt = { .rtm_family = (uint8_t)family, .rtm_dst_len = (uint8_t)(family == AF_INET ? 32 : 128) };
    nlm_put(&m, &rt, sizeof rt);
    nlm_attr(&m, RTA_DST, dst, family == AF_INET ? 4 : 16);
    RtGet g = { .family = family };
    int r = nl_dump(fd, &m, get_cb, &g);
    if (r < 0 || !g.oif) return -1;
    memcpy(gw, g.gw, 16);
    *has_gw = g.has_gw;
    *oif = g.oif;
    return 0;
}

typedef struct { int proto; struct { int family, prefix, oif; uint8_t dst[16]; } v[64]; int n; } ProtoList;

static void proto_cb(const struct nlmsghdr *h, void *ud) {
    ProtoList *l = ud;
    if (h->nlmsg_type != RTM_NEWROUTE || l->n >= 64) return;
    struct rtmsg *rt = NLMSG_DATA(h);
    if (rt->rtm_protocol != l->proto) return;
    struct rtattr *tb[RTA_MAX + 1];
    nl_parse(tb, RTA_MAX, RTM_RTA(rt), RTM_PAYLOAD(h));
    l->v[l->n].family = rt->rtm_family;
    l->v[l->n].prefix = rt->rtm_dst_len;
    l->v[l->n].oif = tb[RTA_OIF] ? (int)*(uint32_t *)RTA_DATA(tb[RTA_OIF]) : 0;
    memset(l->v[l->n].dst, 0, 16);
    if (tb[RTA_DST]) memcpy(l->v[l->n].dst, RTA_DATA(tb[RTA_DST]), rt->rtm_family == AF_INET ? 4 : 16);
    l->n++;
}

/* Deletes the routes with that protocol (LordOS's: RTPROT_LORDOS) towards dst, or all if dst is NULL. */
int rt_flush_proto(int fd, int proto, int family, const void *dst) {
    ProtoList l = { .proto = proto };
    NlMsg m;
    nlm_init(&m, RTM_GETROUTE, NLM_F_DUMP);
    struct rtmsg rt = { .rtm_family = (uint8_t)family };
    nlm_put(&m, &rt, sizeof rt);
    nl_dump(fd, &m, proto_cb, &l);
    int n = 0;
    for (int i = 0; i < l.n; i++) {
        if (dst && memcmp(l.v[i].dst, dst, family == AF_INET ? 4 : 16)) continue;
        if (!rt_route_proto(fd, 0, l.v[i].family, l.v[i].dst, l.v[i].prefix, NULL, l.v[i].oif, 0, 0, proto)) n++;
    }
    return n;
}

/* XFRM interface (IPsec with if_id): traffic routed through it goes into the tunnel. */
int rt_link_new_xfrm(int fd, const char *name, uint32_t if_id) {
    NlMsg m;
    nlm_init(&m, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
    nlm_put(&m, &ifi, sizeof ifi);
    nlm_attr_str(&m, IFLA_IFNAME, name);
    size_t li = nlm_nest_begin(&m, IFLA_LINKINFO);
    nlm_attr(&m, IFLA_INFO_KIND, "xfrm", 4);
    size_t data = nlm_nest_begin(&m, IFLA_INFO_DATA);
    nlm_attr_u32(&m, IFLA_XFRM_IF_ID, if_id);
    nlm_nest_end(&m, data);
    nlm_nest_end(&m, li);
    return nl_talk(fd, &m);
}

int rt_rule(int fd, int add, int family, uint32_t fwmark, int invert, uint32_t table, int suppress_prefix, uint32_t priority) {
    NlMsg m;
    nlm_init(&m, add ? RTM_NEWRULE : RTM_DELRULE, add ? NLM_F_CREATE | NLM_F_EXCL : 0);
    struct fib_rule_hdr fr = { .family = (uint8_t)family, .action = FR_ACT_TO_TBL,
                               .table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC, .flags = invert ? FIB_RULE_INVERT : 0 };
    nlm_put(&m, &fr, sizeof fr);
    if (table >= 256) nlm_attr_u32(&m, FRA_TABLE, table);
    if (fwmark) nlm_attr_u32(&m, FRA_FWMARK, fwmark);
    if (suppress_prefix >= 0) nlm_attr_u32(&m, FRA_SUPPRESS_PREFIXLEN, (uint32_t)suppress_prefix);
    if (priority) nlm_attr_u32(&m, FRA_PRIORITY, priority);
    return nl_talk(fd, &m);
}

/* ---- addresses ---- */

int parse_ip(const char *s, int *family, uint8_t a[16]) {
    memset(a, 0, 16);
    if (inet_pton(AF_INET, s, a) == 1) { *family = AF_INET; return 0; }
    if (inet_pton(AF_INET6, s, a) == 1) { *family = AF_INET6; return 0; }
    return -1;
}

int parse_cidr(const char *s, Cidr *c, int need_prefix) {
    char tmp[64];
    if (strlen(s) >= sizeof tmp) return -1;
    snprintf(tmp, sizeof tmp, "%s", s);
    char *slash = strchr(tmp, '/');
    if (slash) *slash = 0;
    if (parse_ip(tmp, &c->family, c->a)) return -1;
    int max = c->family == AF_INET ? 32 : 128;
    if (!slash) { if (need_prefix) return -1; c->prefix = max; return 0; }
    char *end;
    long p = strtol(slash + 1, &end, 10);
    if (*end || end == slash + 1 || p < 0 || p > max) return -1;
    c->prefix = (int)p;
    return 0;
}

void fmt_ip(int family, const void *a, char *out, size_t n) {
    if (!inet_ntop(family, a, out, (socklen_t)n)) snprintf(out, n, "?");
}

/* ---- names and files ---- */

int valid_ifname(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= IFNAMSIZ || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("-_.", *p)) return 0;
    return 1;
}

int valid_vpn_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 24) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("-_", *p)) return 0;
    return 1;
}

static void mkdirs(const char *path, unsigned mode) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, mode);
}

int write_file_atomic(const char *path, const char *data, size_t len, unsigned mode) {
    char dir[256], tmp[300];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    mkdirs(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    fchmod(fd, mode);
    int ok = write(fd, data, len) == (ssize_t)len;
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

char *read_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    char *p = malloc(max + 1);
    size_t n = 0;
    ssize_t r;
    while (p && n < max && (r = read(fd, p + n, max - n)) > 0) n += (size_t)r;
    close(fd);
    if (!p) return NULL;
    p[n] = 0;
    if (len) *len = n;
    return p;
}

int kv_get(const char *text, const char *key, int idx, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *l = text; l && *l;) {
        const char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll > kl && !strncmp(l, key, kl) && l[kl] == '=') {
            if (idx-- == 0) {
                size_t vl = ll - kl - 1;
                if (vl >= n) vl = n - 1;
                memcpy(out, l + kl + 1, vl);
                out[vl] = 0;
                return 1;
            }
        }
        l = e ? e + 1 : NULL;
    }
    if (n) out[0] = 0;
    return 0;
}
