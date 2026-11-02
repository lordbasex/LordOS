/* freepart.c — the disk side of "install in free space" (docs/ISO.md, "Installing to disk").
 *
 * Reads the first sector and both GPT copies itself, asks sgdisk to verify and print, and believes
 * a disk only when all of that agrees: freespace.c decides. The only write to the table is one
 * `sgdisk -n N:start:end`; before it the table is saved with `sgdisk --backup`, and afterwards the
 * raw entries are compared with the ones read before. Every program has a fixed path. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "freepart.h"
#include "../admin.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define SGDISK     "/usr/sbin/sgdisk"
#define EFIBOOTMGR "/usr/bin/efibootmgr"
#define RUNDIR     "/run/lordos-install"
#define ESP_PROBE  RUNDIR "/esp-probe"
#define TEXT_MAX   32768

/* The architecture's Linux root type, as sgdisk's code and as it lies on disk. */
#if defined(__x86_64__)
#define ROOT_CODE "8304"
static const uint8_t root_type[16] = { 0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d, 0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09 };
#elif defined(__aarch64__)
#define ROOT_CODE "8305"
static const uint8_t root_type[16] = { 0x45, 0xb0, 0x21, 0xb9, 0xf0, 0x1d, 0xc3, 0x41, 0xaf, 0x44, 0x4c, 0x6f, 0x28, 0x0d, 0x3f, 0xae };
#else
#error "the disk installer knows x86-64 and arm64"
#endif

#ifdef FP_TEST
int fp_test_uefi, fp_test_fail;
#endif

