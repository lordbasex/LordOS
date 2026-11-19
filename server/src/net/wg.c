/* wg.c — WireGuard with our own generic netlink (no wg-quick or wireguard-tools), root side.
 *
 * It imports the usual .conf ([Interface]/[Peer]), saves it normalized (only the keys we
 * understand) in /etc/lordos/net/vpn/<name>.conf (0600) with its public metadata in
 * <name>.json (0644). On connect: creates the "wireguard" interface, passes it keys and peers
 * (WG_CMD_SET_DEVICE), addresses, MTU, AllowedIPs routes and DNS. With 0.0.0.0/0 or ::/0 it does
 * the same as wg-quick: fwmark 51820 on the tunnel, default route in table 51820 and the
 * rules "not fwmark 51820 table 51820" and "table main suppress_prefixlength 0".
 * PostUp/PreDown and friends (commands) are never executed: they're ignored with a warning. */
#include "netpriv.h"

#include <net/if.h>                 /* before the linux/ ones */
#include "../admin.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireguard.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WG_TABLE 51820
#define MAX_PEERS 16
#define MAX_IPS 32

typedef struct {
    uint8_t pub[32], psk[32];
    int has_psk;
    char endpoint[200];
    Cidr allowed[MAX_IPS];
    int nallowed;
    int keepalive;
} Peer;

typedef struct {
    uint8_t priv[32];
    int has_priv, generate;
    Cidr addr[8];
    int naddr;
    char dns[8][64];            /* IPs */
    int ndns;
    char search[8][64];         /* domains */
    int nsearch;
    int mtu, listen;
    Peer peer[MAX_PEERS];
    int npeer;
    int ignored;                /* PostUp & co. */
} WgConf;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int valid_domain(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 63) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.' && *p != '~') return 0;
    return 1;
}

/* host:port, [v6]:port, or v6 without brackets not allowed (ambiguous). */
static int valid_endpoint(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= 200) return 0;
    const char *colon = strrchr(s, ':');
    if (!colon || !colon[1]) return 0;
    for (const char *p = colon + 1; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    long port = atol(colon + 1);
    if (port < 1 || port > 65535) return 0;
    for (const char *p = s; p < colon; p++) if (!isalnum((unsigned char)*p) && !strchr("-.:[]", *p)) return 0;
    return 1;
}

static int key32(const char *b64, uint8_t out[32]) {
    uint8_t tmp[40];
    return strlen(b64) == 44 && b64_decode(b64, tmp, sizeof tmp) == 32 ? (memcpy(out, tmp, 32), 0) : -1;
}

