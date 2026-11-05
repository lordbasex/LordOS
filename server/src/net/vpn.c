/* vpn.c — common pieces of the VPNs that aren't WireGuard or OpenVPN (IPsec, PPTP, OpenConnect,
 * Fortinet): the key=value import format, saving (0600) and the public metadata, the routes
 * (full tunnel like OpenVPN's "def1" or individual routes) and the process that launches a
 * fixed-path program and reads its output.
 *
 * Import format: one "key=value" per line; empty lines and those starting with # are
 * ignored. A PEM value ("-----BEGIN ...") continues on the following lines until its
 * "-----END ...-----". Each type defines its allowed keys. */
#include "netpriv.h"
#include "vpn.h"
#include "net.h"
#include "../admin.h"

#include <net/if.h>                 /* before the linux/ ones */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- key=value ---- */

static int in_list(const char *const *l, const char *w) {
    for (int i = 0; l && l[i]; i++) if (!strcmp(l[i], w)) return 1;
    return 0;
}

int vpn_text_ok(const char *s, size_t max) {
    if (strlen(s) > max) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

int vpn_host_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 253) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr(".-:[]", *p)) return 0;
    return s[0] != '-';
}

int vpn_kv_parse(const char *text, const char *const *keys, const char *const *pem_keys, VpnKv *kv, char *err, size_t errn) {
    memset(kv, 0, sizeof *kv);
    char *copy = strdup(text);
    if (!copy) { lerr_set(err, errn, "sys.outOfMemory", NULL); return -1; }
    int lineno = 0, rc = 0;
    for (char *l = copy, *next; l && !rc; l = next) {
        next = strchr(l, '\n');
        if (next) *next++ = 0;
        lineno++;
        size_t n = strlen(l);
        if (n && l[n - 1] == '\r') l[--n] = 0;
        char *t = l;
        while (isspace((unsigned char)*t)) t++;
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) { lerr_set(err, errn, "vpn.expectedKeyValue", "line", lerr_int(lineno), NULL); rc = -1; break; }
        *eq = 0;
        char *k = t, *v = eq + 1;
        char *ke = k + strlen(k);
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
        while (isspace((unsigned char)*v)) v++;
        char *ve = v + strlen(v);
        while (ve > v && isspace((unsigned char)ve[-1])) *--ve = 0;
        if (in_list(pem_keys, k)) {
            if (strncmp(v, "-----BEGIN ", 11)) { lerr_set(err, errn, "vpn.pemRequired", "line", lerr_int(lineno), "key", k, NULL); rc = -1; break; }
            Buf b = {0};
            buf_printf(&b, "%s\n", v);
            int closed = strstr(v + 11, "-----END ") != NULL;
            while (!closed && next) {
                char *pl = next;
                next = strchr(pl, '\n');
                if (next) *next++ = 0;
                lineno++;
                size_t pn = strlen(pl);
                if (pn && pl[pn - 1] == '\r') pl[--pn] = 0;
                for (char *c = pl; *c; c++) if (!isalnum((unsigned char)*c) && !strchr("+/=- :,", *c)) { lerr_set(err, errn, "vpn.badChar", "line", lerr_int(lineno), "key", k, NULL); rc = -1; break; }
                if (rc) break;
                buf_printf(&b, "%s\n", pl);
                if (!strncmp(pl, "-----END ", 9)) closed = 1;
            }
            if (!rc && !closed) { lerr_set(err, errn, "vpn.pemUnclosed", "key", k, NULL); rc = -1; }
            if (rc || kv->npem >= VPN_MAX_PEM) { buf_free(&b); if (!rc) { lerr_set(err, errn, "vpn.tooManyPem", NULL); rc = -1; } break; }
            snprintf(kv->pem_key[kv->npem], sizeof kv->pem_key[0], "%s", k);
            kv->pem[kv->npem++] = b.p;
            continue;
        }
        if (!in_list(keys, k)) { lerr_set(err, errn, "vpn.unknownKey", "line", lerr_int(lineno), "key", k, NULL); rc = -1; break; }
        if (!vpn_text_ok(v, 255)) { lerr_set(err, errn, "vpn.invalidLineValue", "line", lerr_int(lineno), "key", k, NULL); rc = -1; break; }
        if (kv->n >= VPN_MAX_KV) { lerr_set(err, errn, "vpn.tooManyLines", NULL); rc = -1; break; }
        snprintf(kv->key[kv->n], sizeof kv->key[0], "%s", k);
        snprintf(kv->val[kv->n], sizeof kv->val[0], "%s", v);
        kv->n++;
    }
    memset(copy, 0, strlen(text));
    free(copy);
    if (rc) vpn_kv_free(kv);
    return rc;
}

