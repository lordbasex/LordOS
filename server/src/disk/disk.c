/* disk.c — disks, partitions and mounts without root (Disks app; see disk.h and docs/SYSTEM.md).
 *
 * Sources:
 *   /sys/block/<disk>/…                  size, removable, read-only, rotational, model
 *   /run/udev/data/b<major>:<minor>      what udev already probed: file system, label, UUID,
 *                                        partition table, bus (without opening the device)
 *   /proc/self/mountinfo                 where each one is mounted
 * None of this needs root: the system disk is recognized right here, and the helper checks it
 * again with the same functions before touching anything. */
#include "disk.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int disk_valid_name(const char *n) {
    size_t len = n ? strlen(n) : 0;
    if (!len || len > 31 || !islower((unsigned char)n[0])) return 0;
    for (size_t i = 0; i < len; i++) if (!islower((unsigned char)n[i]) && !isdigit((unsigned char)n[i])) return 0;
    return 1;
}

static int read_small(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) { out[0] = 0; return -1; }
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;    /* the model comes with trailing spaces */
    return 0;
}

static long long read_num(const char *path) {
    char b[32];
    if (read_small(path, b, sizeof b)) return -1;
    return atoll(b);
}

static int sysfs(const char *name, const char *rel, char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s/%s", name, rel);
    return read_small(p, out, n);
}

static int dev_numbers(const char *name, unsigned *maj, unsigned *min) {
    char b[32];
    return sysfs(name, "dev", b, sizeof b) || sscanf(b, "%u:%u", maj, min) != 2 ? -1 : 0;
}

int disk_parent(const char *name, char *disk, size_t n) {
    char p[PATH_MAX], real[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    if (access(p, F_OK)) return 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/..", name);
    if (!realpath(p, real)) return 0;
    const char *b = strrchr(real, '/');
    if (!b || !disk_valid_name(b + 1)) return 0;
    snprintf(disk, n, "%s", b + 1);
    return 1;
}

uint64_t disk_size(const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    long long s = read_num(p);
    return s > 0 ? (uint64_t)s * 512 : 0;
}

/* ---- udev database ---- */

int disk_udev_prop(const char *name, const char *key, char *out, size_t n) {
    out[0] = 0;
    unsigned ma, mi;
    if (dev_numbers(name, &ma, &mi)) return -1;
    char p[64];
    snprintf(p, sizeof p, "/run/udev/data/b%u:%u", ma, mi);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    char line[1024];
    size_t kl = strlen(key);
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'E' || line[1] != ':' || strncmp(line + 2, key, kl) || line[2 + kl] != '=') continue;
        char *v = line + 3 + kl;
        v[strcspn(v, "\n")] = 0;
        snprintf(out, n, "%s", v);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

/* ID_FS_LABEL_ENC: unusual characters come as \xNN */
static void unescape_udev(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        if (in[i] == '\\' && in[i + 1] == 'x' && isxdigit((unsigned char)in[i + 2]) && isxdigit((unsigned char)in[i + 3])) {
            char h[3] = { in[i + 2], in[i + 3], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 3;
        } else out[o++] = in[i];
    }
    out[o] = 0;
}

void disk_fs_label(const char *name, char *out, size_t n) {
    char v[512];
    out[0] = 0;
    if (!disk_udev_prop(name, "ID_FS_LABEL_ENC", v, sizeof v)) unescape_udev(v, out, n);
    else disk_udev_prop(name, "ID_FS_LABEL", out, n);
}

/* ---- mountinfo ---- */

typedef struct { unsigned maj, min; char point[PATH_MAX]; char source[256]; char fstype[32]; } Mnt;

static void unescape_octal(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else *o++ = *p;
    }
    *o = 0;
}

/* Calls fn for each mount; if fn returns != 0, stops and returns that. */
static int each_mount(int (*fn)(const Mnt *m, void *u), void *u) {
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) return 0;
    char line[PATH_MAX * 2 + 512];
    int ret = 0;
    Mnt m;
    while (!ret && fgets(line, sizeof line, f)) {
        char point[PATH_MAX], rest[PATH_MAX];
        if (sscanf(line, "%*d %*d %u:%u %*s %4095s", &m.maj, &m.min, point) != 3) continue;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(dash + 3, "%31s %255s", m.fstype, rest) != 2) continue;
        snprintf(m.source, sizeof m.source, "%.255s", rest);
        unescape_octal(point);
        snprintf(m.point, sizeof m.point, "%s", point);
        ret = fn(&m, u);
    }
    fclose(f);
    return ret;
}

