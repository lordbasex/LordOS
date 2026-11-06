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

/* Shared with manual partitioning (manualpart.c). */
extern const uint8_t fp_root_type[16];          /* the architecture's Linux root type, on disk */
extern const char fp_root_code[];               /* the same as sgdisk's code */
int  fp_sys_ull(const char *path, unsigned long long *out);
/* Reads the first sector and both GPT copies; s->ok only when both are sound and agree. -1: unreadable. */
int  fp_snapshot(const char *disk, FpSnap *s);
int  fp_same_snapshot(const FpSnap *a, const FpSnap *b);
/* Something on the disk is mounted, swapped or held. */
int  fp_in_use(const char *disk);
/* Mounts an EFI partition read-only to measure it (esp_fat, esp_read, esp_free, esp_has_ours). */
void fp_look_at_esp(const char *esp, FspFacts *f);
int  fp_settle(void);
/* The kernel sees partition `part` of `disk` exactly at start..end (sectors of lsec bytes). */
int  fp_kernel_sees(const char *disk, const char *part, unsigned lsec, uint64_t start, uint64_t end);

#ifdef FP_TEST
/* tests/freepart_cli.c only, never in lord-server. */
extern int fp_test_uefi;        /* act as if booted through UEFI, with efibootmgr present */
extern int fp_test_fail;        /* 1: after sgdisk wrote; 2: instead of mkfs; 3: sgdisk -n itself gets a bad type */
#endif

#endif
