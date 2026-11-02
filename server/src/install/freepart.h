/* freepart.h — "install in free space": looking at a disk and creating the one new partition, with
 * its undo. Used by install.c; the decisions are in freespace.c. */
#ifndef LORD_FREEPART_H
#define LORD_FREEPART_H

#include "freespace.h"

#define FP_BOOT_FOLDER "LordOS"                 /* EFI/LordOS on the existing ESP */

/* A disk's boot structures, byte for byte. */
typedef struct {
    int ok;                                     /* both GPT copies read and sound */
    unsigned lsec;
    uint64_t total;
    uint8_t mbr[512], hdr1[4096], hdr2[4096], ent1[FSP_MAX_PARTS * FSP_ENTRY_SIZE], ent2[FSP_MAX_PARTS * FSP_ENTRY_SIZE];
    FspGpt g1, g2;
} FpSnap;

typedef struct {
    FspFacts facts;
    FspTable table;
    FpSnap snap;
    FspChoice choice;
    const char *why;                            /* NULL: it qualifies */
    int esps;
    FspRegion region;                           /* largest free one, even when refused */
} FpProbe;

/* What has to be undone if a later step fails. */
typedef struct {
    int active;
    char dev[48], backup[96], esp[48], root[48];
    unsigned num, esp_num;
    FpSnap before;
} FpUndo;

/* Reads the disk and decides. Only reads: sgdisk -v and -p, and the ESP mounted read-only. */
void fp_probe(const char *disk, FpProbe *p);

/* Checks everything again (the region has to be the one the person saw), creates exactly one ext4
 * partition there and proves nothing else in the table moved. On failure the table is back as it
 * was and the code to show is returned; NULL on success, with `u` ready for fp_undo. */
const char *fp_create(const char *disk, uint64_t start, uint64_t end, FpUndo *u);

/* Puts the table back and proves it, byte for byte. No-op when nothing is pending. -1 if it could
 * not be proven: the sgdisk backup is then left in place and its path was logged. */
int fp_undo(FpUndo *u);

/* The install finished: nothing is left to undo. */
void fp_commit(FpUndo *u);

#ifdef FP_TEST
/* tests/freepart_cli.c only, never in lord-server. */
extern int fp_test_uefi;        /* act as if booted through UEFI, with efibootmgr present */
extern int fp_test_fail;        /* 1: after sgdisk wrote; 2: instead of mkfs; 3: sgdisk -n itself gets a bad type */
#endif

#endif
