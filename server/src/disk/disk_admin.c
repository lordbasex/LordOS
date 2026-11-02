/* disk_admin.c — the "disk-*" actions of the privileged helper (admin.c), Disks app.
 *
 *   disk-smart <disk>                       health: ATA SMART (SG_IO, ATA PASS-THROUGH 16) or the
 *                                           NVMe health log (NVME_IOCTL_ADMIN_CMD)
 *   disk-mount <device>                     mounts at /media/<user>/<label> (nosuid,nodev)
 *   disk-unmount <device>                   unmounts whatever is under /media or /run/media
 *   disk-eject <disk>                       unmounts everything, flushes caches and ejects it (USB: powers off)
 *   disk-format <dev>\n<fs>\n<label>        fat32 | exfat | ext4 | ntfs (if mkfs.ntfs exists). A whole
 *                                           disk gets our own MBR table with one partition
 *   disk-label <partition>\n<label>         rename the volume
 *   disk-erase <dev>\nquick|zero             wipes the signatures (quick) or writes zeros everywhere
 *   disk-write-image <disk>\n<path>         copies an ISO/IMG image to the disk and verifies it (SHA-256)
 *   disk-cancel <disk>                      stops a zero erase or an image copy
 *
 * Rules, all on the root side (the app is not trusted):
 *   - the name is a real block device (sd*, vd*, hd*, xvd*, nvme*, mmcblk*, loop*), with
 *     /dev/<name> a block device with the same number as in sysfs;
 *   - never the system disk (disk_is_system: /, /usr, /var, /home, /boot, swap, live…), except
 *     to read its SMART;
 *   - internal disks only for an administrator; removable ones (USB, cards) for any session user,
 *     like udisks;
 *   - only what is mounted under /media or /run/media is unmounted; anything else is refused;
 *   - fixed-path programs (mkfs.*, *label) with validated arguments; mount, unmount, partition,
 *     erase and the image copy are system calls, no programs;
 *   - the image is opened with the user's permissions (setfsuid), only from their home or their
 *     mounts, and never from the disk being written. */
#include "disk.h"
#include "diskkinds.h"
#include "../pkg/crypto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define UDEVADM "/usr/bin/udevadm"
#define MIB (1024u * 1024u)

typedef struct {
    char name[32];                      /* what was asked for */
    char disk[32];                      /* its disk */
    char dev[48];                       /* /dev/<name> */
    int part;                           /* is a partition */
    int removable;
    uint64_t size;
} Target;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static int split2(char *s, char **a, char **b) {
    char *nl = strchr(s, '\n');
    if (!nl) return -1;
    *nl = 0;
    *a = s;
    *b = nl + 1;
    char *e = strchr(*b, '\n');
    if (e) *e = 0;
    return 0;
}

static int known_kind(const char *n) {
    static const char *const pre[] = { "sd", "vd", "hd", "xvd", "nvme", "mmcblk", "loop" };
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

/* want: 0 any, 1 whole disk only, 2 partition only. allow_system: read-only use (SMART):
 * also allowed on the system disk and for any user. */
static int target(const AdminReq *r, const char *name, int want, int allow_system, Target *t) {
    memset(t, 0, sizeof *t);
    if (!disk_valid_name(name) || !known_kind(name)) { admin_err("disk.badDevice", NULL); return -1; }
    char p[PATH_MAX], devno[32];
    snprintf(p, sizeof p, "/sys/class/block/%s/dev", name);
    FILE *f = fopen(p, "re");
    unsigned ma = 0, mi = 0;
    int ok = f && fgets(devno, sizeof devno, f) && sscanf(devno, "%u:%u", &ma, &mi) == 2;
    if (f) fclose(f);
    if (!ok) { admin_err("disk.noDevice", "name", name, NULL); return -1; }
    snprintf(t->name, sizeof t->name, "%s", name);
    t->part = disk_parent(name, t->disk, sizeof t->disk);
    if (!t->part) snprintf(t->disk, sizeof t->disk, "%s", name);
    if (want == 1 && t->part) { admin_err("disk.isPartition", "name", name, NULL); return -1; }
    if (want == 2 && !t->part) { admin_err("disk.notPartition", "name", name, NULL); return -1; }
    snprintf(t->dev, sizeof t->dev, "/dev/%s", name);
    struct stat st;
    if (lstat(t->dev, &st) || !S_ISBLK(st.st_mode) || major(st.st_rdev) != ma || minor(st.st_rdev) != mi) {
        admin_err("disk.notBlock", "name", t->dev, NULL);
        return -1;
    }
    if (!allow_system && disk_is_system(t->disk)) { admin_err("disk.systemDisk", "name", t->disk, NULL); return -1; }
    t->removable = disk_is_removable(t->disk);
    if (!allow_system && !t->removable && !r->admin) { admin_err("disk.internalNeedsAdmin", NULL); return -1; }
    t->size = disk_size(name);
    return 0;
}

/* For the smart-* actions (smartctl_admin.c): a whole disk, the system disk allowed. need_admin:
 * an internal disk also needs an administrator (starting or stopping a self-test). */
int disk_admin_target(const AdminReq *r, const char *name, int need_admin, char *dev, size_t n, int *usb) {
    Target t;
    if (target(r, name, 1, 1, &t)) return -1;
    if (need_admin && !t.removable && !r->admin) { admin_err("disk.internalNeedsAdmin", NULL); return -1; }
    char bus[16];
    *usb = disk_udev_prop(t.disk, "ID_BUS", bus, sizeof bus) == 0 && !strcmp(bus, "usb");
    snprintf(dev, n, "%s", t.dev);
    return 0;
}

/* ---- long jobs that can be cancelled (disk-cancel) ---- */

static volatile sig_atomic_t cancelled;
static void on_cancel(int s) { (void)s; cancelled = 1; }

static void job_path(const char *disk, char *out, size_t n) { snprintf(out, n, "/run/lordos/disk-%s.job", disk); }

static void job_begin(const AdminReq *r, const char *disk) {
    mkdir("/run/lordos", 0755);
    char p[96];
    job_path(disk, p, sizeof p);
    FILE *f = fopen(p, "we");
    if (f) { fprintf(f, "%d %u\n", (int)getpid(), (unsigned)r->uid); fclose(f); }
    struct sigaction sa = { .sa_handler = on_cancel };
    sigaction(SIGTERM, &sa, NULL);
    /* keep the desktop smooth: low CPU and disk priority (best effort, level 7) */
    setpriority(PRIO_PROCESS, 0, 10);
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, (2 << 13) | 7 /* IOPRIO_CLASS_BE, 7 */);
}

