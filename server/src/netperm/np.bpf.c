/* np.bpf.c — lord-netperm's cgroup BPF programs (see docs/FIREWALL.md, step 2).
 *
 * Restricted C for `clang -target bpf -O2` (Makefile, BPF=1). It doesn't use libbpf or its
 * headers: helpers are declared by number and the maps are symbols of the "maps" section that
 * build/bpf2c turns into relocations; lord-netperm creates the maps and fills in their fds.
 *
 * Programs (attached to the sessions' cgroup subtree, lordos.service):
 *   cgroup/connect4, connect6   TCP and connected UDP (also the UDP getaddrinfo uses for sorting)
 *   cgroup/sendmsg4, sendmsg6   unconnected UDP (sendto with an address)
 *   cgroup/bind4, bind6         "no incoming connections" and inbound rules (local port != 0)
 *   cgroup/sock_create          "no network": not even creating an AF_INET/AF_INET6 socket
 *   cgroup/dev                  "no camera", "no USB", "no screen capture" and "no microphone":
 *                               opening video devices, raw USB devices (/dev/bus/usb), hidraw,
 *                               ttyACM, ttyUSB, the framebuffer (by major) and ALSA capture
 *                               nodes (np_snd: for an app's own scope every ALSA node not known
 *                               to be a non-capture one); every other device stays allowed
 *   cgroup/lsm_socket_create    "no Bluetooth": socket(AF_BLUETOOTH) (a per-cgroup LSM program,
 *                               BPF_LSM_CGROUP: the inet sock_create hook never sees that family)
 *   cgroup/skb_ingress, egress  traffic accounting for the Monitor: bytes per app (np_counts) and
 *                               per (app, remote address, port) (np_flows), and (0.8.5) of the
 *                               essential system services (np_sysc); and the cut of connections
 *                               opened before a rule changed: a packet whose connection the app's
 *                               current rules deny is dropped (counted as "blocked") and its socket
 *                               is reported once, so lord-netperm destroys it (SOCK_DESTROY).
 *
 * The same programs are also attached to the user@<uid>.service of each LordOS session, to
 * close the `systemd-run --user` escape (the process ends up there, outside lordos.service). A
 * process that isn't in any app's scope (ancestor at level cfg.level) falls into the strict union
 * of the restrictions of the user's restricted apps (np_union, keyed by the user@<uid> at level
 * cfg.user_level): it's never less restricted than its own policy.
 *
 * Each one: looks up the app by the process's scope (ancestor at level cfg.level) or, failing
 * that, by the user@ union (level cfg.user_level), decides with
 * np_decide (np_shared.h, the same thing tests/npcompile_test.c tests) and returns 1 (allow) or
 * 0 (the kernel returns EPERM to the app). It counts the connection and notifies lord-netperm over
 * the ring buffer only if it's new (np_seen, 5 s). Nothing per packet: only per connection, bind or
 * sendto. A UDP connect() counts only the first time within 5 s, like a sendto: getaddrinfo does one
 * per address to sort them (RFC 6724) and they aren't real connections.
 *
 * The one exception is the packet filter (skb_ingress / skb_egress), which runs per packet (per
 * GRO/GSO segment): only for the sessions' apps, two map lookups and two atomic adds. It's what
 * counts UDP (QUIC) and the bytes of connections that already closed, which the sockets' own
 * counters (tcp_info) can't. Only for an app whose rules can deny something (NpApp.can_deny) does
 * it also decide, with the same np_decide as connect(): that is what cuts a TCP connection or a
 * connected UDP socket (QUIC) that was opened before the rule changed (docs/FIREWALL.md, "Existing
 * connections are cut"). Loopback is never filtered. */
#define NP_BPF 1
#include <linux/bpf.h>
#include "netperm/np_shared.h"

#define SEC(n) __attribute__((section(n), used))

