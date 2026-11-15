/* diskuse.c — whether a partition is free to be mounted under /media (issue #122), and whether Windows left an
 * NTFS volume hibernated. Used by the disk-mount action as root (disk_admin.c) and, read-only, by the list
 * lord.disks.list() shows (disk.c). Tests: tests/disk_test.c, on fake trees (root != "").
 *
 * A partition is in use when:
 *   - it is mounted anywhere (its major:minor in mountinfo; a btrfs file system by its source or, for a
 *     multi-device one, by /sys/fs/btrfs/<UUID>);
 *   - something sits on top of it (/sys/class/block/<p>/holders: LUKS, LVM, RAID, bcache…);
 *   - it is an active swap (/proc/swaps).
 * A partition of the system disk is also refused when its partition table entry is one LordOS or Windows
 * keeps for itself (disk_part_type_reserved) or when that entry is unknown (fail closed).
 *
 * Inputs come only from the kernel (sysfs, /proc) and the udev database (/run/udev/data, written by root's
 * udevd): never from the caller. The NTFS check reads 4 bytes of hiberfil.sys through a mount the kernel
 * made; nothing of the file system is parsed here. */
#include "disk.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int rd(const char *root, const char *rel, char *out, size_t n) {
    char p[PATH_MAX];
    out[0] = 0;
    snprintf(p, sizeof p, "%s%s", root, rel);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) { out[0] = 0; return -1; }
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return 0;
}

static int devno(const char *root, const char *name, unsigned *ma, unsigned *mi) {
    char rel[96], b[32];
    snprintf(rel, sizeof rel, "/sys/class/block/%s/dev", name);
    return rd(root, rel, b, sizeof b) || sscanf(b, "%u:%u", ma, mi) != 2 ? -1 : 0;
}

static void prop(const char *root, unsigned ma, unsigned mi, const char *key, char *out, size_t n) {
    out[0] = 0;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/run/udev/data/b%u:%u", root, ma, mi);
    FILE *f = fopen(p, "re");
    if (!f) return;
    char line[1024];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'E' || line[1] != ':' || strncmp(line + 2, key, kl) || line[2 + kl] != '=') continue;
        char *v = line + 3 + kl;
        v[strcspn(v, "\n")] = 0;
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

/* The last path component of a /dev/… path, after the kernel's octal escapes. */
static const char *dev_leaf(char *path) {
    char *o = path;
    for (char *p = path; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else *o++ = *p;
    }
    *o = 0;
    if (strncmp(path, "/dev/", 5)) return "";
    const char *b = strrchr(path, '/');
    return b + 1;
}

static int mounted(const char *root, const char *name, unsigned ma, unsigned mi) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/proc/self/mountinfo", root);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    char line[PATH_MAX * 2 + 512];
    int hit = 0;
    while (!hit && fgets(line, sizeof line, f)) {
        unsigned a, b;
        if (sscanf(line, "%*d %*d %u:%u", &a, &b) != 2) continue;
        if (a == ma && b == mi) { hit = 1; break; }
        /* btrfs reports an anonymous 0:N: its source names the device */
        char *dash = strstr(line, " - ");
        char src[PATH_MAX];
        if (a == 0 && dash && sscanf(dash + 3, "%*s %4095s", src) == 1 && !strcmp(dev_leaf(src), name)) hit = 1;
    }
    fclose(f);
    return hit;
}

static int held(const char *root, const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/sys/class/block/%s/holders", root, name);
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n > 0;
}

static int swapping(const char *root, const char *name, unsigned ma, unsigned mi) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/proc/swaps", root);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    char line[PATH_MAX + 128];
    int hit = 0;
    if (fgets(line, sizeof line, f)) {                 /* the header */
        while (!hit && fgets(line, sizeof line, f)) {
            char file[PATH_MAX], full[PATH_MAX + 256];
            if (sscanf(line, "%4095s", file) != 1) continue;
            const char *leaf = dev_leaf(file);          /* unescapes file in place */
            snprintf(full, sizeof full, "%s%s", root, file);
            struct stat st;
            if (!strcmp(leaf, name)) hit = 1;
            else if (!stat(full, &st) && S_ISBLK(st.st_mode) && major(st.st_rdev) == ma && minor(st.st_rdev) == mi) hit = 1;
        }
    }
    fclose(f);
    return hit;
}

/* A btrfs file system registered as mounted (every device of a multi-device one, not only the one mountinfo names) */
static int btrfs_mounted(const char *root, const char *uuid) {
    size_t n = strlen(uuid);
    if (n < 32 || n > 40 || strspn(uuid, "0123456789abcdefABCDEF-") != n) return 0;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/sys/fs/btrfs/%s", root, uuid);
    return access(p, F_OK) == 0;
}

