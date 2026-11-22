/* gpureport.c — the graphics compatibility report (gpureport.h). Every file is read with a bounded size and every
 * value copied out is checked against a small character set: a value that doesn't fit is left out, never guessed. */
#include "gpureport.h"
#include "ids.h"
#include "../hw/jbuf.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ---- small readers ---- */

static long rd(const char *path, char *out, size_t n) {
    if (n == 0) return -1;
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) { out[0] = 0; return -1; }
    out[k] = 0;
    return k;
}

static void pathf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= n) out[0] = 0;          /* a cut path would name another file */
}

static int exists(const char *root, const char *rel) {
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s%s", root, rel);
    return p[0] && access(p, F_OK) == 0;
}

/* only these characters, 1..n-1 of them */
static int clean(const char *s, size_t len, const char *extra, size_t n) {
    if (len == 0 || len >= n) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && !strchr(extra, ch)) return 0;
    }
    return 1;
}

/* a sysfs attribute's first line, trimmed, if it is clean */
static void attr(const char *path, char *out, size_t n, const char *extra) {
    char t[256];
    out[0] = 0;
    if (rd(path, t, sizeof t) <= 0) return;
    size_t l = strcspn(t, "\n");
    while (l && t[l - 1] == ' ') l--;
    if (clean(t, l, extra, n)) { memcpy(out, t, l); out[l] = 0; }
}

static void link_base(const char *path, char *out, size_t n) {
    char t[PATH_MAX];
    out[0] = 0;
    ssize_t k = readlink(path, t, sizeof t - 1);
    if (k <= 0) return;
    t[k] = 0;
    const char *b = strrchr(t, '/');
    b = b ? b + 1 : t;
    if (clean(b, strlen(b), "_-.:", n)) snprintf(out, n, "%s", b);
}

/* "0x10de\n" → "10de" */
static void hex_id(const char *path, char out[5]) {
    char t[32];
    out[0] = 0;
    if (rd(path, t, sizeof t) <= 0) return;
    const char *s = !strncmp(t, "0x", 2) ? t + 2 : t;
    size_t l = strcspn(s, "\n");
    if (l != 4) return;
    for (int i = 0; i < 4; i++) { if (!isxdigit((unsigned char)s[i])) return; out[i] = (char)tolower((unsigned char)s[i]); }
    out[4] = 0;
}