/* helpers by number (include/uapi/linux/bpf.h, __BPF_FUNC_MAPPER) */
static void *(*bpf_map_lookup_elem)(void *map, const void *key) = (void *)1;
static long (*bpf_map_update_elem)(void *map, const void *key, const void *value, unsigned long long flags) = (void *)2;
static unsigned long long (*bpf_ktime_get_ns)(void) = (void *)5;
static unsigned long long (*bpf_get_current_ancestor_cgroup_id)(int level) = (void *)123;
static long (*bpf_ringbuf_output)(void *ringbuf, void *data, unsigned long long size, unsigned long long flags) = (void *)130;
static long (*bpf_skb_load_bytes)(const void *skb, unsigned int offset, void *to, unsigned int len) = (void *)26;
static unsigned long long (*bpf_skb_ancestor_cgroup_id)(void *skb, int level) = (void *)83;
static unsigned long long (*bpf_get_socket_cookie)(void *ctx) = (void *)46;
static struct bpf_sock *(*bpf_sk_fullsock)(struct bpf_sock *sk) = (void *)95;
static unsigned long long (*bpf_skb_cgroup_id)(void *skb) = (void *)79;

/* the maps: only their names (lord-netperm defines them, netperm.c) */
char np_cfg[4] SEC("maps");
char np_apps[4] SEC("maps");
char np_union[4] SEC("maps");
char np_lpm4[4] SEC("maps");
char np_lpm6[4] SEC("maps");
char np_seen[4] SEC("maps");
char np_counts[4] SEC("maps");
char np_events[4] SEC("maps");
char np_snd[4] SEC("maps");
char np_flows[4] SEC("maps");
char np_bound[4] SEC("maps");
char np_blk4[4] SEC("maps");
char np_blk6[4] SEC("maps");
char np_sysc[4] SEC("maps");

#define AF_INET  2
#define AF_INET6 10
#define AF_BLUETOOTH 31
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

NP_INLINE int proto_of(np_u32 p) {
    return p == IPPROTO_TCP ? NP_P_TCP : p == IPPROTO_UDP ? NP_P_UDP : NP_P_OTHER;
}

/* Looks up the current process's policy: first its app scope (np_apps, ancestor at level
 * cfg->level); if it isn't there (e.g. it moved out to user@<uid> with systemd-run), the strict
 * union of the user's restricted apps (np_union, ancestor at level cfg->user_level). Returns the
 * NpApp and leaves in *cgid the key to count and report with (the scope or the user@). 0 if it doesn't apply. */
NP_INLINE NpApp *find_app(NpCfg *cfg, np_u64 *cgid) {
    np_u64 id = bpf_get_current_ancestor_cgroup_id((int)cfg->level);
    if (id) {
        NpApp *app = bpf_map_lookup_elem(&np_apps, &id);
        if (app) { *cgid = id; return app; }
    }
    if (cfg->user_level) {
        np_u64 uid = bpf_get_current_ancestor_cgroup_id((int)cfg->user_level);
        if (uid) {
            NpApp *app = bpf_map_lookup_elem(&np_union, &uid);
            if (app) { *cgid = uid; return app; }
        }
    }
    return 0;
}

NP_INLINE void count(np_u64 cgid, int deny) {
    NpCount *c = bpf_map_lookup_elem(&np_counts, &cgid);
    if (!c) {
        NpCount z = { 0, 0, 0, 0, 0 };
        bpf_map_update_elem(&np_counts, &cgid, &z, BPF_NOEXIST);
        c = bpf_map_lookup_elem(&np_counts, &cgid);
        if (!c) return;
    }
    if (deny) __sync_fetch_and_add(&c->denied, 1);
    else __sync_fetch_and_add(&c->allowed, 1);
}

/* The app's trie list for an address (its networks and resolved domains), or 0; *lan: 1 LAN, 0
 * internet, -1 unknown (fam 0). */
