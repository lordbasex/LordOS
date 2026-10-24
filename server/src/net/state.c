/* state.c — network state without root, for lord-server: interfaces, addresses, routes,
 * counters (rtnetlink dumps), DNS, who manages each interface, and change notifications. */
#include "net.h"
#include "netpriv.h"
#include "../errcode.h"

#include <net/if.h>                 /* before the linux/ ones */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- who manages an interface ---- */

/* systemd-networkd configures it (its state file says something other than "unmanaged"), or
 * it appears in /etc/network/interfaces (ifupdown). networkd's state format isn't
 * public, but it has been stable for years; if it changes, the interface shows as free and
 * nothing bad happens (LordOS only touches it if the user configures it). */
static int networkd_manages(int ifindex) {
    char path[64], line[128];
    snprintf(path, sizeof path, "/run/systemd/netif/links/%d", ifindex);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int managed = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "ADMIN_STATE=", 12)) {
            line[strcspn(line, "\n")] = 0;
            managed = strcmp(line + 12, "unmanaged") && strcmp(line + 12, "pending") && strcmp(line + 12, "initialized");
        }
    fclose(f);
    return managed;
}

static int ifupdown_manages_file(const char *path, const char *ifname, int depth) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char w1[32], w2[64];
        if (sscanf(line, "%31s %63s", w1, w2) != 2) continue;
        if ((!strcmp(w1, "iface") || !strcmp(w1, "auto") || !strcmp(w1, "allow-hotplug")) && !strcmp(w2, ifname)) found = 1;
        if (!strcmp(w1, "source-directory") && depth == 0 && !strcmp(w2, "/etc/network/interfaces.d")) {
            DIR *d = opendir(w2);
            struct dirent *e;
            while (d && !found && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char p[340];
                snprintf(p, sizeof p, "%s/%s", w2, e->d_name);
                found = ifupdown_manages_file(p, ifname, 1);
            }
            if (d) closedir(d);
        }
    }
    fclose(f);
    return found;
}

/* NetworkManager (Raspberry Pi OS, most desktop distributions, netplan with renderer NetworkManager)
 * writes one keyfile per device under /run/NetworkManager/devices/<ifindex> with "managed=true" while it
 * drives it. Read without D-Bus; if NetworkManager isn't running the folder is gone (it lives in /run). */
static int networkmanager_manages(int ifindex) {
    char path[64], line[128];
    snprintf(path, sizeof path, "/run/NetworkManager/devices/%d", ifindex);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int managed = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, "managed=true")) managed = 1;
    }
    fclose(f);
    return managed;
}

int managed_by_system(const char *ifname) {
    if (!strcmp(ifname, "lo")) return 1;
    int idx = (int)if_nametoindex(ifname);
    if (idx && networkd_manages(idx)) return 1;
    if (idx && networkmanager_manages(idx)) return 1;
    /* /etc/network/interfaces only counts if ifupdown is installed (live-boot writes that
     * file even without ifupdown: the interface would be left with nobody configuring it) */
    if (access("/sbin/ifup", X_OK) && access("/usr/sbin/ifup", X_OK)) return 0;
    return ifupdown_manages_file("/etc/network/interfaces", ifname, 0);
}

int resolved_active(void) {
    char buf[64];
    ssize_t n = readlink("/etc/resolv.conf", buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "systemd/resolve") != NULL && access("/run/systemd/resolve/resolv.conf", R_OK) == 0;
}

/* ---- dumps ---- */

typedef struct {
    int index, flags, arphrd, mtu, master;
    char name[IFNAMSIZ], kind[24], mac[24], oper[12];
    struct rtnl_link_stats64 st;
    int has_st;
} Link;

typedef struct { Link *v; int n, cap; } Links;

static const char *oper_name(int o) {
    static const char *const n[] = { "unknown", "notpresent", "down", "lowerlayerdown", "testing", "dormant", "up" };
    return o >= 0 && o <= 6 ? n[o] : "unknown";
}