static int numbered(const char *name, const char *prefix) {
    size_t l = strlen(prefix);
    if (strncmp(name, prefix, l) || !name[l]) return 0;
    for (const char *p = name + l; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

/* ---- the cards ---- */

static GrCard *card_by_slot(GpuReport *r, const char *slot) {
    for (int i = 0; slot[0] && i < r->ncard; i++) if (!strcmp(r->card[i].slot, slot)) return &r->card[i];
    return NULL;
}

/* the device folder dev (a DRM card's or a PCI device's): ids, driver, boot_vga */
static void card_fill(GrCard *c, const char *dev, const IdsDb *ids) {
    char p[PATH_MAX + 32], t[16];
    pathf(p, sizeof p, "%s/vendor", dev);
    hex_id(p, c->vendor_id);
    pathf(p, sizeof p, "%s/device", dev);
    hex_id(p, c->device_id);
    pathf(p, sizeof p, "%s/driver", dev);
    link_base(p, c->driver, sizeof c->driver);
    pathf(p, sizeof p, "%s/boot_vga", dev);
    c->boot_vga = rd(p, t, sizeof t) > 0 && t[0] == '1';
    if (c->vendor_id[0]) ids_lookup(ids, c->vendor_id, c->device_id[0] ? c->device_id : NULL, c->vendor, sizeof c->vendor, c->device, sizeof c->device);
    c->desktop = -1;
}

static int by_card(const void *a, const void *b) {
    const GrCard *x = a, *y = b;
    if (!x->card[0] || !y->card[0]) return !x->card[0] - !y->card[0];
    return atoi(x->card + 4) - atoi(y->card + 4);
}

static void cards_read(GpuReport *r, const char *root, const IdsDb *ids) {
    char dir[PATH_MAX];
    /* the DRM cards: what the kernel drives */
    pathf(dir, sizeof dir, "%s/sys/class/drm", root);
    DIR *d = opendir(dir);
    for (struct dirent *e; d && r->ncard < GR_MAX_CARD && (e = readdir(d));) {
        if (!numbered(e->d_name, "card") || strlen(e->d_name) >= sizeof r->card[0].card) continue;
        GrCard *c = &r->card[r->ncard];
        memset(c, 0, sizeof *c);
        snprintf(c->card, sizeof c->card, "%s", e->d_name);
        char dev[PATH_MAX + 32], p[PATH_MAX + 64];
        pathf(dev, sizeof dev, "%s/%s/device", dir, e->d_name);
        card_fill(c, dev, ids);
        /* the PCI slot: the device link's last part, when it looks like one */
        char slot[32];
        link_base(dev, slot, sizeof slot);
        if (strlen(slot) == 12 && slot[4] == ':' && slot[7] == ':' && slot[10] == '.') snprintf(c->slot, sizeof c->slot, "%s", slot);
        pathf(p, sizeof p, "%s/drm", dev);
        DIR *dd = opendir(p);
        for (struct dirent *f; dd && (f = readdir(dd));) if (numbered(f->d_name, "renderD")) c->render_node = 1;
        if (dd) closedir(dd);
        r->ncard++;
    }
    if (d) closedir(d);
    /* PCI display devices with no DRM card: an NVIDIA card on its proprietary driver without nvidia-drm, or one no
     * driver took */
    pathf(dir, sizeof dir, "%s/sys/bus/pci/devices", root);
    d = opendir(dir);
    for (struct dirent *e; d && r->ncard < GR_MAX_CARD && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 12 || card_by_slot(r, e->d_name)) continue;
        char p[PATH_MAX + 32], cls[32];
        pathf(p, sizeof p, "%s/%s/class", dir, e->d_name);
        if (rd(p, cls, sizeof cls) <= 0 || strncmp(cls, "0x03", 4)) continue;
        GrCard *c = &r->card[r->ncard];
        memset(c, 0, sizeof *c);
        if (!clean(e->d_name, 12, ":.", sizeof c->slot)) continue;
        snprintf(c->slot, sizeof c->slot, "%s", e->d_name);
        pathf(p, sizeof p, "%s/%s", dir, e->d_name);
        card_fill(c, p, ids);
        r->ncard++;
    }
    if (d) closedir(d);
    qsort(r->card, (size_t)r->ncard, sizeof r->card[0], by_card);
}

/* ---- NVIDIA ---- */

int gpureport_nv_version(const char *text, char *ver, size_t n, int *open_) {
    ver[0] = 0;
    if (open_) *open_ = -1;
    if (!text) return -1;
    const char *km = strstr(text, "Kernel Module");
    if (!km) return -1;
    const char *line_end = strchr(text, '\n');
    if (line_end && km > line_end) return -1;
    if (open_) *open_ = strstr(text, "Open Kernel Module") && (!line_end || strstr(text, "Open Kernel Module") < line_end);
    /* the first token after "Kernel Module" that is digits and dots ("for x86_64" comes before it in some) */
    for (const char *p = km + 13; *p && *p != '\n';) {
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strcspn(p, " \t\n");
        int ok = l > 2 && l < n && isdigit((unsigned char)p[0]), dots = 0;
        for (size_t i = 0; ok && i < l; i++) { if (p[i] == '.') dots++; else if (!isdigit((unsigned char)p[i])) ok = 0; }
        if (ok && dots >= 1) { memcpy(ver, p, l); ver[l] = 0; return 0; }
        p += l;
    }
    return -1;
}

static int yes_no(const char *path) {
    char t[8];
    if (rd(path, t, sizeof t) <= 0) return -1;
    return t[0] == 'Y' || t[0] == 'y' || t[0] == '1';
}

/* ---- the library folders ---- */

static int list_add(char (*list)[40], int *n, size_t width, const char *s) {
    if (*n >= GR_LIST || !s[0] || strlen(s) >= width) return 0;
    for (int i = 0; i < *n; i++) if (!strcmp(list[i], s)) return 0;
    snprintf(list[(*n)++], width, "%s", s);
    return 1;
}

/* the names of a folder's files ending in suffix, without it: "radeonsi" of radeonsi_drv_video.so */
static void names(const char *dir, const char *prefix, const char *suffix, char *list, int *n, size_t width) {
    DIR *d = opendir(dir);
    for (struct dirent *e; d && *n < GR_LIST && (e = readdir(d));) {
        size_t l = strlen(e->d_name), pl = strlen(prefix), sl = strlen(suffix);
        if (l <= pl + sl || strncmp(e->d_name, prefix, pl) || strcmp(e->d_name + l - sl, suffix)) continue;
        size_t k = l - pl - sl;
        if (!clean(e->d_name + pl, k, "_-+.", width) || (k == 5 && !strncmp(e->d_name + pl, "trace", 5))) continue;   /* libvdpau_trace: a debugging layer */
        char nm[64];
        memcpy(nm, e->d_name + pl, k);
        nm[k] = 0;
        int dup = 0;
        for (int i = 0; i < *n && !dup; i++) dup = !strcmp(list + (size_t)i * width, nm);
        if (!dup) memcpy(list + (size_t)(*n)++ * width, nm, k + 1);    /* k < width: clean() checked it */
    }
    if (d) closedir(d);
}

static void vulkan_read(GpuReport *r, const char *root) {
    static const char *const dirs[] = { "/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d" };
    for (int k = 0; k < 2; k++) {
        char dir[PATH_MAX];
        pathf(dir, sizeof dir, "%s%s", root, dirs[k]);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && r->nvulkan < GR_LIST && (e = readdir(d));) {
            size_t l = strlen(e->d_name);
            if (l <= 5 || l >= 64 || strcmp(e->d_name + l - 5, ".json") || !clean(e->d_name, l, "_-+.", 64)) continue;
            /* "radeon_icd.x86_64.json" → "radeon"; "nvidia_icd.json" → "nvidia" */
            char drv[64];
            snprintf(drv, sizeof drv, "%s", e->d_name);
            char *cut = strstr(drv, "_icd");
            if (!cut) cut = strchr(drv, '.');
            if (cut) *cut = 0;
            char p[PATH_MAX + 80], t[4096], api[16] = "";
            pathf(p, sizeof p, "%s/%s", dir, e->d_name);
            long len = rd(p, t, sizeof t);
            JDoc doc;
            if (len > 0 && !json_parse(t, (size_t)len, &doc, NULL, 0)) {
                const char *a = json_str(json_get(doc.root, "ICD"), "api_version");
                if (a && clean(a, strlen(a), ".", sizeof api)) snprintf(api, sizeof api, "%s", a);
                json_free(&doc);
            }
            char item[40];
            if (strlen(drv) > 20) drv[20] = 0;
            snprintf(item, sizeof item, "%s%s%s", drv, api[0] ? " " : "", api);
            list_add(r->vulkan, &r->nvulkan, sizeof r->vulkan[0], item);
        }
        if (d) closedir(d);
    }
}

