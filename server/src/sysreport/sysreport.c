/* sysreport.c — System Information's collector (see sysreport.h). Every read is a small file opened
 * with O_NOFOLLOW where it's a leaf, cut to its first line and to printable ASCII; directories are
 * listed in name order so two runs on one machine give the same report. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sysreport.h"
#include "ids.h"
#include "../hw/edid.h"
#include "../hw/rpifw.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---- reading ---- */

/* A whole small file (at most max bytes, NUL-terminated); NULL when missing. *len gets its size. */
static char *read_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    char *p = malloc(max + 1);
    size_t got = 0;
    while (p && got < max) {
        ssize_t n = read(fd, p + got, max - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!p) return NULL;
    p[got] = 0;
    if (len) *len = got;
    return p;
}

/* The first line of a sysfs/proc attribute, trimmed, printable ASCII only. "" when missing. */
static void read_attr(const char *path, char *out, size_t size) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    ssize_t i = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
    size_t o = 0;
    for (; i < n && buf[i] && buf[i] != '\n' && o + 1 < size; i++)
        out[o++] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '?';
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = 0;
}

static void attr(const char *root, const char *dir, const char *name, char *out, size_t size) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s/%s", root, dir, name);
    read_attr(p, out, size);
}

static long long attr_num(const char *root, const char *dir, const char *name, long long def) {
    char v[64];
    attr(root, dir, name, v, sizeof v);
    if (!v[0]) return def;
    char *end;
    long long x = strtoll(v, &end, 0);
    return end == v ? def : x;
}

static int exists(const char *root, const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", root, path);
    struct stat st;
    return lstat(p, &st) == 0;
}

/* A hex ID attribute ("0x10de" or "10de") as `digits` lowercase hex digits; "" if not hex. */
static void hex_attr(const char *root, const char *dir, const char *name, int digits, char *out) {
    char v[32];
    attr(root, dir, name, v, sizeof v);
    const char *s = v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = strlen(s);
    out[0] = 0;
    if (!n || n > (size_t)digits) return;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return;
    size_t pad = (size_t)digits - n;
    for (size_t i = 0; i < pad; i++) out[i] = '0';
    for (size_t i = 0; i < n; i++) out[pad + i] = (char)tolower((unsigned char)s[i]);
    out[digits] = 0;
}

/* The last component of a link (a device's "driver", "subsystem"), [A-Za-z0-9_-] only. */
static void link_base(const char *root, const char *dir, const char *name, char *out, size_t size) {
    char p[1024], l[512];
    snprintf(p, sizeof p, "%s%s/%s", root, dir, name);
    ssize_t n = readlink(p, l, sizeof l - 1);
    out[0] = 0;
    if (n <= 0) return;
    l[n] = 0;
    const char *b = strrchr(l, '/');
    b = b ? b + 1 : l;
    size_t o = 0;
    for (; *b && o + 1 < size; b++) if (isalnum((unsigned char)*b) || *b == '_' || *b == '-') out[o++] = *b;
    out[o] = 0;
}

/* The names in <root><dir> (no dot entries), sorted; free with free_list. */
static int list_dir(const char *root, const char *dir, char ***names) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", root, dir);
    *names = NULL;
    DIR *d = opendir(p);
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 4096) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **nn = realloc(*names, (size_t)cap * sizeof *nn);
            if (!nn) break;
            *names = nn;
        }
        if (!((*names)[n] = strdup(e->d_name))) break;
        n++;
    }
    closedir(d);
    /* natural order: "card2" before "card10" */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const char *a = (*names)[j - 1], *b = (*names)[j];
            size_t la = strcspn(a, "0123456789"), lb = strcspn(b, "0123456789");
            int c = (la == lb && !strncmp(a, b, la) && isdigit((unsigned char)a[la]) && isdigit((unsigned char)b[lb]))
                ? (atoll(a + la) > atoll(b + lb) ? 1 : atoll(a + la) < atoll(b + lb) ? -1 : strcmp(a, b))
                : strcmp(a, b);
            if (c <= 0) break;
            char *t = (*names)[j - 1]; (*names)[j - 1] = (*names)[j]; (*names)[j] = t;
        }
    return n;
}

static void free_list(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* "key<spaces>: value" in /proc/cpuinfo-style text (the first one); "" when missing. */
static void field(const char *txt, const char *key, char *out, size_t size) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = txt; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, key, kl) || (p[kl] != ' ' && p[kl] != '\t' && p[kl] != ':')) continue;
        const char *v = strchr(p, ':');
        const char *nl = strchr(p, '\n');
        if (!v || (nl && v > nl)) continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        size_t o = 0;
        for (; *v && *v != '\n' && o + 1 < size; v++) out[o++] = (*v >= 0x20 && *v < 0x7f) ? *v : '?';
        while (o && out[o - 1] == ' ') o--;
        out[o] = 0;
        return;
    }
}

/* os-release: KEY=value or KEY="value". */
static void osr_field(const char *txt, const char *key, char *out, size_t size) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = txt; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        char q = (*v == '"' || *v == '\'') ? *v++ : 0;
        size_t o = 0;
        for (; *v && *v != '\n' && *v != q && o + 1 < size; v++) out[o++] = (*v >= 0x20 && *v < 0x7f) ? *v : '?';
        out[o] = 0;
        return;
    }
}

/* ---- machine ---- */

static const char *chassis_name(int t) {
    switch (t) {
    case 3: case 4: case 5: case 6: case 7: case 15: case 16: case 24: return "desktop";
    case 8: case 9: case 10: case 14: case 31: case 32: return "laptop";
    case 11: return "handheld";
    case 13: return "all-in-one";
    case 17: case 23: case 25: case 28: case 29: return "server";
    case 30: return "tablet";
    case 35: case 36: return "mini-pc";
    case 1: return "other";
    default: return "";
    }
}

/* DMI vendor strings firmware leaves unset. */
static void drop_placeholder(char *s) {
    static const char *junk[] = { "To be filled by O.E.M.", "To Be Filled By O.E.M.", "Default string", "System Product Name",
                                  "System manufacturer", "Not Applicable", "Not Specified", "None", "0123456789", "Type1ProductConfigId" };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) if (!strcmp(s, junk[i])) { s[0] = 0; return; }
}