const char *vpn_kv(const VpnKv *kv, const char *key) {
    for (int i = 0; i < kv->n; i++) if (!strcmp(kv->key[i], key)) return kv->val[i];
    return NULL;
}

const char *vpn_kv_pem(const VpnKv *kv, const char *key) {
    for (int i = 0; i < kv->npem; i++) if (!strcmp(kv->pem_key[i], key)) return kv->pem[i];
    return NULL;
}

void vpn_kv_free(VpnKv *kv) {
    for (int i = 0; i < kv->npem; i++) if (kv->pem[i]) { memset(kv->pem[i], 0, strlen(kv->pem[i])); free(kv->pem[i]); }
    memset(kv, 0, sizeof *kv);
}

/* Saves the config (0600): /etc/lordos/net/vpn/<name>.<type> with the keys and
 * <name>.<key>.pem with each PEM. */
int vpn_kv_save(const char *name, const char *type, const VpnKv *kv) {
    Buf b = {0};
    for (int i = 0; i < kv->n; i++) buf_printf(&b, "%s=%s\n", kv->key[i], kv->val[i]);
    char path[200];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.%s", name, type);
    int ok = !b.oom && write_file_atomic(path, b.p ? b.p : "", b.len, 0600) == 0;
    if (b.p) memset(b.p, 0, b.len);
    buf_free(&b);
    static const char *const pems[] = { "ca", "cert", "key", NULL };
    for (int i = 0; pems[i]; i++) {
        snprintf(path, sizeof path, NET_VPN_DIR "/%s.%s.pem", name, pems[i]);
        unlink(path);
    }
    for (int i = 0; ok && i < kv->npem; i++) {
        snprintf(path, sizeof path, NET_VPN_DIR "/%s.%s.pem", name, kv->pem_key[i]);
        ok = write_file_atomic(path, kv->pem[i], strlen(kv->pem[i]), 0600) == 0;
    }
    return ok ? 0 : -1;
}

int vpn_kv_load(const char *name, const char *type, VpnKv *kv) {
    memset(kv, 0, sizeof *kv);
    char path[200];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.%s", name, type);
    char *t = read_file(path, 65536, NULL);
    if (!t) return -1;
    for (char *l = t, *next; l && *l; l = next) {
        next = strchr(l, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(l, '=');
        if (!eq || kv->n >= VPN_MAX_KV) continue;
        *eq = 0;
        snprintf(kv->key[kv->n], sizeof kv->key[0], "%s", l);
        snprintf(kv->val[kv->n], sizeof kv->val[0], "%s", eq + 1);
        kv->n++;
    }
    memset(t, 0, strlen(t));
    free(t);
    static const char *const pems[] = { "ca", "cert", "key", NULL };
    for (int i = 0; pems[i] && kv->npem < VPN_MAX_PEM; i++) {
        snprintf(path, sizeof path, NET_VPN_DIR "/%s.%s.pem", name, pems[i]);
        char *p = read_file(path, 65536, NULL);
        if (!p) continue;
        snprintf(kv->pem_key[kv->npem], sizeof kv->pem_key[0], "%s", pems[i]);
        kv->pem[kv->npem++] = p;
    }
    return 0;
}

int vpn_meta_save(const char *name, const char *json) {
    char path[200];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.json", name);
    return write_file_atomic(path, json, strlen(json), 0644);
}

/* ---- routes ---- */

int vpn_resolve(const char *host, int *family, uint8_t a[16]) {
    char h[256];
    snprintf(h, sizeof h, "%s", host);
    char *colon = strrchr(h, ':');
    if (h[0] == '[') { memmove(h, h + 1, strlen(h)); char *e = strchr(h, ']'); if (e) *e = 0; }
    else if (colon && strchr(h, ':') == colon) *colon = 0;     /* host:port */
    if (!parse_ip(h, family, a)) return 0;
    struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(h, NULL, &hints, &res) || !res) return -1;
    struct addrinfo *pick = res;
    for (struct addrinfo *x = res; x; x = x->ai_next) if (x->ai_family == AF_INET) { pick = x; break; }
    memset(a, 0, 16);
    if (pick->ai_family == AF_INET) { *family = AF_INET; memcpy(a, &((struct sockaddr_in *)pick->ai_addr)->sin_addr, 4); }
    else { *family = AF_INET6; memcpy(a, &((struct sockaddr_in6 *)pick->ai_addr)->sin6_addr, 16); }
    freeaddrinfo(res);
    return 0;
}