static void libs_read(GpuReport *r, const char *root) {
    char base[PATH_MAX];
    pathf(base, sizeof base, "%s/usr/lib", root);
    DIR *d = opendir(base);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strstr(e->d_name, "-linux-gnu") || strlen(e->d_name) >= 40) continue;
        char lib[PATH_MAX], p[PATH_MAX + 64];
        pathf(lib, sizeof lib, "%s/%s", base, e->d_name);
        pathf(p, sizeof p, "%s/dri", lib);
        names(p, "", "_drv_video.so", &r->va[0][0], &r->nva, sizeof r->va[0]);
        pathf(p, sizeof p, "%s/vdpau", lib);
        names(p, "libvdpau_", ".so.1", &r->vdpau[0][0], &r->nvdpau, sizeof r->vdpau[0]);
        static const char *const cuda[] = { "libcuda.so.1", "nvidia/current/libcuda.so.1" };
        for (int i = 0; i < 2; i++) { pathf(p, sizeof p, "%s/%s", lib, cuda[i]); if (p[0] && !access(p, F_OK)) r->cuda = 1; }
        /* Mesa's version is in its gallium library's name, libgallium-25.0.7-2.so */
        DIR *ld = opendir(lib);
        for (struct dirent *f; ld && !r->mesa[0] && (f = readdir(ld));) {
            size_t l = strlen(f->d_name);
            if (l > 14 && !strncmp(f->d_name, "libgallium-", 11) && !strcmp(f->d_name + l - 3, ".so") && clean(f->d_name + 11, l - 14, ".-+~", sizeof r->mesa))
                snprintf(r->mesa, sizeof r->mesa, "%.*s", (int)(l - 14), f->d_name + 11);
        }
        if (ld) closedir(ld);
    }
    if (d) closedir(d);
    if (exists(root, "/usr/lib64/libcuda.so.1")) r->cuda = 1;
    pathf(base, sizeof base, "%s/etc/OpenCL/vendors", root);
    d = opendir(base);
    for (struct dirent *e; d && (e = readdir(d));) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".icd")) r->opencl++; }
    if (d) closedir(d);
}