static void collect_machine(SysReport *r, const char *root, const char *cpuinfo) {
    SrMachine *m = &r->machine;
    const char *dmi = "/sys/class/dmi/id";
    attr(root, dmi, "sys_vendor", m->vendor, sizeof m->vendor);
    attr(root, dmi, "product_name", m->product, sizeof m->product);
    attr(root, dmi, "product_version", m->version, sizeof m->version);
    attr(root, dmi, "board_vendor", m->board_vendor, sizeof m->board_vendor);
    attr(root, dmi, "board_name", m->board_name, sizeof m->board_name);
    attr(root, dmi, "board_version", m->board_version, sizeof m->board_version);
    attr(root, dmi, "bios_vendor", m->bios_vendor, sizeof m->bios_vendor);
    attr(root, dmi, "bios_version", m->bios_version, sizeof m->bios_version);
    attr(root, dmi, "bios_date", m->bios_date, sizeof m->bios_date);
    drop_placeholder(m->vendor); drop_placeholder(m->product); drop_placeholder(m->version);
    drop_placeholder(m->board_vendor); drop_placeholder(m->board_name); drop_placeholder(m->board_version);
    snprintf(m->chassis, sizeof m->chassis, "%s", chassis_name((int)attr_num(root, dmi, "chassis_type", 2)));

    char p[1024];
    snprintf(p, sizeof p, "%s/proc/device-tree/model", root);
    read_attr(p, m->model, sizeof m->model);

    if (exists(root, "/sys/firmware/efi")) snprintf(m->firmware, sizeof m->firmware, "uefi");
    else if (m->bios_vendor[0]) snprintf(m->firmware, sizeof m->firmware, "bios");
    else if (m->model[0]) snprintf(m->firmware, sizeof m->firmware, "dt");
    m->secure_boot = -1;
    snprintf(p, sizeof p, "%s/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", root);
    size_t len = 0;
    char *sb = read_file(p, 16, &len);
    if (sb && len >= 5) m->secure_boot = sb[4] == 1;     /* 4 bytes of attributes, then the value */
    free(sb);

    /* virtualization: the CPU's "hypervisor" flag, then who the DMI says made the machine */
    char flags[4096];
    field(cpuinfo, "flags", flags, sizeof flags);
    int hv = strstr(flags, " hypervisor") != NULL;
    char compat[256] = "";
    snprintf(p, sizeof p, "%s/proc/device-tree/compatible", root);
    read_attr(p, compat, sizeof compat);
    if (!strncmp(compat, "linux,dummy-virt", 16)) hv = 1;
    if (hv) {
        const char *v = "vm";
        if (strstr(m->vendor, "QEMU") || strstr(compat, "dummy-virt")) v = "kvm";
        else if (strstr(m->vendor, "VMware")) v = "vmware";
        else if (strstr(m->vendor, "innotek") || strstr(m->product, "VirtualBox")) v = "virtualbox";
        else if (strstr(m->vendor, "Microsoft")) v = "hyper-v";
        else if (strstr(m->vendor, "Xen")) v = "xen";
        else if (strstr(m->vendor, "Parallels")) v = "parallels";
        snprintf(m->virt, sizeof m->virt, "%s", v);
        if (!m->chassis[0] || !strcmp(m->chassis, "other")) snprintf(m->chassis, sizeof m->chassis, "vm");
    }
}

/* ---- CPU and memory ---- */