static void job_end(const char *disk) {
    char p[96];
    job_path(disk, p, sizeof p);
    unlink(p);
}

void disk_act_cancel(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    if (!disk_valid_name(arg)) { admin_err("disk.badDevice", NULL); return; }
    char p[96];
    job_path(arg, p, sizeof p);
    FILE *f = fopen(p, "re");
    int pid = 0;
    unsigned uid = 0;
    int ok = f && fscanf(f, "%d %u", &pid, &uid) == 2;
    if (f) fclose(f);
    if (!ok || pid <= 1) { admin_err("disk.nothingRunning", "name", arg, NULL); return; }
    if (uid != r->uid && !r->admin) { admin_err("disk.otherUser", NULL); return; }
    /* it must really be a helper child (lord-admin), not a recycled pid */
    char cp[64], comm[32] = "";
    snprintf(cp, sizeof cp, "/proc/%d/comm", pid);
    FILE *c = fopen(cp, "re");
    if (c) { if (!fgets(comm, sizeof comm, c)) comm[0] = 0; fclose(c); }
    if (strncmp(comm, "lord-admin", 10)) { unlink(p); admin_err("disk.nothingRunning", "name", arg, NULL); return; }
    kill(pid, SIGTERM);
    admin_ok("disk.cancelling", NULL);
}

/* ---- mounts ---- */

static int in_media(const char *point) {
    return !strncmp(point, "/media/", 7) || !strncmp(point, "/run/media/", 11);
}

/* Unmounts the device's mounts under /media; refuses if it is mounted anywhere else. */
static int unmount_dev(const char *name) {
    char mounts[4096];
    if (!disk_mounts(name, mounts, sizeof mounts)) return 0;
    for (char *m = strtok(mounts, "\n"); m; m = strtok(NULL, "\n")) {
        if (!in_media(m)) { admin_err("disk.mountedElsewhere", "name", name, "path", m, NULL); return -1; }
        sync();
        if (umount2(m, UMOUNT_NOFOLLOW)) {
            { if (errno == EBUSY) admin_err("disk.busyFiles", "name", name, "path", m, NULL); else admin_err("disk.unmountFailed", "name", name, "errno", lerr_errno(errno), NULL); }
            return -1;
        }
        admin_log("disk.unmountedPath", "path", m, NULL);
        rmdir(m);
    }
    return 0;
}

static int unmount_disk(const Target *t) {
    if (unmount_dev(t->disk)) return -1;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/block/%s", t->disk);
    DIR *d = opendir(p);
    struct dirent *e;
    int rc = 0;
    while (d && !rc && (e = readdir(d))) {
        char sub[32];
        if (disk_valid_name(e->d_name) && disk_parent(e->d_name, sub, sizeof sub) && !strcmp(sub, t->disk)) rc = unmount_dev(e->d_name);
    }
    if (d) closedir(d);
    return rc;
}

static void sanitize_dir(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) out[o++] = (*p < 0x20 || *p == '/' || *p == 0x7f) ? '_' : (char)*p;
    out[o] = 0;
    while (out[0] == '.') out[0] = '_';
}