/* the kernel command line's graphics options only: never root=, a disk's id or anything else */
static void cmdline_read(GpuReport *r, const char *root) {
    static const char *const want[] = { "nomodeset", "nouveau.", "nvidia-drm.", "nvidia_drm.", "nvidia.", "modprobe.blacklist=",
        "rd.driver.blacklist=", "amdgpu.", "radeon.", "i915.", "xe.", "iommu=", "intel_iommu=", "amd_iommu=", "pcie_aspm=" };
    char p[PATH_MAX], t[4096];
    pathf(p, sizeof p, "%s/proc/cmdline", root);
    if (rd(p, t, sizeof t) <= 0) return;
    size_t used = 0;
    for (char *s = strtok(t, " \t\n"); s; s = strtok(NULL, " \t\n")) {
        size_t l = strlen(s);
        int hit = 0;
        for (size_t i = 0; i < sizeof want / sizeof *want && !hit; i++) {
            size_t wl = strlen(want[i]);
            hit = want[i][wl - 1] == '.' || want[i][wl - 1] == '=' ? !strncmp(s, want[i], wl) : !strcmp(s, want[i]);
        }
        if (!hit || l > 48 || !clean(s, l, "._,=:-", 49) || used + l + 2 > sizeof r->cmdline) continue;
        used += (size_t)snprintf(r->cmdline + used, sizeof r->cmdline - used, "%s%s", used ? " " : "", s);
    }
}

void gpureport_collect(GpuReport *r, const char *root, const char *pci_ids) {
    memset(r, 0, sizeof *r);
    r->secure_boot = -1;
    r->nv_open = r->nv_modeset = r->nv_fbdev = -1;
    if (!root) root = "";
    char p[PATH_MAX], t[1024];
    IdsDb ids = { 0 };
    pathf(p, sizeof p, "%s/usr/share/misc/pci.ids", root);
    ids_open(&ids, pci_ids ? pci_ids : p);
    cards_read(r, root, &ids);
    ids_close(&ids);

    /* the kernel: this machine's, or the fake tree's */
    if (!root[0]) {
        struct utsname u;
        if (!uname(&u)) {
            if (clean(u.release, strlen(u.release), ".-+_~", sizeof r->kernel)) snprintf(r->kernel, sizeof r->kernel, "%s", u.release);
            if (clean(u.machine, strlen(u.machine), "_-", sizeof r->arch)) snprintf(r->arch, sizeof r->arch, "%s", u.machine);
        }
    } else {
        pathf(p, sizeof p, "%s/proc/sys/kernel/osrelease", root);
        attr(p, r->kernel, sizeof r->kernel, ".-+_~");
    }
    if (exists(root, "/sys/firmware/efi")) snprintf(r->firmware, sizeof r->firmware, "uefi");
    else if (exists(root, "/sys/class/dmi/id/bios_vendor")) snprintf(r->firmware, sizeof r->firmware, "bios");
    pathf(p, sizeof p, "%s/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", root);
    long n = rd(p, t, 16);
    if (n >= 5) r->secure_boot = t[4] == 1;              /* 4 bytes of attributes, then the value */
    /* lockdown: "none [integrity] confidentiality" → the bracketed one */
    pathf(p, sizeof p, "%s/sys/kernel/security/lockdown", root);
    if (rd(p, t, sizeof t) > 0) {
        char *a = strchr(t, '['), *b = a ? strchr(a, ']') : NULL;
        if (a && b && clean(a + 1, (size_t)(b - a - 1), "", sizeof r->lockdown)) snprintf(r->lockdown, sizeof r->lockdown, "%.*s", (int)(b - a - 1), a + 1);
    }
    cmdline_read(r, root);

    r->nv_loaded = exists(root, "/sys/module/nvidia");
    r->nouveau_loaded = exists(root, "/sys/module/nouveau");
    pathf(p, sizeof p, "%s/proc/driver/nvidia/version", root);
    if (rd(p, t, sizeof t) > 0) gpureport_nv_version(t, r->nv_version, sizeof r->nv_version, &r->nv_open);
    pathf(p, sizeof p, "%s/sys/module/nvidia_drm/parameters/modeset", root);
    r->nv_modeset = yes_no(p);
    pathf(p, sizeof p, "%s/sys/module/nvidia_drm/parameters/fbdev", root);
    r->nv_fbdev = yes_no(p);
    r->nv_smi = exists(root, "/usr/bin/nvidia-smi");
    r->nv_uvm = exists(root, "/dev/nvidia-uvm");
    libs_read(r, root);
    vulkan_read(r, root);
}