NP_INLINE NpList *lookup_list(NpApp *app, int fam, const np_u8 *addr, int *lan) {
    NpList *l = 0;
    if (fam == 4) {
        *lan = np_is_lan4(addr);
        if (app->has4) {
            NpLpm4Key k;
            k.prefixlen = 64;
            k.data[0] = (np_u8)(app->idx >> 24); k.data[1] = (np_u8)(app->idx >> 16);
            k.data[2] = (np_u8)(app->idx >> 8);  k.data[3] = (np_u8)app->idx;
            k.data[4] = addr[0]; k.data[5] = addr[1]; k.data[6] = addr[2]; k.data[7] = addr[3];
            l = bpf_map_lookup_elem(&np_lpm4, &k);
        }
    } else if (fam == 6) {
        *lan = np_is_lan6(addr);
        if (app->has6) {
            NpLpm6Key k;
            k.prefixlen = 160;
            k.data[0] = (np_u8)(app->idx >> 24); k.data[1] = (np_u8)(app->idx >> 16);
            k.data[2] = (np_u8)(app->idx >> 8);  k.data[3] = (np_u8)app->idx;
            for (int i = 0; i < 16; i++) k.data[4 + i] = addr[i];
            l = bpf_map_lookup_elem(&np_lpm6, &k);
        }
    }
    return l;
}

/* (0.8.5) Is the address in the app's blocklist set (the user's subscribed lists: their networks
 * and the addresses the resolver gave for their domains)? */
NP_INLINE int lookup_blk(NpApp *app, int fam, const np_u8 *addr) {
    if (!app->blk) return 0;
    if (fam == 4) {
        NpLpm4Key k;
        k.prefixlen = 64;
        k.data[0] = (np_u8)(app->blk >> 24); k.data[1] = (np_u8)(app->blk >> 16);
        k.data[2] = (np_u8)(app->blk >> 8);  k.data[3] = (np_u8)app->blk;
        k.data[4] = addr[0]; k.data[5] = addr[1]; k.data[6] = addr[2]; k.data[7] = addr[3];
        return bpf_map_lookup_elem(&np_blk4, &k) != 0;
    }
    if (fam == 6) {
        NpLpm6Key k;
        k.prefixlen = 160;
        k.data[0] = (np_u8)(app->blk >> 24); k.data[1] = (np_u8)(app->blk >> 16);
        k.data[2] = (np_u8)(app->blk >> 8);  k.data[3] = (np_u8)app->blk;
        for (int i = 0; i < 16; i++) k.data[4 + i] = addr[i];
        return bpf_map_lookup_elem(&np_blk6, &k) != 0;
    }
    return 0;
}

/* fam 0: unknown address. addr: 16 bytes (IPv4 in the first 4). always_count: connect and
 * bind always count; sendto only the first time within NP_REPEAT_NS. */
NP_INLINE int handle(int fam, np_u8 *addr, int port, int proto, int dir, int always_count) {
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg || !cfg->on) return 1;
    np_u64 cgid = 0;
    NpApp *app = find_app(cfg, &cgid);
    if (!app) return 1;                                 /* neither an app scope nor a restricted user@ */

    int lan = -1;
    NpList *l = lookup_list(app, fam, addr, &lan);
    NpRes r;
    np_decide(app, l, dir, proto, port, lan, lookup_blk(app, fam, addr), np_is_loop(fam, addr), &r);
    int deny = r.verdict == NP_V_DENY;

    NpSeenKey sk;
    __builtin_memset(&sk, 0, sizeof sk);
    sk.cgid = cgid;
    for (int i = 0; i < 16; i++) sk.addr[i] = addr[i];
    sk.port = (np_u16)port;
    sk.fam = (np_u8)fam; sk.proto = (np_u8)proto; sk.dir = (np_u8)dir; sk.verdict = (np_u8)deny;
    np_u64 now = bpf_ktime_get_ns();
    np_u64 *last = bpf_map_lookup_elem(&np_seen, &sk);
    int fresh = !last || now - *last > NP_REPEAT_NS;
    if (always_count || fresh) count(cgid, deny);
    if (fresh) {
        bpf_map_update_elem(&np_seen, &sk, &now, BPF_ANY);
        NpEvent ev;
        __builtin_memset(&ev, 0, sizeof ev);
        ev.cgid = cgid;
        ev.ts = now;
        for (int i = 0; i < 16; i++) ev.addr[i] = addr[i];
        ev.port = (np_u16)port;
        ev.fam = (np_u8)fam; ev.proto = (np_u8)proto; ev.dir = (np_u8)dir; ev.verdict = (np_u8)r.verdict;
        ev.reason = (np_u8)r.reason; ev.record = (np_u8)r.record;
        ev.rule_id = r.rule_id; ev.flag = r.flag;
        bpf_ringbuf_output(&np_events, &ev, sizeof ev, 0);
    }
    return !deny;
}

