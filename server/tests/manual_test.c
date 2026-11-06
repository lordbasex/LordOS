/* manual_test.c — manual partitioning's pure decisions (src/install/manual.c): the plan's text
 * fields, every rule a plan has to pass, the numbers new partitions get, sgdisk's exact arguments and
 * the proof of the table afterwards. Nothing here touches a disk. */
#include "install/manual.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define T(c) do { checks++; if (!(c)) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)
#define WHY(f, pl, code) do { const char *w_ = mp_decide(f, pl); checks++; \
        if (!(code ? w_ && !strcmp(w_, code) : !w_)) { fails++; printf("FAILED line %d: wanted %s, got %s\n", __LINE__, code ? code : "OK", w_ ? w_ : "OK"); } } while (0)

#define GIB (2097152ull)        /* sectors of 512 bytes */
#define MIB (2048ull)

/* A 64 GiB disk: 1 EFI (100 MiB), 3 other system (20 GiB, ntfs), 4 Linux (10 GiB, ext4), the rest free. */
static FspTable table;
static MpFacts facts;

static void setup(void) {
    memset(&table, 0, sizeof table);
    table.lsec = 512;
    table.max_entries = 128;
    table.total = 64 * GIB;
    table.first = 34;
    table.last = 64 * GIB - 34;
    table.p[0] = (FspPart){ 1, MIB, MIB + 100 * MIB - 1, 1 };
    table.p[1] = (FspPart){ 3, 101 * MIB, 101 * MIB + 20 * GIB - 1, 0 };
    table.p[2] = (FspPart){ 4, 101 * MIB + 20 * GIB, 101 * MIB + 30 * GIB - 1, 0 };
    table.n = 3;
    memset(&facts, 0, sizeof facts);
    facts.uefi = facts.boot_tool = 1;
    facts.mbr = FSP_MBR_PROTECTIVE;
    facts.table_ok = 1;
    facts.table = &table;
    strcpy(facts.fstype[1], "vfat");
    strcpy(facts.fstype[3], "ntfs");
    strcpy(facts.fstype[4], "ext4");
    facts.esp_fat = facts.esp_read = 1;
    facts.esp_free = 60ull << 20;
}

/* free space starts after partition 4 */
static const uint64_t FREE0 = 101 * MIB + 30 * GIB;

static void plan(MpPlan *p) { memset(p, 0, sizeof *p); }

static void parse(void) {
    static MpPlan p;
    plan(&p);
    T(mp_parse_delete("", &p) == 0 && p.ndel == 0);
    T(mp_parse_delete("3", &p) == 0 && p.ndel == 1 && p.del[0] == 3);
    T(mp_parse_delete("3,4,128", &p) == 0 && p.ndel == 3 && p.del[2] == 128);
    const char *bad_del[] = { "0", "129", "3,3", "3,", ",3", " 3", "3 ", "03", "a", "3;4", "-3", "+3", "3,,4", "1000", "99999999999999999999", NULL };
    for (int i = 0; bad_del[i]; i++) { T(mp_parse_delete(bad_del[i], &p) != 0); }
    char longlist[MP_LIST_MAX + 8];
    memset(longlist, '1', sizeof longlist - 1);
    longlist[sizeof longlist - 1] = 0;
    T(mp_parse_delete(longlist, &p) != 0);

    T(mp_parse_create("", &p) == 0 && p.nadd == 0);
    T(mp_parse_create("root:2048:16779263", &p) == 0 && p.nadd == 1 && p.add[0].role == MP_ROOT && p.add[0].start == 2048 && p.add[0].end == 16779263);
    T(mp_parse_create("root:2048:4095,home:4096:8191,swap:8192:9215,esp:9216:10239", &p) == 0 && p.nadd == 4 && p.add[3].role == MP_ESP);
    const char *bad_add[] = { "root", "root:", "root:1", "root:1:", "root:2:1", "root:1:2,root:3:4", "boot:1:2", "ROOT:1:2", "root:01:2",
        "root:1:2:3", "root:1:2,", ",root:1:2", "root:-1:2", "root: 1:2", "root:1:2;home:3:4", "root:1:2,home:3:4,swap:5:6,esp:7:8,root:9:10",
        "root:1:99999999999999999999", "root:1:281474976710657", "verylongrole:1:2", NULL };
    for (int i = 0; bad_add[i]; i++) { T(mp_parse_create(bad_add[i], &p) != 0); }

    T(mp_role("root") == MP_ROOT && mp_role("esp") == MP_ESP && mp_role("") < 0 && mp_role("efi") < 0);
}