/* ---- which card draws the desktop ---- */

/* the cards a process's fd folder holds: marks them, returns how many */
static int held(GpuReport *r, const char *fd_dir) {
    DIR *d = opendir(fd_dir);
    int got = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX + 300], t[PATH_MAX];
        pathf(p, sizeof p, "%s/%s", fd_dir, e->d_name);
        ssize_t k = readlink(p, t, sizeof t - 1);
        if (k <= 0) continue;
        t[k] = 0;
        if (strncmp(t, "/dev/dri/card", 13)) continue;
        for (int i = 0; i < r->ncard; i++)
            if (!strcmp(r->card[i].card, t + 9) && r->card[i].desktop != 1) { r->card[i].desktop = 1; got++; }
    }
    if (d) closedir(d);
    return got;
}

int gpureport_mark_desktop(GpuReport *r, const char *root, const char *fd_dir) {
    int got = 0;
    if (fd_dir) got = held(r, fd_dir);
    else {
        char dir[PATH_MAX];
        pathf(dir, sizeof dir, "%s/proc", root ? root : "");
        DIR *d = opendir(dir);
        for (struct dirent *e; d && !got && (e = readdir(d));) {
            if (!numbered(e->d_name, "")) continue;
            char p[PATH_MAX + 300], comm[32];
            pathf(p, sizeof p, "%s/%s/comm", dir, e->d_name);
            if (rd(p, comm, sizeof comm) <= 0 || strcmp(comm, "lord-server\n")) continue;
            pathf(p, sizeof p, "%s/%s/fd", dir, e->d_name);
            got = held(r, p);
        }
        if (d) closedir(d);
    }
    if (got) for (int i = 0; i < r->ncard; i++) if (r->card[i].desktop != 1) r->card[i].desktop = 0;
    return got;
}

const char *gpureport_kind(const GrCard *c) {
    if (!strcmp(c->vendor_id, "10de")) return "nvidia";
    if (!strcmp(c->vendor_id, "1002")) return "amd";
    if (!strcmp(c->vendor_id, "8086")) return "intel";
    return "other";
}

/* ---- the text ---- */

static const char *tri(int v, const char *yes, const char *no) { return v > 0 ? yes : v == 0 ? no : "unknown"; }

static void list_text(HBuf *b, const char *label, const char *list, int n, size_t width) {
    hb_printf(b, "%s: ", label);
    if (!n) hb_printf(b, "none found");
    for (int i = 0; i < n; i++) hb_printf(b, "%s%s", i ? ", " : "", list + (size_t)i * width);
    hb_printf(b, "\n");
}

