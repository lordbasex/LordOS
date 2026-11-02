/* freespace_test.c — the decisions of "install in free space" (src/install/freespace.c), with hostile
 * sgdisk output and hostile GPTs. Any doubt has to end in a refusal: nothing here may say yes by accident. */
#include "install/freespace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define T(c) do { checks++; if (!(c)) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

#define HEAD(sectors, last) \
    "Disk /dev/sda: " #sectors " sectors, 100.0 GiB\n" \
    "Sector size (logical/physical): 512/512 bytes\n" \
    "Disk identifier (GUID): F677CE3E-1E7B-457E-8DF2-30BEFF21D1B7\n" \
    "Partition table holds up to 128 entries\n" \
    "Main partition table begins at sector 2 and ends at sector 33\n" \
    "First usable sector is 34, last usable sector is " #last "\n" \
    "Partitions will be aligned on 2048-sector boundaries\n" \
    "Total free space is 69565 sectors (34.0 MiB)\n\n" \
    "Number  Start (sector)    End (sector)  Size       Code  Name\n"

static const char good[] = HEAD(209715200, 209715166)
    "   1            2048         1050623   512.0 MiB   EF00  EFI\n"
    "   3         1050624        41943039   19.5 GiB    8300  Warning: named data\n";

/* what real sgdisk 1.0.10 printed */
static const char real_mbr[] = "\n***************************************************************\n"
    "Found invalid GPT and valid MBR; converting MBR to GPT format\nin memory. \n"
    "***************************************************************\n\n" HEAD(131072, 131038)
    "   1            2048           22527   10.0 MiB    8300  Linux filesystem\n";
static const char real_crc[] = "Caution! After loading partitions, the CRC doesn't check out!\n" HEAD(131072, 131038)
    "   1            2048           43007   20.0 MiB    EF00  EFI\n";
static const char real_after[] = HEAD(131072, 131038)
    "   1            2048           43007   20.0 MiB    EF00  EFI\n"
    "Warning! Main partition table CRC mismatch! Loaded backup partition table\ninstead of main partition table!\n";
static const char real_new[] = "Creating new GPT entries in memory.\n" HEAD(131072, 131038);

static void parse_ok(void) {
    FspTable t;
    T(fsp_parse_print(good, &t) == 0);
    T(t.total == 209715200 && t.lsec == 512 && t.first == 34 && t.last == 209715166 && t.max_entries == 128);
    T(t.n == 2 && t.p[0].num == 1 && t.p[0].esp && t.p[1].num == 3 && !t.p[1].esp);
    T(t.p[1].start == 1050624 && t.p[1].end == 41943039);
    unsigned esp;
    T(fsp_esp_count(&t, &esp) == 1 && esp == 1);
    T(fsp_free_number(&t) == 2);                /* a gap in the numbers is used, nothing is renumbered */
    /* older sgdisk's sector line, and 4K disks */
    T(fsp_parse_print("Disk x: 100 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 128 entries\n"
                      "First usable sector is 34, last usable sector is 66\nNumber  Start (sector)    End (sector)  Size       Code  Name\n", &t) == 0);
    T(fsp_parse_print("Disk x: 100 sectors, 1 GiB\nSector size (logical/physical): 4096/4096 bytes\nPartition table holds up to 128 entries\n"
                      "First usable sector is 6, last usable sector is 90\nNumber  Start (sector)    End (sector)  Size       Code  Name\n", &t) == 0 && t.lsec == 4096);
}

