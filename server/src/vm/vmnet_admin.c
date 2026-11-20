/* vmnet_admin.c — Machines' bridged network in the admin helper (vm_admin.h: vm-net-bridge, vm-net-unbridge; docs/
 * VIRTUALIZATION.md 6.5). Our own netlink (net/nl.c), no program run besides udevadm settle. What it makes is one macvtap
 * per machine, named after the machine (vm_bridge_ifname), in bridge mode on a physical wired card, with the machine's
 * MAC and the alias "lordos-vm <uid> <id>" (only root can write an alias, so a user can't pass a card of theirs as one of
 * ours); its /dev/tap<index> belongs to the calling user, 0600. Nothing else: no address, no route, no forwarding. */
#include "vm_admin.h"
#include "vmnet.h"
#include "machine.h"
#include "../net/netpriv.h"

#include <net/if.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define SYS_NET "/sys/class/net"

static int read_line(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static int link_alias(const char *name, char *out, size_t n) {
    char p[96];
    snprintf(p, sizeof p, SYS_NET "/%s/ifalias", name);
    return read_line(p, out, n);
}

static int ether(const char *mac, unsigned char b[6]) {
    unsigned v[6];
    if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) b[i] = (unsigned char)v[i];
    return 0;
}

/* /dev/tap<idx> is open in some process: a running machine's (root reads every process's descriptors) */
static int tap_in_use(int idx) {
    char want[32];
    snprintf(want, sizeof want, "/dev/tap%d", idx);
    DIR *d = opendir("/proc");
    if (!d) return 1;                   /* can't tell: keep it */
    int used = 0;
    for (struct dirent *e; !used && (e = readdir(d));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char fdp[300];
        snprintf(fdp, sizeof fdp, "/proc/%s/fd", e->d_name);
        DIR *f = opendir(fdp);
        if (!f) continue;
        for (struct dirent *x; !used && (x = readdir(f));) {
            if (x->d_name[0] == '.') continue;
            char lp[600], t[64];
            snprintf(lp, sizeof lp, "%s/%s", fdp, x->d_name);
            ssize_t l = readlink(lp, t, sizeof t - 1);
            if (l > 0) { t[l] = 0; if (!strcmp(t, want)) used = 1; }
        }
        closedir(f);
    }
    closedir(d);
    return used;
}

static int del_link(const char *name) {
    int idx = (int)if_nametoindex(name);
    if (!idx) return 0;
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -1;
    int r = rt_link_del(fd, idx);
    close(fd);
    return r;
}

