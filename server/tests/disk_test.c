/* disk_test.c — which partitions disk-mount may mount (src/disk/diskuse.c, issue #122), on a fake tree
 * laid out like the kernel's (sys/class/block, sys/fs/btrfs, proc/self/mountinfo, proc/swaps) and udev's
 * (run/udev/data). A dual-boot NVMe disk, nvme0n1, holds the system:
 *   p1  ESP (GPT), not mounted              → reserved
 *   p2  ext4, mounted on /                  → mounted
 *   p3  swap, active                        → swap
 *   p4  LUKS, opened (holders: dm-0)        → held
 *   p5  Microsoft Reserved                  → reserved
 *   p6  NTFS, Windows' C: (basic data)      → free: the case #122 asks for
 *   p7  Windows Recovery                    → reserved
 *   p8  basic data with the "required" attribute (an OEM recovery) → reserved
 *   p9  no udev entry                       → unknown on the system disk, free elsewhere
 *   p10 btrfs whose file system is mounted through another device (sys/fs/btrfs/<UUID>) → mounted
 *   p11 btrfs mounted with an anonymous 0:N device, named by its source → mounted
 *   p12 BIOS boot                           → reserved
 *   p13 swap through a name with an escape (\040) in /proc/swaps → swap
 * and an MBR disk, sda: sda1 NTFS (0x7) free, sda2 Windows RE (0x27) and sda3 ESP (0xef) reserved,
 * sda4 an extended partition (0x5) reserved. Then the hiberfil.sys check on real folders.
 *
 *   make -C server build/disk-test && (cd server && ./build/disk-test)   (in `make test`) */
#include "../src/disk/disk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("✗ " __VA_ARGS__); printf("\n"); } } while (0)

static char root[256];