static void parse_hostile(void) {
    static FspTable t;
    static const char *const bad[] = {
        "", "\n", "garbage", real_mbr, real_crc, real_after, real_new,
        /* cut before the header or in the middle of a row */
        "Disk /dev/sda: 100 sectors, 1 GiB\n",
        HEAD(131072, 131038) "   1            2048",
        HEAD(131072, 131038) "   1            2048           43007",
        HEAD(131072, 131038) "   1            2048           43007   20.0 MiB",
        HEAD(131072, 131038) "   1            2048           43007   20.0 MiB    EF0\n",
        HEAD(131072, 131038) "   1            2048           43007   20.0 MiB    EF00x y\n",
        /* numbers that aren't */
        HEAD(131072, 131038) "  -1            2048           43007   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) "   0            2048           43007   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) " 129            2048           43007   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) "   1            0x10           43007   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) "   1            2048  99999999999999999999999   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) "   1    18446744073709551616   18446744073709551617   20.0 MiB    EF00  EFI\n",
        HEAD(131072, 131038) "   1           43007            2048   20.0 MiB    EF00  EFI\n",
        /* the same number twice; an empty line inside the table; text after it */
        HEAD(131072, 131038) "   1            2048           4095   1.0 MiB    EF00  a\n   1            4096           8191   2.0 MiB    8300  b\n",
        HEAD(131072, 131038) "   1            2048           4095   1.0 MiB    EF00  a\n\n",
        HEAD(131072, 131038) "   1            2048           4095   1.0 MiB    EF00  a\nHere is a free region\n",
        /* a header lying about the disk */
        "Disk x: 0 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 128 entries\nFirst usable sector is 34, last usable sector is 66\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        "Disk x: 100 sectors, 1 GiB\nSector size (logical): 513 bytes\nPartition table holds up to 128 entries\nFirst usable sector is 34, last usable sector is 66\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        "Disk x: 100 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 129 entries\nFirst usable sector is 34, last usable sector is 66\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        "Disk x: 100 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 128 entries\nFirst usable sector is 66, last usable sector is 34\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        "Disk x: 100 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 128 entries\nFirst usable sector is 34, last usable sector is 100\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        "Disk x: 100 sectors, 1 GiB\nDisk y: 100 sectors, 1 GiB\nSector size (logical): 512 bytes\nPartition table holds up to 128 entries\nFirst usable sector is 34, last usable sector is 66\nNumber  Start (sector)    End (sector)  Size       Code  Name\n",
        NULL };
    for (int i = 0; bad[i]; i++) { checks++; if (!fsp_parse_print(bad[i], &t)) { fails++; printf("FAILED: hostile text %d accepted\n", i); } }

    /* a line longer than anything sgdisk prints */
    size_t n = 8192;
    char *big = malloc(n + sizeof good);
    strcpy(big, good);
    memset(big + strlen(good), 'A', n);
    big[strlen(good) + n - 1] = 0;
    T(fsp_parse_print(big, &t) != 0);
    free(big);

    /* more rows than a table can hold */
    char *many = malloc(1 << 16), *o = many;
    o += sprintf(o, "%s", HEAD(209715200, 209715166));
    for (int i = 1; i <= 129; i++) o += sprintf(o, "%4d %12d %12d   1.0 MiB   8300  x\n", i > 128 ? 128 : i, i * 4096, i * 4096 + 2047);
    T(fsp_parse_print(many, &t) != 0);
    free(many);
}

