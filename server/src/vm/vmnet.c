/* vmnet.c — Machines' bridged network, the parts without root (see vmnet.h). */
#include "vmnet.h"
#include "machine.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int has(const char *sys, const char *name, const char *what) {
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s/%s", sys, name, what);
    return !lstat(p, &st);
}

static int read_int(const char *sys, const char *name, const char *what, int *v) {
    char p[512], b[32];
    snprintf(p, sizeof p, "%s/%s/%s", sys, name, what);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r <= 0) return -1;
    b[r] = 0;
    char *e;
    long x = strtol(b, &e, 10);
    if (e == b) return -1;
    *v = (int)x;
    return 0;
}

int vmnet_card(const char *sys, const char *name) {
    if (!vm_ifname_valid(name) || !has(sys, name, "")) return VMNET_MISSING;
    if (has(sys, name, "wireless") || has(sys, name, "phy80211")) return VMNET_WIFI;
    if (!has(sys, name, "device")) return VMNET_VIRTUAL;
    int type = 0;
    if (read_int(sys, name, "type", &type) || type != 1) return VMNET_NOT_ETHER;      /* ARPHRD_ETHER */
    if (has(sys, name, "master") || has(sys, name, "brport") || has(sys, name, "bonding_slave")) return VMNET_ENSLAVED;
    return VMNET_WIRED;
}

const char *vmnet_card_error(int kind) {
    switch (kind) {
    case VMNET_WIRED: return "";
    case VMNET_MISSING: return "vm.bridgeNoCard";
    case VMNET_WIFI: return "vm.bridgeWifi";
    case VMNET_ENSLAVED: return "vm.bridgeCardBusy";
    default: return "vm.bridgeNotWired";
    }
}

int vmnet_bridge_arg(const char *arg, size_t len, char id[37], char card[16], char mac[18]) {
    if (!arg || len != strlen(arg) || len < 36 + 1 + 1 + 1 + 17 || len > 36 + 1 + 15 + 1 + 17) return -1;
    const char *s1 = strchr(arg, ' '), *s2 = s1 ? strchr(s1 + 1, ' ') : NULL;
    if (!s1 || !s2 || strchr(s2 + 1, ' ') || s1 - arg != 36 || s2 - s1 - 1 < 1 || s2 - s1 - 1 > 15 || strlen(s2 + 1) != 17) return -1;
    memcpy(id, arg, 36); id[36] = 0;
    memcpy(card, s1 + 1, (size_t)(s2 - s1 - 1)); card[s2 - s1 - 1] = 0;
    memcpy(mac, s2 + 1, 18);
    return vm_uuid_valid(id) && vm_ifname_valid(card) && vm_mac_valid(mac) ? 0 : -1;
}

int vmnet_unbridge_arg(const char *arg, size_t len, char id[37]) {
    if (!arg || len != 36 || strlen(arg) != 36) return -1;
    memcpy(id, arg, 37);
    return vm_uuid_valid(id) ? 0 : -1;
}

void vmnet_alias(char *out, size_t n, uid_t uid, const char *id) { snprintf(out, n, "lordos-vm %u %s", (unsigned)uid, id); }

int vmnet_alias_parse(const char *alias, uid_t *uid, char *id) {
    if (strncmp(alias, "lordos-vm ", 10)) return 0;
    const char *p = alias + 10;
    char *e;
    if (*p < '0' || *p > '9') return 0;
    unsigned long u = strtoul(p, &e, 10);
    if (*e != ' ' || u > 0x7fffffffUL) return 0;
    char x[64];
    size_t l = strcspn(e + 1, "\n");
    if (l != 36) return 0;
    memcpy(x, e + 1, 36); x[36] = 0;
    if (e[37] && e[37] != '\n') return 0;
    if (!vm_uuid_valid(x)) return 0;
    *uid = (uid_t)u;
    if (id) memcpy(id, x, 37);
    return 1;
}

char *vmnet_cards_json(const char *sys) {
    DIR *d = opendir(sys);
    char *out = malloc(16 * 64 + 8);
    if (!out) { if (d) closedir(d); return NULL; }
    size_t o = 0;
    int k = 0;
    out[o++] = '[';
    for (struct dirent *e; d && k < 16 && (e = readdir(d));) {
        int kind = vmnet_card(sys, e->d_name);
        if (kind != VMNET_WIRED && kind != VMNET_WIFI) continue;
        if (kind == VMNET_WIFI && !has(sys, e->d_name, "device")) continue;
        char st[16] = "";
        char p[512];
        snprintf(p, sizeof p, "%s/%s/operstate", sys, e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        ssize_t r = fd >= 0 ? read(fd, st, sizeof st - 1) : -1;
        if (fd >= 0) close(fd);
        if (r > 0) st[r] = 0;
        o += (size_t)snprintf(out + o, 64, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"up\":%s}", k ? "," : "", e->d_name,
                              kind == VMNET_WIFI ? "wifi" : "wired", !strncmp(st, "up", 2) ? "true" : "false");
        k++;
    }
    if (d) closedir(d);
    out[o++] = ']';
    out[o] = 0;
    return out;
}
