/* freespace.h — the pure decisions of "install in free space" (docs/ISO.md): reading sgdisk's
 * output and the raw GPT, choosing the region, aligning it, refusing, and proving afterwards that
 * nothing but one new table entry changed. No I/O here: tests/freespace_test.c feeds it hostile input.
 * The part that touches a disk is freepart.c. */
#ifndef LORD_FREESPACE_H
#define LORD_FREESPACE_H

#include <stddef.h>
#include <stdint.h>

#define FSP_MIN_BYTES    (8ull << 30)    /* the size an erase install requires of the disk */
#define FSP_ESP_MIN_FREE (32ull << 20)   /* room grub needs on the existing ESP */
#define FSP_ALIGN_BYTES  (1ull << 20)    /* the new partition starts and ends on 1 MiB */
#define FSP_MAX_PARTS    128             /* table entries accepted; more is refused */
#define FSP_ENTRY_SIZE   128
#define FSP_MAX_TOTAL    (1ull << 48)    /* sectors; more is nonsense and could overflow a size */

typedef enum { FSP_MBR_NONE, FSP_MBR_PROTECTIVE, FSP_MBR_HYBRID, FSP_MBR_DOS } FspMbr;

typedef struct { unsigned num; uint64_t start, end; int esp; } FspPart;

/* A partition table, from sgdisk's text or from the raw entries. */
typedef struct {
    unsigned lsec, max_entries;
    uint64_t total, first, last;
    int n;
    FspPart p[FSP_MAX_PARTS];
} FspTable;

/* A raw GPT header. */
typedef struct {
    uint64_t my, alt, first, last, entries_lba;
    uint32_t count, esize, entries_crc;
    uint8_t guid[16];
} FspGpt;

/* The largest free region, already aligned; bytes = 0 when there is none. */
typedef struct { uint64_t start, end, bytes; } FspRegion;

/* All that is known about a disk when deciding. Everything defaults to "no". */
typedef struct {
    int uefi;                   /* the live system booted through UEFI */
    int boot_tool;              /* efibootmgr exists */
    int busy;                   /* something is mounted, swapped or held on the disk */
    FspMbr mbr;
    int table_ok;               /* raw CRCs, both copies, sgdisk -v, sgdisk -p and the two agree */
    const FspTable *table;      /* only read when table_ok */
    int esp_fat;                /* the ESP really is FAT */
    int esp_read;               /* it could be mounted read-only and measured */
    int esp_has_ours;           /* EFI/LordOS already exists on it */
    uint64_t esp_free;
} FspFacts;

typedef struct { unsigned num, esp_num; FspRegion region; } FspChoice;

uint32_t fsp_crc32(const uint8_t *p, size_t n);

/* 0 erase, 1 free, -1 anything else. "" is erase: what the action always did. */
int fsp_mode(const char *s);

/* What the first sector says; `s` is 512 bytes. */
FspMbr fsp_mbr_kind(const uint8_t *s);
/* Text for install-disks: "gpt", "hybrid", "dos" or "none". */
const char *fsp_mbr_name(FspMbr k);

/* Does the protective first sector still carry the same boot code and signature? */
int fsp_mbr_same(const uint8_t *a, const uint8_t *b);

/* Checks one raw header (backup = 0: at LBA 1; 1: at the disk's end). 0 when it is sound. */
int fsp_gpt_header(const uint8_t *sec, unsigned lsec, uint64_t total, int backup, FspGpt *g);
/* The two headers and their entries (count * 128 bytes each) describe the same table. */
int fsp_gpt_pair(const FspGpt *a, const FspGpt *b, const uint8_t *ea, const uint8_t *eb);
/* Fills a table from raw entries; -1 if an entry is nonsense. */
int fsp_raw_table(const FspGpt *g, unsigned lsec, uint64_t total, const uint8_t *entries, FspTable *t);

/* sgdisk -p and -v. Anything not known to appear on a healthy GPT makes them fail: 0 only when sure. */
int fsp_parse_print(const char *text, FspTable *t);
int fsp_verify_clean(const char *text);
/* The text and the raw table say the same thing. */
int fsp_tables_agree(const FspTable *a, const FspTable *b);

/* Largest aligned gap between the usable limits and the partitions. -1 if partitions overlap or
 * leave the usable area: such a table is not to be believed. */
int fsp_largest_free(const FspTable *t, FspRegion *r);
/* Lowest free partition number, 0 if the table is full. */
unsigned fsp_free_number(const FspTable *t);
/* Number of ESPs, and the first one's number. */
int fsp_esp_count(const FspTable *t, unsigned *num);

/* NULL when the disk qualifies, otherwise the error code to show. */
const char *fsp_decide(const FspFacts *f, FspChoice *c);

/* After the write: only entry idx changed, from empty to start..end of `type`; 0 when so. */
int fsp_entries_changed_only(const uint8_t *before, const uint8_t *after, uint32_t count, uint32_t idx,
                             uint64_t start, uint64_t end, const uint8_t *type);
/* The header kept the disk's identity and layout. */
int fsp_header_kept(const FspGpt *a, const FspGpt *b);

/* /dev/sda + 3 → /dev/sda3, /dev/nvme0n1 + 3 → /dev/nvme0n1p3. */
void fsp_part_path(const char *disk, unsigned num, char *out, size_t n);

#endif