NP_INLINE int addr4(struct bpf_sock_addr *ctx, np_u8 *a) {
    np_u32 v = ctx->user_ip4;                           /* network order: the bytes in memory */
    a[0] = (np_u8)v; a[1] = (np_u8)(v >> 8); a[2] = (np_u8)(v >> 16); a[3] = (np_u8)(v >> 24);
    return 4;
}

/* IPv6; a mapped IPv4 (::ffff:a.b.c.d) comes back as IPv4, same as in fw_decide. */
NP_INLINE int addr6(struct bpf_sock_addr *ctx, np_u8 *a) {
    np_u32 w0 = ctx->user_ip6[0], w1 = ctx->user_ip6[1], w2 = ctx->user_ip6[2], w3 = ctx->user_ip6[3];
    if (w0 == 0 && w1 == 0 && w2 == 0xffff0000u) {      /* 00 00 ff ff in memory */
        a[0] = (np_u8)w3; a[1] = (np_u8)(w3 >> 8); a[2] = (np_u8)(w3 >> 16); a[3] = (np_u8)(w3 >> 24);
        return 4;
    }
    np_u32 w[4] = { w0, w1, w2, w3 };
    for (int i = 0; i < 4; i++) {
        a[i * 4] = (np_u8)w[i]; a[i * 4 + 1] = (np_u8)(w[i] >> 8);
        a[i * 4 + 2] = (np_u8)(w[i] >> 16); a[i * 4 + 3] = (np_u8)(w[i] >> 24);
    }
    return 6;
}

NP_INLINE int port_of(struct bpf_sock_addr *ctx) { return __builtin_bswap16((np_u16)ctx->user_port); }

SEC("cgroup/connect4") int np_connect4(struct bpf_sock_addr *ctx) {
    np_u8 a[16] = { 0 };
    int fam = addr4(ctx, a);
    int proto = proto_of(ctx->protocol);
    return handle(fam, a, port_of(ctx), proto, NP_D_OUT, proto != NP_P_UDP);
}

SEC("cgroup/connect6") int np_connect6(struct bpf_sock_addr *ctx) {
    np_u8 a[16] = { 0 };
    int fam = addr6(ctx, a);
    int proto = proto_of(ctx->protocol);
    return handle(fam, a, port_of(ctx), proto, NP_D_OUT, proto != NP_P_UDP);
}

SEC("cgroup/sendmsg4") int np_sendmsg4(struct bpf_sock_addr *ctx) {
    np_u8 a[16] = { 0 };
    int fam = addr4(ctx, a);
    return handle(fam, a, port_of(ctx), NP_P_UDP, NP_D_OUT, 0);
}

SEC("cgroup/sendmsg6") int np_sendmsg6(struct bpf_sock_addr *ctx) {
    np_u8 a[16] = { 0 };
    int fam = addr6(ctx, a);
    return handle(fam, a, port_of(ctx), NP_P_UDP, NP_D_OUT, 0);
}

/* bind: the remote address isn't known. Only if it's loopback is the connection surely local
 * (like a fw_decide query with that address); otherwise "unknown" = internet. Port 0
 * (ephemeral, used by clients) isn't checked. */
/* A port an app bound (allowed): the packet filter treats the packets on it as an incoming
 * connection (decided as "in" on the local port, like bind), not as one the app opened. Only for
 * the apps' scopes (an np_counts entry). */