static void link_cb(const struct nlmsghdr *h, void *ud) {
    Links *l = ud;
    if (h->nlmsg_type != RTM_NEWLINK) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        Link *v = realloc(l->v, sizeof *v * (size_t)cap);
        if (!v) return;
        l->v = v; l->cap = cap;
    }
    struct ifinfomsg *ifi = NLMSG_DATA(h);
    struct rtattr *tb[IFLA_MAX + 1];
    nl_parse(tb, IFLA_MAX, IFLA_RTA(ifi), IFLA_PAYLOAD(h));
    Link *k = &l->v[l->n];
    memset(k, 0, sizeof *k);
    k->index = ifi->ifi_index;
    k->flags = (int)ifi->ifi_flags;
    k->arphrd = ifi->ifi_type;
    if (!tb[IFLA_IFNAME]) return;
    snprintf(k->name, sizeof k->name, "%s", (char *)RTA_DATA(tb[IFLA_IFNAME]));
    if (tb[IFLA_MTU]) k->mtu = (int)*(uint32_t *)RTA_DATA(tb[IFLA_MTU]);
    if (tb[IFLA_MASTER]) k->master = (int)*(uint32_t *)RTA_DATA(tb[IFLA_MASTER]);
    snprintf(k->oper, sizeof k->oper, "%s", tb[IFLA_OPERSTATE] ? oper_name(*(uint8_t *)RTA_DATA(tb[IFLA_OPERSTATE])) : "unknown");
    if (tb[IFLA_ADDRESS] && RTA_PAYLOAD(tb[IFLA_ADDRESS]) == 6) {
        const uint8_t *m = RTA_DATA(tb[IFLA_ADDRESS]);
        snprintf(k->mac, sizeof k->mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    if (tb[IFLA_STATS64] && RTA_PAYLOAD(tb[IFLA_STATS64]) >= sizeof k->st) { memcpy(&k->st, RTA_DATA(tb[IFLA_STATS64]), sizeof k->st); k->has_st = 1; }
    if (tb[IFLA_LINKINFO]) {
        struct rtattr *li[IFLA_INFO_MAX + 1];
        nl_parse(li, IFLA_INFO_MAX, RTA_DATA(tb[IFLA_LINKINFO]), RTA_PAYLOAD(tb[IFLA_LINKINFO]));
        if (li[IFLA_INFO_KIND]) {
            size_t n = RTA_PAYLOAD(li[IFLA_INFO_KIND]);
            if (n >= sizeof k->kind) n = sizeof k->kind - 1;
            memcpy(k->kind, RTA_DATA(li[IFLA_INFO_KIND]), n);
            k->kind[n] = 0;
        }
    }
    l->n++;
}

static int dump_links(Links *l) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    NlMsg m;
    nlm_init(&m, RTM_GETLINK, NLM_F_DUMP);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
    nlm_put(&m, &ifi, sizeof ifi);
    int r = nl_dump(fd, &m, link_cb, l);
    close(fd);
    return r;
}

typedef struct { Buf *b; int ifindex; int first; } AddrCtx;

static void addr_cb(const struct nlmsghdr *h, void *ud) {
    AddrCtx *c = ud;
    if (h->nlmsg_type != RTM_NEWADDR) return;
    struct ifaddrmsg *ifa = NLMSG_DATA(h);
    if ((int)ifa->ifa_index != c->ifindex) return;
    struct rtattr *tb[IFA_MAX + 1];
    nl_parse(tb, IFA_MAX, IFA_RTA(ifa), IFA_PAYLOAD(h));
    struct rtattr *a = tb[IFA_LOCAL] ? tb[IFA_LOCAL] : tb[IFA_ADDRESS];
    if (!a) return;
    char ip[64];
    fmt_ip(ifa->ifa_family, RTA_DATA(a), ip, sizeof ip);
    uint32_t flags = tb[IFA_FLAGS] ? *(uint32_t *)RTA_DATA(tb[IFA_FLAGS]) : ifa->ifa_flags;
    const char *scope = ifa->ifa_scope == RT_SCOPE_UNIVERSE ? "global" : ifa->ifa_scope == RT_SCOPE_LINK ? "link" : ifa->ifa_scope == RT_SCOPE_HOST ? "host" : "site";
    long valid = -1;
    if (tb[IFA_CACHEINFO]) {
        struct ifa_cacheinfo *ci = RTA_DATA(tb[IFA_CACHEINFO]);
        if (ci->ifa_valid != 0xFFFFFFFFu) valid = ci->ifa_valid;
    }
    buf_printf(c->b, "%s{\"family\":%d,\"address\":\"%s\",\"prefix\":%d,\"scope\":\"%s\",\"dynamic\":%s,\"validLifetime\":%ld}",
               c->first ? "" : ",", ifa->ifa_family == AF_INET ? 4 : 6, ip, ifa->ifa_prefixlen, scope,
               (flags & IFA_F_PERMANENT) ? "false" : "true", valid);
    c->first = 0;
}