static void mkdirs(const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text) {
    char p[1024], d[1024];
    snprintf(d, sizeof d, "%s", rel);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* A block device as sysfs and udev show it: its dev file, and udev's E: lines (NULL: no udev entry). */
static void dev(const char *name, int ma, int mi, const char *udev) {
    char rel[256], v[64];
    snprintf(rel, sizeof rel, "sys/class/block/%s/dev", name);
    snprintf(v, sizeof v, "%d:%d\n", ma, mi);
    put(rel, v);
    if (!udev) return;
    snprintf(rel, sizeof rel, "run/udev/data/b%d:%d", ma, mi);
    put(rel, udev);
}

#define GPT(type, fs, extra) "E:ID_PART_ENTRY_SCHEME=gpt\nE:ID_PART_ENTRY_TYPE=" type "\nE:ID_FS_TYPE=" fs "\n" extra
#define MBR(type, fs) "E:ID_PART_ENTRY_SCHEME=dos\nE:ID_PART_ENTRY_TYPE=" type "\nE:ID_FS_TYPE=" fs "\n"
#define BASIC "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"

static void types(void) {
    static const char *const reserved[] = {
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "21686148-6449-6e6f-744e-656564454649", "e3c9e316-0b5c-4db8-817d-f92df00215ae",
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", "af9b60a0-1431-4f62-bc68-3311714a69ad",
        "bc13c2ff-59e6-4262-a352-b275fd6f7172", "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "e6d6d379-f507-44c2-a23c-238f2a3df928",
        "a19d880f-05fc-4d3b-a006-743f0f84911e", "ca7d7ccb-63ed-4c53-861c-1742536059cc",
    };
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) CHECK(disk_part_type_reserved("gpt", reserved[i], "0x0"), "gpt %s reserved", reserved[i]);
    CHECK(disk_part_type_reserved("gpt", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", ""), "an upper-case GUID is the same type");
    CHECK(!disk_part_type_reserved("gpt", BASIC, "0x0"), "basic data is free");
    CHECK(!disk_part_type_reserved("gpt", "0fc63daf-8483-4772-8e79-3d69d8477de4", ""), "Linux data is free");
    CHECK(!disk_part_type_reserved("gpt", BASIC, "0x8000000000000000"), "basic data with only \"no drive letter\" is free");
    CHECK(disk_part_type_reserved("gpt", BASIC, "0x8000000000000001"), "basic data with \"required\" is reserved");
    CHECK(!disk_part_type_reserved("gpt", BASIC, "0x1zz"), "garbage flags are not read as bits");
    CHECK(!disk_part_type_reserved("gpt", "c12a7328-f81f-11d2-ba4b-00a0c93ec93", ""), "a cut GUID is not the ESP");
    static const char *const mbr_reserved[] = { "0x5", "0xf", "0x85", "0xef", "0x27", "0x42", "0x82", "0x8e", "0xfd", "0xee", "0xEF" };
    for (size_t i = 0; i < sizeof mbr_reserved / sizeof *mbr_reserved; i++) CHECK(disk_part_type_reserved("dos", mbr_reserved[i], ""), "dos %s reserved", mbr_reserved[i]);
    static const char *const mbr_free[] = { "0x7", "0xc", "0xb", "0x83", "0x17" };
    for (size_t i = 0; i < sizeof mbr_free / sizeof *mbr_free; i++) CHECK(!disk_part_type_reserved("dos", mbr_free[i], ""), "dos %s free", mbr_free[i]);
    CHECK(!disk_part_type_reserved("dos", "ef", ""), "a dos type without 0x is not read");
    CHECK(!disk_part_type_reserved("dos", "0xefz", ""), "a dos type with garbage is not read");
    CHECK(!disk_part_type_reserved("dos", "0x80", "0x80"), "the dos boot flag is not GPT's required bit");
    CHECK(!disk_part_type_reserved(NULL, NULL, NULL), "nothing known: not reserved by type (disk_part_use says unknown)");
}

static void tree(void) {
    snprintf(root, sizeof root, "/tmp/disk-test-XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(1); }
    dev("nvme0n1", 259, 0, "E:ID_PART_TABLE_TYPE=gpt\n");
    dev("nvme0n1p1", 259, 1, GPT("c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "vfat", ""));
    dev("nvme0n1p2", 259, 2, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "ext4", ""));
    dev("nvme0n1p3", 259, 3, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "swap", ""));
    dev("nvme0n1p4", 259, 4, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "crypto_LUKS", ""));
    mkdirs("sys/class/block/nvme0n1p4/holders/dm-0");
    mkdirs("sys/class/block/nvme0n1p6/holders");         /* empty: nothing on top */
    dev("nvme0n1p5", 259, 5, GPT("e3c9e316-0b5c-4db8-817d-f92df00215ae", "", ""));
    dev("nvme0n1p6", 259, 6, GPT(BASIC, "ntfs", "E:ID_PART_ENTRY_FLAGS=0x0\nE:ID_FS_LABEL=Windows\n"));
    dev("nvme0n1p7", 259, 7, GPT("de94bba4-06d1-4d40-a16a-bfd50179d6ac", "ntfs", "E:ID_PART_ENTRY_FLAGS=0x8000000000000001\n"));
    dev("nvme0n1p8", 259, 8, GPT(BASIC, "ntfs", "E:ID_PART_ENTRY_FLAGS=0x8000000000000001\n"));
    dev("nvme0n1p9", 259, 9, NULL);
    dev("nvme0n1p10", 259, 10, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "btrfs", "E:ID_FS_UUID=0b3a6e2c-1c5e-4f7a-9a43-7f1d2a4b5c6d\n"));
    mkdirs("sys/fs/btrfs/0b3a6e2c-1c5e-4f7a-9a43-7f1d2a4b5c6d");
    dev("nvme0n1p11", 259, 11, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "btrfs", "E:ID_FS_UUID=9e9e9e9e-0000-4000-8000-000000000011\n"));
    dev("nvme0n1p12", 259, 12, GPT("21686148-6449-6e6f-744e-656564454649", "", ""));
    dev("nvme0n1p13", 259, 13, GPT("0fc63daf-8483-4772-8e79-3d69d8477de4", "swap", ""));
    dev("sda", 8, 0, "E:ID_PART_TABLE_TYPE=dos\n");
    dev("sda1", 8, 1, MBR("0x7", "ntfs"));
    dev("sda2", 8, 2, MBR("0x27", "ntfs"));
    dev("sda3", 8, 3, MBR("0xef", "vfat"));
    dev("sda4", 8, 4, MBR("0x5", ""));
    dev("sdb1", 8, 17, "E:ID_PART_ENTRY_SCHEME=atari\nE:ID_PART_ENTRY_TYPE=BGM\nE:ID_FS_TYPE=vfat\n");
    put("proc/self/mountinfo",
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "23 22 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:5 - proc proc rw\n"
        "30 22 0:45 / /srv/data rw,relatime shared:9 - btrfs /dev/nvme0n1p11 rw,space_cache=v2\n");
    put("proc/swaps",
        "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
        "/dev/nvme0n1p3                          partition\t8388604\t\t0\t\t-2\n"
        "/dev/disk/odd\\040name/nvme0n1p13       partition\t1024\t\t0\t\t-3\n");
}

static const char *use_name(int u) {
    static const char *const n[] = { "free", "mounted", "held", "swap", "reserved", "unknown" };
    return u >= 0 && u <= DISK_USE_UNKNOWN ? n[u] : "?";
}

#define USE(name, sys, want) do { int u_ = disk_part_use(root, name, sys); CHECK(u_ == (want), "%s (system %d): %s, expected %s", name, sys, use_name(u_), use_name(want)); } while (0)

