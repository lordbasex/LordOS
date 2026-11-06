/* manualpart.h — "choose the partitions yourself": looking at a disk, then deleting, creating and
 * formatting exactly what the plan says, with its undo. Used by install.c; the decisions are in
 * manual.c. */
#ifndef LORD_MANUALPART_H
#define LORD_MANUALPART_H

#include "freepart.h"
#include "manual.h"

typedef struct {
    FpProbe fp;                 /* the free space mode's look at the disk: table, snapshot, facts */
    MpFacts facts;
    char tag[48];               /* mp_table_tag, "" when the table couldn't be read */
    const char *why;            /* NULL: manual partitioning can be offered on this disk */
} MppProbe;

typedef struct {
    FpUndo undo;                /* the table's undo; dropped once deleted space may have been written */
    char dev[MP_ROLES][48];     /* /dev/... of each role, "" when not used */
    char home_fs[16];           /* the file system /home ends up with */
    unsigned esp_num;
    int esp_formatted;          /* the EFI partition is new or was formatted: it holds only LordOS */
    int table_changed;
    int kept_new_table;         /* deletions were formatted over: a failure from here on keeps the new table */
} MppResult;

/* Only reads: the table (as the free space mode reads it) and every partition's file system type. */
void mpp_probe(const char *disk, MppProbe *p);

/* The plan against the disk as it is now: the tag install-disks gave has to still be the disk's,
 * and the chosen EFI partition, when kept, is looked at read-only. NULL or the code to show. */
const char *mpp_check(const char *disk, const char *tag, MpPlan *pl, MppProbe *p);

/* Checks everything again, saves the table, deletes and creates in one sgdisk call, proves the
 * table changed exactly so, waits for the kernel to see it, then formats what the plan formats.
 * NULL on success. On failure the table is back as it was, when that is still safe (see
 * docs/ISO.md), and the code to show is returned. */
const char *mpp_apply(const char *disk, const char *tag, MpPlan *pl, MppResult *res);

/* The folders under EFI/ of an EFI partition (EFI/BOOT left out), mounted read-only: the other
 * systems' boot loaders. Returns how many were put in `names`. */
int mpp_esp_loaders(const char *part, char names[][33], int max);

#endif
