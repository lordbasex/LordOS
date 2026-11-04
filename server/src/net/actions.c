/* actions.c — the "net-*" actions of the privileged helper (admin.c). Each one validates its
 * argument here; none passes user text to a shell or as free-form arguments.
 *
 *   net-restore                 at startup: applies the configs in /etc/lordos/net/if and brings up DHCP
 *                               (and Wi-Fi) on the free interfaces. With lord-netd it runs it itself
 *                               at boot ("system"), and for each one that appears later; see admin.c
 *   net-up / net-down <if>
 *   net-config <if>\n<lines>    method=dhcp|static|off, address=ip/prefix (several),
 *                               gateway=ip, dns=ip (several)
 *   net-manage / net-unmanage <if>   hand an interface from systemd-networkd to LordOS and back
 *   net-wifi-scan <if>          LOG per network (JSON) and OK <count>
 *   net-wifi-connect <if>\n<ssid>\n<password>
 *   net-wifi-disconnect <if> / net-wifi-forget <ssid>
 *   net-vpn-import <type>\n<name>\n<config>   type wireguard | openvpn
 *   net-vpn-credentials <name>\n<user>\n<password>
 *   net-vpn-up / net-vpn-down / net-vpn-delete <name>
 *   net-vpn-status [name]       LOG per VPN (JSON) and OK
 *
 * lord-netd also hears the computer wake from sleep (net_wake_event, below; wake.c). */
#include "net.h"
#include "netpriv.h"
#include "vpn.h"
#include "ifwatch.h"
#include "wake.h"
#include "../pkg/pkg.h"

#include <net/if.h>                 /* before the linux/ ones */

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NETWORKCTL "/usr/bin/networkctl"

/* Splits the argument into up to n lines (in place, on a copy). */
static int split_lines(char *s, char **out, int n) {
    int k = 0;
    while (s && k < n) {
        out[k++] = s;
        char *nl = strchr(s, '\n');
        if (!nl) break;
        *nl = 0;
        s = nl + 1;
        if (k == n - 1) { out[k++] = s; break; }    /* the last one takes the rest */
    }
    return k;
}

static int is_wifi(const char *ifname) {
    char p[96];
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless", ifname);
    if (access(p, F_OK) == 0) return 1;
    snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", ifname);
    return access(p, F_OK) == 0;
}

/* Existing physical interface (ethernet or wifi), via sysfs: /sys/class/net/<if>/device. */
static int is_physical(const char *ifname) {
    char p[96];
    snprintf(p, sizeof p, "/sys/class/net/%s/device", ifname);
    return access(p, F_OK) == 0;
}

static int check_if(const char *ifname, int must_be_free) {
    if (!valid_ifname(ifname) || !rt_ifindex(ifname)) { admin_err("net.unknownInterface", "name", ifname, NULL); return -1; }
    if (must_be_free && managed_by_system(ifname)) {
        admin_err("net.systemManaged", "name", ifname, NULL);
        return -1;
    }
    return 0;
}

static void stop_if_daemons(const char *ifname) {
    char d[40];
    snprintf(d, sizeof d, "if:%s", ifname);
    admin_daemon_stop(d);
    snprintf(d, sizeof d, "wpa:%s", ifname);
    admin_daemon_stop(d);
}

static void dhcp_entry(void *arg) { const char *n = arg; dhcp_run(n, 0); }

static int start_dhcp(const char *ifname) {
    static char name_arg[IFNAMSIZ];
    char d[40];
    snprintf(d, sizeof d, "if:%s", ifname);
    if (admin_daemon_running(d)) return 0;
    snprintf(name_arg, sizeof name_arg, "%s", ifname);
    return admin_daemon_fork(d, dhcp_entry, name_arg);
}