static void disk(void) {
    static MpPlan p;
    plan(&p);
    p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1;
    setup(); WHY(&facts, &p, NULL);
    setup(); facts.uefi = 0; WHY(&facts, &p, "install.manual.needUefi");
    setup(); facts.boot_tool = 0; WHY(&facts, &p, "install.manual.noBootTool");
    setup(); facts.busy = 1; WHY(&facts, &p, "install.manual.busy");
    setup(); facts.mbr = FSP_MBR_DOS; WHY(&facts, &p, "install.manual.mbr");
    setup(); facts.mbr = FSP_MBR_HYBRID; WHY(&facts, &p, "install.manual.hybridMbr");
    setup(); facts.mbr = FSP_MBR_NONE; WHY(&facts, &p, "install.manual.noTable");
    setup(); facts.table_ok = 0; WHY(&facts, &p, "install.manual.tableDamaged");
    setup(); facts.table = NULL; WHY(&facts, &p, "install.manual.tableDamaged");
    setup(); T(mp_disk_why(&facts) == NULL);
}

static void roles(void) {
    static MpPlan p;
    /* reinstall: format partition 4 as /, keep the EFI partition */
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1;
    WHY(&facts, &p, NULL);
    T(p.slot[MP_ROOT].num == 4 && p.slot[MP_ROOT].format && !p.slot[MP_ESP].format && !p.slot[MP_HOME].num && !mp_changes_table(&p));
    /* nothing chosen */
    setup(); plan(&p); p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.noRoot");
    setup(); plan(&p); p.part[MP_ROOT] = 4; WHY(&facts, &p, "install.manual.noEsp");
    /* partitions that aren't there, or are being deleted */
    setup(); plan(&p); p.part[MP_ROOT] = 2; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.noSuchPartition");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.del[0] = 2; p.ndel = 1; WHY(&facts, &p, "install.manual.noSuchPartition");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.del[0] = 4; p.ndel = 1; WHY(&facts, &p, "install.manual.deletedInUse");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.del[0] = 1; p.ndel = 1; WHY(&facts, &p, "install.manual.deletedInUse");
    /* one role per partition, one partition per role */
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_HOME] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.usedTwice");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_SWAP] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.usedTwice");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1;
    mp_parse_create("root:" "68786585600:68803362815", &p);    /* also a new root */
    p.add[0].start = FREE0; p.add[0].end = FREE0 + 10 * GIB - 1;
    WHY(&facts, &p, "install.manual.usedTwice");
    /* the EFI partition is only for the EFI role, and that role only takes an EFI partition */
    setup(); plan(&p); p.part[MP_ROOT] = 1; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espForData");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 3; WHY(&facts, &p, "install.manual.notEsp");
    setup(); table.p[0].esp = 1; table.p[2].esp = 1; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espForData");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_SWAP] = 1; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espForData");
    /* / at least 8 GiB */
    setup(); table.p[2].end = table.p[2].start + 8 * GIB - 2; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.rootTooSmall");
    setup(); table.p[2].end = table.p[2].start + 8 * GIB - 1; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, NULL);
    /* a kept /home needs a file system Linux keeps a home on; formatted, anything goes */
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_HOME] = 3; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.homeUnknownFs");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_HOME] = 3; p.part[MP_ESP] = 1; p.home_format = 1; WHY(&facts, &p, NULL);
    T(p.slot[MP_HOME].format);
    setup(); strcpy(facts.fstype[3], "btrfs"); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_HOME] = 3; p.part[MP_ESP] = 1; WHY(&facts, &p, NULL);
    T(!p.slot[MP_HOME].format);
    setup(); facts.fstype[3][0] = 0; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_HOME] = 3; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.homeUnknownFs");
    /* a kept EFI partition has to be FAT, readable and have room; a formatted one doesn't */
    setup(); facts.esp_fat = 0; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espNotFat");
    setup(); facts.esp_read = 0; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espUnreadable");
    setup(); facts.esp_free = FSP_ESP_MIN_FREE - 1; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, "install.manual.espFull");
    setup(); facts.esp_fat = facts.esp_read = 0; facts.esp_free = 0; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.esp_format = 1; WHY(&facts, &p, NULL);
    T(p.slot[MP_ESP].format);
    /* an earlier LordOS's folder on it doesn't stop a reinstall */
    setup(); facts.esp_has_ours = 1; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; WHY(&facts, &p, NULL);
    setup(); table.p[0].end = table.p[0].start + 64 * MIB - 2; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.esp_format = 1; WHY(&facts, &p, "install.manual.espTooSmall");
    /* swap: a partition or a file, not both; a sensible size */
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.part[MP_SWAP] = 3; WHY(&facts, &p, NULL);
    T(p.slot[MP_SWAP].format);
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.part[MP_SWAP] = 3; p.swap_file = 2048; WHY(&facts, &p, "install.manual.twoSwaps");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.swap_file = 2048; WHY(&facts, &p, NULL);
    setup(); table.p[1].end = table.p[1].start + 255 * MIB - 1; plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1; p.part[MP_SWAP] = 3; WHY(&facts, &p, "install.manual.swapSize");
}