char *gpureport_text(const GpuReport *r) {
    HBuf b = { 0 };
    int any_nv = 0;
    hb_printf(&b, "LordOS graphics report (format 1)\n");
    hb_printf(&b, "LordOS: %s\n", r->lordos[0] ? r->lordos : "unknown");
    hb_printf(&b, "Kernel: %s%s%s%s\n", r->kernel[0] ? r->kernel : "unknown", r->arch[0] ? " (" : "", r->arch, r->arch[0] ? ")" : "");
    hb_printf(&b, "Firmware: %s; Secure Boot: %s; lockdown: %s\n", r->firmware[0] ? (strcmp(r->firmware, "uefi") ? "BIOS" : "UEFI") : "unknown",
              tri(r->secure_boot, "on", "off"), r->lockdown[0] ? r->lockdown : "unknown");
    hb_printf(&b, "Graphics kernel options: %s\n", r->cmdline[0] ? r->cmdline : "none");
    hb_printf(&b, "\n");
    if (!r->ncard) hb_printf(&b, "No graphics card found.\n");
    for (int i = 0; i < r->ncard; i++) {
        const GrCard *c = &r->card[i];
        if (!strcmp(gpureport_kind(c), "nvidia")) any_nv = 1;
        hb_printf(&b, "GPU %d: %s%s%s", i + 1, c->vendor[0] ? c->vendor : "unknown maker", c->device[0] ? " " : "", c->device);
        if (c->vendor_id[0]) hb_printf(&b, " [%s:%s]", c->vendor_id, c->device_id[0] ? c->device_id : "?");
        hb_printf(&b, "\n  driver: %s", c->driver[0] ? c->driver : "none");
        if (!strcmp(c->driver, "nvidia") && r->nv_version[0]) hb_printf(&b, " %s (%s kernel modules)", r->nv_version, r->nv_open > 0 ? "open" : r->nv_open == 0 ? "proprietary" : "unknown");
        else if (r->mesa[0] && (!strcmp(c->driver, "nouveau") || !strcmp(c->driver, "amdgpu") || !strcmp(c->driver, "radeon") || !strcmp(c->driver, "i915")
                 || !strcmp(c->driver, "xe") || !strcmp(c->driver, "v3d") || !strcmp(c->driver, "vc4") || !strcmp(c->driver, "virtio-pci") || !strcmp(c->driver, "virtio_gpu")))
            hb_printf(&b, " (Mesa %s)", r->mesa);
        hb_printf(&b, "\n  draws the desktop: %s; boot display: %s; DRM node: %s; render node: %s\n",
                  tri(c->desktop, "yes", "no"), c->boot_vga ? "yes" : "no", c->card[0] ? c->card : "none", c->render_node ? "yes" : "no");
    }
    if (any_nv || r->nv_loaded || r->nouveau_loaded || r->nv_version[0]) {
        hb_printf(&b, "\nNVIDIA\n");
        hb_printf(&b, "  nvidia module: %s", r->nv_loaded ? "loaded" : "not loaded");
        if (r->nv_version[0]) hb_printf(&b, ", %s (%s kernel modules)", r->nv_version, r->nv_open > 0 ? "open" : r->nv_open == 0 ? "proprietary" : "unknown");
        hb_printf(&b, "\n  nvidia-drm modeset: %s; fbdev: %s\n", r->nv_modeset < 0 ? "module not loaded" : r->nv_modeset ? "on" : "off",
                  r->nv_fbdev < 0 ? "unknown" : r->nv_fbdev ? "on" : "off");
        hb_printf(&b, "  nouveau module: %s\n", r->nouveau_loaded ? "loaded" : "not loaded");
        hb_printf(&b, "  CUDA: libcuda %s; nvidia-smi %s; /dev/nvidia-uvm %s\n", r->cuda ? "installed" : "not installed",
                  r->nv_smi ? "installed" : "not installed", r->nv_uvm ? "present" : "absent");
    }
    hb_printf(&b, "\nMesa: %s\n", r->mesa[0] ? r->mesa : "not found");
    list_text(&b, "Vulkan drivers", &r->vulkan[0][0], r->nvulkan, sizeof r->vulkan[0]);
    list_text(&b, "Video decoding (VA-API)", &r->va[0][0], r->nva, sizeof r->va[0]);
    list_text(&b, "Video decoding (VDPAU)", &r->vdpau[0][0], r->nvdpau, sizeof r->vdpau[0]);
    hb_printf(&b, "OpenCL drivers: %d\n", r->opencl);
    hb_printf(&b, "\nThis session\n");
    if (!r->session) hb_printf(&b, "  not known from the command line (Settings > Graphics cards includes it)\n");
    else {
        hb_printf(&b, "  composition: %s%s%s\n", r->composition[0] ? r->composition : "unknown",
                  strcmp(r->composition, "gpu") && r->gpu_init[0] ? "; GPU start: " : "", strcmp(r->composition, "gpu") ? r->gpu_init : "");
        hb_printf(&b, "  OpenGL ES renderer: %s\n", r->renderer[0] ? r->renderer : "none");
    }
    return hb_take(&b);
}

/* ---- JSON ---- */