/* Applies the saved config (key=value text). */
static int apply_config(const char *ifname, const char *t) {
    char method[16];
    kv_get(t, "method", 0, method, sizeof method);
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    int idx = rt_ifindex(ifname);
    if (!strcmp(method, "off")) {
        stop_if_daemons(ifname);
        rt_addr_flush(fd, idx, AF_INET);
        rt_addr_flush(fd, idx, AF_INET6);
        rt_link_set(fd, idx, 0);
        dns_revert(ifname);
        close(fd);
        return 0;
    }
    int up = rt_link_set(fd, idx, 1);
    if (up && up != -ERFKILL) { close(fd); return -1; }  /* wpa_supplicant waits for a blocked radio by itself */
    if (!strcmp(method, "static")) {
        char d[40];
        snprintf(d, sizeof d, "if:%s", ifname);
        admin_daemon_stop(d);
        rt_addr_flush(fd, idx, AF_INET);
        char v[128];
        for (int i = 0; kv_get(t, "address", i, v, sizeof v); i++) {
            Cidr c;
            if (!parse_cidr(v, &c, 1)) rt_addr(fd, 1, idx, c.family, c.a, c.prefix, 0, 0);
        }
        if (kv_get(t, "gateway", 0, v, sizeof v) && v[0]) {
            int fam;
            uint8_t gw[16], any[16] = {0};
            if (!parse_ip(v, &fam, gw)) rt_route(fd, 1, fam, any, 0, gw, idx, is_wifi(ifname) ? 600 : 100, 0);
        }
        char servers[400] = "";
        for (int i = 0; kv_get(t, "dns", i, v, sizeof v); i++)
            snprintf(servers + strlen(servers), sizeof servers - strlen(servers), "%s%s", i ? " " : "", v);
        if (servers[0]) dns_set(ifname, servers, NULL, DNS_PLAIN); else dns_revert(ifname);
        close(fd);
        if (is_wifi(ifname)) wifi_start(ifname);        /* associating is still wpa_supplicant's job */
        return 0;
    }
    close(fd);
    if (strcmp(method, "dhcp")) return 0;               /* not configured: just brought up */
    if (is_wifi(ifname)) return wifi_start(ifname);
    return start_dhcp(ifname);
}

static char *load_if_conf(const char *ifname) {
    char path[128];
    snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", ifname);
    return read_file(path, 8192, NULL);
}

/* The person turned on an interface saved as "off": that choice is gone, or the next boot takes it back
 * down. Nothing else is lost, since "off" keeps no addresses. Returns the config to apply (malloc). */
static char *load_if_conf_on(const char *ifname) {
    char *t = load_if_conf(ifname), method[16] = "";
    if (!t || !kv_get(t, "method", 0, method, sizeof method) || strcmp(method, "off")) return t;
    free(t);
    char path[128];
    snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", ifname);
    write_file_atomic(path, "method=dhcp\n", 12, 0644);
    return strdup("method=dhcp\n");
}

/* ---- each interface gets its saved state once, whenever it appears (ifwatch.c) ---- */

static IfTable if_seen;
static int if_watch_fd = -1, if_pending, if_force;
static long if_next_ms;

static long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int op_candidate(const char *name) {
    if (!valid_ifname(name)) return 0;
    char path[128];
    snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", name);
    return access(path, F_OK) == 0 || is_physical(name);
}

/* udev keeps a record of a device once it has finished with it, renaming included. */
static int op_initialized(int index) {
    if (if_force || access("/run/udev/control", F_OK)) return 1;     /* nothing renames it: a container */
    char path[48];
    snprintf(path, sizeof path, "/run/udev/data/n%d", index);
    return access(path, F_OK) == 0;
}

static int op_busy(const char *name) {
    char d[40];
    snprintf(d, sizeof d, "if:%s", name);
    if (admin_daemon_running(d)) return 1;
    snprintf(d, sizeof d, "wpa:%s", name);
    return admin_daemon_running(d);
}

static int op_apply(const char *name) {
    char *t = load_if_conf(name);
    int r = apply_config(name, t ? t : "method=dhcp\n");
    free(t);
    return r;
}

static const IfOps if_ops = { op_candidate, op_initialized, managed_by_system, op_busy, op_apply, stop_if_daemons };

static int if_scan(int booting, int *applied) {
    IfNow now[IFW_MAX];
    int n = 0;
    struct if_nameindex *ifs = if_nameindex();
    for (struct if_nameindex *i = ifs; i && i->if_index && n < IFW_MAX; i++) {
        now[n].index = (int)i->if_index;
        snprintf(now[n++].name, sizeof now[0].name, "%s", i->if_name);
    }
    if (ifs) if_freenameindex(ifs);
    else return if_pending;         /* no list right now: says nothing about what left */
    return ifwatch_step(&if_seen, now, n, &if_ops, mono_ms(), booting, applied);
}

/* ---- after the computer wakes from sleep (issue #14; wake.c) ---- */

static WakeTable wake_t;
static long wake_next = -1, wake_last_ms = -100000, wake_gap;
/* what each radio was doing when the computer went to sleep */
typedef struct { char name[16]; int connected; } WakeSnap;
static WakeSnap wake_snap[WAKE_MAX];
static int wake_nsnap, wake_snap_set;
static long wake_snap_gap;