void disk_act_mount(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    Target t;
    if (target(r, arg, 0, 0, &t)) return;
    if (disk_mounts(t.name, NULL, 0)) { admin_err("disk.alreadyMounted", "name", t.name, NULL); return; }
    char fs[32], label[256], uuid[96];
    disk_udev_prop(t.name, "ID_FS_TYPE", fs, sizeof fs);
    disk_fs_label(t.name, label, sizeof label);                      /* sanitize_dir removes / and control characters */
    disk_udev_prop(t.name, "ID_FS_UUID", uuid, sizeof uuid);
    int k = -1;
    for (size_t i = 0; i < sizeof DISK_KINDS / sizeof *DISK_KINDS; i++) if (!strcmp(fs, DISK_KINDS[i].udev)) k = (int)i;
    /* the Store's Disk Formats item gates the desktop's mount of these; the kernel's drivers, the boot partition, the
     * installer and the live system don't come through here */
    if (k >= 0 && (!strcmp(fs, "vfat") || !strcmp(fs, "exfat") || !strcmp(fs, "ntfs") || !strcmp(fs, "hfsplus") || !strcmp(fs, "apfs")) &&
        access("/usr/lib/lordos/disk-formats.enabled", F_OK)) { admin_err("disk.formatsNeeded", "fs", fs, NULL); return; }
    if (k < 0) { { if (fs[0]) admin_err("disk.cannotMountFs", "fs", fs, NULL); else admin_err("disk.unknownFs", "name", t.name, NULL); } return; }
    char base[96], dir[PATH_MAX], leaf[200];
    mkdir("/media", 0755);
    snprintf(base, sizeof base, "/media/%.64s", r->user);
    if (mkdir(base, 0750) == 0 || errno == EEXIST) { if (chown(base, 0, r->gid)) { /* keep going */ } chmod(base, 0750); }
    sanitize_dir(label[0] ? label : uuid[0] ? uuid : t.name, leaf, sizeof leaf);
    snprintf(dir, sizeof dir, "%s/%s", base, leaf);
    for (int i = 2; mkdir(dir, 0700) && i < 100; i++) {
        struct stat st;
        /* an empty leftover folder (not a mount point) is reused */
        if (errno == EEXIST && !lstat(dir, &st) && S_ISDIR(st.st_mode) && rmdir(dir) == 0 && mkdir(dir, 0700) == 0) break;
        snprintf(dir, sizeof dir, "%s/%s %d", base, leaf, i);
    }
    char opts[160] = "";
    if (DISK_KINDS[k].owner) snprintf(opts, sizeof opts, "uid=%u,gid=%u,%s", (unsigned)r->uid, (unsigned)r->gid,
                                 !strcmp(fs, "ntfs") ? "umask=022" : !strcmp(fs, "apfs") ? "" : !strcmp(fs, "hfsplus") ? "umask=022" : !strcmp(fs, "vfat") ? "fmask=0022,dmask=0022,shortname=mixed,utf8" : "fmask=0022,dmask=0022");
    char ro[8] = "0";
    char sp[PATH_MAX];
    snprintf(sp, sizeof sp, "/sys/class/block/%s/ro", t.name);
    FILE *f = fopen(sp, "re");
    if (f) { if (!fgets(ro, sizeof ro, f)) ro[0] = '0'; fclose(f); }
    unsigned long flags = MS_NOSUID | MS_NODEV | MS_RELATIME | ((DISK_KINDS[k].ro || ro[0] == '1') ? MS_RDONLY : 0);
    if (mount(t.dev, dir, DISK_KINDS[k].kernel, flags, opts[0] ? opts : NULL)) {
        int e = errno;
        rmdir(dir);
        /* Files offers the driver's package; APFS has none on an architecture without the item's marker */
        if (e == ENODEV) { admin_err(!strcmp(fs, "apfs") && access("/usr/lib/lordos/disk-formats.apfs", F_OK) ? "disk.noDriverHere" : "disk.noDriver", "fs", fs, NULL); return; }
        admin_err("disk.mountFailed", "name", t.name, "errno", lerr_errno(e), NULL);
        return;
    }
    admin_say("OK", "%s", dir);   /* i18n-ok */
}

void disk_act_unmount(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    Target t;
    if (target(r, arg, 0, 0, &t)) return;
    if (t.part && !disk_mounts(t.name, NULL, 0)) { admin_err("disk.notMounted", "name", t.name, NULL); return; }
    if (t.part ? unmount_dev(t.name) : unmount_disk(&t)) return;
    admin_ok("disk.unmounted", NULL);
}

/* ---- SCSI / ATA ---- */

static int sg_cmd(int fd, const uint8_t *cdb, int cdblen, void *buf, unsigned len, int dir, uint8_t *sense, unsigned slen) {
    sg_io_hdr_t h;
    memset(&h, 0, sizeof h);
    h.interface_id = 'S';
    h.cmdp = (unsigned char *)cdb;
    h.cmd_len = (unsigned char)cdblen;
    h.dxfer_direction = dir;
    h.dxferp = buf;
    h.dxfer_len = len;
    h.sbp = sense;
    h.mx_sb_len = (unsigned char)slen;
    h.timeout = 15000;
    if (ioctl(fd, SG_IO, &h)) return -1;
    if (h.host_status || (h.driver_status & 0x07)) return -1;      /* 0x08 = DRIVER_SENSE: sense data attached, not an error */
    return h.status;                    /* 0 good, 2 CHECK CONDITION (with sense) */
}

static int ata_smart_read(int fd, uint8_t feature, uint8_t out[512]) {
    uint8_t cdb[16] = { 0x85, 4 << 1, 0x0e, 0, feature, 0, 1, 0, 0, 0, 0x4f, 0, 0xc2, 0, 0xb0, 0 };
    uint8_t sense[32] = { 0 };
    memset(out, 0, 512);
    return sg_cmd(fd, cdb, 16, out, 512, SG_DXFER_FROM_DEV, sense, sizeof sense) == 0 ? 0 : -1;
}