/* Full tunnel without touching the system's default route (like "redirect-gateway def1"):
 * 0.0.0.0/1 and 128.0.0.0/1 through the tunnel, and a route to the server via where it went
 * before. All with protocol RTPROT_LORDOS, so they can always be deleted (also after a cut). */
int vpn_routes_full(const char *ifn, int sfam, const uint8_t server[16]) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    int idx = rt_ifindex(ifn);
    uint8_t gw[16];
    int has_gw = 0, oif = 0;
    if (!rt_get(fd, sfam, server, gw, &has_gw, &oif) && oif != idx)
        rt_route_proto(fd, 1, sfam, server, sfam == AF_INET ? 32 : 128, has_gw ? gw : NULL, oif, 0, 0, RTPROT_LORDOS);
    uint8_t lo[16] = {0}, hi[16] = {0x80};
    int r = rt_route_proto(fd, 1, AF_INET, lo, 1, NULL, idx, 0, 0, RTPROT_LORDOS);
    if (!r) r = rt_route_proto(fd, 1, AF_INET, hi, 1, NULL, idx, 0, 0, RTPROT_LORDOS);
    close(fd);
    if (r) { char e[200]; admin_event("net vpn - error %s", lerr_set(e, sizeof e, "vpn.routesFailed", "dev", ifn, "errno", lerr_errno(-r), NULL)); }
    return r ? -1 : 0;
}

/* Individual routes through the tunnel ("10.0.0.0/8,192.168.1.0/24"). */
int vpn_routes_list(const char *ifn, const char *list) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    int idx = rt_ifindex(ifn), n = 0;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", list);
    for (char *t = strtok(copy, ", "); t; t = strtok(NULL, ", ")) {
        Cidr c;
        if (!parse_cidr(t, &c, 1) && !rt_route_proto(fd, 1, c.family, c.a, c.prefix, NULL, idx, 0, 0, RTPROT_LORDOS)) n++;
    }
    close(fd);
    return n;
}

/* Removes the route to the server (the only one that doesn't go away by itself with the tunnel's interface). */
void vpn_routes_clear(int sfam, const uint8_t server[16]) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return;
    rt_flush_proto(fd, RTPROT_LORDOS, sfam, server);
    close(fd);
}

int vpn_routes_valid(const char *list) {
    char copy[256];
    if (strlen(list) >= sizeof copy) return 0;
    snprintf(copy, sizeof copy, "%s", list);
    int n = 0;
    for (char *t = strtok(copy, ", "); t; t = strtok(NULL, ", ")) { Cidr c; if (parse_cidr(t, &c, 1)) return 0; n++; }
    return n > 0;
}

/* ---- process ---- */

volatile sig_atomic_t vpn_stop;
static void on_term(int s) { (void)s; vpn_stop = 1; }