/* Time spent asleep since boot: CLOCK_BOOTTIME keeps counting while suspended, CLOCK_MONOTONIC doesn't. */
static long sleep_gap_ms(void) {
    struct timespec b, m;
    clock_gettime(CLOCK_BOOTTIME, &b);
    clock_gettime(CLOCK_MONOTONIC, &m);
    return (long)(b.tv_sec - m.tv_sec) * 1000 + (b.tv_nsec - m.tv_nsec) / 1000000;
}

static int wpa_running(const char *name) {
    char d[40];
    snprintf(d, sizeof d, "wpa:%s", name);
    return admin_daemon_running(d);
}

static int wpa_completed(const char *name) {
    char st[2048];
    return wifi_ctrl(name, "STATUS", st, sizeof st) > 0 && (!strncmp(st, "wpa_state=COMPLETED", 19) || strstr(st, "\nwpa_state=COMPLETED"));
}

static int wpa_has_networks(const char *name) {
    char r[4096];
    if (wifi_ctrl(name, "LIST_NETWORKS", r, sizeof r) <= 0) return 0;
    char *nl = strchr(r, '\n');                    /* the first line is the header */
    return nl && nl[1] != 0;
}

static void wake_logf(const char *name, const char *what, int why) {
    if (why != WK_OK) {
        fprintf(stderr, "lord-netd: after sleep: %s: %s (%s)\n", name, what, wake_why(why));
        admin_event("net wake %s %s %s", name, what, wake_why(why));
    } else {
        fprintf(stderr, "lord-netd: after sleep: %s: %s\n", name, what);
        admin_event("net wake %s %s", name, what);
    }
}

static int wk_active(const char *name) { return rt_ifindex(name) && wpa_running(name) && !managed_by_system(name); }

static int if_ipv4(const char *name, uint32_t *addr) {
    struct ifaddrs *ifa = NULL;
    int found = 0;
    if (getifaddrs(&ifa)) return 0;
    for (struct ifaddrs *i = ifa; i && !found; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, name)) {
            *addr = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            found = 1;
        }
    freeifaddrs(ifa);
    return found;
}

/* Its gateway: the DHCP lease's, or the static config's (IPv4). 0 if it has none. */
static uint32_t if_gateway(const char *name) {
    uint32_t gw = 0;
    if (!dhcp_lease_router(name, &gw) && gw) return gw;
    char *t = load_if_conf(name), v[64];
    int fam;
    uint8_t a[16];
    if (t && kv_get(t, "gateway", 0, v, sizeof v) && !parse_ip(v, &fam, a) && fam == AF_INET) memcpy(&gw, a, 4);
    free(t);
    return gw;
}

int net_wake_health(const char *name) {
    if (!wpa_completed(name)) return WK_NOT_ASSOCIATED;
    char p[96], c[8] = "";
    snprintf(p, sizeof p, "/sys/class/net/%s/carrier", name);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(c, sizeof c, f)) c[0] = 0; fclose(f); }
    if (c[0] != '1') return WK_NO_CARRIER;
    uint32_t addr = 0, gw;
    if (!if_ipv4(name, &addr)) return WK_NO_ADDRESS;
    if (!(gw = if_gateway(name))) return WK_OK;          /* nothing to ask: associated with an address will do */
    return arp_probe(name, addr, gw, 3, 300) ? WK_NO_GATEWAY : WK_OK;
}

static void dhcp_renew(const char *name) {
    char d[40];
    snprintf(d, sizeof d, "if:%s", name);
    admin_daemon_signal(d, SIGUSR2);
}

static void wk_reassociate(const char *name) {
    char r[64];
    wifi_ctrl(name, "DISCONNECT", r, sizeof r);
    wifi_ctrl(name, "RECONNECT", r, sizeof r);
    dhcp_renew(name);
}

static void wk_reset_stop(const char *name) {
    stop_if_daemons(name);
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd >= 0) { rt_link_set(fd, rt_ifindex(name), 0); close(fd); }
}

static void wk_reset_start(const char *name) { op_apply(name); }

static const WakeOps wake_ops = { wk_active, net_wake_health, dhcp_renew, wk_reassociate, wk_reset_stop, op_busy, wk_reset_start, wake_logf };