/* Parses the text (the user's .conf or the saved one). err explains the first bad line. */
static int wg_parse(const char *text, WgConf *c, char *err, size_t errn) {
    memset(c, 0, sizeof *c);
    char *copy = strdup(text);
    if (!copy) { lerr_set(err, errn, "sys.outOfMemory", NULL); return -1; }
    int section = 0, lineno = 0, rc = 0;
    for (char *l = copy, *next; l && !rc; l = next) {
        next = strchr(l, '\n');
        if (next) *next++ = 0;
        lineno++;
        char *h = strchr(l, '#');
        if (h) *h = 0;
        l = trim(l);
        if (!*l) continue;
        if (!strcasecmp(l, "[Interface]")) { section = 1; continue; }
        if (!strcasecmp(l, "[Peer]")) {
            if (c->npeer >= MAX_PEERS) { lerr_set(err, errn, "wg.tooManyPeers", NULL); rc = -1; break; }
            section = 2; c->npeer++;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq || !section) { lerr_set(err, errn, "vpn.expectedKeyValue", "line", lerr_int(lineno), NULL); rc = -1; break; }
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        Peer *p = section == 2 ? &c->peer[c->npeer - 1] : NULL;
        if (!strcasecmp(k, "PostUp") || !strcasecmp(k, "PostDown") || !strcasecmp(k, "PreUp") || !strcasecmp(k, "PreDown") ||
            !strcasecmp(k, "SaveConfig") || !strcasecmp(k, "Table")) { c->ignored++; continue; }
        if (section == 1 && !strcasecmp(k, "PrivateKey")) {
            if (!strcmp(v, "generate")) c->generate = 1;
            else if (key32(v, c->priv)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "PrivateKey", NULL); rc = -1; }
            else c->has_priv = 1;
        } else if (section == 1 && !strcasecmp(k, "Address")) {
            for (char *t = strtok(v, ","); t && !rc; t = strtok(NULL, ",")) {
                t = trim(t);
                if (c->naddr >= 8 || parse_cidr(t, &c->addr[c->naddr], 0)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "Address", "value", t, NULL); rc = -1; }
                else c->naddr++;
            }
        } else if (section == 1 && !strcasecmp(k, "DNS")) {
            for (char *t = strtok(v, ","); t && !rc; t = strtok(NULL, ",")) {
                t = trim(t);
                int fam;
                uint8_t a[16];
                if (!parse_ip(t, &fam, a)) { if (c->ndns < 8) snprintf(c->dns[c->ndns++], 64, "%s", t); }
                else if (valid_domain(t)) { if (c->nsearch < 8) snprintf(c->search[c->nsearch++], 64, "%s", t); }
                else { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "DNS", "value", t, NULL); rc = -1; }
            }
        } else if (section == 1 && !strcasecmp(k, "MTU")) {
            c->mtu = atoi(v);
            if (c->mtu < 576 || c->mtu > 9000) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "MTU", NULL); rc = -1; }
        } else if (section == 1 && !strcasecmp(k, "ListenPort")) {
            c->listen = atoi(v);
            if (c->listen < 1 || c->listen > 65535) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "ListenPort", NULL); rc = -1; }
        } else if (section == 1 && !strcasecmp(k, "FwMark")) {
            c->ignored++;
        } else if (p && !strcasecmp(k, "PublicKey")) {
            if (key32(v, p->pub)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "PublicKey", NULL); rc = -1; }
        } else if (p && !strcasecmp(k, "PresharedKey")) {
            if (key32(v, p->psk)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "PresharedKey", NULL); rc = -1; }
            else p->has_psk = 1;
        } else if (p && !strcasecmp(k, "Endpoint")) {
            if (!valid_endpoint(v)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "Endpoint", NULL); rc = -1; }
            else snprintf(p->endpoint, sizeof p->endpoint, "%s", v);
        } else if (p && !strcasecmp(k, "AllowedIPs")) {
            for (char *t = strtok(v, ","); t && !rc; t = strtok(NULL, ",")) {
                t = trim(t);
                if (!*t) continue;
                if (p->nallowed >= MAX_IPS || parse_cidr(t, &p->allowed[p->nallowed], 0)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "AllowedIPs", "value", t, NULL); rc = -1; }
                else p->nallowed++;
            }
        } else if (p && !strcasecmp(k, "PersistentKeepalive")) {
            p->keepalive = !strcasecmp(v, "off") ? 0 : atoi(v);
            if (p->keepalive < 0 || p->keepalive > 65535) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", "PersistentKeepalive", NULL); rc = -1; }
        } else { lerr_set(err, errn, "vpn.unknownKey", "line", lerr_int(lineno), "key", k, NULL); rc = -1; }
    }
    free(copy);
    if (!rc && !c->has_priv && !c->generate) { lerr_set(err, errn, "vpn.missingKey", "key", "PrivateKey", NULL); rc = -1; }
    if (!rc && !c->npeer) { lerr_set(err, errn, "vpn.missingKey", "key", "[Peer]", NULL); rc = -1; }
    for (int i = 0; !rc && i < c->npeer; i++) {
        uint8_t zero[32] = {0};
        if (!memcmp(c->peer[i].pub, zero, 32)) { lerr_set(err, errn, "wg.peerNoPublicKey", NULL); rc = -1; }
    }
    return rc;
}

static void cidr_str(const Cidr *c, char *out, size_t n) {
    char ip[64];
    fmt_ip(c->family, c->a, ip, sizeof ip);
    snprintf(out, n, "%s/%d", ip, c->prefix);
}

