/* manualpart.c — the disk side of "choose the partitions yourself" (docs/ISO.md, "Manual
 * partitioning").
 *
 * The disk is read and believed exactly as in the free space mode (freepart.c: both GPT copies by
 * hand, sgdisk -v and -p, and the two agreeing); manual.c decides. The table is written by one
 * `sgdisk -d ... -n ... -t ... -c ...` call, after `sgdisk --backup`, and the raw entries are then
 * compared with the ones read before: deleted entries empty, new ones as planned, every other byte
 * as it was. Every program has a fixed path and every argument is built here from numbers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "manualpart.h"
#include "../admin.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define SGDISK    "/usr/sbin/sgdisk"
#define RUNDIR    "/run/lordos-install"
#define ESP_LIST  RUNDIR "/esp-list"

/* blkid's TYPE of one partition, probed on the device itself (not the cache); "" when none. */
static void fs_type(const char *part, char *out, size_t n) {
    const char *const argv[] = { "/usr/sbin/blkid", "-p", "-s", "TYPE", "-o", "value", part, NULL };
    char buf[64] = "";
    out[0] = 0;
    if (admin_run_capture(argv, buf, sizeof buf)) return;
    buf[strcspn(buf, "\n")] = 0;
    for (const char *c = buf; *c; c++) if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_') return;
    snprintf(out, n, "%s", buf);
}

void mpp_probe(const char *disk, MppProbe *p) {
    memset(&p->facts, 0, sizeof p->facts);
    p->tag[0] = 0;
    fp_probe(disk, &p->fp);
    MpFacts *f = &p->facts;
    f->uefi = p->fp.facts.uefi;
    f->boot_tool = p->fp.facts.boot_tool;
    f->busy = p->fp.facts.busy;
    f->mbr = p->fp.facts.mbr;
    f->table_ok = p->fp.facts.table_ok;
    f->table = &p->fp.table;
    if (p->fp.snap.ok) mp_table_tag(&p->fp.snap.g1, p->tag, sizeof p->tag);
    if (f->table_ok && !f->busy)
        for (int i = 0; i < p->fp.table.n; i++) {
            unsigned num = p->fp.table.p[i].num;
            char path[48];
            if (num < 1 || num > FSP_MAX_PARTS) continue;
            fsp_part_path(disk, num, path, sizeof path);
            fs_type(path, f->fstype[num], sizeof f->fstype[num]);
        }
    p->why = mp_disk_why(f);
}

const char *mpp_check(const char *disk, const char *tag, MpPlan *pl, MppProbe *p) {
    mpp_probe(disk, p);
    if (p->why) return p->why;
    if (!tag[0] || strcmp(tag, p->tag)) return "install.manual.diskChanged";
    if (pl->part[MP_ESP] && !pl->esp_format) {
        FspFacts ef;
        char path[48];
        memset(&ef, 0, sizeof ef);
        fsp_part_path(disk, pl->part[MP_ESP], path, sizeof path);
        fp_look_at_esp(path, &ef);
        p->facts.esp_fat = ef.esp_fat;
        p->facts.esp_read = ef.esp_read;
        p->facts.esp_free = ef.esp_free;
        p->facts.esp_has_ours = ef.esp_has_ours;
    }
    return mp_decide(&p->facts, pl);
}

/* The kernel dropped a deleted partition. */
static int kernel_dropped(const char *disk, unsigned num) {
    char path[48], sys[128];
    fsp_part_path(disk, num, path, sizeof path);
    snprintf(sys, sizeof sys, "/sys/block/%s/%s", disk, strrchr(path, '/') + 1);
    for (int i = 0; i < 50; i++) {
        if (access(sys, F_OK)) return 1;
        usleep(200000);
    }
    return 0;
}

static int proven(const MppResult *r, const char *disk, const MpPlan *pl) {
    static FpSnap now;
    if (fp_snapshot(disk, &now) || !now.ok) return 0;
    const FpSnap *b = &r->undo.before;
    if (!fsp_mbr_same(b->mbr, now.mbr)) return 0;
    if (!fsp_header_kept(&b->g1, &now.g1) || !fsp_header_kept(&b->g2, &now.g2)) return 0;
    return mp_entries_proven(b->ent1, now.ent1, b->g1.count, pl, fp_root_type) == 0;
}

static int run_ok(const char *const argv[]) { return admin_run(argv) == 0; }

/* wipefs, then the role's file system. */
static const char *format(MpRole role, const char *dev) {
    static const char *const fail[MP_ROLES] = { "install.formatRootFailed", "install.manual.formatHomeFailed",
                                                "install.manual.formatSwapFailed", "install.formatEfiFailed" };
    const char *const w[] = { "/usr/sbin/wipefs", "-a", "-q", dev, NULL };
    if (!run_ok(w)) return fail[role];
    int ok = 0;
    switch (role) {
    case MP_ROOT: { const char *const a[] = { "/usr/sbin/mkfs.ext4", "-F", "-q", "-L", "LordOS", dev, NULL }; ok = run_ok(a); break; }
    case MP_HOME: { const char *const a[] = { "/usr/sbin/mkfs.ext4", "-F", "-q", "-L", "home", dev, NULL }; ok = run_ok(a); break; }
    case MP_SWAP: { const char *const a[] = { "/usr/sbin/mkswap", "-q", "-L", "swap", dev, NULL }; ok = run_ok(a); break; }
    case MP_ESP:  { const char *const a[] = { "/usr/sbin/mkfs.vfat", "-F", "32", "-n", "EFI", dev, NULL }; ok = run_ok(a); break; }
    default: break;
    }
    return ok ? NULL : fail[role];
}