void net_wake_event(int sleeping, const char *source) {
    long gap = sleep_gap_ms();
    struct if_nameindex *ifs = if_nameindex();
    if (sleeping) {
        wake_nsnap = 0;
        for (struct if_nameindex *i = ifs; i && i->if_index && wake_nsnap < WAKE_MAX; i++)
            if (is_wifi(i->if_name) && wpa_running(i->if_name)) {
                snprintf(wake_snap[wake_nsnap].name, sizeof wake_snap[0].name, "%s", i->if_name);
                wake_snap[wake_nsnap++].connected = wpa_completed(i->if_name);
            }
        wake_snap_set = 1;
        wake_snap_gap = gap;
        if (ifs) if_freenameindex(ifs);
        fprintf(stderr, "lord-netd: going to sleep (%s)\n", source);
        return;
    }
    long now = mono_ms();
    wake_gap = gap;
    if (now - wake_last_ms < 5000) { if (ifs) if_freenameindex(ifs); return; }     /* both sources heard the same wake */
    wake_last_ms = now;
    /* the snapshot is trustworthy only if it was taken before the sleep (time asleep grew since) */
    int snap_ok = wake_snap_set && gap - wake_snap_gap > 1000;
    const char *names[WAKE_MAX];
    int n = 0, wired = 0;
    for (struct if_nameindex *i = ifs; i && i->if_index; i++) {
        const char *name = i->if_name;
        if (managed_by_system(name)) continue;
        if (!is_wifi(name)) {
            char d[40];
            snprintf(d, sizeof d, "if:%s", name);
            if (admin_daemon_running(d)) { dhcp_renew(name); wake_logf(name, "renew", WK_OK); wired++; }
            continue;
        }
        if (!wpa_running(name) || n == WAKE_MAX) continue;
        int want = 0;
        if (snap_ok) { for (int k = 0; k < wake_nsnap; k++) if (!strcmp(wake_snap[k].name, name)) want = wake_snap[k].connected; }
        else want = wpa_has_networks(name);                 /* not known: a radio with saved networks */
        if (want) names[n++] = name;
    }
    fprintf(stderr, "lord-netd: woke from sleep (%s, %ld s asleep): renewing %d wired, checking %d Wi-Fi\n",
            source, snap_ok ? (gap - wake_snap_gap) / 1000 : -1L, wired, n);
    wake_begin(&wake_t, names, n, now);
    wake_snap_set = 0;
    if (ifs) if_freenameindex(ifs);
    wake_next = n ? now + WAKE_SETTLE_MS : -1;
}

int net_watch_open(void) {
    wake_gap = sleep_gap_ms();
    if_watch_fd = nl_open(NETLINK_ROUTE, RTMGRP_LINK);
    if (if_watch_fd >= 0) fcntl(if_watch_fd, F_SETFL, fcntl(if_watch_fd, F_GETFL) | O_NONBLOCK);
    return if_watch_fd;
}

int net_watch_timeout(void) {
    long at = if_pending ? if_next_ms : -1;
    if (wake_next >= 0 && (at < 0 || wake_next < at)) at = wake_next;
    if (at < 0) return -1;
    long left = at - mono_ms();
    return left < 0 ? 0 : (int)left;
}

void net_watch_run(int readable) {
    /* a wake nobody announced (no system bus): the time asleep jumped */
    long gap = sleep_gap_ms();
    if (gap - wake_gap > 3000) net_wake_event(0, "clock");
    wake_gap = gap;
    if (wake_next >= 0 && mono_ms() >= wake_next) wake_next = wake_step(&wake_t, &wake_ops, mono_ms());
    if (readable) { char junk[8192]; while (recv(if_watch_fd, junk, sizeof junk, MSG_DONTWAIT) > 0) {} }
    if (!readable && (!if_pending || mono_ms() < if_next_ms)) return;
    int applied = 0;
    if_pending = if_scan(0, &applied);
    if_next_ms = mono_ms() + 500;
    if (applied) admin_event("net link restored %d", applied);
}