static void verify(void) {
    T(fsp_verify_clean("\nNo problems found. 69565 free sectors (34.0 MiB) available in 2\nsegments, the largest of which is 67551 (33.0 MiB) in size.\n") == 0);
    T(fsp_verify_clean("") != 0);
    T(fsp_verify_clean("Identified 1 problems!\n") != 0);
    T(fsp_verify_clean("Caution! After loading partitions, the CRC doesn't check out!\nNo problems found.\n") != 0);
    T(fsp_verify_clean("\n****\nFound invalid GPT and valid MBR; converting MBR to GPT format\nin memory. \n****\n\n\nNo problems found. 1 free\n") != 0);
    T(fsp_verify_clean("No problems found. Warning! but\n") != 0);
    /* a partition off its alignment is a note about performance, not about the table (a full disk gave "damaged") */
#define END_NOTE "\nCaution: Partition 2 doesn't end on a 2048-sector boundary. This may\nresult in problems with some disk encryption tools.\n"
#define CLEAN "\nNo problems found. 2014 free sectors (1007.0 KiB) available in 1\nsegments, the largest of which is 2014 (1007.0 KiB) in size.\n"
    T(fsp_verify_clean(END_NOTE CLEAN) == 0);
    T(fsp_verify_clean(END_NOTE END_NOTE CLEAN) == 0);
    T(fsp_verify_clean("\nCaution: Partition 3 doesn't begin on a 2048-sector boundary. This may\nresult in degraded performance on some modern (2009 and later) hard disks.\n" CLEAN) != 0);
    T(fsp_verify_clean(END_NOTE) != 0);                                 /* a note alone proves nothing */
    T(fsp_verify_clean(END_NOTE "Identified 1 problems!\n") != 0);
    T(fsp_verify_clean(CLEAN END_NOTE) != 0);                           /* after the verdict: not the known shape */
    T(fsp_verify_clean("\nCaution: Partition 2 doesn't end on a 2048-sector boundary. This may\nresult in something else.\n" CLEAN) != 0);
    T(fsp_verify_clean("\nCaution: Partition 2 overlaps partition 3 boundary. This may\nresult in problems with some disk encryption tools.\n" CLEAN) != 0);
    T(fsp_verify_clean("\nCaution: Partition x doesn't end on a 2048-sector boundary. This may\nresult in problems with some disk encryption tools.\n" CLEAN) != 0);
}

static FspTable table(uint64_t last, unsigned lsec) {
    FspTable t;
    memset(&t, 0, sizeof t);
    t.lsec = lsec;
    t.max_entries = 128;
    t.first = lsec == 512 ? 34 : 6;
    t.last = last;
    t.total = last + 34;
    return t;
}
static void add(FspTable *t, unsigned num, uint64_t s, uint64_t e, int esp) {
    FspPart p = { num, s, e, esp };
    t->p[t->n++] = p;
}

static void region(void) {
    FspRegion r;
    FspTable t = table(209715166, 512);                 /* 100 GiB */
    add(&t, 1, 2048, 1050623, 1);
    add(&t, 2, 1050624, 41943039, 0);
    T(fsp_largest_free(&t, &r) == 0);
    T(r.start == 41943040 && r.end % 2048 == 2047 && r.end <= 209715166 && r.start % 2048 == 0);
    T(r.bytes == (r.end - r.start + 1) * 512);

    /* the largest gap wins, wherever it is, and table order doesn't matter */
    t = table(209715166, 512);
    add(&t, 2, 100000000, 209000000, 0);
    add(&t, 1, 2048, 4095, 1);
    T(fsp_largest_free(&t, &r) == 0 && r.start == 4096 && r.end == 99999743);

    /* unaligned neighbours: never touch them */
    t = table(4194270, 512);
    add(&t, 1, 34, 2100, 1);
    add(&t, 2, 3000000, 3000001, 0);
    T(fsp_largest_free(&t, &r) == 0 && r.start == 4096 && r.end == 2998271);
    T(r.start > 2100 && r.end < 3000000);

    /* 4K sectors: 1 MiB is 256 sectors */
    t = table(26214394, 4096);
    add(&t, 1, 256, 131327, 1);
    T(fsp_largest_free(&t, &r) == 0 && r.start == 131328 && r.start % 256 == 0 && r.end % 256 == 255);
    T(r.bytes == (r.end - r.start + 1) * 4096);

    /* a gap smaller than the alignment is no gap */
    t = table(2048 + 100, 512);
    add(&t, 1, 34, 2047, 1);
    T(fsp_largest_free(&t, &r) == 0 && r.bytes == 0);

    /* a full disk */
    t = table(4194270, 512);
    add(&t, 1, 34, 4194270, 1);
    T(fsp_largest_free(&t, &r) == 0 && r.bytes == 0);

    /* nonsense tables are not believed */
    t = table(4194270, 512);
    add(&t, 1, 2048, 100000, 1);
    add(&t, 2, 50000, 200000, 0);
    T(fsp_largest_free(&t, &r) != 0);                   /* overlap */
    t = table(4194270, 512);
    add(&t, 1, 2048, 100000, 1);
    add(&t, 2, 2048, 100000, 0);
    T(fsp_largest_free(&t, &r) != 0);                   /* the same partition twice */
    t = table(4194270, 512);
    add(&t, 1, 2048, 5000000, 1);
    T(fsp_largest_free(&t, &r) != 0);                   /* past the last usable sector */
    t = table(4194270, 512);
    add(&t, 1, 10, 5000, 1);
    T(fsp_largest_free(&t, &r) != 0);                   /* over the table itself */
    t = table(4194270, 512);
    t.lsec = 0;
    T(fsp_largest_free(&t, &r) != 0);

    t = table(4194270, 512);
    t.max_entries = 2;
    add(&t, 1, 2048, 4095, 1);
    add(&t, 2, 4096, 8191, 0);
    T(fsp_free_number(&t) == 0);
}