const char *mpp_apply(const char *disk, const char *tag, MpPlan *pl, MppResult *res) {
    static MppProbe p;
    static FpSnap again;
    FpUndo *u = &res->undo;
    memset(res, 0, sizeof *res);
    /* everything again, now: nothing the wizard or an earlier check said counts */
    const char *why = mpp_check(disk, tag, pl, &p);
    if (why) return why;

    for (int r = 0; r < MP_ROLES; r++) if (pl->slot[r].num) fsp_part_path(disk, pl->slot[r].num, res->dev[r], sizeof res->dev[r]);
    res->esp_num = pl->slot[MP_ESP].num;
    res->esp_formatted = pl->slot[MP_ESP].format;
    snprintf(res->home_fs, sizeof res->home_fs, "%s",
             !pl->slot[MP_HOME].num ? "" : pl->slot[MP_HOME].format ? "ext4" : p.facts.fstype[pl->slot[MP_HOME].num]);
    res->table_changed = mp_changes_table(pl);
    unsigned lsec = p.fp.snap.lsec;

    if (res->table_changed) {
        snprintf(u->dev, sizeof u->dev, "/dev/%s", disk);
        snprintf(u->backup, sizeof u->backup, RUNDIR "/gpt-%s.backup", disk);
        u->before = p.fp.snap;
        mkdir(RUNDIR, 0700);
        unlink(u->backup);
        char arg[140];
        snprintf(arg, sizeof arg, "--backup=%s", u->backup);
        { const char *const a[] = { SGDISK, arg, u->dev, NULL };
          struct stat st;
          if (admin_run(a) || stat(u->backup, &st) || !st.st_size) { unlink(u->backup); return "install.free.backupFailed"; } }
        /* the table one last time, against the bytes the decision was made on */
        if (fp_snapshot(disk, &again) || !fp_same_snapshot(&p.fp.snap, &again) || fp_in_use(disk)) { unlink(u->backup); return "install.manual.diskChanged"; }

        static char buf[8192];
        const char *argv[MP_ARGV_MAX];
        if (mp_sgdisk_argv(pl, SGDISK, u->dev, fp_root_code, buf, sizeof buf, argv) < 0) { unlink(u->backup); return "install.badPlan"; }
        u->active = 1;
        if (admin_run(argv)) { fp_undo(u); return "install.partitionFailed"; }
#ifdef FP_TEST
        if (fp_test_fail == 1) { fp_undo(u); return "install.manual.verifyFailed"; }
#endif
        fp_settle();
        if (!proven(res, disk, pl)) { fp_undo(u); return "install.manual.verifyFailed"; }
        for (int i = 0; i < pl->ndel; i++) if (!kernel_dropped(disk, pl->del[i])) { fp_undo(u); return "install.noPartitions"; }
    }
    for (int r = 0; r < MP_ROLES; r++)
        if (pl->slot[r].num && !fp_kernel_sees(disk, res->dev[r], lsec, pl->slot[r].start, pl->slot[r].end)) { fp_undo(u); return "install.noPartitions"; }

    /* from the first format on, space that was a deleted partition may be written: putting the old
     * table back would bring back partitions whose data is no longer there */
    if (pl->ndel) { fp_commit(u); res->kept_new_table = 1; }
#ifdef FP_TEST
    if (fp_test_fail == 2) { fp_undo(u); return "install.formatRootFailed"; }
#endif
    for (int r = 0; r < MP_ROLES; r++)
        if (pl->slot[r].num && pl->slot[r].format && (why = format((MpRole)r, res->dev[r]))) { fp_undo(u); return why; }
    return NULL;
}

int mpp_esp_loaders(const char *part, char names[][33], int max) {
    int n = 0;
    mkdir(RUNDIR, 0700);
    mkdir(ESP_LIST, 0700);
    if (mount(part, ESP_LIST, "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return 0;
    DIR *d = opendir(ESP_LIST "/EFI");
    struct dirent *e;
    while (d && n < max && (e = readdir(d))) {
        if (!mp_loader_name_ok(e->d_name) || !strcasecmp(e->d_name, "BOOT")) continue;
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, ESP_LIST "/EFI/%s", e->d_name);
        if (lstat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        size_t l = strlen(e->d_name);             /* at most 32: mp_loader_name_ok */
        memcpy(names[n], e->d_name, l + 1);
        n++;
    }
    if (d) closedir(d);
    if (umount2(ESP_LIST, 0)) umount2(ESP_LIST, MNT_DETACH);
    return n;
}