void net_act_restore(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    mkdir(NET_RUN_DIR, 0755);
    /* Normally the network belongs to the system: lord-netd brings it up at boot (arg "system", see
     * admin.c) and the sessions' net-restore never gets here. Without lord-netd (a standalone
     * lord-server, in development) the session's helper brings it up; with several, only the first
     * (its pid in net-owner, while alive), so two don't start two DHCP clients on the same NIC. */
    if (!(len == 6 && !strcmp(arg, "system"))) {
        char *o = read_file(NET_RUN_DIR "/net-owner", 32, NULL);
        if (o) {
            pid_t p = (pid_t)atoi(o);
            free(o);
            if (p > 1 && p != getpid() && kill(p, 0) == 0) { admin_ok("net.otherSession", NULL); return; }
        }
        FILE *of = fopen(NET_RUN_DIR "/net-owner", "w");
        if (of) { fprintf(of, "%d\n", (int)getpid()); fclose(of); }
    } else unlink(NET_RUN_DIR "/net-owner");
    /* VPN routes left over from a session that was cut off (protocol RTPROT_LORDOS) */
    int rfd = nl_open(NETLINK_ROUTE, 0);
    if (rfd >= 0) { rt_flush_proto(rfd, RTPROT_LORDOS, AF_INET, NULL); rt_flush_proto(rfd, RTPROT_LORDOS, AF_INET6, NULL); close(rfd); }
    /* no config: the free physical ones use DHCP, like NetworkManager. Under lord-netd, one that isn't
     * ready yet (or isn't there at all) is picked up by net_watch_run; a session has no such loop */
    int n = 0, sys = len == 6 && !strcmp(arg, "system");
    if_force = !sys;
    if_seen.n = 0;
    if_pending = if_scan(1, &n);
    if_force = 0;
    if_next_ms = mono_ms() + 500;
    admin_ok("net.restored", "count", lerr_int(n), NULL);
}

void net_act_up(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (check_if(arg, 1)) return;
    int fd = nl_open(NETLINK_ROUTE, 0);
    int e = fd >= 0 ? rt_link_set(fd, rt_ifindex(arg), 1) : -1;
    if (fd >= 0) close(fd);
    char *t = load_if_conf_on(arg);
    if (!e) apply_config(arg, t ? t : (is_physical(arg) ? "method=dhcp\n" : "method=none\n"));
    free(t);
    if (e) admin_err("net.upFailed", "name", arg, "errno", lerr_errno(-e), NULL); else admin_ok("net.up", "name", arg, NULL);
}

void net_act_down(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (check_if(arg, 1)) return;
    stop_if_daemons(arg);
    int fd = nl_open(NETLINK_ROUTE, 0);
    int e = fd >= 0 ? rt_link_set(fd, rt_ifindex(arg), 0) : -1;
    if (fd >= 0) close(fd);
    if (e) admin_err("net.downFailed", "name", arg, "errno", lerr_errno(-e), NULL); else admin_ok("net.down", "name", arg, NULL);
}

void net_act_config(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    char *copy = strdup(arg);
    if (!copy) { admin_err("sys.outOfMemory", NULL); return; }
    char *nl = strchr(copy, '\n');
    char *body = nl ? nl + 1 : (char *)"";
    if (nl) *nl = 0;
    if (check_if(copy, 1)) { free(copy); return; }
    /* the config is rebuilt only from what was validated */
    Buf b = {0};
    char method[16] = "";
    int naddr = 0, ok = 1;
    for (char *l = body, *next; l && *l && ok; l = next) {
        next = strchr(l, '\n');
        if (next) *next++ = 0;
        if (!*l) continue;
        char *eq = strchr(l, '=');
        if (!eq) { ok = 0; break; }
        *eq = 0;
        const char *k = l, *v = eq + 1;
        Cidr c;
        int fam;
        uint8_t a[16];
        if (!strcmp(k, "method")) {
            if (strcmp(v, "dhcp") && strcmp(v, "static") && strcmp(v, "off")) ok = 0;
            else snprintf(method, sizeof method, "%s", v);
        } else if (!strcmp(k, "address")) { if (parse_cidr(v, &c, 1) || naddr++ >= 8) ok = 0; else buf_printf(&b, "address=%s\n", v); }
        else if (!strcmp(k, "gateway")) { if (*v && parse_ip(v, &fam, a)) ok = 0; else if (*v) buf_printf(&b, "gateway=%s\n", v); }
        else if (!strcmp(k, "dns")) { if (parse_ip(v, &fam, a)) ok = 0; else buf_printf(&b, "dns=%s\n", v); }
        else ok = 0;
        if (!ok) admin_err("net.invalidValue", "key", k, "value", v, NULL);
    }
    if (ok && !method[0]) { ok = 0; admin_err("net.missingMethod", NULL); }
    if (ok && !strcmp(method, "static") && !naddr) { ok = 0; admin_err("net.missingAddress", NULL); }
    if (ok) {
        Buf f = {0};
        buf_printf(&f, "method=%s\n%s", method, b.p ? b.p : "");
        char path[128];
        snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", copy);
        if (f.oom || write_file_atomic(path, f.p, f.len, 0644)) admin_err("net.saveFailed", NULL);
        else if (apply_config(copy, f.p)) admin_err("net.applyFailed", "name", copy, NULL);
        else admin_ok("net.configured", "name", copy, "method", method, NULL);
        buf_free(&f);
    }
    buf_free(&b);
    free(copy);
}