static void geometry(void) {
    static MpPlan p;
    char s[200];
    /* a new / in free space beside the other systems, keeping the EFI partition */
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 20 * GIB - 1));
    T(mp_parse_create(s, &p) == 0);
    WHY(&facts, &p, NULL);
    T(p.add[0].num == 2 && p.slot[MP_ROOT].num == 2 && p.slot[MP_ROOT].is_new && mp_changes_table(&p));
    /* misaligned, outside, overlapping */
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)FREE0 + 1, (unsigned long long)(FREE0 + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.notAligned");
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 20 * GIB - 2)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.notAligned");
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)(64 * GIB - 20 * GIB), (unsigned long long)(64 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.outside");
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)(FREE0 - MIB), (unsigned long long)(FREE0 + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.overlap");
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu,home:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 20 * GIB - 1),
             (unsigned long long)(FREE0 + 10 * GIB), (unsigned long long)(FREE0 + 30 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.overlap");
    /* over a partition that is deleted, it is allowed; that number stays empty */
    setup(); plan(&p); p.part[MP_ESP] = 1; mp_parse_delete("3", &p);
    snprintf(s, sizeof s, "root:%llu:%llu,swap:%llu:%llu", (unsigned long long)(101 * MIB), (unsigned long long)(101 * MIB + 18 * GIB - 1),
             (unsigned long long)(101 * MIB + 18 * GIB), (unsigned long long)(101 * MIB + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, NULL);
    T(p.add[0].num == 2 && p.add[1].num == 5 && p.slot[MP_SWAP].num == 5);
    /* sizes of new partitions */
    setup(); plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 8 * GIB - MIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.rootTooSmall");
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "home:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 1023 * MIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.homeTooSmall");
    setup(); plan(&p); p.part[MP_ROOT] = 4;
    snprintf(s, sizeof s, "esp:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 99 * MIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.espTooSmall");
    setup(); plan(&p); p.part[MP_ROOT] = 4;
    snprintf(s, sizeof s, "esp:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 512 * MIB - 1)); mp_parse_create(s, &p);
    facts.esp_fat = 0;                         /* a new one is formatted: what was there doesn't matter */
    WHY(&facts, &p, NULL);
    T(p.slot[MP_ESP].is_new && p.slot[MP_ESP].format && p.slot[MP_ESP].num == 2);
    setup(); plan(&p); p.part[MP_ROOT] = 4; p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "swap:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 65 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.outside");
    /* a full table */
    setup(); table.max_entries = 4; table.p[3] = (FspPart){ 2, 34, 2047, 0 }; table.n = 4;
    plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", (unsigned long long)FREE0, (unsigned long long)(FREE0 + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, "install.manual.tableFull");
    /* a 4 KiB sector disk aligns on 256 sectors */
    setup(); table.lsec = 4096; table.last = 8 * GIB - 6; table.first = 6;
    table.p[0] = (FspPart){ 1, 256, 256 + 25600 - 1, 1 }; table.n = 1;
    plan(&p); p.part[MP_ESP] = 1;
    snprintf(s, sizeof s, "root:%llu:%llu", 256ull * 101, 256ull * 101 + 2621440 - 1); mp_parse_create(s, &p);
    WHY(&facts, &p, NULL);
}

