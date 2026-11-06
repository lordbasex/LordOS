/* manual.h — the pure decisions of "choose the partitions yourself" (docs/ISO.md, "Manual
 * partitioning"): reading the plan's fields, checking them against the disk's table, numbering the
 * new partitions, building sgdisk's arguments and proving afterwards that the table changed exactly
 * as planned and in no other way. No I/O here: tests/manual_test.c feeds it hostile input. The part
 * that touches a disk is manualpart.c. */
#ifndef LORD_MANUAL_H
#define LORD_MANUAL_H

#include "freespace.h"

#define MP_ROOT_MIN      FSP_MIN_BYTES   /* 8 GiB, what an erase install requires of the whole disk */
#define MP_HOME_MIN      (1ull << 30)
#define MP_SWAP_MIN      (256ull << 20)
#define MP_SWAP_MAX      (64ull << 30)
#define MP_ESP_NEW_MIN   (100ull << 20)  /* a new EFI partition */
#define MP_ESP_FMT_MIN   (64ull << 20)   /* an existing one that gets formatted */
#define MP_LIST_MAX      1024            /* the plan's text fields */

typedef enum { MP_ROOT, MP_HOME, MP_SWAP, MP_ESP, MP_ROLES } MpRole;

/* Where one role goes. num 0: the role is not used (only home and swap may be left out). */
typedef struct {
    unsigned num;
    int is_new;                 /* a partition this plan creates */
    uint64_t start, end;        /* sectors, inclusive */
    int format;                 /* it gets a new file system: always but for a kept /home or EFI partition */
} MpSlot;

typedef struct {
    unsigned del[FSP_MAX_PARTS];
    int ndel;
    struct { MpRole role; uint64_t start, end; unsigned num; } add[MP_ROLES];
    int nadd;
    unsigned part[MP_ROLES];    /* existing partitions chosen for a role, 0 = none */
    int home_format, esp_format;
    long swap_file;             /* MiB of /swapfile */
    MpSlot slot[MP_ROLES];      /* filled by mp_decide */
} MpPlan;

/* What the disk says. Everything defaults to "no". fstype[n] is blkid's TYPE of partition n. */
typedef struct {
    int uefi, boot_tool, busy;
    FspMbr mbr;
    int table_ok;
    const FspTable *table;
    char fstype[FSP_MAX_PARTS + 1][16];
    /* the existing EFI partition chosen to be kept, looked at read-only */
    int esp_fat, esp_read, esp_has_ours;
    uint64_t esp_free;
} MpFacts;

/* "root", "home", "swap" or "esp"; -1 for anything else. */
int mp_role(const char *s);
const char *mp_role_name(MpRole r);

/* "2,5": partitions to delete. 0, or -1 for anything but distinct numbers 1..128 joined by commas. */
int mp_parse_delete(const char *s, MpPlan *pl);
/* "root:2048:16779263,swap:...": new partitions, one per role at most. 0 or -1. */
int mp_parse_create(const char *s, MpPlan *pl);

/* The disk's reasons not to offer manual partitioning at all; NULL when it can be offered. */
const char *mp_disk_why(const MpFacts *f);

/* NULL when the plan can be carried out on this disk, otherwise the error code to show. Fills
 * pl->slot and the new partitions' numbers (the lowest free ones after the deletions). */
const char *mp_decide(const MpFacts *f, MpPlan *pl);

/* Does the plan change the table? (no: only existing partitions get formatted) */
int mp_changes_table(const MpPlan *pl);

/* sgdisk's arguments for every deletion and creation, in one call. root_code is the architecture's
 * root type ("8304"/"8305"). Pointers into `buf`; argv ends with NULL. Returns the count, -1 if it
 * doesn't fit. */
#define MP_ARGV_MAX (4 + FSP_MAX_PARTS * 2 + MP_ROLES * 6)
int mp_sgdisk_argv(const MpPlan *pl, const char *sgdisk, const char *dev, const char *root_code,
                   char *buf, size_t n, const char *argv[MP_ARGV_MAX]);

/* After the write: deleted entries are empty, new ones carry their type and sectors, and every other
 * entry is byte for byte what it was. 0 when so. root_type is the architecture's root GUID on disk. */
int mp_entries_proven(const uint8_t *before, const uint8_t *after, uint32_t count, const MpPlan *pl,
                      const uint8_t *root_type);

/* What install-disks reports and install-run must get back: the disk's GUID and the CRC of its
 * entries, so any change to the table between the two is noticed. */
void mp_table_tag(const FspGpt *g, char *out, size_t n);

/* File systems a kept /home may have, and their fstab check pass. */
int mp_home_fs_ok(const char *fstype);
int mp_fsck_pass(const char *fstype);

/* An EFI folder name that may be shown as it is: [A-Za-z0-9._-], 1..32 characters. */
int mp_loader_name_ok(const char *s);

#endif