/* systemd-networkd stops managing it with a .network that marks it Unmanaged. */
void net_act_manage(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (check_if(arg, 0)) return;
    if (!managed_by_system(arg)) { admin_ok("net.alreadyLordos", "name", arg, NULL); return; }
    char path[160];
    snprintf(path, sizeof path, "/etc/systemd/network/00-lordos-%s.network", arg);
    char body[200];
    int n = snprintf(body, sizeof body, "# LordOS manages this interface (net-manage)\n[Match]\nName=%s\n\n[Link]\nUnmanaged=yes\n", arg);
    if (write_file_atomic(path, body, (size_t)n, 0644)) { admin_err("sys.writeFailed", "path", path, NULL); return; }
    char out[256];
    const char *argv[] = { NETWORKCTL, "reload", NULL };
    if (access(NETWORKCTL, X_OK) == 0) admin_run_capture(argv, out, sizeof out);
    for (int i = 0; i < 30 && managed_by_system(arg); i++) usleep(100000);
    if (managed_by_system(arg)) { unlink(path); admin_run_capture(argv, out, sizeof out); admin_err("net.stillSystem", "name", arg, NULL); return; }
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd >= 0) { rt_addr_flush(fd, rt_ifindex(arg), AF_INET); close(fd); }
    char *t = load_if_conf(arg);
    if (!t) {
        char cp[128];
        snprintf(cp, sizeof cp, NET_IF_DIR "/%s.conf", arg);
        write_file_atomic(cp, "method=dhcp\n", 12, 0644);
    }
    apply_config(arg, t ? t : "method=dhcp\n");
    free(t);
    admin_ok("net.managed", "name", arg, NULL);
}

void net_act_unmanage(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (check_if(arg, 0)) return;
    stop_if_daemons(arg);
    char path[160];
    snprintf(path, sizeof path, NET_IF_DIR "/%s.conf", arg);
    unlink(path);
    snprintf(path, sizeof path, "/etc/systemd/network/00-lordos-%s.network", arg);
    if (unlink(path) == 0 && access(NETWORKCTL, X_OK) == 0) {
        char out[256];
        const char *argv[] = { NETWORKCTL, "reload", NULL };
        admin_run_capture(argv, out, sizeof out);
        const char *rc[] = { NETWORKCTL, "reconfigure", arg, NULL };
        admin_run_capture(rc, out, sizeof out);
    }
    admin_ok("net.unmanaged", "name", arg, NULL);
}

/* ---- Wi-Fi ---- */

static int check_wifi(const char *ifname) {
    if (check_if(ifname, 1)) return -1;
    if (!is_wifi(ifname)) { admin_err("wifi.notWifi", "name", ifname, NULL); return -1; }
    return 0;
}

void net_act_wifi_scan(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (check_wifi(arg)) return;
    char d[40];
    snprintf(d, sizeof d, "wpa:%s", arg);
    if (!admin_daemon_running(d)) { admin_err("wifi.noSupplicant", "name", arg, NULL); return; }
    wifi_act_scan(arg);
}

void net_act_wifi_connect(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    char *copy = strdup(arg), *l[3] = { NULL, NULL, NULL };
    if (!copy) { admin_err("sys.outOfMemory", NULL); return; }
    int k = split_lines(copy, l, 3);
    if (k < 2) { admin_err("admin.invalidArgument", NULL); free(copy); return; }
    if (!check_wifi(l[0])) {
        free(load_if_conf_on(l[0]));                /* joining a network is turning the radio on */
        wifi_act_connect(l[0], l[1], k > 2 ? l[2] : "");
    }
    memset(copy, 0, len);
    free(copy);
}

void net_act_wifi_disconnect(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (!check_wifi(arg)) wifi_act_disconnect(arg);
}

void net_act_wifi_forget(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    wifi_act_forget(arg);
}

/* ---- VPN ---- */