/* Device number of a mount: the one in mountinfo, or (btrfs, 0:N) the one of its /dev/… source */
static int mount_dev(const Mnt *m, unsigned *ma, unsigned *mi) {
    if (m->maj) { *ma = m->maj; *mi = m->min; return 0; }
    struct stat st;
    if (strncmp(m->source, "/dev/", 5) || stat(m->source, &st) || !S_ISBLK(st.st_mode)) return -1;
    *ma = major(st.st_rdev);
    *mi = minor(st.st_rdev);
    return 0;
}

typedef struct { unsigned maj, min; char *out; size_t n, used; int count; } MountsQ;

static int mounts_cb(const Mnt *m, void *u) {
    MountsQ *q = u;
    unsigned ma, mi;
    if (mount_dev(m, &ma, &mi) || ma != q->maj || mi != q->min) return 0;
    if (q->out && q->used + strlen(m->point) + 2 < q->n) {
        q->used += (size_t)snprintf(q->out + q->used, q->n - q->used, "%s%s", q->count ? "\n" : "", m->point);
    }
    q->count++;
    return 0;
}

int disk_mounts(const char *name, char *out, size_t n) {
    if (out && n) out[0] = 0;
    MountsQ q = { .out = out, .n = n };
    if (dev_numbers(name, &q.maj, &q.min)) return 0;
    each_mount(mounts_cb, &q);
    return q.count;
}

/* ---- system disk ---- */

/* Adds to set (space-separated names) the disks under name: for a partition, its disk; for
 * dm/md, the disks of its slaves (recursively). */
static void base_disks(const char *name, char *set, size_t n, int depth) {
    if (depth > 8 || !disk_valid_name(name)) return;
    char parent[32];
    if (disk_parent(name, parent, sizeof parent)) name = parent;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s/slaves", name);
    DIR *d = opendir(p);
    int slaves = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            slaves++;
            base_disks(e->d_name, set, n, depth + 1);
        }
        closedir(d);
    }
    if (slaves) return;
    char key[40];
    snprintf(key, sizeof key, " %s ", name);
    if (!strstr(set, key) && strlen(set) + strlen(name) + 2 < n) { strcat(set, name); strcat(set, " "); }
}

static void name_of_dev(unsigned ma, unsigned mi, char *out, size_t n) {
    out[0] = 0;
    char p[64], real[PATH_MAX];
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", ma, mi);
    if (!realpath(p, real)) return;
    const char *b = strrchr(real, '/');
    if (b && disk_valid_name(b + 1)) snprintf(out, n, "%s", b + 1);
}

static const char *const system_points[] = {
    "/", "/usr", "/var", "/home", "/boot", "/boot/efi", "/efi", "/run/live/medium", "/lib/live/mount/medium", "/run/initramfs/live",
};

typedef struct { char set[1024]; } SysQ;

static int sys_cb(const Mnt *m, void *u) {
    SysQ *q = u;
    int hit = 0;
    for (size_t i = 0; i < sizeof system_points / sizeof *system_points; i++) if (!strcmp(m->point, system_points[i])) hit = 1;
    if (!hit) return 0;
    unsigned ma, mi;
    char name[32];
    if (mount_dev(m, &ma, &mi)) return 0;
    name_of_dev(ma, mi, name, sizeof name);
    if (name[0]) base_disks(name, q->set, sizeof q->set, 0);
    return 0;
}