static void jlist(HBuf *b, const char *key, const char *list, int n, size_t width) {
    hb_printf(b, ",\"%s\":[", key);
    for (int i = 0; i < n; i++) { if (i) hb_printf(b, ","); hb_str(b, list + (size_t)i * width); }
    hb_printf(b, "]");
}

char *gpureport_json(const GpuReport *r) {
    HBuf b = { 0 };
    hb_printf(&b, "{\"cards\":[");
    for (int i = 0; i < r->ncard; i++) {
        const GrCard *c = &r->card[i];
        hb_printf(&b, "%s{\"card\":", i ? "," : "");
        hb_str(&b, c->card);
        hb_printf(&b, ",\"slot\":"); hb_str(&b, c->slot);
        hb_printf(&b, ",\"vendorId\":"); hb_str(&b, c->vendor_id);
        hb_printf(&b, ",\"deviceId\":"); hb_str(&b, c->device_id);
        hb_printf(&b, ",\"vendor\":"); hb_str(&b, c->vendor);
        hb_printf(&b, ",\"device\":"); hb_str(&b, c->device);
        hb_printf(&b, ",\"driver\":"); hb_str(&b, c->driver);
        hb_printf(&b, ",\"kind\":\"%s\",\"bootVga\":%s,\"renderNode\":%s,\"desktop\":%d}", gpureport_kind(c), c->boot_vga ? "true" : "false",
                  c->render_node ? "true" : "false", c->desktop);
    }
    hb_printf(&b, "],\"nvidia\":{\"loaded\":%s,\"version\":", r->nv_loaded ? "true" : "false");
    hb_str(&b, r->nv_version);
    hb_printf(&b, ",\"open\":%d,\"modeset\":%d,\"fbdev\":%d,\"nouveau\":%s,\"cuda\":%s,\"smi\":%s,\"uvm\":%s}", r->nv_open, r->nv_modeset, r->nv_fbdev,
              r->nouveau_loaded ? "true" : "false", r->cuda ? "true" : "false", r->nv_smi ? "true" : "false", r->nv_uvm ? "true" : "false");
    hb_printf(&b, ",\"secureBoot\":%d,\"lockdown\":", r->secure_boot);
    hb_str(&b, r->lockdown);
    hb_printf(&b, ",\"mesa\":");
    hb_str(&b, r->mesa);
    jlist(&b, "vulkan", &r->vulkan[0][0], r->nvulkan, sizeof r->vulkan[0]);
    jlist(&b, "va", &r->va[0][0], r->nva, sizeof r->va[0]);
    jlist(&b, "vdpau", &r->vdpau[0][0], r->nvdpau, sizeof r->vdpau[0]);
    hb_printf(&b, ",\"opencl\":%d,\"kernel\":", r->opencl);
    hb_str(&b, r->kernel);
    hb_printf(&b, ",\"arch\":");
    hb_str(&b, r->arch);
    hb_printf(&b, ",\"session\":");
    if (!r->session) hb_printf(&b, "null");
    else {
        hb_printf(&b, "{\"composition\":"); hb_str(&b, r->composition);
        hb_printf(&b, ",\"gpuInit\":"); hb_str(&b, r->gpu_init);
        hb_printf(&b, ",\"renderer\":"); hb_str(&b, r->renderer);
        hb_printf(&b, "}");
    }
    hb_printf(&b, "}");
    return hb_take(&b);
}

/* ---- redaction ---- */

static int word_ch(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c >= 0x80; }

/* a name worth replacing: 2+ characters, not a word every report has */
static int usable(const char *s) {
    if (!s || strlen(s) < 2 || strlen(s) > 255) return 0;
    static const char *const common[] = { "root", "localhost", "none", "unknown", "yes", "no", "on", "off" };
    for (size_t i = 0; i < sizeof common / sizeof *common; i++) if (!strcmp(s, common[i])) return 0;
    return 1;
}

static size_t hexrun(const char *s, size_t max) { size_t i = 0; while (i < max && isxdigit((unsigned char)s[i])) i++; return i; }

/* a MAC address at s: 6 pairs of hex digits joined by ':' (or all by '-') */
static size_t mac_at(const char *s) {
    char sep = 0;
    for (int k = 0; k < 6; k++) {
        const char *p = s + k * 3;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return 0;
        if (k < 5) {
            if (k == 0) sep = p[2];
            if ((sep != ':' && sep != '-') || p[2] != sep) return 0;
        }
    }
    return isxdigit((unsigned char)s[17]) ? 0 : 17;
}