static FspTable qual;
static FspFacts facts(void) {
    qual = table(209715166, 512);
    add(&qual, 1, 2048, 1050623, 1);
    add(&qual, 2, 1050624, 41943039, 0);
    FspFacts f = { .uefi = 1, .boot_tool = 1, .mbr = FSP_MBR_PROTECTIVE, .table_ok = 1, .table = &qual, .esp_fat = 1, .esp_read = 1, .esp_free = 64ull << 20 };
    return f;
}
#define REFUSES(field, value, code) do { FspFacts f = facts(); f.field = value; FspChoice c; const char *w = fsp_decide(&f, &c); T(w && !strcmp(w, code)); } while (0)

static void decide(void) {
    FspFacts f = facts();
    FspChoice c;
    T(fsp_decide(&f, &c) == NULL && c.num == 3 && c.esp_num == 1 && c.region.start == 41943040);
    REFUSES(uefi, 0, "install.free.needUefi");
    REFUSES(busy, 1, "install.free.busy");
    REFUSES(mbr, FSP_MBR_DOS, "install.free.mbr");
    REFUSES(mbr, FSP_MBR_HYBRID, "install.free.hybridMbr");
    REFUSES(mbr, FSP_MBR_NONE, "install.free.noTable");
    REFUSES(table_ok, 0, "install.free.tableDamaged");
    REFUSES(esp_fat, 0, "install.free.espNotFat");
    REFUSES(esp_read, 0, "install.free.espUnreadable");
    REFUSES(esp_free, FSP_ESP_MIN_FREE - 1, "install.free.espFull");
    REFUSES(esp_has_ours, 1, "install.free.espHasLordOS");
    REFUSES(boot_tool, 0, "install.free.noBootTool");

    /* no ESP, two ESPs */
    f = facts(); qual.p[0].esp = 0;
    T(!strcmp(fsp_decide(&f, &c), "install.free.noEsp"));
    f = facts(); qual.p[1].esp = 1;
    T(!strcmp(fsp_decide(&f, &c), "install.free.manyEsps"));
    /* exactly the minimum passes, one MiB less doesn't */
    f = facts();
    qual.last = qual.p[1].end + (FSP_MIN_BYTES >> 9);
    T(fsp_decide(&f, &c) == NULL && c.region.bytes == FSP_MIN_BYTES);
    qual.last -= 2048;
    T(!strcmp(fsp_decide(&f, &c), "install.free.noRoom"));
    /* overlapping table */
    f = facts(); qual.p[1].start = 2048;
    T(!strcmp(fsp_decide(&f, &c), "install.free.tableDamaged"));
    /* full table */
    f = facts(); qual.max_entries = 2;
    T(!strcmp(fsp_decide(&f, &c), "install.free.tableFull"));
}

/* ---- raw GPT ---- */
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void reseal(uint8_t *h) { memset(h + 16, 0, 4); put32(h + 16, fsp_crc32(h, 92)); }

static void mkhdr(uint8_t *h, uint64_t my, uint64_t alt, uint64_t el, uint32_t ecrc) {
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x10000);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, 34);
    put64(h + 48, 1000000 - 34);
    memset(h + 56, 0x5a, 16);
    put64(h + 72, el);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, ecrc);
    reseal(h);
}