/* RETURN STATUS: LBA mid/high 4Fh/C2h = good, F4h/2Ch = threshold exceeded. */
static int ata_smart_status(int fd) {
    uint8_t cdb[16] = { 0x85, 3 << 1, 0x20, 0, 0xda, 0, 0, 0, 0, 0, 0x4f, 0, 0xc2, 0, 0xb0, 0 };
    uint8_t sense[64] = { 0 };
    if (sg_cmd(fd, cdb, 16, NULL, 0, SG_DXFER_NONE, sense, sizeof sense) < 0) return -1;
    if ((sense[0] & 0x7f) == 0x72) {
        for (int i = 8; i + 13 < 64 && i < 8 + sense[7]; i += 2 + sense[i + 1]) {
            if (sense[i] != 0x09) continue;
            uint8_t mid = sense[i + 9], high = sense[i + 11];
            if (mid == 0x4f && high == 0xc2) return 1;
            if (mid == 0xf4 && high == 0x2c) return 0;
            return -1;
        }
    }
    return -1;
}

void disk_act_smart(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    Target t;
    if (target(r, arg, 1, 1, &t)) return;     /* the system disk too: read only */
    if (strncmp(t.name, "sd", 2) && strncmp(t.name, "nvme", 4) && strncmp(t.name, "hd", 2)) {
        admin_err("disk.noSmart", "name", t.name, NULL);
        return;
    }
    double t0 = now_s();
    int fd = open(t.dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { admin_err("disk.openFailed", "name", t.dev, "errno", lerr_errno(errno), NULL); return; }
    SmartReport rep;
    int ok;
    if (!strncmp(t.name, "nvme", 4)) {
        uint8_t log[512] = { 0 };
        struct nvme_admin_cmd c;
        memset(&c, 0, sizeof c);
        c.opcode = 0x02;                /* Get Log Page */
        c.nsid = 0xffffffff;
        c.addr = (uint64_t)(uintptr_t)log;
        c.data_len = sizeof log;
        c.cdw10 = (127u << 16) | 0x02;  /* 128 dwords, log page 02h */
        ok = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c) == 0 && smart_nvme_parse(log, &rep) == 0;
    } else {
        uint8_t data[512], th[512];
        int has_data = ata_smart_read(fd, 0xd0, data) == 0;
        int has_th = has_data && ata_smart_read(fd, 0xd1, th) == 0;
        int status = ata_smart_status(fd);
        ok = (has_data || status >= 0) && smart_ata_parse(has_data ? data : (uint8_t[512]){ 0 }, has_th ? th : NULL, status, &rep) == 0;
        if (ok && !has_data) rep.nattr = 0;
    }
    close(fd);
    if (!ok) { admin_err("disk.smartNoReply", NULL); return; }
    char j[1024];
    if (!smart_summary_json(&rep, j, sizeof j)) admin_say("LOG", "smart %s", j);   /* i18n-ok */
    for (int i = 0; i < rep.nattr; i++) if (!smart_attr_json(&rep.attr[i], j, sizeof j)) admin_say("LOG", "attr %s", j);   /* i18n-ok */
    admin_say("OK", "%.1f ms", (now_s() - t0) * 1000);   /* i18n-ok */
}

/* ---- writing to the device ---- */

static void uevent_change(const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s/uevent", name);
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { if (write(fd, "change", 6) < 0) { /* keep going */ } close(fd); }
}

static void settle(void) {
    if (access(UDEVADM, X_OK)) return;
    const char *const a[] = { UDEVADM, "settle", "--timeout=10", NULL };
    char out[256];
    admin_run_capture(a, out, sizeof out);
}