static int sys_ull(const char *path, unsigned long long *out) {
    char v[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *ok = fgets(v, sizeof v, f);
    fclose(f);
    if (!ok || !isdigit((unsigned char)v[0])) return -1;
    *out = strtoull(v, NULL, 10);
    return 0;
}

static int read_at(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Reads the boot structures. Sets s->ok only when both GPT copies are sound and agree; the first
 * sector is always read if the disk can be read at all. Returns -1 if it can't. */
static int fp_snapshot(const char *disk, FpSnap *s) {
    char p[128], dev[48];
    unsigned long long v, sectors512;
    memset(s, 0, sizeof *s);
    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", disk);
    if (sys_ull(p, &v) || (v != 512 && v != 4096)) return -1;
    s->lsec = (unsigned)v;
    snprintf(p, sizeof p, "/sys/block/%s/size", disk);
    if (sys_ull(p, &sectors512) || !sectors512) return -1;
    s->total = sectors512 / (s->lsec / 512);
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = -1;
    if (read_at(fd, s->mbr, sizeof s->mbr, 0)) goto out;
    r = 0;
    if (read_at(fd, s->hdr1, s->lsec, s->lsec) || fsp_gpt_header(s->hdr1, s->lsec, s->total, 0, &s->g1)) goto out;
    if (read_at(fd, s->hdr2, s->lsec, (s->total - 1) * s->lsec) || fsp_gpt_header(s->hdr2, s->lsec, s->total, 1, &s->g2)) goto out;
    size_t bytes = (size_t)s->g1.count * FSP_ENTRY_SIZE;
    if (s->g2.count != s->g1.count) goto out;
    if (read_at(fd, s->ent1, bytes, s->g1.entries_lba * s->lsec) || read_at(fd, s->ent2, bytes, s->g2.entries_lba * s->lsec)) goto out;
    if (fsp_gpt_pair(&s->g1, &s->g2, s->ent1, s->ent2)) goto out;
    s->ok = 1;
out:
    close(fd);
    return r;
}

static int same_snapshot(const FpSnap *a, const FpSnap *b) {
    if (!a->ok || !b->ok || a->lsec != b->lsec || a->total != b->total) return 0;
    size_t bytes = (size_t)a->g1.count * FSP_ENTRY_SIZE;
    return !memcmp(a->mbr, b->mbr, sizeof a->mbr) && !memcmp(a->hdr1, b->hdr1, a->lsec) && !memcmp(a->hdr2, b->hdr2, a->lsec)
        && a->g1.count == b->g1.count && !memcmp(a->ent1, b->ent1, bytes) && !memcmp(a->ent2, b->ent2, bytes);
}

/* Mounted, swapped or held by device-mapper, md or anything else that claims a partition. */
static int in_use(const char *disk) {
    char dev[48], line[1024], p[PATH_MAX];
    int busy = 0;
    size_t dl = (size_t)snprintf(dev, sizeof dev, "/dev/%s", disk);
    FILE *f = fopen("/proc/self/mounts", "r");
    while (f && !busy && fgets(line, sizeof line, f)) if (!strncmp(line, dev, dl)) busy = 1;
    if (f) fclose(f);
    f = fopen("/proc/swaps", "r");
    while (f && !busy && fgets(line, sizeof line, f)) if (!strncmp(line, dev, dl)) busy = 1;
    if (f) fclose(f);
    snprintf(p, sizeof p, "/sys/block/%s", disk);
    DIR *d = opendir(p);
    struct dirent *e;
    size_t nl = strlen(disk);
    while (d && !busy && (e = readdir(d))) {
        if (strncmp(e->d_name, disk, nl) && strcmp(e->d_name, "holders")) continue;
        if (strcmp(e->d_name, "holders")) snprintf(p, sizeof p, "/sys/block/%s/%s/holders", disk, e->d_name);
        else snprintf(p, sizeof p, "/sys/block/%s/holders", disk);
        DIR *h = opendir(p);
        struct dirent *x;
        while (h && (x = readdir(h))) if (x->d_name[0] != '.') busy = 1;
        if (h) closedir(h);
    }
    if (d) closedir(d);
    return busy;
}

static int capture(const char *const argv[], char *out, size_t cap) {
    int r = admin_run_capture(argv, out, cap);
    return r || strlen(out) >= cap - 1 ? -1 : 0;         /* a cut text says nothing about the disk */
}

/* Mounts the ESP read-only, without exec, to measure it and look for our folder. */
static void look_at_esp(const char *esp, FspFacts *f) {
    const char *const argv[] = { "/usr/sbin/blkid", "-p", "-s", "TYPE", "-o", "value", esp, NULL };
    char type[64] = "";
    if (admin_run_capture(argv, type, sizeof type)) return;
    type[strcspn(type, "\n")] = 0;
    if (strcmp(type, "vfat")) return;
    f->esp_fat = 1;
    mkdir(RUNDIR, 0700);
    mkdir(ESP_PROBE, 0700);
    if (mount(esp, ESP_PROBE, "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return;
    struct statvfs st;
    int measured = statvfs(ESP_PROBE, &st) == 0;
    int ours = access(ESP_PROBE "/EFI/" FP_BOOT_FOLDER, F_OK) == 0;
    if (umount2(ESP_PROBE, 0)) { umount2(ESP_PROBE, MNT_DETACH); return; }
    if (!measured) return;
    f->esp_free = (uint64_t)st.f_bavail * st.f_frsize;
    f->esp_has_ours = ours;
    f->esp_read = 1;
}

static int disk_table(const char *dev, FpProbe *p) {
    static char text[TEXT_MAX];
    FspTable raw;
    const char *const verify[] = { SGDISK, "-v", dev, NULL }, *const print[] = { SGDISK, "-p", dev, NULL };
    if (capture(verify, text, sizeof text) || fsp_verify_clean(text)) return -1;
    if (capture(print, text, sizeof text) || fsp_parse_print(text, &p->table)) return -1;
    if (fsp_raw_table(&p->snap.g1, p->snap.lsec, p->snap.total, p->snap.ent1, &raw)) return -1;
    return fsp_tables_agree(&p->table, &raw) ? 0 : -1;
}

void fp_probe(const char *disk, FpProbe *p) {
    char dev[48];
    memset(p, 0, sizeof *p);
    FspFacts *f = &p->facts;
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    f->uefi = access("/sys/firmware/efi", F_OK) == 0;
    f->boot_tool = access(EFIBOOTMGR, X_OK) == 0;
#ifdef FP_TEST
    if (fp_test_uefi) f->uefi = f->boot_tool = 1;
#endif
    f->busy = in_use(disk);
    f->table = &p->table;
    if (fp_snapshot(disk, &p->snap)) { p->why = "install.free.tableDamaged"; return; }
    f->mbr = fsp_mbr_kind(p->snap.mbr);
    /* sgdisk only ever sees a disk that already looks like a clean GPT */
    if (f->mbr == FSP_MBR_PROTECTIVE && p->snap.ok && !disk_table(dev, p)) {
        f->table_ok = 1;
        unsigned esp;
        if (fsp_largest_free(&p->table, &p->region)) f->table_ok = 0;
        p->esps = fsp_esp_count(&p->table, &esp);
        if (f->table_ok && p->esps == 1 && !f->busy) {
            char path[48];
            fsp_part_path(disk, esp, path, sizeof path);
            look_at_esp(path, f);
        }
    }
    p->why = fsp_decide(f, &p->choice);
}

/* ---- creating the partition ---- */

static int settle(void) {
    const char *const a[] = { "/usr/bin/udevadm", "settle", "--timeout=10", NULL };
    return admin_run(a);
}

/* The kernel sees the new partition exactly where it was asked to be. */
static int kernel_sees(const char *disk, const char *part, unsigned lsec, uint64_t start, uint64_t end) {
    const char *base = strrchr(part, '/') + 1;
    char p[128];
    unsigned long long s, n;
    uint64_t scale = lsec / 512;                 /* sysfs counts 512-byte units */
    for (int i = 0; i < 50; i++) {
        if (!access(part, F_OK)) break;
        usleep(200000);
    }
    if (access(part, F_OK)) return 0;
    snprintf(p, sizeof p, "/sys/block/%s/%s/start", disk, base);
    if (sys_ull(p, &s)) return 0;
    snprintf(p, sizeof p, "/sys/block/%s/%s/size", disk, base);
    if (sys_ull(p, &n)) return 0;
    return s == start * scale && n == (end - start + 1) * scale;
}

/* The write did what it should, and only that. */
static int proven(const FpUndo *u, const char *disk, uint64_t start, uint64_t end) {
    static FpSnap now;
    if (fp_snapshot(disk, &now) || !now.ok) return 0;
    const FpSnap *b = &u->before;
    if (!fsp_mbr_same(b->mbr, now.mbr)) return 0;
    if (!fsp_header_kept(&b->g1, &now.g1) || !fsp_header_kept(&b->g2, &now.g2)) return 0;
    return fsp_entries_changed_only(b->ent1, now.ent1, b->g1.count, u->num - 1, start, end, root_type) == 0;
}

const char *fp_create(const char *disk, uint64_t start, uint64_t end, FpUndo *u) {
    static FpProbe p;
    static FpSnap again;
    memset(u, 0, sizeof *u);
    /* everything again, now: nothing the wizard or an earlier check said counts */
    fp_probe(disk, &p);
    if (p.why) return p.why;
    if (!start || p.choice.region.start != start || p.choice.region.end != end) return "install.free.diskChanged";

    snprintf(u->dev, sizeof u->dev, "/dev/%s", disk);
    snprintf(u->backup, sizeof u->backup, RUNDIR "/gpt-%s.backup", disk);
    u->num = p.choice.num;
    u->esp_num = p.choice.esp_num;
    u->before = p.snap;
    fsp_part_path(disk, u->num, u->root, sizeof u->root);
    fsp_part_path(disk, u->esp_num, u->esp, sizeof u->esp);

    mkdir(RUNDIR, 0700);
    unlink(u->backup);
    char arg[140];
    snprintf(arg, sizeof arg, "--backup=%s", u->backup);
    { const char *const a[] = { SGDISK, arg, u->dev, NULL };
      struct stat st;
      if (admin_run(a) || stat(u->backup, &st) || !st.st_size) { unlink(u->backup); return "install.free.backupFailed"; } }

    /* the region and the table one last time, against the bytes taken above */
    if (fp_snapshot(disk, &again) || !same_snapshot(&p.snap, &again) || in_use(disk)) { unlink(u->backup); return "install.free.diskChanged"; }

    char n[64], t[24], c[24];
    snprintf(n, sizeof n, "%u:%llu:%llu", u->num, (unsigned long long)start, (unsigned long long)end);
    snprintf(t, sizeof t, "%u:" ROOT_CODE, u->num);
    snprintf(c, sizeof c, "%u:LordOS", u->num);
#ifdef FP_TEST
    if (fp_test_fail == 3) snprintf(t, sizeof t, "%u:zzzz", u->num);
#endif
    const char *const a[] = { SGDISK, "-n", n, "-t", t, "-c", c, u->dev, NULL };
    u->active = 1;
    if (admin_run(a)) { fp_undo(u); return "install.partitionFailed"; }
#ifdef FP_TEST
    if (fp_test_fail == 1) { fp_undo(u); return "install.free.verifyFailed"; }
#endif
    settle();
    if (!proven(u, disk, start, end)) { fp_undo(u); return "install.free.verifyFailed"; }
    if (!kernel_sees(disk, u->root, u->before.lsec, start, end)) { fp_undo(u); return "install.noPartitions"; }
#ifdef FP_TEST
    if (fp_test_fail == 2) { fp_undo(u); return "install.formatRootFailed"; }
#endif
    { const char *const m[] = { "/usr/sbin/mkfs.ext4", "-F", "-q", "-L", "LordOS", u->root, NULL };
      if (admin_run(m)) { fp_undo(u); return "install.formatRootFailed"; } }
    return NULL;
}

int fp_undo(FpUndo *u) {
    static FpSnap now;
    if (!u->active) return 0;
    u->active = 0;
    const char *disk = strrchr(u->dev, '/') + 1;
    /* sgdisk refused before saving anything: there is nothing to write back */
    if (!fp_snapshot(disk, &now) && same_snapshot(&u->before, &now)) { unlink(u->backup); return 0; }
    admin_log("install.free.rollingBack", NULL);
    char arg[140];
    snprintf(arg, sizeof arg, "--load-backup=%s", u->backup);
    const char *const a[] = { SGDISK, arg, u->dev, NULL };
    int r = admin_run(a);
    sync();
    settle();
    if (r || fp_snapshot(disk, &now) || !same_snapshot(&u->before, &now)) {
        admin_log("install.free.rollbackFailed", "path", u->backup, NULL);
        return -1;
    }
    unlink(u->backup);
    admin_log("install.free.rolledBack", NULL);
    return 0;
}

void fp_commit(FpUndo *u) {
    if (u->active) unlink(u->backup);
    u->active = 0;
}