/* Rewrites the config without what we don't use (and with the generated key). */
static void wg_emit(const WgConf *c, Buf *b) {
    char k[64], s[80];
    b64_encode(c->priv, 32, k, sizeof k);
    buf_printf(b, "[Interface]\nPrivateKey = %s\n", k);
    for (int i = 0; i < c->naddr; i++) { cidr_str(&c->addr[i], s, sizeof s); buf_printf(b, "Address = %s\n", s); }
    for (int i = 0; i < c->ndns; i++) buf_printf(b, "DNS = %s\n", c->dns[i]);
    for (int i = 0; i < c->nsearch; i++) buf_printf(b, "DNS = %s\n", c->search[i]);
    if (c->mtu) buf_printf(b, "MTU = %d\n", c->mtu);
    if (c->listen) buf_printf(b, "ListenPort = %d\n", c->listen);
    for (int i = 0; i < c->npeer; i++) {
        const Peer *p = &c->peer[i];
        b64_encode(p->pub, 32, k, sizeof k);
        buf_printf(b, "\n[Peer]\nPublicKey = %s\n", k);
        if (p->has_psk) { b64_encode(p->psk, 32, k, sizeof k); buf_printf(b, "PresharedKey = %s\n", k); }
        if (p->endpoint[0]) buf_printf(b, "Endpoint = %s\n", p->endpoint);
        for (int j = 0; j < p->nallowed; j++) { cidr_str(&p->allowed[j], s, sizeof s); buf_printf(b, "AllowedIPs = %s\n", s); }
        if (p->keepalive) buf_printf(b, "PersistentKeepalive = %d\n", p->keepalive);
    }
}