/* a UUID at s: 8-4-4-4-12 hex digits */
static size_t uuid_at(const char *s) {
    static const int part[] = { 8, 4, 4, 4, 12 };
    size_t at = 0;
    for (int k = 0; k < 5; k++) {
        if (hexrun(s + at, 13) != (size_t)part[k]) return 0;
        at += (size_t)part[k];
        if (k < 4) { if (s[at] != '-') return 0; at++; }
    }
    return isxdigit((unsigned char)s[at]) ? 0 : at;
}

/* an e-mail address starting at s: its length, 0 if none */
static size_t email_at(const char *s) {
    size_t a = 0;
    while (isalnum((unsigned char)s[a]) || (s[a] && strchr("._%+-", s[a]))) a++;
    if (!a || s[a] != '@') return 0;
    size_t z = a + 1;
    int dot = 0;
    while (isalnum((unsigned char)s[z]) || s[z] == '.' || s[z] == '-') { if (s[z] == '.' && z > a + 1) dot = 1; z++; }
    return dot && z > a + 2 ? z : 0;
}

static int ci_prefix(const char *s, const char *w) {
    for (; *w; s++, w++) if (tolower((unsigned char)*s) != *w) return 0;
    return 1;
}

char *gpureport_redacted(const char *text, const char *user, const char *host, const char *home) {
    HBuf b = { 0 };
    if (!text) return NULL;
    int u_ok = usable(user), h_ok = usable(host);
    size_t ul = u_ok ? strlen(user) : 0, hl = h_ok ? strlen(host) : 0;
    size_t homel = home && strlen(home) > 1 && home[0] == '/' ? strlen(home) : 0;
    while (homel > 1 && home[homel - 1] == '/') homel--;
    const char *s = text;
    hb_printf(&b, "%s", "");
    while (*s) {
        int at_word = s == text || !word_ch((unsigned char)s[-1]);
        size_t k;
        if (homel && !strncmp(s, home, homel) && !word_ch((unsigned char)s[homel])) { hb_printf(&b, "~"); s += homel; continue; }
        if (!strncmp(s, "/home/", 6) && s[6] && s[6] != '/' && s[6] != ' ' && s[6] != '\n') {
            hb_printf(&b, "/home/<user>");
            s += 6;
            while (*s && *s != '/' && *s != ' ' && *s != '\n' && *s != ':' && *s != '"' && *s != '\'') s++;
            continue;
        }
        if (at_word && (k = mac_at(s))) { hb_printf(&b, "<mac>"); s += k; continue; }
        if (at_word && (k = uuid_at(s))) { hb_printf(&b, "<uuid>"); s += k; continue; }
        if (at_word && ci_prefix(s, "serial")) {
            /* "serial: X", "SerialNumber=X", "serial X": the word, then the value goes */
            const char *p = s + 6;
            while (isalpha((unsigned char)*p) || *p == '_') p++;
            const char *v = p;
            while (*v == ' ' || *v == ':' || *v == '=' || *v == '\t') v++;
            if (v > p && *v && *v != '\n') {
                hb_printf(&b, "%.*s<hidden>", (int)(v - s), s);
                while (*v && *v != '\n' && *v != ' ' && *v != ',' && *v != ';') v++;
                s = v;
                continue;
            }
        }
        if (at_word && (k = email_at(s))) { hb_printf(&b, "<email>"); s += k; continue; }
        if (at_word && ul && !strncmp(s, user, ul) && !word_ch((unsigned char)s[ul])) { hb_printf(&b, "<user>"); s += ul; continue; }
        if (at_word && hl && !strncmp(s, host, hl) && !word_ch((unsigned char)s[hl])) { hb_printf(&b, "<host>"); s += hl; continue; }
        hb_printf(&b, "%c", *s);
        s++;
    }
    char *out = hb_take(&b);
    if (out && strlen(out) > GR_TEXT_MAX) {
        char *cut = out + GR_TEXT_MAX - 8;       /* room for the mark: the text is longer than GR_TEXT_MAX */
        while (cut > out && cut[-1] != '\n') cut--;
        if (cut == out) cut = out + GR_TEXT_MAX - 8;
        memcpy(cut, "[cut]\n", 7);
    }
    return out;
}