static int vpn_type(const char *name, char *type, size_t n) {
    char path[160];
    snprintf(path, sizeof path, NET_VPN_DIR "/%s.json", name);
    char *t = read_file(path, 8192, NULL);
    if (!t) return -1;
    const char *p = strstr(t, "\"type\":\"");
    int ok = 0;
    if (p) {
        p += 8;
        char ty[24];
        size_t k = strcspn(p, "\"");
        if (k < sizeof ty) { memcpy(ty, p, k); ty[k] = 0; ok = vpn_type_known(ty); if (ok) snprintf(type, n, "%s", ty); }
    }
    if (!ok) { free(t); return -1; }
    free(t);
    return 0;
}

static int check_vpn(const char *name, char *type, size_t n) {
    if (!valid_vpn_name(name) || vpn_type(name, type, n)) { admin_err("vpn.unknown", "name", name, NULL); return -1; }
    return 0;
}

void net_act_vpn_import(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char *copy = strdup(arg), *l[3] = { NULL, NULL, NULL };
    if (!copy) { admin_err("sys.outOfMemory", NULL); return; }
    int k = split_lines(copy, l, 3);
    char err[200] = "";
    if (k < 3) admin_err("admin.invalidArgument", NULL);
    else if (!valid_vpn_name(l[1])) admin_err("vpn.invalidName", NULL);
    else if (!strcmp(l[0], "wireguard")) {
        char pub[64];
        if (wg_import(l[1], l[2], pub, err, sizeof err)) admin_say("ERR", "%s", err); /* i18n-ok: err is a code line */
        else admin_say("OK", "%s", pub);                 /* i18n-ok: the public key (data), to give to the server */
    } else if (!strcmp(l[0], "openvpn")) {
        if (access("/usr/sbin/openvpn", X_OK)) admin_err("vpn.missingProgram", "type", "openvpn", "package", "openvpn", NULL);
        else if (ovpn_import(l[1], l[2], err, sizeof err)) admin_say("ERR", "%s", err); /* i18n-ok: err is a code line */
        else admin_ok("vpn.imported", NULL);
    } else if (!vpn_type_known(l[0])) admin_err("vpn.unsupportedType", "type", l[0], NULL);
    else if (!vpn_type_available(l[0])) admin_err("vpn.missingProgram", "type", l[0], "package", vpn_type_package(l[0]), NULL);
    else {
        int (*imp)(const char *, const char *, char *, size_t) = !strcmp(l[0], "ipsec") ? ipsec_import : !strcmp(l[0], "pptp") ? pptp_import
                                                               : !strcmp(l[0], "openconnect") ? oc_import : forti_import;
        if (imp(l[1], l[2], err, sizeof err)) admin_say("ERR", "%s", err); /* i18n-ok: err is a code line */
        else admin_ok("vpn.imported", NULL);
    }
    memset(copy, 0, len);
    free(copy);
}

void net_act_vpn_credentials(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char *copy = strdup(arg), *l[3] = { NULL, NULL, NULL }, type[24];
    if (!copy) { admin_err("sys.outOfMemory", NULL); return; }
    int k = split_lines(copy, l, 3);
    if (k < 3) admin_err("admin.invalidArgument", NULL);
    else if (!check_vpn(l[0], type, sizeof type)) {
        if (strcmp(type, "openvpn")) admin_err("vpn.credentialsOnlyOpenvpn", NULL);
        else if (ovpn_set_credentials(l[0], l[1], l[2])) admin_err("vpn.invalidCredentials", NULL);
        else admin_ok("vpn.saved", NULL);
    }
    memset(copy, 0, len);
    free(copy);
}

static void ovpn_entry(void *arg) { ovpn_run((const char *)arg); }
static void ipsec_entry(void *arg) { ipsec_run((const char *)arg); }
static void pptp_entry(void *arg) { pptp_run((const char *)arg); }
static void oc_entry(void *arg) { oc_run((const char *)arg); }
static void forti_entry(void *arg) { forti_run((const char *)arg); }