void vpn_ifname(const char *type, const char *name, char *out, size_t n) {
    /* prefix by type; the total fits in IFNAMSIZ - 1 = 15 */
    const char *pre = !strcmp(type, "wireguard") ? "wg" : !strcmp(type, "openvpn") ? "ovpn" : !strcmp(type, "ipsec") ? "ips"
                    : !strcmp(type, "pptp") ? "ppp" : !strcmp(type, "openconnect") ? "oc" : "fvpn";
    snprintf(out, n, "%s-%.*s", pre, (int)(14 - strlen(pre)), name);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

int wg_import(const char *name, const char *text, char *pub_out, char *err, size_t errn) {
    WgConf c;
    if (wg_parse(text, &c, err, errn)) return -1;
    if (c.generate) { if (random_bytes(c.priv, 32)) { lerr_set(err, errn, "wg.noRandom", NULL); return -1; } x25519_clamp(c.priv); }
    uint8_t pub[32];
    x25519_public(pub, c.priv);
    b64_encode(pub, 32, pub_out, 64);
    if (c.ignored) admin_log("wg.ignoredLines", "count", lerr_int(c.ignored), NULL);
    Buf b = {0};
    wg_emit(&c, &b);
    char path[160];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.conf", name);
    int ok = b.p && write_file_atomic(path, b.p, b.len, 0600) == 0;
    if (b.p) memset(b.p, 0, b.len);
    buf_free(&b);
    memset(&c.priv, 0, sizeof c.priv);
    /* public metadata */
    char ifn[IFNAMSIZ], addr[80] = "";
    vpn_ifname("wireguard", name, ifn, sizeof ifn);
    if (c.naddr) cidr_str(&c.addr[0], addr, sizeof addr);
    int full = 0;
    for (int i = 0; i < c.npeer; i++) for (int j = 0; j < c.peer[i].nallowed; j++) if (!c.peer[i].allowed[j].prefix) full = 1;
    Buf m = {0};
    buf_printf(&m, "{\"name\":");
    buf_json_str(&m, name);
    buf_printf(&m, ",\"type\":\"wireguard\",\"ifname\":\"%s\",\"server\":", ifn);
    buf_json_str(&m, c.peer[0].endpoint);
    buf_printf(&m, ",\"address\":");
    buf_json_str(&m, addr);
    buf_printf(&m, ",\"fullTunnel\":%s,\"publicKey\":\"%s\",\"peers\":%d}\n", full ? "true" : "false", pub_out, c.npeer);
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.json", name);
    ok = ok && m.p && write_file_atomic(path, m.p, m.len, 0644) == 0;
    buf_free(&m);
    if (!ok) { lerr_set(err, errn, "net.saveFailed", NULL); return -1; }
    return 0;
}

static int resolve_endpoint(const char *ep, struct sockaddr_storage *ss, socklen_t *len) {
    char host[200], port[8];
    const char *colon = strrchr(ep, ':');
    size_t hl = (size_t)(colon - ep);
    if (hl >= sizeof host) return -1;
    memcpy(host, ep, hl);
    host[hl] = 0;
    snprintf(port, sizeof port, "%s", colon + 1);
    char *h = host;
    if (h[0] == '[') { h++; char *e = strchr(h, ']'); if (e) *e = 0; }
    struct addrinfo hints = { .ai_socktype = SOCK_DGRAM, .ai_family = AF_UNSPEC }, *res = NULL;
    if (getaddrinfo(h, port, &hints, &res) || !res) return -1;
    struct addrinfo *pick = res;
    for (struct addrinfo *a = res; a; a = a->ai_next) if (a->ai_family == AF_INET) { pick = a; break; }
    memcpy(ss, pick->ai_addr, pick->ai_addrlen);
    *len = pick->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int wg_set_device(const char *ifn, const WgConf *c, uint32_t fwmark, char *err, size_t errn) {
    int fd = nl_open(NETLINK_GENERIC, 0);
    if (fd < 0) { lerr_set(err, errn, "net.netlinkFailed", NULL); return -1; }
    int fam = genl_family(fd, WG_GENL_NAME);
    if (fam < 0) { close(fd); lerr_set(err, errn, "wg.noKernel", NULL); return -1; }
    NlMsg m;
    nlm_init(&m, (uint16_t)fam, 0);
    struct genlmsghdr g = { .cmd = WG_CMD_SET_DEVICE, .version = WG_GENL_VERSION };
    nlm_put(&m, &g, sizeof g);
    nlm_attr_str(&m, WGDEVICE_A_IFNAME, ifn);
    nlm_attr(&m, WGDEVICE_A_PRIVATE_KEY, c->priv, 32);
    nlm_attr_u32(&m, WGDEVICE_A_FLAGS, WGDEVICE_F_REPLACE_PEERS);
    nlm_attr_u16(&m, WGDEVICE_A_LISTEN_PORT, (uint16_t)c->listen);
    nlm_attr_u32(&m, WGDEVICE_A_FWMARK, fwmark);
    size_t peers = nlm_nest_begin(&m, WGDEVICE_A_PEERS);
    for (int i = 0; i < c->npeer; i++) {
        const Peer *p = &c->peer[i];
        size_t pn = nlm_nest_begin(&m, (uint16_t)i);
        nlm_attr(&m, WGPEER_A_PUBLIC_KEY, p->pub, 32);
        nlm_attr_u32(&m, WGPEER_A_FLAGS, WGPEER_F_REPLACE_ALLOWEDIPS);
        if (p->has_psk) nlm_attr(&m, WGPEER_A_PRESHARED_KEY, p->psk, 32);
        if (p->endpoint[0]) {
            struct sockaddr_storage ss;
            socklen_t sl;
            if (resolve_endpoint(p->endpoint, &ss, &sl)) { nlm_free(&m); close(fd); lerr_set(err, errn, "vpn.resolveFailed", "host", p->endpoint, NULL); return -1; }
            nlm_attr(&m, WGPEER_A_ENDPOINT, &ss, sl);
        }
        nlm_attr_u16(&m, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, (uint16_t)p->keepalive);
        size_t ips = nlm_nest_begin(&m, WGPEER_A_ALLOWEDIPS);
        for (int j = 0; j < p->nallowed; j++) {
            size_t an = nlm_nest_begin(&m, (uint16_t)j);
            nlm_attr_u16(&m, WGALLOWEDIP_A_FAMILY, (uint16_t)p->allowed[j].family);
            nlm_attr(&m, WGALLOWEDIP_A_IPADDR, p->allowed[j].a, p->allowed[j].family == AF_INET ? 4 : 16);
            nlm_attr_u8(&m, WGALLOWEDIP_A_CIDR_MASK, (uint8_t)p->allowed[j].prefix);
            nlm_nest_end(&m, an);
        }
        nlm_nest_end(&m, ips);
        nlm_nest_end(&m, pn);
    }
    nlm_nest_end(&m, peers);
    int r = nl_talk(fd, &m);
    close(fd);
    if (r) lerr_set(err, errn, "wg.setDeviceFailed", "errno", lerr_errno(-r), NULL);
    return r ? -1 : 0;
}

static void write_sysctl(const char *path, const char *v) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(v, f); fclose(f); }
}

static int load_conf(const char *name, WgConf *c, char *err, size_t errn) {
    char path[160];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.conf", name);
    char *t = read_file(path, 65536, NULL);
    if (!t) { lerr_set(err, errn, "vpn.unknown", "name", name, NULL); return -1; }
    int r = wg_parse(t, c, err, errn);
    memset(t, 0, strlen(t));
    free(t);
    return r;
}