NP_INLINE void remember_bound(int port, int proto) {
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg) return;
    np_u64 id = bpf_get_current_ancestor_cgroup_id((int)cfg->level);
    if (!id || !bpf_map_lookup_elem(&np_counts, &id)) return;
    NpBoundKey k;
    __builtin_memset(&k, 0, sizeof k);
    k.cgid = id;
    k.port = (np_u16)port;
    k.proto = (np_u8)proto;
    np_u8 one = 1;
    bpf_map_update_elem(&np_bound, &k, &one, BPF_ANY);
}

SEC("cgroup/bind4") int np_bind4(struct bpf_sock_addr *ctx) {
    int port = port_of(ctx);
    if (!port) return 1;
    np_u8 a[16] = { 0 };
    addr4(ctx, a);
    int fam = a[0] == 127 ? 4 : 0;
    if (!fam) a[0] = a[1] = a[2] = a[3] = 0;
    int proto = proto_of(ctx->protocol);
    int ok = handle(fam, a, port, proto, NP_D_IN, 1);
    if (ok) remember_bound(port, proto);
    return ok;
}

SEC("cgroup/bind6") int np_bind6(struct bpf_sock_addr *ctx) {
    int port = port_of(ctx);
    if (!port) return 1;
    np_u8 a[16] = { 0 };
    int fam = addr6(ctx, a);
    int local = fam == 4 ? a[0] == 127 : np_is_lan6(a) && a[0] == 0 && a[15] == 1;   /* ::1 */
    if (!local) {
        for (int i = 0; i < 16; i++) a[i] = 0;
        fam = 0;
    }
    int proto = proto_of(ctx->protocol);
    int ok = handle(fam, a, port, proto, NP_D_IN, 1);
    if (ok) remember_bound(port, proto);
    return ok;
}

/* "no network": the app can't create internet sockets (nor ICMP, which doesn't go through connect). */
SEC("cgroup/sock_create") int np_sock_create(struct bpf_sock *sk) {
    np_u32 f = sk->family;
    if (f != AF_INET && f != AF_INET6) return 1;
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg || !cfg->on) return 1;
    np_u64 cgid = bpf_get_current_ancestor_cgroup_id((int)cfg->level);
    if (!cgid) return 1;
    NpApp *app = bpf_map_lookup_elem(&np_apps, &cgid);
    if (!app || !(app->flags & NP_R_NO_NETWORK)) return 1;
    np_u8 a[16] = { 0 };
    handle(0, a, 0, proto_of(sk->protocol), NP_D_OUT, 1);   /* counts and reports (denies because of the restriction) */
    return 0;
}

/* A denied device open or Bluetooth socket: reported once per 5 s (np_seen). Not counted: the
 * Monitor's allowed/denied counters are about connections. */
NP_INLINE void report_dev(np_u64 cgid, int dir, np_u32 major, np_u32 minor, np_u32 flag) {
    NpSeenKey sk;
    __builtin_memset(&sk, 0, sizeof sk);
    sk.cgid = cgid;
    sk.addr[0] = (np_u8)minor; sk.addr[1] = (np_u8)(minor >> 8); sk.addr[2] = (np_u8)(minor >> 16); sk.addr[3] = (np_u8)(minor >> 24);
    sk.port = (np_u16)major;
    sk.dir = (np_u8)dir; sk.verdict = 1;
    np_u64 now = bpf_ktime_get_ns();
    np_u64 *last = bpf_map_lookup_elem(&np_seen, &sk);
    if (last && now - *last <= NP_REPEAT_NS) return;
    bpf_map_update_elem(&np_seen, &sk, &now, BPF_ANY);
    NpEvent ev;
    __builtin_memset(&ev, 0, sizeof ev);
    ev.cgid = cgid;
    ev.ts = now;
    for (int i = 0; i < 4; i++) ev.addr[i] = sk.addr[i];
    ev.port = (np_u16)major;
    ev.proto = NP_P_OTHER; ev.dir = (np_u8)dir; ev.verdict = NP_V_DENY;
    ev.reason = NP_BY_RESTRICTED; ev.record = 1;
    ev.flag = flag;
    bpf_ringbuf_output(&np_events, &ev, sizeof ev, 0);
}