int disk_part_type_reserved(const char *scheme, const char *type, const char *flags) {
    if (!scheme || !type) return 0;
    if (!strcmp(scheme, "gpt")) {
        static const char *const guid[] = {
            "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",    /* EFI System Partition */
            "21686148-6449-6e6f-744e-656564454649",    /* BIOS boot */
            "e3c9e316-0b5c-4db8-817d-f92df00215ae",    /* Microsoft Reserved (MSR) */
            "de94bba4-06d1-4d40-a16a-bfd50179d6ac",    /* Windows Recovery Environment */
            "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3",    /* Windows LDM metadata */
            "af9b60a0-1431-4f62-bc68-3311714a69ad",    /* Windows LDM data */
            "bc13c2ff-59e6-4262-a352-b275fd6f7172",    /* Linux extended boot (XBOOTLDR) */
            "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f",    /* Linux swap */
            "e6d6d379-f507-44c2-a23c-238f2a3df928",    /* Linux LVM */
            "a19d880f-05fc-4d3b-a006-743f0f84911e",    /* Linux RAID */
            "ca7d7ccb-63ed-4c53-861c-1742536059cc",    /* Linux LUKS */
        };
        for (size_t i = 0; i < sizeof guid / sizeof *guid; i++) if (!strcasecmp(type, guid[i])) return 1;
        /* attribute bit 0, "required partition" (platform/OEM recovery partitions carry it) */
        if (flags && !strncasecmp(flags, "0x", 2)) {
            char *end;
            unsigned long long a = strtoull(flags + 2, &end, 16);
            if (end != flags + 2 && !*end && (a & 1)) return 1;
        }
        return 0;
    }
    if (!strcmp(scheme, "dos")) {
        if (strncasecmp(type, "0x", 2)) return 0;
        char *end;
        unsigned long t = strtoul(type + 2, &end, 16);
        if (end == type + 2 || *end) return 0;
        static const unsigned long mbr[] = {
            0x05, 0x0f, 0x85,                           /* extended partitions */
            0xef,                                       /* EFI System Partition */
            0x27,                                       /* Windows Recovery Environment (hidden NTFS) */
            0x42,                                       /* Windows LDM */
            0x82, 0x8e, 0xfd,                           /* Linux swap, LVM, RAID */
            0xee,                                       /* GPT protective */
        };
        for (size_t i = 0; i < sizeof mbr / sizeof *mbr; i++) if (t == mbr[i]) return 1;
    }
    return 0;
}

int disk_part_use(const char *root, const char *name, int system) {
    if (!root) root = "";
    unsigned ma, mi;
    if (!disk_valid_name(name) || devno(root, name, &ma, &mi)) return DISK_USE_UNKNOWN;
    char fs[32], uuid[64];
    prop(root, ma, mi, "ID_FS_TYPE", fs, sizeof fs);
    prop(root, ma, mi, "ID_FS_UUID", uuid, sizeof uuid);
    if (mounted(root, name, ma, mi) || (!strcmp(fs, "btrfs") && btrfs_mounted(root, uuid))) return DISK_USE_MOUNTED;
    if (held(root, name)) return DISK_USE_HELD;
    if (swapping(root, name, ma, mi)) return DISK_USE_SWAP;
    if (!system) return DISK_USE_FREE;
    char scheme[16], type[64], flags[32];
    prop(root, ma, mi, "ID_PART_ENTRY_SCHEME", scheme, sizeof scheme);
    prop(root, ma, mi, "ID_PART_ENTRY_TYPE", type, sizeof type);
    prop(root, ma, mi, "ID_PART_ENTRY_FLAGS", flags, sizeof flags);
    if ((strcmp(scheme, "gpt") && strcmp(scheme, "dos")) || !type[0]) return DISK_USE_UNKNOWN;
    return disk_part_type_reserved(scheme, type, flags) ? DISK_USE_RESERVED : DISK_USE_FREE;
}

int disk_hiber_header(const unsigned char *b, size_t n) {
    return n >= 4 && !strncasecmp((const char *)b, "hibr", 4);
}

int disk_ntfs_hibernated(int dirfd) {
    static const char *const names[] = { "hiberfil.sys", "HIBERFIL.SYS" };
    int fd = -1;
    for (size_t i = 0; fd < 0 && i < sizeof names / sizeof *names; i++) {
        fd = openat(dirfd, names[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0 && errno != ENOENT) return 1;       /* there but unreadable: can't tell */
    }
    if (fd < 0) return 0;                               /* no hibernation file */
    struct stat st;
    unsigned char b[4];
    int h;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) h = 1;
    else if (st.st_size < 4) h = 0;
    else h = pread(fd, b, sizeof b, 0) != (ssize_t)sizeof b || disk_hiber_header(b, sizeof b);
    close(fd);
    return h;
}