static void dump_addrs(int fd, Buf *b, int ifindex) {
    AddrCtx c = { b, ifindex, 1 };
    NlMsg m;
    nlm_init(&m, RTM_GETADDR, NLM_F_DUMP);
    struct ifaddrmsg ifa = { .ifa_family = AF_UNSPEC };
    nlm_put(&m, &ifa, sizeof ifa);
    nl_dump(fd, &m, addr_cb, &c);
}

static const char *link_type(const Link *k) {
    char p[96];
    if (k->arphrd == ARPHRD_LOOPBACK) return "loopback";
    if (!strcmp(k->kind, "wireguard")) return "wireguard";
    if (!strcmp(k->kind, "tun")) return "tun";
    if (!strcmp(k->kind, "bridge")) return "bridge";
    if (k->kind[0]) return k->kind;                     /* veth, vlan, bond, dummy... */
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless", k->name);
    if (access(p, F_OK) == 0) return "wifi";
    snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", k->name);
    if (access(p, F_OK) == 0) return "wifi";
    if (k->arphrd == ARPHRD_ETHER) return "ethernet";
    if (k->arphrd == ARPHRD_NONE) return "tun";
    return "other";
}

char *net_if_config_json(const char *ifname) {
    if (!valid_ifname(ifname)) return NULL;
    char path[128];
    snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", ifname);
    char *t = read_file(path, 8192, NULL);
    Buf b = {0};
    if (!t) { buf_printf(&b, "null"); return b.p; }
    char v[256];
    kv_get(t, "method", 0, v, sizeof v);
    buf_printf(&b, "{\"method\":");
    buf_json_str(&b, v);
    buf_printf(&b, ",\"addresses\":[");
    for (int i = 0; kv_get(t, "address", i, v, sizeof v); i++) { if (i) buf_printf(&b, ","); buf_json_str(&b, v); }
    kv_get(t, "gateway", 0, v, sizeof v);
    buf_printf(&b, "],\"gateway\":");
    buf_json_str(&b, v);
    buf_printf(&b, ",\"dns\":[");
    for (int i = 0; kv_get(t, "dns", i, v, sizeof v); i++) { if (i) buf_printf(&b, ","); buf_json_str(&b, v); }
    buf_printf(&b, "]}");
    free(t);
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

char *net_interfaces_json(void) {
    Links l = {0};
    if (dump_links(&l) < 0) { free(l.v); return NULL; }
    int fd = nl_open(NETLINK_ROUTE, 0);
    Buf b = {0};
    buf_printf(&b, "[");
    for (int i = 0; i < l.n; i++) {
        Link *k = &l.v[i];
        const char *type = link_type(k);
        char conf[128];
        snprintf(conf, sizeof conf, NET_IF_DIR "/%s.conf", k->name);
        const char *managed = managed_by_system(k->name) ? "system" : access(conf, F_OK) == 0 ? "lordos" : "none";
#ifdef LORD_STANDALONE
        managed = "system";
#endif
        if (!strcmp(type, "wireguard") || !strcmp(type, "tun")) {
            /* those of LordOS's VPNs */
            char meta[128];
            snprintf(meta, sizeof meta, NET_RUN_DIR "/vpn/%s", k->name);
            if (access(meta, F_OK) == 0) managed = "lordos";
        }
        char master[IFNAMSIZ] = "";
        if (k->master) if_indextoname((unsigned)k->master, master);
        buf_printf(&b, "%s{\"name\":", i ? "," : "");
        buf_json_str(&b, k->name);
        buf_printf(&b, ",\"index\":%d,\"type\":\"%s\",\"kind\":", k->index, type);
        buf_json_str(&b, k->kind);
        buf_printf(&b, ",\"up\":%s,\"carrier\":%s,\"operstate\":\"%s\",\"mac\":\"%s\",\"mtu\":%d,\"master\":",
                   (k->flags & IFF_UP) ? "true" : "false", (k->flags & IFF_LOWER_UP) ? "true" : "false", k->oper, k->mac, k->mtu);
        buf_json_str(&b, master);
        buf_printf(&b, ",\"managed\":\"%s\",\"rxBytes\":%llu,\"txBytes\":%llu,\"rxPackets\":%llu,\"txPackets\":%llu,\"rxErrors\":%llu,\"txErrors\":%llu,\"addresses\":[",
                   managed, (unsigned long long)k->st.rx_bytes, (unsigned long long)k->st.tx_bytes,
                   (unsigned long long)k->st.rx_packets, (unsigned long long)k->st.tx_packets,
                   (unsigned long long)k->st.rx_errors, (unsigned long long)k->st.tx_errors);
        if (fd >= 0) dump_addrs(fd, &b, k->index);
        char *cfg = net_if_config_json(k->name);
        buf_printf(&b, "],\"config\":%s}", cfg ? cfg : "null");
        free(cfg);
    }
    buf_printf(&b, "]");
    if (fd >= 0) close(fd);
    free(l.v);
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

typedef struct { Buf *b; int first; } RouteCtx;

static void route_cb(const struct nlmsghdr *h, void *ud) {
    RouteCtx *c = ud;
    if (h->nlmsg_type != RTM_NEWROUTE) return;
    struct rtmsg *rt = NLMSG_DATA(h);
    struct rtattr *tb[RTA_MAX + 1];
    nl_parse(tb, RTA_MAX, RTM_RTA(rt), RTM_PAYLOAD(h));
    uint32_t table = tb[RTA_TABLE] ? *(uint32_t *)RTA_DATA(tb[RTA_TABLE]) : rt->rtm_table;
    if (rt->rtm_type != RTN_UNICAST || table == RT_TABLE_LOCAL) return;
    if (rt->rtm_family == AF_INET6 && rt->rtm_dst_len == 64 && tb[RTA_DST] && ((uint8_t *)RTA_DATA(tb[RTA_DST]))[0] == 0xfe) return;
    char dst[80] = "default", gw[64] = "", dev[IFNAMSIZ] = "";
    if (rt->rtm_dst_len && tb[RTA_DST]) {
        char ip[64];
        fmt_ip(rt->rtm_family, RTA_DATA(tb[RTA_DST]), ip, sizeof ip);
        snprintf(dst, sizeof dst, "%s/%d", ip, rt->rtm_dst_len);
    }
    if (tb[RTA_GATEWAY]) fmt_ip(rt->rtm_family, RTA_DATA(tb[RTA_GATEWAY]), gw, sizeof gw);
    if (tb[RTA_OIF]) if_indextoname(*(uint32_t *)RTA_DATA(tb[RTA_OIF]), dev);
    uint32_t metric = tb[RTA_PRIORITY] ? *(uint32_t *)RTA_DATA(tb[RTA_PRIORITY]) : 0;
    const char *proto;
    char pbuf[12];
    switch (rt->rtm_protocol) {
    case RTPROT_KERNEL: proto = "kernel"; break;
    case RTPROT_BOOT: proto = "boot"; break;
    case RTPROT_STATIC: proto = "static"; break;
    case RTPROT_DHCP: proto = "dhcp"; break;
    case RTPROT_RA: proto = "ra"; break;
    default: snprintf(pbuf, sizeof pbuf, "%d", rt->rtm_protocol); proto = pbuf;
    }
    buf_printf(c->b, "%s{\"family\":%d,\"dst\":\"%s\",\"gateway\":\"%s\",\"dev\":\"%s\",\"metric\":%u,\"table\":%u,\"protocol\":\"%s\"}",
               c->first ? "" : ",", rt->rtm_family == AF_INET ? 4 : 6, dst, gw, dev, metric, table, proto);
    c->first = 0;
}

char *net_routes_json(void) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return NULL;
    Buf b = {0};
    buf_printf(&b, "[");
    RouteCtx c = { &b, 1 };
    int fams[2] = { AF_INET, AF_INET6 };
    for (int i = 0; i < 2; i++) {
        NlMsg m;
        nlm_init(&m, RTM_GETROUTE, NLM_F_DUMP);
        struct rtmsg rt = { .rtm_family = (uint8_t)fams[i] };
        nlm_put(&m, &rt, sizeof rt);
        nl_dump(fd, &m, route_cb, &c);
    }
    buf_printf(&b, "]");
    close(fd);
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

char *net_dns_json(void) {
    int resolved = resolved_active();
    const char *path = resolved ? "/run/systemd/resolve/resolv.conf" : "/etc/resolv.conf";
    FILE *f = fopen(path, "r");
    Buf b = {0};
    buf_printf(&b, "{\"source\":\"%s\",\"servers\":[", resolved ? "systemd-resolved" : "resolv.conf");
    char line[512], search[512] = "";
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {
        char w[16], v[256];
        if (sscanf(line, "%15s %255s", w, v) != 2) continue;
        if (!strcmp(w, "nameserver")) { if (n++) buf_printf(&b, ","); buf_json_str(&b, v); }
        if (!strcmp(w, "search") || !strcmp(w, "domain")) {
            char *rest = line + strlen(w);
            rest[strcspn(rest, "\n")] = 0;
            snprintf(search, sizeof search, "%s", rest);
        }
    }
    if (f) fclose(f);
    buf_printf(&b, "],\"search\":[");
    n = 0;
    for (char *tok = strtok(search, " \t"); tok; tok = strtok(NULL, " \t")) { if (n++) buf_printf(&b, ","); buf_json_str(&b, tok); }
    buf_printf(&b, "]}");
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

/* Concatenates the (public) .json files of a folder into an array. */
static char *dir_json_array(const char *dir) {
    Buf b = {0};
    buf_printf(&b, "[");
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        char *t = read_file(p, 8192, NULL);
        if (!t || t[0] != '{') { free(t); continue; }
        t[strcspn(t, "\n")] = 0;
        buf_printf(&b, "%s%s", n++ ? "," : "", t);
        free(t);
    }
    if (d) closedir(d);
    buf_printf(&b, "]");
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

char *net_vpn_list_json(void) { return dir_json_array(NET_VPN_DIR); }
char *net_wifi_known_json(void) { return dir_json_array(NET_WIFI_DIR); }

/* ---- .ovpn with files next to it ---- */

static const char *const inline_dirs[] = { "ca", "cert", "key", "tls-auth", "tls-crypt", "tls-crypt-v2", "extra-certs", "crl-verify", NULL };

char *net_ovpn_inline(const char *path, char *err, size_t errn) {
    size_t len;
    char *t = read_file(path, 256 * 1024, &len);
    if (!t) { lerr_set(err, errn, "sys.readFailed", "path", path, NULL); return NULL; }
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    Buf b = {0};
    for (char *l = t; l && *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char w[32] = "", a1[400] = "", a2[16] = "";
        int k = sscanf(l, "%31s %399s %15s", w, a1, a2);
        int done = 0;
        for (int i = 0; k >= 2 && inline_dirs[i] && !done; i++) {
            if (strcmp(w, inline_dirs[i]) || !strcmp(a1, "[inline]")) continue;
            char p[1024];
            if (a1[0] == '/') snprintf(p, sizeof p, "%s", a1); else snprintf(p, sizeof p, "%s/%s", dir, a1);
            char *f = read_file(p, 64 * 1024, NULL);
            if (!f) { lerr_set(err, errn, "ovpn.missingFile", "file", a1, NULL); free(t); buf_free(&b); return NULL; }
            buf_printf(&b, "<%s>\n%s%s</%s>\n", w, f, f[0] && f[strlen(f) - 1] == '\n' ? "" : "\n", w);
            if (!strcmp(w, "tls-auth") && k == 3) buf_printf(&b, "key-direction %s\n", a2);
            free(f);
            done = 1;
        }
        if (!done && k >= 2 && !strcmp(w, "auth-user-pass")) { buf_printf(&b, "auth-user-pass\n"); done = 1; }
        if (!done) buf_printf(&b, "%s\n", l);
        l = e ? e + 1 : NULL;
    }
    free(t);
    if (b.oom || !b.p) { buf_free(&b); lerr_set(err, errn, "sys.outOfMemory", NULL); return NULL; }
    return b.p;
}

/* ---- notifications ---- */

static int mon_fd = -1;

int net_open(void) {
    mon_fd = nl_open(NETLINK_ROUTE, RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE);
    if (mon_fd < 0) return -1;
    fcntl(mon_fd, F_SETFL, fcntl(mon_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

int net_fd(void) { return mon_fd; }

void net_dispatch(void (*fn)(const char *what)) {
    char buf[16384];
    int link = 0, addr = 0, route = 0;
    for (;;) {
        ssize_t r = recv(mon_fd, buf, sizeof buf, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == ENOBUFS) { link = addr = route = 1; continue; }   /* notifications were lost */
        if (r <= 0) break;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)r); h = NLMSG_NEXT(h, r)) {
            if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) link = 1;
            else if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) addr = 1;
            else if (h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE) route = 1;
        }
    }
    if (link) fn("link");
    if (addr) fn("addr");
    if (route) fn("route");
}