void net_act_vpn_up(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    char type[24], err[200] = "";
    if (check_vpn(arg, type, sizeof type)) return;
    if (!strcmp(type, "wireguard")) {
        if (wg_up(arg, err, sizeof err)) { admin_say("ERR", "%s", err); admin_event("net vpn %s error %s", arg, err); } /* i18n-ok: err is a code line */
        else { admin_ok("vpn.connected", NULL); admin_event("net vpn %s connected", arg); }
        return;
    }
    static char name_arg[32];
    char d[40];
    snprintf(d, sizeof d, "vpn:%s", arg);
    snprintf(name_arg, sizeof name_arg, "%s", arg);
    if (admin_daemon_running(d)) { admin_ok("vpn.alreadyConnecting", NULL); return; }
    if (!vpn_type_available(type)) { admin_err("vpn.missingProgram", "type", type, "package", vpn_type_package(type), NULL); return; }
    void (*fn)(void *) = !strcmp(type, "openvpn") ? ovpn_entry : !strcmp(type, "ipsec") ? ipsec_entry : !strcmp(type, "pptp") ? pptp_entry
                       : !strcmp(type, "openconnect") ? oc_entry : forti_entry;
    if (!strcmp(type, "ipsec") && access("/run/charon.pid", F_OK) == 0) {
        /* charon uses a single /run/charon.pid: one IPsec at a time */
        char *t = read_file("/run/charon.pid", 32, NULL);
        int busy = t && atoi(t) > 1 && kill(atoi(t), 0) == 0;
        free(t);
        if (busy) { admin_err("vpn.ipsecBusy", NULL); return; }
    }
    if (admin_daemon_fork(d, fn, name_arg)) admin_err("vpn.startFailed", NULL);
    else admin_ok("vpn.connecting", NULL);
}

void net_act_vpn_down(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    char type[24];
    if (check_vpn(arg, type, sizeof type)) return;
    if (!strcmp(type, "wireguard")) {
        if (wg_down(arg)) admin_err("vpn.notConnected", NULL);
        else { admin_ok("vpn.disconnected", NULL); admin_event("net vpn %s disconnected", arg); }
    } else if (ovpn_down(arg)) admin_err("vpn.notConnected", NULL);      /* "vpn:<name>", any type */
    else admin_ok("vpn.disconnecting", NULL);
}

void net_act_vpn_delete(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    char type[24];
    if (check_vpn(arg, type, sizeof type)) return;
    if (!strcmp(type, "wireguard")) wg_down(arg); else ovpn_down(arg);
    static const char *const ext[] = { "json", "conf", "ovpn", "auth", "ipsec", "pptp", "openconnect", "fortinet", "ca.pem", "cert.pem", "key.pem" };
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        char p[160];
        snprintf(p, sizeof p, NET_VPN_DIR "/%s.%s", arg, ext[i]);
        unlink(p);
    }
    /* generated configs */
    static const char *const gen[] = { NET_DIR "/ppp/%s.options", NET_DIR "/fortinet/%s.conf", "/etc/strongswan.d/lordos/%s.conf" };
    for (size_t i = 0; i < sizeof gen / sizeof gen[0]; i++) {
        char p[200];
        snprintf(p, sizeof p, gen[i], arg);
        unlink(p);
    }
    admin_ok("vpn.deleted", NULL);
}

static void status_one(const char *name) {
    char type[24];
    if (!valid_vpn_name(name) || vpn_type(name, type, sizeof type)) return;
    Buf b = {0};
    if (!strcmp(type, "wireguard")) wg_status_json(name, &b);
    else if (!strcmp(type, "openvpn")) ovpn_status_json(name, &b);
    else vpn_status_json(name, type, &b);
    if (b.p && !b.oom) admin_say("LOG", "%s", b.p);
    buf_free(&b);
}

void net_act_vpn_status(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    if (*arg) { status_one(arg); admin_say("OK", "1"); return; } /* i18n-ok: a count (data) */
    DIR *d = opendir(NET_VPN_DIR);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l - 5 > 24 || strcmp(e->d_name + l - 5, ".json")) continue;
        char name[32];
        memcpy(name, e->d_name, l - 5);
        name[l - 5] = 0;
        status_one(name);
        n++;
    }
    if (d) closedir(d);
    admin_say("OK", "%d", n); /* i18n-ok: a count (data) */
}

/* net-vpn-enable openconnect|fortinet: installs the (optional) program with LordOS's package
 * engine (pkg-install, see pkg/pkg.c). Runs in a separate process (A_FORK). */
void net_act_vpn_enable(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    const char *pkgs = !strcmp(arg, "openconnect") ? "openconnect" : !strcmp(arg, "fortinet") ? "openfortivpn" : NULL;
    if (!pkgs) { admin_err("admin.invalidArgument", NULL); return; }
    if (vpn_type_available(arg)) { admin_ok("vpn.alreadyInstalled", NULL); return; }
    pkg_act_install(r, pkgs, strlen(pkgs));
}