void vpn_signals(void) {
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

pid_t vpn_spawn(const char *const argv[], int *out_fd, int *in_fd, const char *extra_env) {
    int o[2], i[2] = { -1, -1 };
    if (pipe2(o, O_CLOEXEC)) return -1;
    if (in_fd && pipe2(i, O_CLOEXEC)) { close(o[0]); close(o[1]); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(in_fd ? i[0] : null, 0);
        dup2(o[1], 1);
        dup2(o[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGPIPE, SIG_DFL);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", (char *)extra_env, NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(o[1]);
    if (in_fd) close(i[0]);
    if (pid < 0) { close(o[0]); if (in_fd) close(i[1]); return -1; }
    *out_fd = o[0];
    if (in_fd) *in_fd = i[1];
    return pid;
}

void vpn_reap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) { if (waitpid(pid, NULL, WNOHANG) == pid) return; usleep(100000); }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void vpn_mark(const char *ifn, const char *name, int set) {
    char mark[128];
    snprintf(mark, sizeof mark, NET_RUN_DIR "/vpn/%s", ifn);
    if (!set) { unlink(mark); return; }
    mkdir(NET_RUN_DIR, 0755);
    mkdir(NET_RUN_DIR "/vpn", 0755);
    char c[64];
    int n = snprintf(c, sizeof c, "%s\n0\n", name);
    write_file_atomic(mark, c, (size_t)n, 0644);
}

/* Generic state: the resident process and the interface. */
int vpn_status_json(const char *name, const char *type, Buf *out) {
    char ifn[IFNAMSIZ], d[40];
    vpn_ifname(type, name, ifn, sizeof ifn);
    snprintf(d, sizeof d, "vpn:%s", name);
    char ip[64] = "";
    int idx = rt_ifindex(ifn);
    if (idx) {
        /* first IPv4 of the interface */
        struct ifreq r = {0};
        int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        snprintf(r.ifr_name, sizeof r.ifr_name, "%s", ifn);
        if (s >= 0 && ioctl(s, SIOCGIFADDR, &r) == 0) fmt_ip(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, ip, sizeof ip);
        if (s >= 0) close(s);
    }
    buf_printf(out, "{\"name\":");
    buf_json_str(out, name);
    buf_printf(out, ",\"type\":\"%s\",\"ifname\":\"%s\",\"running\":%s,\"active\":%s,\"address\":\"%s\"}", type, ifn,
               admin_daemon_running(d) ? "true" : "false", idx ? "true" : "false", ip);
    return 0;
}

/* ---- types ---- */

static const struct { const char *type, *package, *bins[3]; int optional, insecure; } types[] = {
    { "wireguard", "", { NULL }, 0, 0 },
    { "openvpn", "openvpn", { "/usr/sbin/openvpn", NULL }, 0, 0 },
    { "ipsec", "strongswan-charon libcharon-extauth-plugins",
      { "/usr/lib/ipsec/charon", "/usr/lib/ipsec/plugins/libstrongswan-eap-mschapv2.so", NULL }, 0, 0 },
    { "pptp", "pptp-linux ppp", { "/usr/sbin/pptp", "/usr/sbin/pppd", NULL }, 0, 1 },
    { "openconnect", "openconnect", { "/usr/sbin/openconnect", NULL }, 1, 0 },
    { "fortinet", "openfortivpn", { "/usr/bin/openfortivpn", "/usr/sbin/pppd", NULL }, 1, 0 },
};
#define NTYPES (sizeof types / sizeof types[0])

int vpn_type_known(const char *type) {
    for (size_t i = 0; i < NTYPES; i++) if (!strcmp(types[i].type, type)) return 1;
    return 0;
}

int vpn_type_available(const char *type) {
    for (size_t i = 0; i < NTYPES; i++) {
        if (strcmp(types[i].type, type)) continue;
        if (!strcmp(type, "wireguard")) {           /* kernel module (loaded automatically when creating the interface) */
            struct utsname u;
            char p[200];
            if (access("/sys/module/wireguard", F_OK) == 0) return 1;
            if (uname(&u)) return 0;
            snprintf(p, sizeof p, "/lib/modules/%s/kernel/drivers/net/wireguard/wireguard.ko.xz", u.release);
            if (access(p, F_OK) == 0) return 1;
            snprintf(p, sizeof p, "/lib/modules/%s/kernel/drivers/net/wireguard/wireguard.ko", u.release);
            return access(p, F_OK) == 0;
        }
        for (int k = 0; k < 3 && types[i].bins[k]; k++) if (access(types[i].bins[k], F_OK)) return 0;
        return 1;
    }
    return 0;
}

const char *vpn_type_package(const char *type) {
    for (size_t i = 0; i < NTYPES; i++) if (!strcmp(types[i].type, type)) return types[i].package;
    return "";
}

char *net_vpn_types_json(void) {
    Buf b = {0};
    buf_printf(&b, "[");
    for (size_t i = 0; i < NTYPES; i++)
        buf_printf(&b, "%s{\"type\":\"%s\",\"available\":%s,\"optional\":%s,\"package\":\"%s\",\"insecure\":%s}", i ? "," : "",
                   types[i].type, vpn_type_available(types[i].type) ? "true" : "false", types[i].optional ? "true" : "false",
                   types[i].package, types[i].insecure ? "true" : "false");
    buf_printf(&b, "]");
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}