/* "no camera" / "no USB": the device controller of cgroup v2 is this kind of program. It runs on
 * open() and mknod() of a device node; 1 = allow, 0 = EPERM. Programs attached higher up (systemd's
 * DeviceAllow) still apply: the kernel allows only what all of them allow. Only the devices of
 * np_dev_restriction are ever denied, and only to a restricted app. */
/* Under user@<uid> the union applies to every process, PipeWire and WirePlumber included: the
 * user owns that subtree and can move any of their processes into any of its cgroups, so an
 * exemption by cgroup would be an open door. The cost (docs/FIREWALL.md): while a camera- or
 * Bluetooth-restricted app runs, the user's own services can't open a new camera or create a new
 * Bluetooth socket either; what they already have open keeps working. */
SEC("cgroup/dev") int np_dev(struct bpf_cgroup_dev_ctx *ctx) {
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg || !cfg->on) return 1;
    np_u32 need = np_dev_restriction(ctx->access_type & 0xffff, ctx->major, cfg->hidraw_major);
    int snd = !need && (ctx->access_type & 0xffff) == NP_DEV_CHAR && ctx->major == NP_MAJOR_SND;
    if (!need && !snd) return 1;
    np_u64 cgid = 0;
    NpApp *app = find_app(cfg, &cgid);
    if (!app) return 1;
    if (snd) {
        /* Recording straight from the card. In the app's own scope this fails closed: an ALSA node
         * the last scan didn't classify (a USB microphone plugged in since) counts as capture.
         * Outside it only the known capture nodes count (lord-netperm keeps no-mic out of the
         * user@ union today, so this is for a union that ever carries it). */
        np_u32 minor = ctx->minor;
        np_u32 *kind = bpf_map_lookup_elem(&np_snd, &minor);
        np_u64 sid = bpf_get_current_ancestor_cgroup_id((int)cfg->level);
        int own = sid && sid == cgid;
        if (kind ? *kind == NP_SND_CAPTURE : own) need = NP_R_NO_MIC;
        else return 1;
    }
    if (!(app->flags & need)) return 1;
    report_dev(cgid, NP_D_DEV, ctx->major, ctx->minor, need);
    return 0;
}

/* "no Bluetooth": socket(AF_BLUETOOTH, ...) of a restricted app. A cgroup LSM program on the
 * socket_create hook (BPF_LSM_CGROUP, kernel >= 6.0 with the bpf LSM active); ctx are the hook's
 * arguments (family, type, protocol, kern). kern = 1 is the kernel's own socket: never touched. */
SEC("cgroup/lsm_socket_create") int np_bt_socket(unsigned long long *ctx) {
    int family = (int)ctx[0];
    int kern = (int)ctx[3];
    if (family != AF_BLUETOOTH || kern) return 1;
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg || !cfg->on) return 1;
    np_u64 cgid = 0;
    NpApp *app = find_app(cfg, &cgid);
    if (!app || !(app->flags & NP_R_NO_BLUETOOTH)) return 1;
    report_dev(cgid, NP_D_BT, 0, 0, NP_R_NO_BLUETOOTH);
    return 0;
}

/* ---- the packet filter: traffic accounting and the cut of open connections ---- */

#define LOOPBACK_IFINDEX 1      /* "lo" is always ifindex 1 (in every network namespace) */
#define TCP_FLAG_RST 0x04

/* A dropped packet of a socket the app's rules now deny: reported once per socket (np_seen keyed
 * by its cookie), so lord-netperm destroys it (TCP gets a reset instead of a stall, a connected UDP
 * socket an error) and the Monitor counts one more denied connection. If the socket is still
 * there 5 s later (a kernel without SOCK_DESTROY), it's asked again, without counting. */