static void argv_built(void) {
    static MpPlan p;
    char s[200], buf[4096];
    const char *argv[MP_ARGV_MAX];
    setup(); plan(&p); p.part[MP_ESP] = 1; mp_parse_delete("3", &p);
    snprintf(s, sizeof s, "root:%llu:%llu,swap:%llu:%llu", (unsigned long long)(101 * MIB), (unsigned long long)(101 * MIB + 18 * GIB - 1),
             (unsigned long long)(101 * MIB + 18 * GIB), (unsigned long long)(101 * MIB + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, NULL);
    int n = mp_sgdisk_argv(&p, "/usr/sbin/sgdisk", "/dev/sda", "8304", buf, sizeof buf, argv);
    static const char *const want[] = { "/usr/sbin/sgdisk", "-d", "3", "-n", "2:206848:37955583", "-t", "2:8304", "-c", "2:LordOS",
                                        "-n", "5:37955584:42149887", "-t", "5:8200", "-c", "5:swap", "/dev/sda", NULL };
    T(n == 16);
    for (int i = 0; want[i] && i < n; i++) if (strcmp(argv[i], want[i])) { fails++; printf("FAILED argv[%d]: %s, wanted %s\n", i, argv[i], want[i]); }
    T(argv[n] == NULL);
    /* it never overflows its buffer */
    T(mp_sgdisk_argv(&p, "/usr/sbin/sgdisk", "/dev/sda", "8304", buf, 40, argv) < 0);
    /* a plan with numbers not decided yet builds nothing */
    plan(&p); mp_parse_create("root:2048:4095", &p);
    T(mp_sgdisk_argv(&p, "/usr/sbin/sgdisk", "/dev/sda", "8304", buf, sizeof buf, argv) < 0);
}

static const uint8_t root_type[16] = { 0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d, 0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09 };
static const uint8_t swap_type[16] = { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43, 0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f };

static void put_entry(uint8_t *e, const uint8_t *type, uint8_t guidbyte, uint64_t start, uint64_t end) {
    memset(e, 0, FSP_ENTRY_SIZE);
    memcpy(e, type, 16);
    memset(e + 16, guidbyte, 16);
    for (int i = 0; i < 8; i++) { e[32 + i] = (uint8_t)(start >> (8 * i)); e[40 + i] = (uint8_t)(end >> (8 * i)); }
    e[56] = 'x';
}

static void proof(void) {
    static MpPlan p;
    static uint8_t before[128 * FSP_ENTRY_SIZE], after[128 * FSP_ENTRY_SIZE];
    char s[200];
    setup(); plan(&p); p.part[MP_ESP] = 1; mp_parse_delete("3", &p);
    snprintf(s, sizeof s, "root:%llu:%llu,swap:%llu:%llu", (unsigned long long)(101 * MIB), (unsigned long long)(101 * MIB + 18 * GIB - 1),
             (unsigned long long)(101 * MIB + 18 * GIB), (unsigned long long)(101 * MIB + 20 * GIB - 1)); mp_parse_create(s, &p);
    WHY(&facts, &p, NULL);
    memset(before, 0, sizeof before);
    put_entry(before + 0 * FSP_ENTRY_SIZE, root_type, 1, table.p[0].start, table.p[0].end);
    put_entry(before + 2 * FSP_ENTRY_SIZE, root_type, 3, table.p[1].start, table.p[1].end);
    put_entry(before + 3 * FSP_ENTRY_SIZE, root_type, 4, table.p[2].start, table.p[2].end);
    memcpy(after, before, sizeof after);
    memset(after + 2 * FSP_ENTRY_SIZE, 0, FSP_ENTRY_SIZE);
    put_entry(after + 1 * FSP_ENTRY_SIZE, root_type, 0x22, p.add[0].start, p.add[0].end);
    put_entry(after + 4 * FSP_ENTRY_SIZE, swap_type, 0x55, p.add[1].start, p.add[1].end);
    T(mp_entries_proven(before, after, 128, &p, root_type) == 0);
    /* any other byte, anywhere */
    after[3 * FSP_ENTRY_SIZE + 60] ^= 1; T(mp_entries_proven(before, after, 128, &p, root_type) != 0); after[3 * FSP_ENTRY_SIZE + 60] ^= 1;
    after[100 * FSP_ENTRY_SIZE + 3] = 1; T(mp_entries_proven(before, after, 128, &p, root_type) != 0); after[100 * FSP_ENTRY_SIZE + 3] = 0;
    /* the deleted entry not empty */
    after[2 * FSP_ENTRY_SIZE + 70] = 1; T(mp_entries_proven(before, after, 128, &p, root_type) != 0); after[2 * FSP_ENTRY_SIZE + 70] = 0;
    /* a new one elsewhere, of another type, or without its own GUID */
    after[1 * FSP_ENTRY_SIZE + 32] ^= 1; T(mp_entries_proven(before, after, 128, &p, root_type) != 0); after[1 * FSP_ENTRY_SIZE + 32] ^= 1;
    after[4 * FSP_ENTRY_SIZE + 0] ^= 1; T(mp_entries_proven(before, after, 128, &p, root_type) != 0); after[4 * FSP_ENTRY_SIZE + 0] ^= 1;
    memset(after + 4 * FSP_ENTRY_SIZE + 16, 0, 16); T(mp_entries_proven(before, after, 128, &p, root_type) != 0);
    memset(after + 4 * FSP_ENTRY_SIZE + 16, 0x55, 16);
    T(mp_entries_proven(before, after, 128, &p, root_type) == 0);
    /* a deleted partition that was empty before isn't a deletion */
    memset(before + 2 * FSP_ENTRY_SIZE, 0, FSP_ENTRY_SIZE); T(mp_entries_proven(before, after, 128, &p, root_type) != 0);
    /* nonsense counts */
    T(mp_entries_proven(before, after, 0, &p, root_type) != 0);
    T(mp_entries_proven(before, after, 4, &p, root_type) != 0);       /* number 5 is outside 4 entries */
}

static void small(void) {
    FspGpt g;
    char tag[64];
    memset(&g, 0, sizeof g);
    for (int i = 0; i < 16; i++) g.guid[i] = (uint8_t)(i * 17);
    g.entries_crc = 0xdeadbeef;
    mp_table_tag(&g, tag, sizeof tag);
    T(!strcmp(tag, "00112233445566778899aabbccddeeff-deadbeef"));
    T(mp_home_fs_ok("ext4") && mp_home_fs_ok("xfs") && mp_home_fs_ok("btrfs") && !mp_home_fs_ok("ntfs") && !mp_home_fs_ok("vfat") && !mp_home_fs_ok(""));
    T(mp_fsck_pass("ext4") == 2 && mp_fsck_pass("btrfs") == 0 && mp_fsck_pass("xfs") == 0);
    T(mp_loader_name_ok("debian") && mp_loader_name_ok("Microsoft") && mp_loader_name_ok("a.b_c-d"));
    T(!mp_loader_name_ok("") && !mp_loader_name_ok(".") && !mp_loader_name_ok("..") && !mp_loader_name_ok("a b") && !mp_loader_name_ok("a\"b")
      && !mp_loader_name_ok("a/b") && !mp_loader_name_ok("\x1b[31m") && !mp_loader_name_ok("123456789012345678901234567890123"));
    T(fsp_mode("manual") == 2 && fsp_mode("Manual") < 0 && fsp_mode("manual ") < 0);
}

int main(void) {
    parse();
    disk();
    roles();
    geometry();
    argv_built();
    proof();
    small();
    printf("manual-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