static int zero_range(int fd, uint64_t off, uint64_t len) {
    static uint8_t z[MIB];
    while (len) {
        size_t k = len > sizeof z ? sizeof z : (size_t)len;
        ssize_t w = pwrite(fd, z, k, (off_t)off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        off += (uint64_t)w;
        len -= (uint64_t)w;
    }
    return 0;
}

/* Wipes signatures: the first and last MiB (MBR, GPT and its backup, leading superblocks). */
static int wipe_ends(int fd, uint64_t size) {
    uint64_t a = size < 2 * MIB ? size : MIB;
    if (zero_range(fd, 0, a)) return -1;
    if (size > 2 * MIB && zero_range(fd, size - MIB, MIB)) return -1;
    return 0;
}

static int open_excl(const Target *t, int extra) {
    int fd = open(t->dev, O_RDWR | O_EXCL | O_CLOEXEC | extra);
    if (fd < 0) { if (errno == EBUSY) admin_err("disk.busy", "name", t->name, NULL); else admin_err("disk.openFailed", "name", t->name, "errno", lerr_errno(errno), NULL); }
    return fd;
}

static void part_name(const char *disk, int n, char *out, size_t cap) {
    size_t l = strlen(disk);
    snprintf(out, cap, "%.24s%s%d", disk, l && isdigit((unsigned char)disk[l - 1]) ? "p" : "", n);
}

/* MBR table with a single partition from 1 MiB to the end. */
static int write_mbr(int fd, uint64_t size, uint8_t type) {
    uint64_t sectors = size / 512;
    if (sectors > 0xffffffffull) return -2;
    if (sectors < 4096) return -3;
    uint8_t mbr[512] = { 0 };
    uint32_t sig = 0;
    FILE *u = fopen("/dev/urandom", "re");
    if (u) { if (fread(&sig, sizeof sig, 1, u) != 1) sig = (uint32_t)time(NULL); fclose(u); }
    memcpy(mbr + 440, &sig, 4);
    uint8_t *p = mbr + 446;
    uint32_t start = 2048, count = (uint32_t)(sectors - start);
    p[0] = 0x00;
    p[1] = 0xfe; p[2] = 0xff; p[3] = 0xff;       /* CHS out of range: LBA is used */
    p[4] = type;
    p[5] = 0xfe; p[6] = 0xff; p[7] = 0xff;
    memcpy(p + 8, &start, 4);
    memcpy(p + 12, &count, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    return pwrite(fd, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr ? 0 : -1;
}

/* Validates the label for that file system; for FAT it uppercases it. */
static int valid_label(const char *fs, char *label) {
    size_t n = strlen(label), cps = 0;
    if (label[0] == '-') return 0;
    for (const unsigned char *p = (const unsigned char *)label; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return 0;
        if ((*p & 0xc0) != 0x80) cps++;
    }
    if (!strcmp(fs, "fat32") || !strcmp(fs, "vfat")) {
        if (n > 11) return 0;
        for (char *p = label; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
            if (!isalnum((unsigned char)*p) && !strchr(" _-", *p)) return 0;
        }
        return 1;
    }
    if (strpbrk(label, "/\\:*?\"<>|")) return 0;
    if (!strcmp(fs, "exfat")) return cps <= 15;
    if (!strncmp(fs, "ext", 3)) return n <= 16;
    if (!strcmp(fs, "ntfs")) return cps <= 32;
    return 0;
}

void disk_act_format(const AdminReq *r, const char *arg, size_t len) {
    /* <device>\n<fs>[\n<label>] */
    char *buf = strndup(arg, len), *f[3] = { NULL, NULL, NULL };
    int nf = 0;
    for (char *s = buf; s && nf < 3; nf++) {
        f[nf] = s;
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        s = nl ? nl + 1 : NULL;
        if (nf == 2 && nl) { nf = -1; break; }      /* too many lines */
    }
    if (!buf || nf < 2) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    char *name = f[0], fsname[16], lab[64];
    if (strlen(f[1]) >= sizeof fsname || (f[2] && strlen(f[2]) >= sizeof lab)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    snprintf(fsname, sizeof fsname, "%s", f[1]);
    snprintf(lab, sizeof lab, "%s", f[2] ? f[2] : "");
    struct { const char *id, *prog; uint8_t type; } DISK_KINDS[] = {
        { "fat32", "/usr/sbin/mkfs.fat", 0x0c }, { "exfat", "/usr/sbin/mkfs.exfat", 0x07 },
        { "ext4", "/usr/sbin/mkfs.ext4", 0x83 }, { "ntfs", "/usr/sbin/mkfs.ntfs", 0x07 },
    };
    int k = -1;
    for (int i = 0; i < 4; i++) if (!strcmp(fsname, DISK_KINDS[i].id)) k = i;
    if (k < 0) { admin_err("disk.unknownFsName", "fs", fsname, NULL); free(buf); return; }
    if (access(DISK_KINDS[k].prog, X_OK)) { admin_err("disk.missingTool", "program", DISK_KINDS[k].prog, NULL); free(buf); return; }
    if (!valid_label(fsname, lab)) { admin_err("disk.badLabelFor", "fs", fsname, NULL); free(buf); return; }
    Target t;
    if (target(r, name, 0, 0, &t)) { free(buf); return; }
    free(buf);
    if (t.part ? unmount_dev(t.name) : unmount_disk(&t)) return;
    int fd = open_excl(&t, 0);
    if (fd < 0) return;
    char dev[48];
    snprintf(dev, sizeof dev, "%s", t.dev);
    if (!t.part) {
        admin_say("LOG", "progress phase=table");   /* i18n-ok */
        int e = wipe_ends(fd, t.size);
        if (!e) e = write_mbr(fd, t.size, DISK_KINDS[k].type);
        if (e) {
            close(fd);
            admin_err(e == -2 ? "disk.tooLargeForMbr" : e == -3 ? "disk.tooSmall" : "disk.tableFailed", NULL);
            return;
        }
        fsync(fd);
        if (!strncmp(t.disk, "loop", 4)) {        /* an image without "partscan": turn it on */
            struct loop_info64 li;
            if (!ioctl(fd, LOOP_GET_STATUS64, &li) && !(li.lo_flags & LO_FLAGS_PARTSCAN)) {
                li.lo_flags |= LO_FLAGS_PARTSCAN;
                ioctl(fd, LOOP_SET_STATUS64, &li);
            }
        }
        if (ioctl(fd, BLKRRPART) && errno != EINVAL) admin_log("disk.rereadWarning", "errno", lerr_errno(errno), NULL);
        close(fd);
        settle();
        char pn[32], sp[PATH_MAX];
        part_name(t.disk, 1, pn, sizeof pn);
        snprintf(sp, sizeof sp, "/sys/class/block/%s", pn);
        for (int i = 0; i < 50 && access(sp, F_OK); i++) usleep(100000);
        snprintf(dev, sizeof dev, "/dev/%s", pn);
        struct stat st;
        for (int i = 0; i < 50 && (stat(dev, &st) || !S_ISBLK(st.st_mode)); i++) usleep(100000);
        if (stat(dev, &st) || !S_ISBLK(st.st_mode)) { admin_err("disk.noNewPartition", "name", dev, NULL); return; }
        snprintf(t.name, sizeof t.name, "%s", pn);
    } else {
        int e = zero_range(fd, 0, t.size < MIB ? t.size : MIB);    /* old signatures at the start */
        fsync(fd);
        close(fd);
        if (e) { admin_err("disk.writeFailed", "name", t.name, NULL); return; }
    }
    admin_say("LOG", "progress phase=mkfs");   /* i18n-ok */
    char owner[48];
    snprintf(owner, sizeof owner, "root_owner=%u:%u", (unsigned)r->uid, (unsigned)r->gid);
    int rc;
    if (k == 0) {
        const char *a[] = { DISK_KINDS[k].prog, "-F", "32", "-I", "-n", lab[0] ? lab : "NO NAME", dev, NULL };
        rc = admin_run(a);
    } else if (k == 1) {
        const char *a[] = { DISK_KINDS[k].prog, "-L", lab, dev, NULL };
        const char *b[] = { DISK_KINDS[k].prog, dev, NULL };
        rc = admin_run(lab[0] ? a : b);
    } else if (k == 2) {
        const char *a[] = { DISK_KINDS[k].prog, "-F", "-q", "-L", lab, "-E", owner, dev, NULL };
        rc = admin_run(a);
    } else {
        const char *a[] = { DISK_KINDS[k].prog, "-Q", "-F", "-L", lab, dev, NULL };
        rc = admin_run(a);
    }
    uevent_change(t.name);
    settle();
    if (rc) admin_err("sys.programFailed", "program", DISK_KINDS[k].prog, "code", lerr_int(rc), NULL);
    else admin_say("OK", "%s", t.name);   /* i18n-ok */
}

void disk_act_label(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *name, *lab;
    if (!buf || split2(buf, &name, &lab)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    Target t;
    if (target(r, name, 0, 0, &t)) { free(buf); return; }
    char fs[32], label[64];
    disk_udev_prop(t.name, "ID_FS_TYPE", fs, sizeof fs);
    if (strlen(lab) >= sizeof label) { admin_err("disk.badLabel", NULL); free(buf); return; }
    snprintf(label, sizeof label, "%s", lab);
    free(buf);
    const char *prog = !strcmp(fs, "vfat") ? "/usr/sbin/fatlabel" : !strcmp(fs, "exfat") ? "/usr/sbin/exfatlabel"
                     : !strncmp(fs, "ext", 3) && strlen(fs) == 4 ? "/usr/sbin/e2label" : !strcmp(fs, "ntfs") ? "/usr/sbin/ntfslabel" : NULL;
    if (!prog) { admin_err("disk.cannotRenameFs", "fs", fs, NULL); return; }
    if (access(prog, X_OK)) { admin_err("disk.missingTool", "program", prog, NULL); return; }
    if (!valid_label(fs, label)) { admin_err("disk.badLabelFor", "fs", fs, NULL); return; }
    if (strncmp(fs, "ext", 3) && disk_mounts(t.name, NULL, 0)) { admin_err("disk.unmountFirst", NULL); return; }
    const char *a[] = { prog, t.dev, label, NULL };
    int rc = admin_run(a);
    uevent_change(t.name);
    settle();
    if (rc) admin_err("sys.programFailed", "program", prog, "code", lerr_int(rc), NULL);
    else admin_say("OK", "%s", label);   /* i18n-ok */
}

void disk_act_erase(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *name, *mode;
    if (!buf || split2(buf, &name, &mode)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    int full = !strcmp(mode, "zero");
    if (!full && strcmp(mode, "quick")) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    Target t;
    if (target(r, name, 0, 0, &t)) { free(buf); return; }
    free(buf);
    if (t.part ? unmount_dev(t.name) : unmount_disk(&t)) return;
    int fd = open_excl(&t, 0);
    if (fd < 0) return;
    int e = 0;
    if (!full) e = wipe_ends(fd, t.size);
    else {
        job_begin(r, t.disk);
        static uint8_t z[4 * MIB];
        double last = 0;
        for (uint64_t off = 0; off < t.size && !e && !cancelled;) {
            size_t k = t.size - off > sizeof z ? sizeof z : (size_t)(t.size - off);
            ssize_t w = pwrite(fd, z, k, (off_t)off);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; e = -1; break; }
            off += (uint64_t)w;
            if (now_s() - last > 0.5 || off == t.size) {
                last = now_s();
                fdatasync(fd);          /* real progress, and don't fill the page cache */
                admin_say("LOG", "progress phase=erase done=%llu total=%llu", (unsigned long long)off, (unsigned long long)t.size);   /* i18n-ok */
            }
        }
        job_end(t.disk);
    }
    fsync(fd);
    if (!t.part) ioctl(fd, BLKRRPART);
    close(fd);
    uevent_change(t.name);
    settle();
    if (cancelled) admin_err("disk.cancelled", NULL);
    else if (e) admin_err("disk.writeFailed", "name", t.name, "errno", lerr_errno(errno), NULL);
    else admin_ok("disk.erased", NULL);
}

/* ---- image → disk ---- */

/* Opens the image with the user's permissions, only from their home or their mounts. */
static int open_image(const AdminReq *r, const char *path, struct stat *st) {
    if (path[0] != '/') return -1;
    char real[PATH_MAX], roots[3][PATH_MAX];
    if (!realpath(r->home, roots[0])) return -1;
    snprintf(roots[1], sizeof roots[1], "/media/%s", r->user);
    snprintf(roots[2], sizeof roots[2], "/run/media/%s", r->user);
    setfsgid(r->gid);
    setfsuid(r->uid);
    int fd = -1;
    if (realpath(path, real)) {
        int inside = 0;
        for (int i = 0; i < 3; i++) {
            size_t l = strlen(roots[i]);
            if (!strncmp(real, roots[i], l) && real[l] == '/') inside = 1;
        }
        if (inside) fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    setfsuid(0);
    setfsgid(0);
    if (fd >= 0 && (fstat(fd, st) || !S_ISREG(st->st_mode) || st->st_size <= 0)) { close(fd); fd = -1; }
    return fd;
}

static ssize_t read_full(int fd, uint8_t *b, size_t n) {
    size_t o = 0;
    while (o < n) {
        ssize_t k = read(fd, b + o, n - o);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (!k) break;
        o += (size_t)k;
    }
    return (ssize_t)o;
}

void disk_act_write_image(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *name, *path;
    if (!buf || split2(buf, &name, &path)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    Target t;
    if (target(r, name, 1, 0, &t)) { free(buf); return; }
    struct stat st;
    int in = open_image(r, path, &st);
    free(buf);
    if (in < 0) { admin_err("disk.imageOpenFailed", NULL); return; }
    uint64_t total = (uint64_t)st.st_size;
    if (total > t.size) { close(in); admin_err("disk.imageTooLarge", "name", t.name, "size", lerr_int((long long)total), "capacity", lerr_int((long long)t.size), NULL); return; }
    {   /* the image cannot be on the same disk */
        char dn[32] = "", base[32] = "";
        char p[64], real[PATH_MAX];
        snprintf(p, sizeof p, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
        if (realpath(p, real) && strrchr(real, '/')) snprintf(dn, sizeof dn, "%s", strrchr(real, '/') + 1);
        if (dn[0] && !disk_parent(dn, base, sizeof base)) snprintf(base, sizeof base, "%s", dn);
        if (base[0] && !strcmp(base, t.disk)) { close(in); admin_err("disk.imageOnTarget", NULL); return; }
    }
    if (unmount_disk(&t)) { close(in); return; }
    int out = open_excl(&t, O_DIRECT);
    if (out < 0) { close(in); return; }
    job_begin(r, t.disk);
    int ssz = 512;
    if (ioctl(out, BLKSSZGET, &ssz) || ssz < 512 || ssz > 65536) ssz = 512;
    posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
    const size_t CH = 4 * MIB;
    uint8_t *b = NULL;
    if (posix_memalign((void **)&b, 4096, CH)) { close(in); close(out); job_end(t.disk); admin_err("sys.outOfMemory", NULL); return; }
    Sha256 hs;
    sha256_init(&hs);
    uint64_t off = 0;
    double last = 0, t0 = now_s();
    int err = 0;
    while (off < total && !cancelled) {
        ssize_t k = read_full(in, b, CH);
        if (k <= 0) { err = 1; break; }
        sha256_update(&hs, b, (size_t)k);
        size_t wl = (size_t)k;
        if (wl % (size_t)ssz) {          /* a tail that is not a sector multiple: without O_DIRECT */
            fcntl(out, F_SETFL, fcntl(out, F_GETFL) & ~O_DIRECT);
        }
        for (size_t o = 0; o < wl;) {
            ssize_t w = pwrite(out, b + o, wl - o, (off_t)(off + o));
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; err = 2; break; }
            o += (size_t)w;
        }
        if (err) break;
        off += (uint64_t)k;
        if (now_s() - last > 0.5 || off == total) {
            last = now_s();
            admin_say("LOG", "progress phase=write done=%llu total=%llu", (unsigned long long)off, (unsigned long long)total);   /* i18n-ok */
        }
    }
    close(in);
    uint8_t want[32], got[32];
    sha256_final(&hs, want);
    if (!err && !cancelled && fsync(out)) err = 2;
    double tw = now_s() - t0;
    /* verification: read the disk back (bypassing the cache) and compare the SHA-256 */
    if (!err && !cancelled) {
        ioctl(out, BLKFLSBUF);
        close(out);
        out = open(t.dev, O_RDONLY | O_DIRECT | O_CLOEXEC);
        if (out < 0) err = 3;
        Sha256 hv;
        sha256_init(&hv);
        last = 0;
        for (uint64_t o = 0; !err && o < total && !cancelled;) {
            size_t want_n = total - o > CH ? CH : (size_t)(total - o);
            size_t rd = (want_n + (size_t)ssz - 1) / (size_t)ssz * (size_t)ssz;
            ssize_t k = pread(out, b, rd, (off_t)o);
            if (k < (ssize_t)want_n) { err = 3; break; }
            sha256_update(&hv, b, want_n);
            o += want_n;
            if (now_s() - last > 0.5 || o == total) {
                last = now_s();
                admin_say("LOG", "progress phase=verify done=%llu total=%llu", (unsigned long long)o, (unsigned long long)total);   /* i18n-ok */
            }
        }
        sha256_final(&hv, got);
    }
    if (out >= 0) { ioctl(out, BLKRRPART); close(out); }
    free(b);
    job_end(t.disk);
    uevent_change(t.disk);
    settle();
    char hex[65];
    hex_encode(want, 32, hex);
    if (cancelled) admin_err("disk.cancelledPartial", NULL);
    else if (err == 1) admin_err("disk.imageReadFailed", NULL);
    else if (err == 2) admin_err("disk.writeFailed", "name", t.name, "errno", lerr_errno(errno), NULL);
    else if (err == 3) admin_err("disk.verifyReadFailed", "name", t.name, NULL);
    else if (memcmp(want, got, 32)) admin_err("disk.verifyMismatch", NULL);
    else {
        char ms[24], mbps[24];
        snprintf(ms, sizeof ms, "%.0f", tw * 1000);
        snprintf(mbps, sizeof mbps, "%.1f", tw > 0 ? total / tw / 1e6 : 0);
        admin_ok("disk.verified", "sha256", hex, "ms", ms, "mbps", mbps, NULL);
    }
}

/* ---- eject ---- */

void disk_act_eject(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    Target t;
    if (target(r, arg, 1, 0, &t)) return;
    if (unmount_disk(&t)) return;
    sync();
    /* nobody else holds it exclusively (an erase or a copy in progress) */
    int ex = open(t.dev, O_RDONLY | O_EXCL | O_NONBLOCK | O_CLOEXEC);
    if (ex < 0 && errno == EBUSY) { admin_err("disk.busyWait", "name", t.name, NULL); return; }
    if (ex >= 0) close(ex);
    int fd = open(t.dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        ioctl(fd, BLKFLSBUF);
        if (!strncmp(t.name, "sd", 2)) {
            uint8_t cdb[6] = { 0x1b, 0, 0, 0, 0x02, 0 };    /* START STOP UNIT: LoEj=1, Start=0 */
            uint8_t sense[32];
            sg_cmd(fd, cdb, 6, NULL, 0, SG_DXFER_NONE, sense, sizeof sense);
        }
        close(fd);
    }
    /* over USB: power the device off (like "Power Off" in GNOME Disks) */
    char tr[16] = "", path[256] = "";
    disk_udev_prop(t.name, "ID_BUS", tr, sizeof tr);
    disk_udev_prop(t.name, "ID_PATH", path, sizeof path);
    if (!strcmp(tr, "usb") || strstr(path, "-usb-")) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s/device/delete", t.disk);
        int d = open(p, O_WRONLY | O_CLOEXEC);
        if (d >= 0) { if (write(d, "1", 1) < 0) { /* keep going */ } close(d); }
    }
    admin_ok("disk.ejected", NULL);
}

/* ---- for the LordOS Imager (imager/imager_admin.c) ---- */

int disk_admin_whole(const AdminReq *r, const char *name, DiskTarget *o) {
    Target t;
    if (target(r, name, 1, 0, &t)) return -1;
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", t.name);
    snprintf(o->disk, sizeof o->disk, "%s", t.disk);
    snprintf(o->dev, sizeof o->dev, "%s", t.dev);
    o->part = t.part;
    o->removable = t.removable;
    o->size = t.size;
    struct stat st;
    if (lstat(t.dev, &st) || !S_ISBLK(st.st_mode)) { admin_err("disk.notBlock", "name", t.dev, NULL); return -1; }
    o->major = major(st.st_rdev);
    o->minor = minor(st.st_rdev);
    return 0;
}

static void to_target(const DiskTarget *d, Target *t) {
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", d->name);
    snprintf(t->disk, sizeof t->disk, "%s", d->disk);
    snprintf(t->dev, sizeof t->dev, "%s", d->dev);
    t->part = d->part;
    t->removable = d->removable;
    t->size = d->size;
}

int disk_admin_unmount(const DiskTarget *d) { Target t; to_target(d, &t); return unmount_disk(&t); }
int disk_admin_open_excl(const DiskTarget *d, int extra) { Target t; to_target(d, &t); return open_excl(&t, extra); }
int disk_admin_open_user_file(const AdminReq *r, const char *path, struct stat *st) { return open_image(r, path, st); }
int disk_admin_wipe_ends(int fd, uint64_t size) { return wipe_ends(fd, size); }
void disk_admin_job_begin(const AdminReq *r, const char *disk) { job_begin(r, disk); }
void disk_admin_job_end(const char *disk) { job_end(disk); }
int disk_admin_cancelled(void) { return cancelled != 0; }
void disk_admin_reread(const char *disk) { uevent_change(disk); settle(); }

int disk_admin_file_on(const struct stat *st, const char *disk) {
    char dn[32] = "", base[32] = "", p[64], real[PATH_MAX];
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", major(st->st_dev), minor(st->st_dev));
    if (realpath(p, real) && strrchr(real, '/')) snprintf(dn, sizeof dn, "%s", strrchr(real, '/') + 1);
    if (dn[0] && !disk_parent(dn, base, sizeof base)) snprintf(base, sizeof base, "%s", dn);
    return base[0] && !strcmp(base, disk);
}