static void full_tunnel_rules(int fd, int add, int family) {
    rt_rule(fd, add, family, 0, 0, RT_TABLE_MAIN, 0, 32764);          /* table main suppress_prefixlength 0 */
    rt_rule(fd, add, family, WG_TABLE, 1, WG_TABLE, -1, 32765);       /* not fwmark 51820 table 51820 */
}

int wg_up(const char *name, char *err, size_t errn) {
    WgConf c;
    if (load_conf(name, &c, err, errn)) return -1;
    char ifn[IFNAMSIZ];
    vpn_ifname("wireguard", name, ifn, sizeof ifn);
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) { lerr_set(err, errn, "net.netlinkFailed", NULL); return -1; }
    int idx = rt_ifindex(ifn);
    if (idx) { rt_link_del(fd, idx); idx = 0; }
    int r = rt_link_new(fd, ifn, "wireguard");
    if (r) { close(fd); lerr_set(err, errn, "vpn.createIfFailed", "name", ifn, "errno", lerr_errno(-r), NULL); return -1; }
    idx = rt_ifindex(ifn);
    int full4 = 0, full6 = 0;
    for (int i = 0; i < c.npeer; i++)
        for (int j = 0; j < c.peer[i].nallowed; j++)
            if (!c.peer[i].allowed[j].prefix) { if (c.peer[i].allowed[j].family == AF_INET) full4 = 1; else full6 = 1; }
    if (wg_set_device(ifn, &c, (full4 || full6) ? WG_TABLE : 0, err, errn)) { rt_link_del(fd, idx); close(fd); memset(&c, 0, sizeof c); return -1; }
    memset(c.priv, 0, sizeof c.priv);
    for (int i = 0; i < c.naddr; i++) rt_addr(fd, 1, idx, c.addr[i].family, c.addr[i].a, c.addr[i].prefix, 0, 0);
    rt_link_mtu(fd, idx, c.mtu ? (unsigned)c.mtu : 1420);
    rt_link_set(fd, idx, 1);
    for (int i = 0; i < c.npeer; i++)
        for (int j = 0; j < c.peer[i].nallowed; j++) {
            Cidr *a = &c.peer[i].allowed[j];
            if (!a->prefix) rt_route(fd, 1, a->family, a->a, 0, NULL, idx, 0, WG_TABLE);
            else rt_route(fd, 1, a->family, a->a, a->prefix, NULL, idx, 0, 0);
        }
    if (full4) { write_sysctl("/proc/sys/net/ipv4/conf/all/src_valid_mark", "1"); full_tunnel_rules(fd, 1, AF_INET); }
    if (full6) full_tunnel_rules(fd, 1, AF_INET6);
    close(fd);
    if (c.ndns || c.nsearch) {
        char servers[600] = "", domains[600] = "";
        for (int i = 0; i < c.ndns; i++) snprintf(servers + strlen(servers), sizeof servers - strlen(servers), "%s%s", i ? " " : "", c.dns[i]);
        for (int i = 0; i < c.nsearch; i++) snprintf(domains + strlen(domains), sizeof domains - strlen(domains), "%s%s", i ? " " : "", c.search[i]);
        dns_set(ifn, servers, domains, full4 || full6 ? DNS_FULL : DNS_SPLIT);
    }
    mkdir(NET_RUN_DIR, 0755);
    mkdir(NET_RUN_DIR "/vpn", 0755);
    char mark[128], content[80];
    snprintf(mark, sizeof mark, NET_RUN_DIR "/vpn/%s", ifn);
    int n = snprintf(content, sizeof content, "%s\n%d\n", name, (full4 ? 1 : 0) | (full6 ? 2 : 0));
    write_file_atomic(mark, content, (size_t)n, 0644);
    return 0;
}

int wg_down(const char *name) {
    char ifn[IFNAMSIZ], mark[128];
    vpn_ifname("wireguard", name, ifn, sizeof ifn);
    snprintf(mark, sizeof mark, NET_RUN_DIR "/vpn/%s", ifn);
    char *t = read_file(mark, 256, NULL);
    int full = 0;
    if (t) { char *nl = strchr(t, '\n'); if (nl) full = atoi(nl + 1); free(t); }
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    if (full & 1) full_tunnel_rules(fd, 0, AF_INET);
    if (full & 2) full_tunnel_rules(fd, 0, AF_INET6);
    int idx = rt_ifindex(ifn);
    if (idx) dns_revert(ifn);
    int r = idx ? rt_link_del(fd, idx) : -ENODEV;
    close(fd);
    unlink(mark);
    return r ? -1 : 0;
}