static void arm_core(unsigned part, char *out, size_t size) {
    static const struct { unsigned part; const char *name; } parts[] = {
        { 0xd03, "Cortex-A53" }, { 0xd04, "Cortex-A35" }, { 0xd05, "Cortex-A55" }, { 0xd07, "Cortex-A57" },
        { 0xd08, "Cortex-A72" }, { 0xd09, "Cortex-A73" }, { 0xd0a, "Cortex-A75" }, { 0xd0b, "Cortex-A76" },
        { 0xd0c, "Neoverse-N1" }, { 0xd0d, "Cortex-A77" }, { 0xd41, "Cortex-A78" }, { 0xd44, "Cortex-X1" },
        { 0xd46, "Cortex-A510" }, { 0xd47, "Cortex-A710" }, { 0xd80, "Cortex-A520" }, { 0xd81, "Cortex-A720" },
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
        if (parts[i].part == part) { snprintf(out, size, "%s", parts[i].name); return; }
}

/* "48K", "1024 K", "32M" → KiB */
static int cache_kb(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    while (*end == ' ') end++;
    if (*end == 'M') v *= 1024;
    return v > 0 && v < (1L << 30) ? (int)v : 0;
}

static void collect_cpu(SysReport *r, const char *root, const char *cpuinfo) {
    SrCpu *c = &r->cpu;
    field(cpuinfo, "model name", c->model, sizeof c->model);
    field(cpuinfo, "vendor_id", c->vendor, sizeof c->vendor);
    field(cpuinfo, "microcode", c->microcode, sizeof c->microcode);
    if (!c->vendor[0]) {
        char impl[16], part[16];
        field(cpuinfo, "CPU implementer", impl, sizeof impl);
        field(cpuinfo, "CPU part", part, sizeof part);
        if (impl[0]) {
            unsigned long i = strtoul(impl, NULL, 0);
            snprintf(c->vendor, sizeof c->vendor, "%s", i == 0x41 ? "ARM" : i == 0x51 ? "Qualcomm" : i == 0x61 ? "Apple" : "other");
        }
        if (!c->model[0] && part[0]) arm_core((unsigned)strtoul(part, NULL, 0), c->model, sizeof c->model);
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/proc/sys/kernel/arch", root);
    read_attr(p, c->arch, sizeof c->arch);
    if (!c->arch[0] && !root[0]) { struct utsname u; if (!uname(&u)) snprintf(c->arch, sizeof c->arch, "%.15s", u.machine); }

    /* instruction sets that matter for LordOS's own code and for apps */
    static const char *x86[] = { "sse4_2", "avx", "avx2", "fma", "avx512f", "aes", "sha_ni", "vaes" };
    static const char *arm[] = { "asimd", "sve", "sve2", "aes", "sha2", "crc32", "atomics" };
    char flags[4096];
    field(cpuinfo, "flags", flags, sizeof flags);
    int is_arm = !flags[0];
    if (is_arm) field(cpuinfo, "Features", flags, sizeof flags);
    const char **want = is_arm ? arm : x86;
    size_t nwant = is_arm ? sizeof arm / sizeof arm[0] : sizeof x86 / sizeof x86[0];
    for (size_t i = 0; i < nwant; i++) {
        size_t wl = strlen(want[i]);
        for (const char *f = flags; (f = strstr(f, want[i])); f += wl)
            if ((f == flags || f[-1] == ' ') && (f[wl] == ' ' || !f[wl])) {
                size_t o = strlen(c->flags);
                snprintf(c->flags + o, sizeof c->flags - o, "%s%s", o ? " " : "", want[i]);
                break;
            }
    }

    /* topology: unique (package, core) pairs among the CPUs listed in sysfs */
    char **cpus;
    int n = list_dir(root, "/sys/devices/system/cpu", &cpus);
    long pk[1024], co[1024];
    int npairs = 0, maxpk = -1;
    for (int i = 0; i < n; i++) {
        if (strncmp(cpus[i], "cpu", 3) || !isdigit((unsigned char)cpus[i][3])) continue;
        char d[128];
        snprintf(d, sizeof d, "/sys/devices/system/cpu/%s", cpus[i]);
        if (attr_num(root, d, "online", 1) == 0) continue;
        c->threads++;
        snprintf(d, sizeof d, "/sys/devices/system/cpu/%s/topology", cpus[i]);
        long pkg = (long)attr_num(root, d, "physical_package_id", 0), core = (long)attr_num(root, d, "core_id", c->threads);
        if (pkg > maxpk) maxpk = (int)pkg;
        int seen = 0;
        for (int j = 0; j < npairs && !seen; j++) seen = pk[j] == pkg && co[j] == core;
        if (!seen && npairs < 1024) { pk[npairs] = pkg; co[npairs] = core; npairs++; }
    }
    free_list(cpus, n);
    c->cores = npairs;
    c->sockets = maxpk + 1;
    if (!c->threads) {                  /* no sysfs: count /proc/cpuinfo's processors */
        for (const char *q = cpuinfo; (q = strstr(q, "processor")); q += 9) if (q == cpuinfo || q[-1] == '\n') c->threads++;
        c->cores = c->threads;
        c->sockets = c->threads ? 1 : 0;
    }

    const char *f = "/sys/devices/system/cpu/cpu0/cpufreq";
    c->min_mhz = (int)(attr_num(root, f, "cpuinfo_min_freq", 0) / 1000);
    c->max_mhz = (int)(attr_num(root, f, "cpuinfo_max_freq", 0) / 1000);
    attr(root, f, "scaling_governor", c->governor, sizeof c->governor);

    char **idx;
    n = list_dir(root, "/sys/devices/system/cpu/cpu0/cache", &idx);
    for (int i = 0; i < n; i++) {
        if (strncmp(idx[i], "index", 5)) continue;
        char d[128], type[24], size[24];
        snprintf(d, sizeof d, "/sys/devices/system/cpu/cpu0/cache/%s", idx[i]);
        long lvl = (long)attr_num(root, d, "level", 0);
        attr(root, d, "type", type, sizeof type);
        attr(root, d, "size", size, sizeof size);
        int kb = cache_kb(size);
        if (lvl == 1 && !strcmp(type, "Data")) c->l1d_kb = kb;
        else if (lvl == 1 && !strcmp(type, "Instruction")) c->l1i_kb = kb;
        else if (lvl == 2) c->l2_kb = kb;
        else if (lvl == 3) c->l3_kb = kb;
    }
    free_list(idx, n);
}

static void collect_memory(SysReport *r, const char *root) {
    char p[1024], v[64];
    snprintf(p, sizeof p, "%s/proc/meminfo", root);
    char *txt = read_file(p, 16384, NULL);
    if (txt) {
        field(txt, "MemTotal", v, sizeof v); r->mem.total_kb = strtoull(v, NULL, 10);
        field(txt, "SwapTotal", v, sizeof v); r->mem.swap_kb = strtoull(v, NULL, 10);
        free(txt);
    }
    char **b;
    int n = list_dir(root, "/sys/block", &b);
    for (int i = 0; i < n && r->mem.zram_n < SR_MAX_ZRAM; i++) {
        if (strncmp(b[i], "zram", 4)) continue;
        char d[96], algo[128];
        snprintf(d, sizeof d, "/sys/block/%s", b[i]);
        long long size = attr_num(root, d, "disksize", 0);
        if (size <= 0) continue;
        int k = r->mem.zram_n++;
        r->mem.zram_kb[k] = (uint64_t)size / 1024;
        attr(root, d, "comp_algorithm", algo, sizeof algo);      /* "lzo [lz4] zstd": the one in brackets */
        const char *a = strchr(algo, '['), *e = a ? strchr(a, ']') : NULL;
        if (a && e) snprintf(r->mem.zram_algo[k], sizeof r->mem.zram_algo[k], "%.*s", (int)(e - a - 1), a + 1);
    }
    free_list(b, n);
}

/* ---- graphics and displays ---- */

static void collect_gpus(SysReport *r, const char *root, const IdsDb *pci) {
    char **e;
    int n = list_dir(root, "/sys/class/drm", &e);
    for (int i = 0; i < n && r->ngpu < SR_MAX_GPU; i++) {
        if (strncmp(e[i], "card", 4) || strchr(e[i], '-')) continue;
        SrGpu *g = &r->gpu[r->ngpu++];
        char d[160];
        snprintf(g->card, sizeof g->card, "%.11s", e[i]);
        snprintf(d, sizeof d, "/sys/class/drm/%s/device", e[i]);
        link_base(root, d, "driver", g->driver, sizeof g->driver);
        hex_attr(root, d, "vendor", 4, g->vendor_id);
        hex_attr(root, d, "device", 4, g->device_id);
        snprintf(g->bus, sizeof g->bus, "%s", g->vendor_id[0] ? "pci" : "platform");
        if (g->vendor_id[0]) ids_lookup(pci, g->vendor_id, g->device_id, g->vendor, sizeof g->vendor, g->device, sizeof g->device);
        g->boot_vga = attr_num(root, d, "boot_vga", 0) == 1;
        char **dn;
        char dd[192];
        snprintf(dd, sizeof dd, "%s/drm", d);
        int m = list_dir(root, dd, &dn);
        for (int j = 0; j < m; j++) if (!strncmp(dn[j], "renderD", 7)) g->render_node = 1;
        free_list(dn, m);
        size_t cl = strlen(e[i]);
        for (int j = 0; j < n; j++) {
            if (strncmp(e[j], e[i], cl) || e[j][cl] != '-' || strstr(e[j], "Writeback")) continue;
            char cd[160], st[24];
            snprintf(cd, sizeof cd, "/sys/class/drm/%s", e[j]);
            attr(root, cd, "status", st, sizeof st);
            g->connectors++;
            if (!strcmp(st, "connected")) g->connected++;
        }
    }
    free_list(e, n);
}

static void collect_displays(SysReport *r, const char *root) {
    char **e;
    int n = list_dir(root, "/sys/class/drm", &e);
    for (int i = 0; i < n && r->ndisplay < SR_MAX_DISPLAY; i++) {
        const char *dash = strchr(e[i], '-');
        if (strncmp(e[i], "card", 4) || !dash || strstr(e[i], "Writeback")) continue;
        char d[160], st[24], en[24];
        snprintf(d, sizeof d, "/sys/class/drm/%s", e[i]);
        attr(root, d, "status", st, sizeof st);
        if (strcmp(st, "connected")) continue;
        SrDisplay *x = &r->display[r->ndisplay++];
        x->connected = 1;
        attr(root, d, "enabled", en, sizeof en);
        x->enabled = !strcmp(en, "enabled");
        snprintf(x->card, sizeof x->card, "%.*s", (int)(dash - e[i]) < 11 ? (int)(dash - e[i]) : 11, e[i]);
        snprintf(x->connector, sizeof x->connector, "%.31s", dash + 1);
        attr(root, d, "modes", x->mode, sizeof x->mode);
        char p[1024];
        snprintf(p, sizeof p, "%s%s/edid", root, d);
        size_t len = 0;
        uint8_t *raw = (uint8_t *)read_file(p, 512, &len);
        Edid ed;
        if (raw && len >= 128 && !edid_parse(raw, len, &ed)) {
            /* the identity without the serial: name, maker, product, year, size, native mode */
            snprintf(x->name, sizeof x->name, "%s", ed.name);
            snprintf(x->pnp, sizeof x->pnp, "%s", ed.vendor);
            snprintf(x->product, sizeof x->product, "%04x", ed.product);
            x->year = ed.year;
            x->width_cm = ed.width_cm;
            x->height_cm = ed.height_cm;
            x->diag_tenths = edid_diagonal_tenths(&ed);
            x->pref_w = ed.pref_w;
            x->pref_h = ed.pref_h;
        }
        free(raw);
    }
    free_list(e, n);
}

/* ---- storage ---- */

static void collect_disks(SysReport *r, const char *root) {
    char **b;
    int n = list_dir(root, "/sys/block", &b);
    static const char *skip[] = { "loop", "ram", "zram", "dm-", "md", "nbd", "mtdblock", "fd" };
    for (int i = 0; i < n && r->ndisk < SR_MAX_DISK; i++) {
        int sk = strstr(b[i], "boot") || strstr(b[i], "rpmb");
        for (size_t k = 0; k < sizeof skip / sizeof skip[0] && !sk; k++) sk = !strncmp(b[i], skip[k], strlen(skip[k]));
        if (sk) continue;
        char d[96];
        snprintf(d, sizeof d, "/sys/block/%s", b[i]);
        long long sectors = attr_num(root, d, "size", 0);
        if (sectors <= 0) continue;     /* an empty card reader */
        SrDisk *k = &r->disk[r->ndisk++];
        snprintf(k->name, sizeof k->name, "%.15s", b[i]);
        k->bytes = (uint64_t)sectors * 512;
        char q[128];
        snprintf(q, sizeof q, "%s/queue", d);
        k->rotational = attr_num(root, q, "rotational", 0) == 1;
        k->removable = attr_num(root, d, "removable", 0) == 1;
        snprintf(q, sizeof q, "%s/device", d);
        attr(root, q, "model", k->model, sizeof k->model);
        attr(root, q, "vendor", k->vendor, sizeof k->vendor);
        if (!strncmp(b[i], "nvme", 4)) {
            snprintf(k->transport, sizeof k->transport, "nvme");
            char spd[32], wid[8];
            snprintf(q, sizeof q, "%s/device/device", d);
            attr(root, q, "current_link_speed", spd, sizeof spd);
            attr(root, q, "current_link_width", wid, sizeof wid);
            if (spd[0] && wid[0]) snprintf(k->link, sizeof k->link, "%.20s x%.4s", spd, wid);
        } else if (!strncmp(b[i], "mmcblk", 6)) {
            snprintf(k->transport, sizeof k->transport, "mmc");
        } else if (!strncmp(b[i], "vd", 2)) {
            snprintf(k->transport, sizeof k->transport, "virtio");
        } else {
            char p[1024], l[1024];
            snprintf(p, sizeof p, "%s%s", root, d);
            ssize_t m = readlink(p, l, sizeof l - 1);
            l[m > 0 ? m : 0] = 0;
            snprintf(k->transport, sizeof k->transport, "%s", strstr(l, "/usb") ? "usb" : strstr(l, "/ata") ? "sata" : "scsi");
        }
    }
    free_list(b, n);
}

/* ---- buses ---- */

static void collect_pci(SysReport *r, const char *root, const IdsDb *ids) {
    char **e;
    int n = list_dir(root, "/sys/bus/pci/devices", &e);
    for (int i = 0; i < n && r->npci < SR_MAX_PCI; i++) {
        char d[128];
        snprintf(d, sizeof d, "/sys/bus/pci/devices/%s", e[i]);
        SrPci *x = &r->pci[r->npci];
        hex_attr(root, d, "vendor", 4, x->vendor_id);
        hex_attr(root, d, "device", 4, x->device_id);
        hex_attr(root, d, "class", 6, x->slot_class);
        if (!x->vendor_id[0]) { memset(x, 0, sizeof *x); continue; }
        link_base(root, d, "driver", x->driver, sizeof x->driver);
        ids_lookup(ids, x->vendor_id, x->device_id, x->vendor, sizeof x->vendor, x->device, sizeof x->device);
        char cname[48], sname[48];
        ids_pci_class(ids, x->slot_class, cname, sizeof cname, sname, sizeof sname);
        snprintf(x->class_name, sizeof x->class_name, "%s", sname[0] ? sname : cname);
        r->npci++;
    }
    free_list(e, n);
}

static void collect_usb(SysReport *r, const char *root, const IdsDb *ids) {
    char **e;
    int n = list_dir(root, "/sys/bus/usb/devices", &e);
    for (int i = 0; i < n && r->nusb < SR_MAX_USB; i++) {
        if (strchr(e[i], ':') || !strncmp(e[i], "usb", 3)) continue;   /* interfaces and root hubs */
        char d[128];
        snprintf(d, sizeof d, "/sys/bus/usb/devices/%s", e[i]);
        SrUsb *u = &r->usb[r->nusb];
        hex_attr(root, d, "idVendor", 4, u->vendor_id);
        hex_attr(root, d, "idProduct", 4, u->product_id);
        if (!u->vendor_id[0]) { memset(u, 0, sizeof *u); continue; }
        hex_attr(root, d, "bDeviceClass", 2, u->cls);
        char s[16];
        attr(root, d, "speed", s, sizeof s);
        u->speed_mbps = atoi(s);
        char ifd[160];
        snprintf(ifd, sizeof ifd, "%s/%s:1.0", d, e[i]);
        link_base(root, ifd, "driver", u->driver, sizeof u->driver);
        ids_lookup(ids, u->vendor_id, u->product_id, u->vendor, sizeof u->vendor, u->product, sizeof u->product);
        r->nusb++;
    }
    free_list(e, n);
}

/* ---- network, audio, Bluetooth, power, thermal ---- */

static void collect_net(SysReport *r, const char *root) {
    char **e;
    int n = list_dir(root, "/sys/class/net", &e);
    for (int i = 0; i < n && r->nnet < SR_MAX_NET; i++) {
        if (!strcmp(e[i], "lo")) continue;
        char d[96], w[128], st[16];
        snprintf(d, sizeof d, "/sys/class/net/%s", e[i]);
        SrNet *x = &r->net[r->nnet++];
        int has_dev = exists(root, (snprintf(w, sizeof w, "%s/device", d), w));
        snprintf(w, sizeof w, "%s/wireless", d);
        int wifi = exists(root, w) || exists(root, (snprintf(w, sizeof w, "%s/phy80211", d), w));
        long long type = attr_num(root, d, "type", 1);
        snprintf(x->kind, sizeof x->kind, "%s", wifi ? "wifi" : !has_dev ? "virtual" : type == 1 ? "ethernet" : "other");
        snprintf(w, sizeof w, "%s/device", d);
        link_base(root, w, "driver", x->driver, sizeof x->driver);
        attr(root, d, "operstate", st, sizeof st);
        x->up = !strcmp(st, "up");
        x->speed_mbps = x->up ? (int)attr_num(root, d, "speed", -1) : -1;
        if (x->speed_mbps <= 0) x->speed_mbps = -1;
    }
    free_list(e, n);
}

/* A card's name for people. ALSA's short name is often just its id ("vc4-hdmi-0"), and a USB card's is the
 * chip's generic string, so: a USB card by its IDs in usb.ids (/proc/asound/cardN/usbid) or, where that list isn't
 * installed, by the start of ALSA's long name ("C-Media Electronics Inc. USB Audio Device at usb-1000…", the bus
 * path dropped); the Raspberry Pi's two HDMI outputs by their port; anything else as ALSA calls it. */
static void audio_name(const char *root, const IdsDb *usb, int card, const char *driver, const char *shortname, const char *longname, char *out) {
    char d[64], id[16], vname[SR_NAME] = "", dname[SR_NAME] = "";
    int port;
    snprintf(d, sizeof d, "/proc/asound/card%d", card);
    attr(root, d, "usbid", id, sizeof id);
    if (strlen(id) == 9 && id[4] == ':') {
        id[4] = 0;
        ids_lookup(usb, id, id + 5, vname, sizeof vname, dname, sizeof dname);
        if (dname[0]) { snprintf(out, SR_NAME, "%.40s %.54s", vname, dname); return; }
        while (*longname == ' ') longname++;
        const char *at = strstr(longname, " at ");
        if (at && at != longname) {
            size_t o = 0;
            for (const char *s = longname; s < at && o + 1 < SR_NAME; s++) out[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
            out[o] = 0;
            return;
        }
    }
    if (!strcmp(driver, "vc4-hdmi") && sscanf(shortname, "vc4-hdmi-%d", &port) == 1) {
        snprintf(out, SR_NAME, "Raspberry Pi HDMI %d audio", port);
        return;
    }
    snprintf(out, SR_NAME, "%s", shortname);
}

static void collect_audio(SysReport *r, const char *root, const IdsDb *usb) {
    char p[1024];
    snprintf(p, sizeof p, "%s/proc/asound/cards", root);
    char *txt = read_file(p, 16384, NULL);
    /* " 0 [PCH            ]: HDA-Intel - HDA Intel PCH": the text after " - " */
    for (char *l = txt; l && *l && r->naudio < SR_MAX_AUDIO; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char *b = strchr(l, '['), *end = strstr(l, "]: "), *dash = end ? strstr(end, " - ") : NULL;
        if (b && dash) {
            char name[SR_NAME], driver[32] = "";
            size_t o = 0;
            for (const char *s = dash + 3; *s && o + 1 < sizeof name; s++) name[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
            name[o] = 0;
            size_t dl = (size_t)(dash - end) - 3;
            if (dl < sizeof driver) { memcpy(driver, end + 3, dl); driver[dl] = 0; }
            char *next = nl ? nl + 1 : NULL, *next_nl = next ? strchr(next, '\n') : NULL;
            if (next_nl) *next_nl = 0;
            audio_name(root, usb, atoi(l), driver, name, next ? next : "", r->audio[r->naudio++]);
            if (next_nl) *next_nl = '\n';
        }
        l = nl ? nl + 1 : NULL;
    }
    free(txt);
}

static void collect_bt(SysReport *r, const char *root) {
    char **e;
    int n = list_dir(root, "/sys/class/bluetooth", &e);
    for (int i = 0; i < n && r->nbt < 4; i++) {
        if (strncmp(e[i], "hci", 3) || strchr(e[i], ':')) continue;
        char d[96];
        snprintf(d, sizeof d, "/sys/class/bluetooth/%s/device", e[i]);
        link_base(root, d, "driver", r->bt_driver[r->nbt], sizeof r->bt_driver[0]);
        r->nbt++;
    }
    free_list(e, n);
}

static void collect_power(SysReport *r, const char *root) {
    char **e;
    int n = list_dir(root, "/sys/class/power_supply", &e);
    for (int i = 0; i < n && r->npower < SR_MAX_POWER; i++) {
        char d[128], type[24], scope[24];
        snprintf(d, sizeof d, "/sys/class/power_supply/%s", e[i]);
        attr(root, d, "scope", scope, sizeof scope);
        if (!strcmp(scope, "Device")) continue;     /* a mouse's or headset's battery */
        attr(root, d, "type", type, sizeof type);
        SrPower *x = &r->power[r->npower++];
        snprintf(x->kind, sizeof x->kind, "%s", !strcmp(type, "Battery") ? "battery" : !strcmp(type, "Mains") ? "ac"
                 : !strncmp(type, "USB", 3) ? "usb" : !strcmp(type, "UPS") ? "ups" : "other");
        attr(root, d, "manufacturer", x->vendor, sizeof x->vendor);
        attr(root, d, "model_name", x->model, sizeof x->model);
        attr(root, d, "technology", x->technology, sizeof x->technology);
        attr(root, d, "status", x->status, sizeof x->status);
        x->capacity = (int)attr_num(root, d, "capacity", -1);
        x->online = (int)attr_num(root, d, "online", -1);
        long long full = attr_num(root, d, "energy_full", -1), design = attr_num(root, d, "energy_full_design", -1);
        if (full < 0 || design <= 0) { full = attr_num(root, d, "charge_full", -1); design = attr_num(root, d, "charge_full_design", -1); }
        x->health = full >= 0 && design > 0 ? (int)(full * 100 / design) : -1;
    }
    free_list(e, n);
}

static void collect_thermal(SysReport *r, const char *root) {
    char **e;
    int n = list_dir(root, "/sys/class/thermal", &e);
    for (int i = 0; i < n && r->nthermal < SR_MAX_THERMAL; i++) {
        if (strncmp(e[i], "thermal_zone", 12)) continue;
        char d[96];
        snprintf(d, sizeof d, "/sys/class/thermal/%s", e[i]);
        SrThermal *t = &r->thermal[r->nthermal++];
        attr(root, d, "type", t->type, sizeof t->type);
        t->milli_c = (int)attr_num(root, d, "temp", INT32_MIN);
    }
    free_list(e, n);
    n = list_dir(root, "/sys/class/hwmon", &e);
    for (int i = 0; i < n && r->nthermal < SR_MAX_THERMAL; i++) {
        char d[96];
        snprintf(d, sizeof d, "/sys/class/hwmon/%s", e[i]);
        SrThermal *t = &r->thermal[r->nthermal];
        attr(root, d, "name", t->type, sizeof t->type);
        if (!t->type[0]) continue;
        t->milli_c = (int)attr_num(root, d, "temp1_input", INT32_MIN);
        char **f;
        int m = list_dir(root, d, &f);
        for (int j = 0; j < m; j++) if (!strncmp(f[j], "fan", 3) && strstr(f[j], "_input")) t->fans++;
        free_list(f, m);
        r->nthermal++;
    }
    free_list(e, n);
}

/* ---- Raspberry Pi ---- */

static void collect_pi(SysReport *r, const char *root, const char *cpuinfo) {
    char p[1024], compat[512] = "";
    snprintf(p, sizeof p, "%s/proc/device-tree/compatible", root);
    size_t len = 0;
    char *c = read_file(p, sizeof compat - 1, &len);
    if (c) {
        for (size_t i = 0; i < len; i++) if (!c[i]) c[i] = ' ';     /* NUL-separated list */
        snprintf(compat, sizeof compat, "%s", c);
        free(c);
    }
    r->pi.is_pi = strstr(compat, "raspberrypi") != NULL;
    r->pi.throttled = -1;
    r->pi.uv_alarm = -1;
    if (!r->pi.is_pi) return;
    snprintf(r->pi.model, sizeof r->pi.model, "%s", r->machine.model);
    field(cpuinfo, "Revision", r->pi.revision, sizeof r->pi.revision);
    /* the bootloader that started this boot: the device tree the firmware hands over, world-readable, where
     * rpi-eeprom-update itself looks first. version: a git hash; build-timestamp: big-endian seconds. */
    char ver[64];
    snprintf(p, sizeof p, "%s/proc/device-tree/chosen/bootloader/version", root);
    read_attr(p, ver, sizeof ver);
    size_t vl = strlen(ver);
    if (vl >= 7 && vl < sizeof r->pi.bootloader_version && strspn(ver, "0123456789abcdef") == vl)
        snprintf(r->pi.bootloader_version, sizeof r->pi.bootloader_version, "%s", ver);
    snprintf(p, sizeof p, "%s/proc/device-tree/chosen/bootloader/build-timestamp", root);
    char *ts = read_file(p, 8, &len);
    if (ts && len == 4) {
        const unsigned char *u = (const unsigned char *)ts;
        time_t t = (time_t)(((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) | ((uint32_t)u[2] << 8) | u[3]);
        struct tm tm;
        if (t >= 1000000000 && gmtime_r(&t, &tm)) strftime(r->pi.bootloader_date, sizeof r->pi.bootloader_date, "%Y-%m-%d", &tm);
    }
    free(ts);
    /* what the firmware says about power and throttling (hw/rpifw.c, shared with LordBench): rpi_volt's
     * under-voltage alarm (the only one the LordOS rpi-6.18.y kernel has) and get_throttled where an older
     * kernel has it; whatever the kernel doesn't show stays unknown */
    RpiFw fw;
    rpifw_read(root, &fw);
    r->pi.throttled = fw.throttled;
    r->pi.uv_alarm = fw.uv_alarm;
}

/* ---- the boot medium (0.8.7) ----
 *
 * What the system started from, as one word: the device under / (installed) or under live-boot's
 * /run/live/medium (live), found in /proc/self/mountinfo by its major:minor and classified by its sysfs path,
 * all world-readable. Device mapper and RAID layers are followed through "slaves". Only the class leaves this
 * function (and a USB medium's link speed): never the device's name, its model, a serial or a path. */

#define BOOT_MAX_DEPTH 4

/* The sysfs path ("/sys/devices/…") a block link points at (<root><link> is a symlink such as
 * /sys/dev/block/8:1 -> ../../devices/…/block/sda/sda1); "" when it isn't one. */
static void blk_sysfs(const char *root, const char *link, char *out, size_t size) {
    char p[1024], l[1024];
    out[0] = 0;
    snprintf(p, sizeof p, "%s%s", root, link);
    ssize_t n = readlink(p, l, sizeof l - 1);
    if (n <= 0) return;
    l[n] = 0;
    const char *d = strstr(l, "devices/");
    if (!d || strstr(d, "/../") || strstr(d, "/./")) return;
    snprintf(out, size, "/sys/%s", d);
}

/* The last component of a path ("" when the path ends in '/'). */
static const char *base_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* A block device name as the kernel writes them ([A-Za-z0-9_-], at most 31 characters). */
static int blk_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 31) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    return 1;
}

/* attr() for the long sysfs paths of the boot medium: nothing ("") when <root><dir>/<name> doesn't fit */
static void attr_long(const char *root, const char *dir, const char *name, char *out, size_t size) {
    char p[2400];
    out[0] = 0;
    int n = snprintf(p, sizeof p, "%s%s/%s", root, dir, name);
    if (n > 0 && (size_t)n < sizeof p) read_attr(p, out, size);
}

/* A USB device's link speed in Mbps: the deepest "<bus>-<port>[.<port>…]" component after "usbN" in a disk's
 * sysfs path holds "speed" (1.5, 12, 480, 5000…); -1 when it can't be read. */
static int usb_link_mbps(const char *root, const char *dev) {
    char best[1024];
    best[0] = 0;
    size_t len = strlen(dev);
    if (len >= sizeof best) return -1;
    int after_usb = 0;
    for (size_t i = 1; i <= len; i++) {
        if (dev[i] != '/' && dev[i] != 0) continue;
        size_t s = i;                                   /* the component is dev[start, i) */
        while (s > 0 && dev[s - 1] != '/') s--;
        const char *c = dev + s;
        size_t cl = i - s;
        if (cl > 3 && !strncmp(c, "usb", 3) && isdigit((unsigned char)c[3])) { after_usb = 1; continue; }
        if (!after_usb || !cl || !isdigit((unsigned char)c[0])) continue;
        size_t dash = 0;
        int ok = 1;
        for (size_t k = 0; k < cl; k++) {
            if (c[k] == '-') dash++;
            else if (!isdigit((unsigned char)c[k]) && c[k] != '.') ok = 0;
        }
        if (ok && dash == 1) { memcpy(best, dev, i); best[i] = 0; }
    }
    if (!best[0]) return -1;
    char v[32];
    attr_long(root, best, "speed", v, sizeof v);
    if (!v[0]) return -1;
    char *end;
    double mbps = strtod(v, &end);
    if (end == v || mbps <= 0 || mbps > 1000000) return -1;
    return mbps < 1 ? 1 : (int)mbps;
}

/* Classify the block device at <root><link> (a /sys/dev/block or /sys/class/block symlink). */
static void boot_classify(SrSoftware *s, const char *root, const char *link, int depth) {
    char dev[1024], disk[1024];
    blk_sysfs(root, link, dev, sizeof dev);
    if (!dev[0] || depth > BOOT_MAX_DEPTH) return;
    /* a partition's disk is its parent folder */
    char part[1100];
    snprintf(part, sizeof part, "%s%s/partition", root, dev);
    struct stat st;
    snprintf(disk, sizeof disk, "%s", dev);
    if (!lstat(part, &st)) { char *sl = strrchr(disk, '/'); if (sl && sl != disk) *sl = 0; }
    const char *name = base_of(disk);
    if (!blk_name_ok(name)) return;

    /* device mapper (LUKS, LVM) and software RAID: the first layer below */
    if (!strncmp(name, "dm-", 3) || !strncmp(name, "md", 2)) {
        char sl[1100], **b;
        snprintf(sl, sizeof sl, "%s/slaves", dev);
        int n = list_dir(root, sl, &b);
        if (n > 0 && blk_name_ok(b[0])) {
            char next[96];
            snprintf(next, sizeof next, "/sys/class/block/%s", b[0]);
            boot_classify(s, root, next, depth + 1);
        }
        free_list(b, n);
        return;
    }
    const char *cls = "";
    if (!strncmp(name, "loop", 4) || !strncmp(name, "zram", 4) || !strncmp(name, "ram", 3)) cls = "";
    else if (!strncmp(name, "nbd", 3) || strstr(disk, "/session")) cls = "network";   /* NBD, iSCSI */
    else if (!strncmp(name, "sr", 2)) cls = "optical";
    else if (strstr(disk, "/usb")) cls = "usb";
    else if (!strncmp(name, "mmcblk", 6)) {
        char d[1100], type[16];
        if ((size_t)snprintf(d, sizeof d, "%s/device", disk) >= sizeof d) return;
        attr_long(root, d, "type", type, sizeof type);
        cls = !strcmp(type, "SD") ? "sd" : !strcmp(type, "MMC") ? "emmc" : "";
    } else cls = "internal";
    snprintf(s->boot_medium, sizeof s->boot_medium, "%s", cls);
    if (!strcmp(cls, "usb")) s->boot_link_mbps = usb_link_mbps(root, disk);
}

/* Whether the kernel command line holds the word w (or w=… when w ends in '='). Nothing of it is kept. */
int sysreport_cmdline_has(const char *cmdline, const char *w) {
    size_t n = strlen(w);
    for (const char *p = cmdline; (p = strstr(p, w)); p += n) {
        int start = p == cmdline || p[-1] == ' ';
        char e = p[n];
        if (start && (w[n - 1] == '=' || e == 0 || e == ' ' || e == '\n')) return 1;
    }
    return 0;
}

static void collect_boot(SrSoftware *s, const char *root) {
    s->boot_medium[0] = 0;
    s->boot_link_mbps = -1;
    char p[1024];
    snprintf(p, sizeof p, "%s/proc/cmdline", root);
    char *cmd = read_file(p, 4096, NULL);
    if (s->live && cmd) s->boot_toram = sysreport_cmdline_has(cmd, "toram") || sysreport_cmdline_has(cmd, "toram=");
    int fetched = s->live && cmd && (sysreport_cmdline_has(cmd, "fetch=") || sysreport_cmdline_has(cmd, "netboot="));
    free(cmd);

    const char *want = s->live ? "/run/live/medium" : "/";
    snprintf(p, sizeof p, "%s/proc/self/mountinfo", root);
    char *mi = read_file(p, 1u << 20, NULL);
    unsigned ma = 0, mn = 0;
    int found = 0;
    char fstype[32] = "", source[128] = "";
    /* "<id> <parent> <major>:<minor> <root> <mount point> <options> [optional…] - <fstype> <source> <super>";
     * the last line for a mount point is the one on top */
    for (char *line = mi, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        unsigned a, b;
        char mroot[256], point[256];
        if (sscanf(line, "%*u %*u %u:%u %255s %255s", &a, &b, mroot, point) != 4 || strcmp(point, want)) continue;
        const char *dash = strstr(line, " - ");
        if (!dash) continue;
        char ft[32] = "", src[128] = "";
        if (sscanf(dash + 3, "%31s %127s", ft, src) < 1) continue;
        ma = a; mn = b; found = 1;
        snprintf(fstype, sizeof fstype, "%s", ft);
        snprintf(source, sizeof source, "%s", src);
    }
    free(mi);

    if (found && (!strcmp(fstype, "nfs") || !strcmp(fstype, "nfs4") || !strcmp(fstype, "cifs") || !strcmp(fstype, "smb3"))) {
        snprintf(s->boot_medium, sizeof s->boot_medium, "network");
    } else if (found && ma != 0) {
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u", ma, mn);
        boot_classify(s, root, p, 0);
    } else if (found && !strncmp(source, "/dev/", 5) && blk_name_ok(base_of(source))) {
        /* btrfs and others show an anonymous 0:N device: the source names the block device */
        snprintf(p, sizeof p, "/sys/class/block/%s", base_of(source));
        boot_classify(s, root, p, 0);
    }
    if (!s->boot_medium[0] && fetched) snprintf(s->boot_medium, sizeof s->boot_medium, "network");
}

/* ---- software ---- */

static void collect_software(SysReport *r, const char *root) {
    SrSoftware *s = &r->sw;
    char p[1024];
    snprintf(p, sizeof p, "%s/etc/os-release", root);
    char *txt = read_file(p, 8192, NULL);
    if (!txt) { snprintf(p, sizeof p, "%s/usr/lib/os-release", root); txt = read_file(p, 8192, NULL); }
    if (txt) {
        osr_field(txt, "PRETTY_NAME", s->os, sizeof s->os);
        osr_field(txt, "ID", s->os_id, sizeof s->os_id);
        osr_field(txt, "VERSION_ID", s->os_version, sizeof s->os_version);
        free(txt);
    }
    snprintf(p, sizeof p, "%s/proc/sys/kernel/osrelease", root);
    read_attr(p, s->kernel, sizeof s->kernel);
    snprintf(p, sizeof p, "%s/sys/kernel/security/lsm", root);
    read_attr(p, s->lsm, sizeof s->lsm);
    s->bpf_lsm = strstr(s->lsm, "bpf") != NULL;
    s->cgroup2 = exists(root, "/sys/fs/cgroup/cgroup.controllers");
    s->landlock_abi = -1;
#ifdef SYS_landlock_create_ruleset
    if (!root[0]) {
        long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, 1u);   /* LANDLOCK_CREATE_RULESET_VERSION */
        s->landlock_abi = abi > 0 ? (int)abi : 0;
    }
#endif

    /* the kernel options LordOS's isolation, firewall and memory handling depend on */
    static const char *opts[] = {
        "CONFIG_SECURITY_LANDLOCK", "CONFIG_SECCOMP_FILTER", "CONFIG_USER_NS", "CONFIG_BPF_SYSCALL", "CONFIG_BPF_JIT",
        "CONFIG_CGROUP_BPF", "CONFIG_BPF_LSM", "CONFIG_INET_DIAG_DESTROY", "CONFIG_INET_UDP_DIAG", "CONFIG_MEMCG",
        "CONFIG_PSI", "CONFIG_ZRAM", "CONFIG_DRM", "CONFIG_UHID",
    };
    snprintf(p, sizeof p, "%s/boot/config-%s", root, s->kernel);
    txt = s->kernel[0] && !strchr(s->kernel, '/') ? read_file(p, 1u << 20, NULL) : NULL;
    for (size_t i = 0; i < sizeof opts / sizeof opts[0] && s->nkconf < SR_MAX_KCONF; i++) {
        SrKconf *k = &s->kconf[s->nkconf++];
        snprintf(k->name, sizeof k->name, "%s", opts[i]);
        if (!txt) continue;
        char key[64];
        snprintf(key, sizeof key, "\n%s=", opts[i]);
        const char *v = strstr(txt, key);       /* absent or "# … is not set": n */
        if (v) { v += strlen(key); snprintf(k->value, sizeof k->value, "%c", *v == 'y' || *v == 'm' ? *v : '?'); }
        else snprintf(k->value, sizeof k->value, "n");
    }
    free(txt);

    s->sessiond = exists(root, "/run/lordos/cgroup.sock");
    s->netperm = exists(root, "/run/lordos/netperm.sock");
    s->fand = exists(root, "/run/lordos/fand.sock");
    s->netd = exists(root, "/run/lordos/netd.sock");
    /* the live session from the ISO (live-boot's marker, as LordBench reads it): a shared report says so
     * (docs/BENCHMARK.md, "From the live USB") */
    s->live = exists(root, "/run/live");
    collect_boot(s, root);
}

/* ---- all of it ---- */

void sysreport_collect(SysReport *r, const char *root, const char *pci_ids, const char *usb_ids) {
    if (!root) root = "";
    memset(r, 0, sizeof *r);
    r->schema = 1;
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) strftime(r->date, sizeof r->date, "%Y-%m-%d", &tm);

    char p[1024];
    snprintf(p, sizeof p, "%s/proc/cpuinfo", root);
    char *cpuinfo = read_file(p, 1u << 20, NULL);
    const char *ci = cpuinfo ? cpuinfo : "";

    IdsDb pci, usb;
    char ip[1024];
    snprintf(ip, sizeof ip, "%s/usr/share/misc/pci.ids", root);
    ids_open(&pci, pci_ids ? pci_ids : ip);
    snprintf(ip, sizeof ip, "%s/usr/share/misc/usb.ids", root);
    ids_open(&usb, usb_ids ? usb_ids : ip);

    collect_machine(r, root, ci);
    collect_cpu(r, root, ci);
    collect_memory(r, root);
    collect_gpus(r, root, &pci);
    collect_displays(r, root);
    collect_disks(r, root);
    collect_pci(r, root, &pci);
    collect_usb(r, root, &usb);
    collect_net(r, root);
    collect_audio(r, root, &usb);
    collect_bt(r, root);
    collect_power(r, root);
    collect_thermal(r, root);
    collect_pi(r, root, ci);
    collect_software(r, root);

    ids_close(&pci);
    ids_close(&usb);
    free(cpuinfo);
}