NP_INLINE void report_cut(struct __sk_buff *skb, np_u64 cgid, const NpFlowKey *k, const np_u8 *la, int lport, int rport,
                          int proto, int dir, int port, const NpRes *r) {
    np_u64 cookie = bpf_get_socket_cookie(skb);
    NpSeenKey sk;
    __builtin_memset(&sk, 0, sizeof sk);
    sk.cgid = cgid;
    for (int i = 0; i < 8; i++) sk.addr[i] = (np_u8)(cookie >> (i * 8));
    sk.dir = NP_D_CUT; sk.verdict = 1;
    np_u64 now = bpf_ktime_get_ns();
    np_u64 *last = bpf_map_lookup_elem(&np_seen, &sk);
    if (last && now - *last <= NP_REPEAT_NS) return;
    int first = !last;
    bpf_map_update_elem(&np_seen, &sk, &now, BPF_ANY);
    if (first) count(cgid, 1);
    NpEvent ev;
    __builtin_memset(&ev, 0, sizeof ev);
    ev.cgid = cgid;
    ev.ts = now;
    for (int i = 0; i < 16; i++) { ev.addr[i] = k->addr[i]; ev.laddr[i] = la[i]; }
    ev.port = (np_u16)port;
    ev.lport = (np_u16)lport; ev.rport = (np_u16)rport;
    ev.fam = k->fam; ev.proto = (np_u8)proto; ev.dir = (np_u8)dir; ev.verdict = NP_V_DENY;
    ev.reason = (np_u8)r->reason; ev.record = (np_u8)r->record;
    ev.rule_id = r->rule_id; ev.flag = r->flag;
    ev.cut = first ? 1 : 2;
    ev.cookie = cookie;
    /* which sockets may be destroyed: a TCP connection, not a listener (a SYN from a denied peer
     * mustn't take the server away from the allowed ones), and a connected UDP socket (QUIC), not
     * an unconnected one that also talks to allowed addresses */
    struct bpf_sock *s = skb->sk;
    if (s) s = bpf_sk_fullsock(s);
    if (s) ev.destroy = proto == NP_P_TCP ? s->state != 10 /* TCP_LISTEN */ : proto == NP_P_UDP && s->dst_port != 0;
    bpf_ringbuf_output(&np_events, &ev, sizeof ev, 0);
}

/* (0.8.5) The essential system services (name resolution, time, the network service), which are
 * not apps: lord-netperm attaches these programs to their own cgroups too and keys np_sysc by the
 * socket's cgroup. They are only counted, never filtered: their traffic can't be blocked. */
NP_INLINE int count_system(struct __sk_buff *skb, int in) {
    np_u64 id = bpf_skb_cgroup_id(skb);
    if (!id) return 1;
    NpCount *c = bpf_map_lookup_elem(&np_sysc, &id);
    if (!c) return 1;
    np_u64 len = skb->len;
    if (in) __sync_fetch_and_add(&c->rx, len);
    else __sync_fetch_and_add(&c->tx, len);
    return 1;
}

/* Only for apps lord-netperm knows (it creates their np_counts entry when the scope appears), so
 * nothing else (the login screen, the system) is counted, stored or filtered. Loopback isn't
 * counted or filtered. The skb starts at the IP header in both directions. Returns 1 (let it
 * through) or 0 (drop). An allowed packet adds its bytes to its app (the socket's cgroup, the
 * scope at cfg->level) and to the (app, remote address, port) flow (0.8.5: the port too: the
 * remote one, or the local one of a port the app bound); a dropped one to "blocked". */