static void system_set(char *set, size_t n) {
    SysQ q;
    snprintf(q.set, sizeof q.set, " ");
    each_mount(sys_cb, &q);
    /* active swaps */
    FILE *f = fopen("/proc/swaps", "re");
    char line[512];
    if (f && fgets(line, sizeof line, f)) {
        while (fgets(line, sizeof line, f)) {
            char file[PATH_MAX];
            struct stat st;
            if (sscanf(line, "%4095s", file) != 1) continue;
            unescape_octal(file);
            if (!stat(file, &st) && S_ISBLK(st.st_mode)) {
                char name[32];
                name_of_dev(major(st.st_rdev), minor(st.st_rdev), name, sizeof name);
                if (name[0]) base_disks(name, q.set, sizeof q.set, 0);
            }
        }
    }
    if (f) fclose(f);
    snprintf(set, n, "%s", q.set);
}

int disk_is_system(const char *disk) {
    if (!disk_valid_name(disk)) return 1;
    char set[1024], key[40];
    system_set(set, sizeof set);
    snprintf(key, sizeof key, " %s ", disk);
    return strstr(set, key) != NULL;
}

int disk_is_removable(const char *disk) {
    char b[16], bus[32], path[256];
    if (!sysfs(disk, "removable", b, sizeof b) && !strcmp(b, "1")) return 1;
    if (!disk_udev_prop(disk, "ID_BUS", bus, sizeof bus) && !strcmp(bus, "usb")) return 1;
    if (!disk_udev_prop(disk, "ID_PATH", path, sizeof path) && strstr(path, "-usb-")) return 1;
    return !strncmp(disk, "mmcblk", 6);
}

/* ---- JSON ---- */

typedef struct { char *p; size_t n, cap; int bad; } Buf;

static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    if (b->bad) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { b->bad = 1; return; }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->n + (size_t)k + 1) cap *= 2;
        char *nb = realloc(b->p, cap);
        if (!nb) { b->bad = 1; return; }
        b->p = nb;
        b->cap = cap;
    }
}

static void bstr(Buf *b, const char *key, const char *s) {
    bput(b, "\"%s\":\"", key);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') bput(b, "\\%c", *c);
        else if (*c < 0x20) bput(b, "\\u%04x", *c);
        else bput(b, "%c", *c);
    }
    bput(b, "\",");
}