static void uses(void) {
    USE("nvme0n1p1", 1, DISK_USE_RESERVED);
    USE("nvme0n1p2", 1, DISK_USE_MOUNTED);
    USE("nvme0n1p3", 1, DISK_USE_SWAP);
    USE("nvme0n1p4", 1, DISK_USE_HELD);
    USE("nvme0n1p5", 1, DISK_USE_RESERVED);
    USE("nvme0n1p6", 1, DISK_USE_FREE);
    USE("nvme0n1p7", 1, DISK_USE_RESERVED);
    USE("nvme0n1p8", 1, DISK_USE_RESERVED);
    USE("nvme0n1p9", 1, DISK_USE_UNKNOWN);
    USE("nvme0n1p10", 1, DISK_USE_MOUNTED);
    USE("nvme0n1p11", 1, DISK_USE_MOUNTED);
    USE("nvme0n1p12", 1, DISK_USE_RESERVED);
    USE("nvme0n1p13", 1, DISK_USE_SWAP);
    USE("sda1", 1, DISK_USE_FREE);
    USE("sda2", 1, DISK_USE_RESERVED);
    USE("sda3", 1, DISK_USE_RESERVED);
    USE("sda4", 1, DISK_USE_RESERVED);
    USE("sdb1", 1, DISK_USE_UNKNOWN);          /* a table LordOS doesn't classify: refused on the system disk */
    /* off the system disk only "in use" counts: another disk's ESP stays mountable as before */
    USE("nvme0n1p1", 0, DISK_USE_FREE);
    USE("nvme0n1p9", 0, DISK_USE_FREE);
    USE("sdb1", 0, DISK_USE_FREE);
    USE("nvme0n1p2", 0, DISK_USE_MOUNTED);
    USE("nvme0n1p3", 0, DISK_USE_SWAP);
    USE("nvme0n1p4", 0, DISK_USE_HELD);
    /* names that aren't devices */
    USE("nvme0n1p99", 1, DISK_USE_UNKNOWN);
    USE("../sda1", 1, DISK_USE_UNKNOWN);
    USE("", 1, DISK_USE_UNKNOWN);
    /* a partition mounted after the fact turns "mounted" (the root side reads it on every call) */
    put("proc/self/mountinfo", "22 1 259:2 / / rw - ext4 /dev/nvme0n1p2 rw\n40 22 259:6 / /media/ana/Windows rw,nosuid,nodev - ntfs3 /dev/nvme0n1p6 rw\n");
    USE("nvme0n1p6", 1, DISK_USE_MOUNTED);
    USE("nvme0n1p11", 1, DISK_USE_FREE);       /* its mount is gone */
}

static void hiber(void) {
    CHECK(disk_hiber_header((const unsigned char *)"HIBR", 4), "HIBR is hibernated");
    CHECK(disk_hiber_header((const unsigned char *)"hibrxxxx", 8), "hibr is hibernated");
    CHECK(disk_hiber_header((const unsigned char *)"HiBr", 4), "any case");
    CHECK(!disk_hiber_header((const unsigned char *)"wake", 4), "wake: resumed, not hibernated");
    CHECK(!disk_hiber_header((const unsigned char *)"\0\0\0\0", 4), "zeros: not hibernated");
    CHECK(!disk_hiber_header((const unsigned char *)"HIB", 3), "too short");

    char d[300], p[400];
    snprintf(d, sizeof d, "%s/vol", root);
    mkdir(d, 0755);
    int dfd = open(d, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0, "open the volume folder");
    CHECK(disk_ntfs_hibernated(dfd) == 0, "no hiberfil.sys: not hibernated");
    snprintf(p, sizeof p, "%s/hiberfil.sys", d);
    FILE *f = fopen(p, "w"); fputs("HIBR", f); for (int i = 0; i < 4092; i++) fputc(0, f); fclose(f);
    CHECK(disk_ntfs_hibernated(dfd) == 1, "HIBR: hibernated (Fast Startup)");
    f = fopen(p, "w"); fputs("wake", f); fclose(f);
    CHECK(disk_ntfs_hibernated(dfd) == 0, "wake: not hibernated");
    f = fopen(p, "w"); fclose(f);
    CHECK(disk_ntfs_hibernated(dfd) == 0, "an empty file: not hibernated");
    unlink(p);
    if (symlink("/etc/passwd", p)) perror("symlink");
    CHECK(disk_ntfs_hibernated(dfd) == 1, "a link is never followed: can't tell, read-only");
    unlink(p);
    mkdir(p, 0755);
    CHECK(disk_ntfs_hibernated(dfd) == 1, "a folder: can't tell, read-only");
    rmdir(p);
    if (mkfifo(p, 0644)) perror("mkfifo");
    CHECK(disk_ntfs_hibernated(dfd) == 1, "a FIFO: never blocks, read-only");
    unlink(p);
    snprintf(p, sizeof p, "%s/HIBERFIL.SYS", d);
    f = fopen(p, "w"); fputs("hibr", f); fclose(f);
    CHECK(disk_ntfs_hibernated(dfd) == 1, "an upper-case name is found too");
    unlink(p);
    close(dfd);
}

int main(void) {
    types();
    tree();
    uses();
    hiber();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) { /* leave it */ }
    printf("disk-test: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