/* ---- state ---- */

typedef struct { Buf *b; int npeer; } StatusCtx;

static void status_cb(const struct nlmsghdr *h, void *ud) {
    StatusCtx *s = ud;
    struct rtattr *tb[WGDEVICE_A_MAX + 1];
    nl_parse(tb, WGDEVICE_A_MAX, (char *)NLMSG_DATA(h) + GENL_HDRLEN, h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN);
    if (!tb[WGDEVICE_A_PEERS]) return;
    int l = (int)RTA_PAYLOAD(tb[WGDEVICE_A_PEERS]);
    for (struct rtattr *p = RTA_DATA(tb[WGDEVICE_A_PEERS]); RTA_OK(p, l); p = RTA_NEXT(p, l)) {
        struct rtattr *pt[WGPEER_A_MAX + 1];
        nl_parse(pt, WGPEER_A_MAX, RTA_DATA(p), RTA_PAYLOAD(p));
        char pub[64] = "", ep[80] = "";
        if (pt[WGPEER_A_PUBLIC_KEY]) b64_encode(RTA_DATA(pt[WGPEER_A_PUBLIC_KEY]), 32, pub, sizeof pub);
        if (pt[WGPEER_A_ENDPOINT]) {
            struct sockaddr *sa = RTA_DATA(pt[WGPEER_A_ENDPOINT]);
            char ip[64];
            if (sa->sa_family == AF_INET) { struct sockaddr_in *s4 = (void *)sa; fmt_ip(AF_INET, &s4->sin_addr, ip, sizeof ip); snprintf(ep, sizeof ep, "%s:%d", ip, ntohs(s4->sin_port)); }
            else if (sa->sa_family == AF_INET6) { struct sockaddr_in6 *s6 = (void *)sa; fmt_ip(AF_INET6, &s6->sin6_addr, ip, sizeof ip); snprintf(ep, sizeof ep, "[%s]:%d", ip, ntohs(s6->sin6_port)); }
        }
        long long hs = 0;
        unsigned long long rx = 0, tx = 0;
        if (pt[WGPEER_A_LAST_HANDSHAKE_TIME]) { struct { int64_t s, ns; } *t = RTA_DATA(pt[WGPEER_A_LAST_HANDSHAKE_TIME]); hs = t->s; }
        if (pt[WGPEER_A_RX_BYTES]) memcpy(&rx, RTA_DATA(pt[WGPEER_A_RX_BYTES]), 8);
        if (pt[WGPEER_A_TX_BYTES]) memcpy(&tx, RTA_DATA(pt[WGPEER_A_TX_BYTES]), 8);
        buf_printf(s->b, "%s{\"publicKey\":\"%s\",\"endpoint\":\"%s\",\"lastHandshake\":%lld,\"rxBytes\":%llu,\"txBytes\":%llu}",
                   s->npeer++ ? "," : "", pub, ep, hs, rx, tx);
    }
}

int wg_status_json(const char *name, Buf *out) {
    char ifn[IFNAMSIZ];
    vpn_ifname("wireguard", name, ifn, sizeof ifn);
    int active = rt_ifindex(ifn) != 0;
    buf_printf(out, "{\"name\":");
    buf_json_str(out, name);
    buf_printf(out, ",\"type\":\"wireguard\",\"ifname\":\"%s\",\"active\":%s,\"peers\":[", ifn, active ? "true" : "false");
    if (active) {
        int fd = nl_open(NETLINK_GENERIC, 0);
        int fam = fd >= 0 ? genl_family(fd, WG_GENL_NAME) : -1;
        if (fam >= 0) {
            NlMsg m;
            nlm_init(&m, (uint16_t)fam, NLM_F_DUMP);
            struct genlmsghdr g = { .cmd = WG_CMD_GET_DEVICE, .version = WG_GENL_VERSION };
            nlm_put(&m, &g, sizeof g);
            nlm_attr_str(&m, WGDEVICE_A_IFNAME, ifn);
            StatusCtx s = { out, 0 };
            nl_dump(fd, &m, status_cb, &s);
        }
        if (fd >= 0) close(fd);
    }
    buf_printf(out, "]}");
    return 0;
}