static void gpt(void) {
    static uint8_t e[128 * 128], e2[128 * 128], a[512], b[512];
    uint64_t total = 1000000;
    static const uint8_t type[16] = { 1, 2, 3 };
    memset(e, 0, sizeof e);
    memcpy(e, type, 16);
    put64(e + 32, 2048);
    put64(e + 40, 4095);
    uint32_t c = fsp_crc32(e, sizeof e);
    FspGpt g, g2, tmp;
    mkhdr(a, 1, total - 1, 2, c);
    mkhdr(b, total - 1, 1, total - 33, c);
    T(fsp_gpt_header(a, 512, total, 0, &g) == 0 && fsp_gpt_header(b, 512, total, 1, &g2) == 0);
    T(fsp_gpt_pair(&g, &g2, e, e) == 0);
    T(fsp_header_kept(&g, &g));
    FspTable t;
    T(fsp_raw_table(&g, 512, total, e, &t) == 0 && t.n == 1 && t.p[0].start == 2048 && !t.p[0].esp);

    /* every way a header can lie */
    T(fsp_gpt_header(a, 512, total, 1, &tmp) != 0);             /* primary where the backup should be */
    T(fsp_gpt_header(b, 512, total, 0, &tmp) != 0);
    T(fsp_gpt_header(a, 512, total + 1, 0, &tmp) != 0);         /* the backup isn't at the end of the disk */
    T(fsp_gpt_header(a, 1024, total, 0, &tmp) != 0);
    T(fsp_gpt_header(a, 512, 1ull << 60, 0, &tmp) != 0);
#define BREAK(stmt, ok_seal) do { static uint8_t h[512]; memcpy(h, a, 512); stmt; if (ok_seal) reseal(h); T(fsp_gpt_header(h, 512, total, 0, &tmp) != 0); } while (0)
    BREAK(h[0] = 'X', 1);
    BREAK(h[50] ^= 1, 0);                                        /* bad CRC */
    BREAK(put32(h + 12, 91), 0);
    BREAK(put32(h + 12, 4097), 0);
    BREAK(put32(h + 84, 64), 1);
    BREAK(put32(h + 80, 129), 1);
    BREAK(put32(h + 80, 0), 1);
    BREAK(put32(h + 80, 0xffffffffu), 1);
    BREAK(put64(h + 40, 0), 1);
    BREAK(put64(h + 40, 2000000), 1);
    BREAK(put64(h + 48, total), 1);
    BREAK(put64(h + 72, 0), 1);
    BREAK(put64(h + 72, 30), 1);                                 /* entries running into the usable area */

    /* the two copies disagree */
    memcpy(e2, e, sizeof e);
    e2[40] ^= 1;
    T(fsp_gpt_pair(&g, &g2, e, e2) != 0);
    T(fsp_gpt_pair(&g, &g2, e2, e) != 0);
    tmp = g2; tmp.guid[0] ^= 1;
    T(fsp_gpt_pair(&g, &tmp, e, e) != 0);
    T(!fsp_header_kept(&g, &tmp));
    tmp = g2; tmp.last--;
    T(fsp_gpt_pair(&g, &tmp, e, e) != 0);

    /* start after end */
    memcpy(e2, e, sizeof e);
    put64(e2 + 32, 5000);
    T(fsp_raw_table(&g, 512, total, e2, &t) != 0);

    /* what the text says and what the disk says */
    FspTable x, y;
    T(fsp_parse_print(good, &x) == 0);
    y = x;
    T(fsp_tables_agree(&x, &y));
    y.p[1].end--;
    T(!fsp_tables_agree(&x, &y));
    y = x; y.n--;
    T(!fsp_tables_agree(&x, &y));
    y = x; y.p[0].num = 2;
    T(!fsp_tables_agree(&x, &y));
    y = x; y.p[0].esp = 0;
    T(!fsp_tables_agree(&x, &y));
    y = x; y.last--;
    T(!fsp_tables_agree(&x, &y));

    /* after the write: only the one empty entry may change */
    static uint8_t after[128 * 128];
    static const uint8_t root[16] = { 9, 9, 9 };
    memcpy(after, e, sizeof e);
    memcpy(after + 128 * 2, root, 16);
    memset(after + 128 * 2 + 16, 7, 16);
    put64(after + 128 * 2 + 32, 8192);
    put64(after + 128 * 2 + 40, 16383);
    T(fsp_entries_changed_only(e, after, 128, 2, 8192, 16383, root) == 0);
    T(fsp_entries_changed_only(e, after, 128, 2, 8192, 16384, root) != 0);      /* another end */
    T(fsp_entries_changed_only(e, after, 128, 2, 8192, 16383, type) != 0);      /* another type */
    T(fsp_entries_changed_only(e, after, 128, 3, 8192, 16383, root) != 0);      /* another slot */
    T(fsp_entries_changed_only(e, after, 128, 128, 8192, 16383, root) != 0);
    T(fsp_entries_changed_only(e, e, 128, 2, 8192, 16383, root) != 0);          /* nothing was added */
    T(fsp_entries_changed_only(after, after, 128, 2, 8192, 16383, root) != 0);  /* the slot wasn't empty */
    after[33] ^= 1;
    T(fsp_entries_changed_only(e, after, 128, 2, 8192, 16383, root) != 0);      /* an existing entry moved */
    after[33] ^= 1;
    after[127] ^= 1;
    T(fsp_entries_changed_only(e, after, 128, 2, 8192, 16383, root) != 0);      /* its name changed */
}