/* File system and mount fields of a device (disk or partition). */
static void fs_fields(Buf *b, const char *name) {
    char v[512], label[256];
    disk_udev_prop(name, "ID_FS_TYPE", v, sizeof v);
    bstr(b, "fstype", v);
    disk_fs_label(name, label, sizeof label);
    bstr(b, "label", label);
    disk_udev_prop(name, "ID_FS_UUID", v, sizeof v);
    bstr(b, "uuid", v);
    disk_udev_prop(name, "ID_FS_VERSION", v, sizeof v);
    bstr(b, "fsversion", v);
    char mounts[2048];
    disk_mounts(name, mounts, sizeof mounts);
    char *nl = strchr(mounts, '\n');
    if (nl) *nl = 0;
    bstr(b, "mountpoint", mounts);
    struct statvfs sv;
    if (mounts[0] && !statvfs(mounts, &sv))
        bput(b, "\"used\":%llu,\"avail\":%llu,\"fssize\":%llu,", (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize,
             (unsigned long long)sv.f_bavail * sv.f_frsize, (unsigned long long)sv.f_blocks * sv.f_frsize);
    else bput(b, "\"used\":null,\"avail\":null,\"fssize\":null,");
}

/* Optical drives (sr*) are listed only while they hold a disc (an empty drive has size 0) */
static int skip_disk(const char *n) {
    static const char *const pre[] = { "ram", "zram", "dm-", "md", "fd", "nbd", "mtdblock", "zd" };
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    if (!strncmp(n, "loop", 4)) {      /* only loops backed by a file (tests, images) */
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", n);
        return access(p, F_OK) != 0;
    }
    return 0;
}

static void transport(const char *name, char *out, size_t n) {
    char bus[32] = "", path[256] = "";
    disk_udev_prop(name, "ID_BUS", bus, sizeof bus);
    disk_udev_prop(name, "ID_PATH", path, sizeof path);
    if (!strncmp(name, "sr", 2)) snprintf(out, n, "optical");
    else if (!strcmp(bus, "usb") || strstr(path, "-usb-")) snprintf(out, n, "usb");
    else if (!strncmp(name, "nvme", 4)) snprintf(out, n, "nvme");
    else if (!strncmp(name, "mmcblk", 6)) snprintf(out, n, "mmc");
    else if (!strncmp(name, "loop", 4)) snprintf(out, n, "loop");
    else if (!strncmp(name, "vd", 2)) snprintf(out, n, "virtio");
    else if (!strcmp(bus, "ata") || strstr(path, "-ata-")) snprintf(out, n, "sata");
    else if (bus[0]) snprintf(out, n, "%s", bus);
    else snprintf(out, n, "scsi");
}

typedef struct { char name[32]; int number; } Part;

static int part_cmp(const void *a, const void *b) { return ((const Part *)a)->number - ((const Part *)b)->number; }

char *disk_list_json(void) {
    DIR *d = opendir("/sys/block");
    if (!d) return NULL;
    char sys[1024];
    system_set(sys, sizeof sys);
    Buf b = { 0 };
    bput(&b, "[");
    int first = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.' || !disk_valid_name(n) || skip_disk(n)) continue;
        uint64_t size = disk_size(n);
        if (!size) continue;
        char v[512], key[40];
        bput(&b, "%s{", first ? "" : ",");
        first = 0;
        bstr(&b, "name", n);
        snprintf(key, sizeof key, " %.31s ", n);
        char model[256] = "", vendor[128] = "";
        if (sysfs(n, "device/model", model, sizeof model)) disk_udev_prop(n, "ID_MODEL", model, sizeof model);
        sysfs(n, "device/vendor", vendor, sizeof vendor);
        if (!strncmp(n, "loop", 4)) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", n);
            read_small(p, model, sizeof model);
        }
        bstr(&b, "model", model);
        bstr(&b, "vendor", vendor);
        disk_udev_prop(n, "ID_SERIAL_SHORT", v, sizeof v);
        bstr(&b, "serial", v);
        transport(n, v, sizeof v);
        bstr(&b, "transport", v);
        disk_udev_prop(n, "ID_PART_TABLE_TYPE", v, sizeof v);
        bstr(&b, "table", v);
        char ro[8] = "0", rot[8] = "0";
        sysfs(n, "ro", ro, sizeof ro);
        sysfs(n, "queue/rotational", rot, sizeof rot);
        /* a disc is never written here (format, erase and image copies refuse it), whatever the drive says */
        bput(&b, "\"size\":%llu,\"removable\":%s,\"ro\":%s,\"rotational\":%s,\"system\":%s,", (unsigned long long)size,
             disk_is_removable(n) ? "true" : "false", !strcmp(ro, "1") || !strncmp(n, "sr", 2) ? "true" : "false", !strcmp(rot, "1") ? "true" : "false",
             strstr(sys, key) ? "true" : "false");
        fs_fields(&b, n);
        /* partitions */
        Part parts[128];
        int np = 0;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s", n);
        DIR *pd = opendir(p);
        struct dirent *pe;
        while (pd && (pe = readdir(pd)) && np < 128) {
            if (!disk_valid_name(pe->d_name)) continue;
            char pp[PATH_MAX];
            snprintf(pp, sizeof pp, "/sys/block/%s/%s/partition", n, pe->d_name);
            long long num = read_num(pp);
            if (num <= 0) continue;
            snprintf(parts[np].name, sizeof parts[np].name, "%.31s", pe->d_name);
            parts[np++].number = (int)num;
        }
        if (pd) closedir(pd);
        qsort(parts, (size_t)np, sizeof *parts, part_cmp);
        bput(&b, "\"partitions\":[");
        for (int i = 0; i < np; i++) {
            const char *pn = parts[i].name;
            bput(&b, "%s{", i ? "," : "");
            bstr(&b, "name", pn);
            disk_udev_prop(pn, "ID_PART_ENTRY_NAME", v, sizeof v);
            char pl[256];
            unescape_udev(v, pl, sizeof pl);
            bstr(&b, "partlabel", pl);
            disk_udev_prop(pn, "ID_PART_ENTRY_TYPE", v, sizeof v);
            bstr(&b, "parttype", v);
            fs_fields(&b, pn);
            char sp[PATH_MAX];
            snprintf(sp, sizeof sp, "/sys/block/%.31s/%.31s/start", n, pn);
            long long start = read_num(sp);
            bput(&b, "\"number\":%d,\"size\":%llu,\"start\":%lld}", parts[i].number, (unsigned long long)disk_size(pn), start < 0 ? 0 : start * 512);
        }
        bput(&b, "]}");
    }
    closedir(d);
    bput(&b, "]");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