NP_INLINE int filter(struct __sk_buff *skb, int in) {
    if (skb->ifindex == LOOPBACK_IFINDEX) return 1;
    np_u32 zero = 0;
    NpCfg *cfg = bpf_map_lookup_elem(&np_cfg, &zero);
    if (!cfg) return 1;
    np_u64 cgid = bpf_skb_ancestor_cgroup_id(skb, (int)cfg->level);
    NpCount *c = cgid ? bpf_map_lookup_elem(&np_counts, &cgid) : 0;
    if (!c) return count_system(skb, in);
    /* the IP header in one read (an IP packet is at least 20 bytes; IPv6 needs 40) */
    np_u8 h[40];
    if (bpf_skb_load_bytes(skb, 0, h, 20)) return 1;
    NpFlowKey k;
    __builtin_memset(&k, 0, sizeof k);
    k.cgid = cgid;
    np_u8 la[16];
    __builtin_memset(la, 0, sizeof la);
    int v6 = h[0] >> 4 == 6, l4 = -1, proto;
    if (v6) {
        if (bpf_skb_load_bytes(skb, 0, h, 40)) return 1;
        /* the remote address: the source when it comes in, the destination when it goes out */
        __builtin_memcpy(k.addr, in ? h + 8 : h + 24, 16);
        __builtin_memcpy(la, in ? h + 24 : h + 8, 16);
        k.fam = 6;
        proto = proto_of(h[6]);                                  /* next header (an extension header: no ports) */
        l4 = 40;
    } else if (h[0] >> 4 == 4) {
        __builtin_memcpy(k.addr, in ? h + 12 : h + 16, 4);
        __builtin_memcpy(la, in ? h + 16 : h + 12, 4);
        k.fam = 4;
        proto = proto_of(h[9]);
        if (!(((np_u32)h[6] << 8 | h[7]) & 0x1fff)) l4 = (h[0] & 0xf) * 4;   /* not a later fragment */
    } else return 1;
    np_u64 len = skb->len;

    int lport = 0, rport = 0, rst = 0, dir = NP_D_OUT;
    if (l4 >= 0 && proto != NP_P_OTHER) {
        np_u8 p[14];                                             /* ports, and the TCP flags at 13 */
        if (!bpf_skb_load_bytes(skb, (np_u32)l4, p, proto == NP_P_TCP ? 14 : 4)) {
            int sport = p[0] << 8 | p[1], dport = p[2] << 8 | p[3];
            lport = in ? dport : sport;
            rport = in ? sport : dport;
            rst = proto == NP_P_TCP && (p[13] & TCP_FLAG_RST);
        }
    }
    int port = rport;
    if (lport) {
        NpBoundKey bk;
        __builtin_memset(&bk, 0, sizeof bk);
        bk.cgid = cgid;
        bk.port = (np_u16)lport;
        bk.proto = (np_u8)proto;
        if (bpf_map_lookup_elem(&np_bound, &bk)) { dir = NP_D_IN; port = lport; }   /* a connection to the app's server */
    }
    int deny = 0;
    NpRes r;
    __builtin_memset(&r, 0, sizeof r);
    NpApp *app = cfg->on ? bpf_map_lookup_elem(&np_apps, &cgid) : 0;
    if (app && app->can_deny) {
        int lan = -1;
        NpList *l = lookup_list(app, k.fam, k.addr, &lan);
        np_decide(app, l, dir, proto, port, lan, lookup_blk(app, k.fam, k.addr), np_is_loop(k.fam, k.addr), &r);
        /* a reset always goes through: it is what tells the other end the connection is over */
        deny = r.verdict == NP_V_DENY && !rst;
    }

    /* the flow's port and protocol go into the key only after the decision: as stack values known
     * to the verifier they would multiply the states it walks through np_decide */
    k.proto = (np_u8)proto;
    k.dir = (np_u8)dir;
    k.port = (np_u16)port;
    NpFlow *f = bpf_map_lookup_elem(&np_flows, &k);
    if (!f) {
        NpFlow z = { 0, 0, 0 };
        bpf_map_update_elem(&np_flows, &k, &z, BPF_NOEXIST);
        f = bpf_map_lookup_elem(&np_flows, &k);
    }
    if (deny) {
        __sync_fetch_and_add(&c->blocked, len);
        if (f) __sync_fetch_and_add(&f->blocked, len);
        report_cut(skb, cgid, &k, la, lport, rport, proto, dir, port, &r);
        return 0;
    }
    if (in) __sync_fetch_and_add(&c->rx, len);
    else __sync_fetch_and_add(&c->tx, len);
    if (f) {
        if (in) __sync_fetch_and_add(&f->rx, len);
        else __sync_fetch_and_add(&f->tx, len);
    }
    return 1;
}

SEC("cgroup/skb_ingress") int np_skb_in(struct __sk_buff *skb) { return filter(skb, 1); }
SEC("cgroup/skb_egress") int np_skb_out(struct __sk_buff *skb) { return filter(skb, 0); }