/* the macvtap: made, aliased and up; its index or -errno */
static int make_macvtap(const char *name, int lower, const unsigned char mac[6], const char *alias) {
    int fd = nl_open(NETLINK_ROUTE, 0);
    if (fd < 0) return -errno;
    NlMsg m;
    nlm_init(&m, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
    nlm_put(&m, &ifi, sizeof ifi);
    nlm_attr_str(&m, IFLA_IFNAME, name);
    nlm_attr_u32(&m, IFLA_LINK, (uint32_t)lower);
    nlm_attr(&m, IFLA_ADDRESS, mac, 6);
    size_t li = nlm_nest_begin(&m, IFLA_LINKINFO);
    nlm_attr(&m, IFLA_INFO_KIND, "macvtap", 7);
    size_t da = nlm_nest_begin(&m, IFLA_INFO_DATA);
    nlm_attr_u32(&m, IFLA_MACVLAN_MODE, MACVLAN_MODE_BRIDGE);
    nlm_nest_end(&m, da);
    nlm_nest_end(&m, li);
    int r = nl_talk(fd, &m);
    int idx = r ? 0 : (int)if_nametoindex(name);
    if (!r && !idx) r = -ENODEV;
    if (!r) {
        /* no address of this computer's on it, not even IPv6's link-local one (which would be made from the guest's own
         * MAC and clash with the guest's): set before it goes up */
        nlm_init(&m, RTM_NEWLINK, 0);
        struct ifinfomsg ifc = { .ifi_family = AF_UNSPEC, .ifi_index = idx };
        nlm_put(&m, &ifc, sizeof ifc);
        size_t af = nlm_nest_begin(&m, IFLA_AF_SPEC);
        size_t a6 = nlm_nest_begin(&m, AF_INET6);
        nlm_attr_u8(&m, IFLA_INET6_ADDR_GEN_MODE, IN6_ADDR_GEN_MODE_NONE);
        nlm_nest_end(&m, a6);
        nlm_nest_end(&m, af);
        r = nl_talk(fd, &m);
        if (r == -EAFNOSUPPORT) r = 0;          /* no IPv6 on this computer: nothing to keep off */
        if (r) rt_link_del(fd, idx);
    }
    if (!r) {
        nlm_init(&m, RTM_NEWLINK, 0);
        struct ifinfomsg up = { .ifi_family = AF_UNSPEC, .ifi_index = idx, .ifi_change = IFF_UP, .ifi_flags = IFF_UP };
        nlm_put(&m, &up, sizeof up);
        nlm_attr(&m, IFLA_IFALIAS, alias, strlen(alias));
        r = nl_talk(fd, &m);
        if (r) rt_link_del(fd, idx);
    }
    close(fd);
    return r ? r : idx;
}

/* /dev/tap<idx> to the user, 0600: the node checked by what it is (the macvtap's own device number), never followed */
static int give_tap(const char *name, int idx, uid_t uid, gid_t gid) {
    char p[96], dev[32];
    snprintf(p, sizeof p, SYS_NET "/%s/macvtap/tap%d/dev", name, idx);
    unsigned ma, mi;
    if (read_line(p, dev, sizeof dev) || sscanf(dev, "%u:%u", &ma, &mi) != 2) return -1;
    /* devtmpfs makes the node, then udev runs its rules on it: let them finish before it changes hands */
    const char *const settle[] = { "/usr/bin/udevadm", "settle", "--timeout=5", NULL };
    admin_run(settle);
    snprintf(p, sizeof p, "/dev/tap%d", idx);
    int fd = -1;
    for (int i = 0; i < 50 && (fd = open(p, O_PATH | O_NOFOLLOW | O_CLOEXEC)) < 0; i++) { struct timespec t = { 0, 20000000 }; nanosleep(&t, NULL); }
    if (fd < 0) return -1;
    struct stat st;
    int ok = !fstat(fd, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == ma && minor(st.st_rdev) == mi &&
             !fchownat(fd, "", uid, gid, AT_EMPTY_PATH);
    if (ok) {
        char fp[64];
        snprintf(fp, sizeof fp, "/proc/self/fd/%d", fd);
        ok = !chmod(fp, 0600);
    }
    close(fd);
    return ok ? 0 : -1;
}

void vm_act_net_bridge(const AdminReq *r, const char *arg, size_t len) {
    char id[37], card[16], mac[18], name[VM_IFNAME_MAX + 1], alias[96], have[128];
    if (vmnet_bridge_arg(arg, len, id, card, mac) || vm_bridge_ifname(id, name)) { admin_err("admin.invalidArgument", NULL); return; }
    int kind = vmnet_card(SYS_NET, card);
    if (kind != VMNET_WIRED) { admin_err(vmnet_card_error(kind), "card", card, NULL); return; }
    unsigned char hw[6];
    char own[32], p[96];
    snprintf(p, sizeof p, SYS_NET "/%s/address", card);
    if (ether(mac, hw) || (!read_line(p, own, sizeof own) && !strcmp(own, mac))) { admin_err("admin.invalidArgument", NULL); return; }
    vmnet_alias(alias, sizeof alias, r->uid, id);
    int idx = (int)if_nametoindex(name);
    if (idx) {
        /* already there: ours, for this user and machine, on that card with that address, is used again */
        uid_t u;
        char hid[37], addr[32], lower[96];
        if (link_alias(name, have, sizeof have) || !vmnet_alias_parse(have, &u, hid) || u != r->uid || strcmp(hid, id)) {
            admin_err("vm.bridgeBusy", "name", name, NULL); return;
        }
        struct stat st;
        snprintf(p, sizeof p, SYS_NET "/%s/address", name);
        snprintf(lower, sizeof lower, SYS_NET "/%s/lower_%s", name, card);
        if (read_line(p, addr, sizeof addr) || strcmp(addr, mac) || lstat(lower, &st)) {
            if (tap_in_use(idx)) { admin_err("vm.bridgeInUse", "name", name, NULL); return; }
            if (del_link(name)) { admin_err("vm.bridgeFailed", "name", name, NULL); return; }
            idx = 0;
        }
    }
    if (!idx) {
        idx = make_macvtap(name, (int)if_nametoindex(card), hw, alias);
        if (idx < 0) { fprintf(stderr, "admin: vm-net-bridge: %s on %s: %s\n", name, card, strerror(-idx)); admin_err("vm.bridgeFailed", "name", name, NULL); return; }
    }
    if (give_tap(name, idx, r->uid, r->gid)) { del_link(name); admin_err("vm.bridgeFailed", "name", name, NULL); return; }
    admin_ok("vm.bridgeReady", "name", name, NULL);
}

void vm_act_net_unbridge(const AdminReq *r, const char *arg, size_t len) {
    char id[37], name[VM_IFNAME_MAX + 1], have[128], hid[37];
    if (vmnet_unbridge_arg(arg, len, id) || vm_bridge_ifname(id, name)) { admin_err("admin.invalidArgument", NULL); return; }
    int idx = (int)if_nametoindex(name);
    if (!idx) { admin_ok("vm.bridgeGone", NULL); return; }
    uid_t u;
    if (link_alias(name, have, sizeof have) || !vmnet_alias_parse(have, &u, hid) || u != r->uid || strcmp(hid, id)) { admin_err("vm.bridgeBusy", "name", name, NULL); return; }
    if (tap_in_use(idx)) { admin_err("vm.bridgeInUse", "name", name, NULL); return; }
    if (del_link(name)) { admin_err("vm.bridgeFailed", "name", name, NULL); return; }
    admin_ok("vm.bridgeGone", NULL);
}

void vm_net_sweep(uid_t uid) {
    DIR *d = opendir(SYS_NET);
    if (!d) return;
    char names[64][VM_IFNAME_MAX + 1];
    int n = 0;
    for (struct dirent *e; n < 64 && (e = readdir(d));) {
        char have[128];
        uid_t u;
        if (strncmp(e->d_name, "lvm", 3) || strlen(e->d_name) > VM_IFNAME_MAX) continue;
        if (link_alias(e->d_name, have, sizeof have) || !vmnet_alias_parse(have, &u, NULL) || u != uid) continue;
        snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        int idx = (int)if_nametoindex(names[i]);
        if (idx && !tap_in_use(idx) && !del_link(names[i])) fprintf(stderr, "admin: removed %s, a machine's bridged network nobody uses\n", names[i]);
    }
}