char *disk_tools_json(void) {
    char *out = malloc(160);
    if (!out) return NULL;
    snprintf(out, 160, "{\"fat32\":%d,\"exfat\":%d,\"ext4\":%d,\"ntfs\":%d,\"smartctl\":%d}",
             !access("/usr/sbin/mkfs.fat", X_OK), !access("/usr/sbin/mkfs.exfat", X_OK),
             !access("/usr/sbin/mkfs.ext4", X_OK), !access("/usr/sbin/mkfs.ntfs", X_OK),
             !access("/usr/sbin/smartctl", X_OK));
    return out;
}

/* ---- notifications: mountinfo (POLLPRI) + udev (netlink, group 2) in a single epoll ---- */

/* Fingerprint of mountinfo. Polling /proc/self/mountinfo "consumes" the event: the main loop's
 * poll on the epoll already uses it up and epoll_wait then doesn't report it. So on every
 * notification the content (a few KB) is compared with the previous one. */
static uint64_t mounts_hash(void) {
    uint64_t h = 1469598103934665603ull;
    int fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[8192];
    for (ssize_t r; (r = read(fd, buf, sizeof buf)) > 0;)
        for (ssize_t i = 0; i < r; i++) h = (h ^ (uint8_t)buf[i]) * 1099511628211ull;
    close(fd);
    return h;
}

static uint64_t last_mounts;

static int ep = -1, mfd = -1, ufd = -1;

int disk_watch_fd(void) { return ep; }

int disk_watch(int on) {
    if (!on) {
        if (ep >= 0) close(ep);
        if (mfd >= 0) close(mfd);
        if (ufd >= 0) close(ufd);
        ep = mfd = ufd = -1;
        return 0;
    }
    if (ep >= 0) return 0;
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) return -1;
    last_mounts = mounts_hash();
    mfd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mfd >= 0) {
        struct epoll_event ev = { .events = EPOLLPRI | EPOLLERR, .data.fd = mfd };
        epoll_ctl(ep, EPOLL_CTL_ADD, mfd, &ev);
    }
    /* events udevd sends after processing the device (its database is already up to date) */
    ufd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (ufd >= 0) {
        struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 2 };
        if (bind(ufd, (struct sockaddr *)&sa, sizeof sa)) { close(ufd); ufd = -1; }
        else {
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = ufd };
            epoll_ctl(ep, EPOLL_CTL_ADD, ufd, &ev);
        }
    }
    return 0;
}


int disk_watch_dispatch(void) {
    if (ep < 0) return 0;
    struct epoll_event evs[4];
    int n = epoll_wait(ep, evs, 4, 0), changed = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.fd != ufd) continue;
        char buf[8192];
        for (;;) {
            ssize_t r = recv(ufd, buf, sizeof buf, 0);
            if (r <= 0) break;
            if (memmem(buf, (size_t)r, "SUBSYSTEM=block", 15)) changed = 1;
        }
    }
    uint64_t h = mounts_hash();
    if (h != last_mounts) { last_mounts = h; changed = 1; }
    return changed;
}

/* The disk's identity as lord.disks.list() shows it (the LordOS Imager re-checks it right before
 * writing, so a stick swapped under the same name is refused): the model (sysfs or udev; a loop
 * device's backing file) and the short serial (udev, may be empty). */
int disk_identity(const char *name, char *model, size_t mn, char *serial, size_t sn) {
    if (!disk_valid_name(name) || !mn || !sn) return -1;
    model[0] = serial[0] = 0;
    if (sysfs(name, "device/model", model, mn)) disk_udev_prop(name, "ID_MODEL", model, mn);
    if (!strncmp(name, "loop", 4)) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", name);
        read_small(p, model, mn);
    }
    disk_udev_prop(name, "ID_SERIAL_SHORT", serial, sn);
    return 0;
}