static void mbr(void) {
    static uint8_t s[512], o[512];
    T(fsp_mbr_kind(s) == FSP_MBR_NONE);
    s[510] = 0x55; s[511] = 0xaa;
    T(fsp_mbr_kind(s) == FSP_MBR_NONE);
    s[446 + 4] = 0xee;
    T(fsp_mbr_kind(s) == FSP_MBR_PROTECTIVE);
    T(!strcmp(fsp_mbr_name(fsp_mbr_kind(s)), "gpt"));
    memcpy(o, s, 512);
    T(fsp_mbr_same(s, o));
    o[100] ^= 1;
    T(!fsp_mbr_same(s, o));                     /* the boot code changed */
    o[100] ^= 1;
    o[511] = 0;
    T(!fsp_mbr_same(s, o));
    o[511] = 0xaa;
    s[446 + 16 + 4] = 0x83;
    T(fsp_mbr_kind(s) == FSP_MBR_HYBRID);
    T(!fsp_mbr_same(s, s));
    s[446 + 4] = 0x07;
    T(fsp_mbr_kind(s) == FSP_MBR_DOS);
    s[446 + 4] = 0xee; s[446 + 16 + 4] = 0xee;
    T(fsp_mbr_kind(s) == FSP_MBR_HYBRID);
    s[511] = 0;
    T(fsp_mbr_kind(s) == FSP_MBR_NONE);
}

static void small(void) {
    T(fsp_mode("") == 0 && fsp_mode("erase") == 0 && fsp_mode("free") == 1);
    T(fsp_mode("FREE") < 0 && fsp_mode("free ") < 0 && fsp_mode("erase;") < 0 && fsp_mode("x") < 0);
    char p[48];
    fsp_part_path("sda", 3, p, sizeof p);
    T(!strcmp(p, "/dev/sda3"));
    fsp_part_path("nvme0n1", 3, p, sizeof p);
    T(!strcmp(p, "/dev/nvme0n1p3"));
    fsp_part_path("mmcblk0", 12, p, sizeof p);
    T(!strcmp(p, "/dev/mmcblk0p12"));
    T(fsp_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
}

int main(void) {
    parse_ok();
    parse_hostile();
    verify();
    region();
    decide();
    gpt();
    mbr();
    small();
    printf("freespace-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
