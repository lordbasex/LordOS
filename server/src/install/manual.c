/* manual.c — see manual.h. No I/O, nothing here can touch a disk. */
#include "manual.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const role_names[MP_ROLES] = { "root", "home", "swap", "esp" };
/* sgdisk's codes and names of the new partitions; root's code is the architecture's (install.c) */
static const char *const role_codes[MP_ROLES] = { NULL, "8302", "8200", "ef00" };
static const char *const role_labels[MP_ROLES] = { "LordOS", "home", "swap", "EFI" };
/* the same types as they lie on disk: Linux /home, Linux swap, EFI system */
static const uint8_t home_guid[16] = { 0xe1, 0xc7, 0x3a, 0x93, 0xb4, 0x2e, 0x13, 0x4f, 0xb8, 0x44, 0x0e, 0x14, 0xe2, 0xae, 0xf9, 0x15 };
static const uint8_t swap_guid[16] = { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43, 0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f };
static const uint8_t esp_guid[16]  = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

int mp_role(const char *s) {
    for (int r = 0; r < MP_ROLES; r++) if (!strcmp(s, role_names[r])) return r;
    return -1;
}

const char *mp_role_name(MpRole r) { return r < MP_ROLES ? role_names[r] : "?"; }

/* A decimal number of at most `digits` digits, no sign, no spaces, no leading zero. */
static int number(const char *s, const char **end, int digits, uint64_t *out) {
    if (!isdigit((unsigned char)*s) || (s[0] == '0' && isdigit((unsigned char)s[1]))) return -1;
    char *e;
    errno = 0;
    unsigned long long v = strtoull(s, &e, 10);
    if (errno || e - s > digits) return -1;
    *out = v;
    *end = e;
    return 0;
}

int mp_parse_delete(const char *s, MpPlan *pl) {
    pl->ndel = 0;
    if (strlen(s) > MP_LIST_MAX) return -1;
    if (!*s) return 0;
    for (;;) {
        uint64_t n;
        if (number(s, &s, 3, &n) || n < 1 || n > FSP_MAX_PARTS) return -1;
        for (int i = 0; i < pl->ndel; i++) if (pl->del[i] == n) return -1;
        pl->del[pl->ndel++] = (unsigned)n;
        if (!*s) return 0;
        if (*s++ != ',' || pl->ndel >= FSP_MAX_PARTS) return -1;
    }
}

int mp_parse_create(const char *s, MpPlan *pl) {
    pl->nadd = 0;
    if (strlen(s) > MP_LIST_MAX) return -1;
    if (!*s) return 0;
    for (;;) {
        char name[8];
        size_t l = strcspn(s, ":");
        if (!s[l] || l >= sizeof name) return -1;
        memcpy(name, s, l);
        name[l] = 0;
        int r = mp_role(name);
        if (r < 0) return -1;
        for (int i = 0; i < pl->nadd; i++) if ((int)pl->add[i].role == r) return -1;
        uint64_t a, b;
        s += l + 1;
        if (number(s, &s, 19, &a) || *s++ != ':' || number(s, &s, 19, &b) || a > b || b > FSP_MAX_TOTAL) return -1;
        pl->add[pl->nadd].role = (MpRole)r;
        pl->add[pl->nadd].start = a;
        pl->add[pl->nadd].end = b;
        pl->add[pl->nadd].num = 0;
        pl->nadd++;
        if (!*s) return 0;
        if (*s++ != ',' || pl->nadd >= MP_ROLES) return -1;
    }
}

const char *mp_disk_why(const MpFacts *f) {
    if (!f->uefi) return "install.manual.needUefi";
    if (!f->boot_tool) return "install.manual.noBootTool";
    if (f->busy) return "install.manual.busy";
    if (f->mbr == FSP_MBR_DOS) return "install.manual.mbr";
    if (f->mbr == FSP_MBR_HYBRID) return "install.manual.hybridMbr";
    if (f->mbr != FSP_MBR_PROTECTIVE) return "install.manual.noTable";
    if (!f->table_ok || !f->table) return "install.manual.tableDamaged";
    return NULL;
}

static const FspPart *by_num(const FspTable *t, unsigned num) {
    for (int i = 0; i < t->n; i++) if (t->p[i].num == num) return &t->p[i];
    return NULL;
}

static int deleted(const MpPlan *pl, unsigned num) {
    for (int i = 0; i < pl->ndel; i++) if (pl->del[i] == num) return 1;
    return 0;
}

static int overlaps(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return a <= d && c <= b; }

const char *mp_decide(const MpFacts *f, MpPlan *pl) {
    memset(pl->slot, 0, sizeof pl->slot);
    const char *why = mp_disk_why(f);
    if (why) return why;
    const FspTable *t = f->table;
    if (t->n < 0 || t->n > FSP_MAX_PARTS || (t->lsec != 512 && t->lsec != 4096) || t->first > t->last) return "install.manual.tableDamaged";
    if (pl->ndel < 0 || pl->ndel > FSP_MAX_PARTS || pl->nadd < 0 || pl->nadd > MP_ROLES) return "install.badPlan";

    for (int i = 0; i < pl->ndel; i++) if (!by_num(t, pl->del[i])) return "install.manual.noSuchPartition";

    /* existing partitions chosen for a role: there, kept in the table, one role each, of the right kind */
    for (int r = 0; r < MP_ROLES; r++) {
        unsigned n = pl->part[r];
        if (!n) continue;
        const FspPart *p = by_num(t, n);
        if (!p) return "install.manual.noSuchPartition";
        if (deleted(pl, n)) return "install.manual.deletedInUse";
        for (int q = 0; q < r; q++) if (pl->part[q] == n) return "install.manual.usedTwice";
        if (r == MP_ESP && !p->esp) return "install.manual.notEsp";
        if (r != MP_ESP && p->esp) return "install.manual.espForData";
        pl->slot[r].num = n;
        pl->slot[r].start = p->start;
        pl->slot[r].end = p->end;
    }
    for (int i = 0; i < pl->nadd; i++) {
        MpRole r = pl->add[i].role;
        if (r >= MP_ROLES) return "install.badPlan";
        if (pl->part[r] || pl->slot[r].is_new) return "install.manual.usedTwice";
        pl->slot[r].is_new = 1;
        pl->slot[r].start = pl->add[i].start;
        pl->slot[r].end = pl->add[i].end;
    }
    if (!pl->slot[MP_ROOT].num && !pl->slot[MP_ROOT].is_new) return "install.manual.noRoot";
    if (!pl->slot[MP_ESP].num && !pl->slot[MP_ESP].is_new) return "install.manual.noEsp";
    if ((pl->slot[MP_SWAP].num || pl->slot[MP_SWAP].is_new) && pl->swap_file > 0) return "install.manual.twoSwaps";

    /* the new partitions: aligned, inside the usable area, over nothing that stays or is also new */
    uint64_t align = FSP_ALIGN_BYTES / t->lsec;
    for (int i = 0; i < pl->nadd; i++) {
        uint64_t s = pl->add[i].start, e = pl->add[i].end;
        if (s % align || (e + 1) % align) return "install.manual.notAligned";
        if (s < t->first || e > t->last || s > e) return "install.manual.outside";
        for (int k = 0; k < t->n; k++)
            if (!deleted(pl, t->p[k].num) && overlaps(s, e, t->p[k].start, t->p[k].end)) return "install.manual.overlap";
        for (int j = 0; j < i; j++) if (overlaps(s, e, pl->add[j].start, pl->add[j].end)) return "install.manual.overlap";
    }
    /* their numbers: the lowest the table doesn't use. A deleted number is left empty, so the proof
     * afterwards never has to tell a deleted entry from a new one */
    unsigned next = 1;
    for (int i = 0; i < pl->nadd; i++) {
        while (next <= t->max_entries && next <= FSP_MAX_PARTS && (by_num(t, next) || deleted(pl, next))) next++;
        if (next > t->max_entries || next > FSP_MAX_PARTS) return "install.manual.tableFull";
        pl->add[i].num = next;
        pl->slot[pl->add[i].role].num = next;
        next++;
    }

    /* sizes */
    uint64_t sz[MP_ROLES];
    for (int r = 0; r < MP_ROLES; r++) sz[r] = pl->slot[r].num ? (pl->slot[r].end - pl->slot[r].start + 1) * t->lsec : 0;
    if (sz[MP_ROOT] < MP_ROOT_MIN) return "install.manual.rootTooSmall";
    if (pl->slot[MP_HOME].is_new && sz[MP_HOME] < MP_HOME_MIN) return "install.manual.homeTooSmall";
    if (pl->slot[MP_SWAP].num && (sz[MP_SWAP] < MP_SWAP_MIN || sz[MP_SWAP] > MP_SWAP_MAX)) return "install.manual.swapSize";
    if (pl->slot[MP_ESP].is_new && sz[MP_ESP] < MP_ESP_NEW_MIN) return "install.manual.espTooSmall";

    /* what gets a new file system */
    pl->slot[MP_ROOT].format = 1;
    pl->slot[MP_SWAP].format = pl->slot[MP_SWAP].num != 0;
    pl->slot[MP_HOME].format = pl->slot[MP_HOME].num && (pl->slot[MP_HOME].is_new || pl->home_format);
    pl->slot[MP_ESP].format = pl->slot[MP_ESP].is_new || pl->esp_format;
    if (!pl->slot[MP_ESP].is_new && pl->esp_format && sz[MP_ESP] < MP_ESP_FMT_MIN) return "install.manual.espTooSmall";

    if (pl->slot[MP_HOME].num && !pl->slot[MP_HOME].format && !mp_home_fs_ok(f->fstype[pl->slot[MP_HOME].num])) return "install.manual.homeUnknownFs";
    if (!pl->slot[MP_ESP].format) {
        if (!f->esp_fat) return "install.manual.espNotFat";
        if (!f->esp_read) return "install.manual.espUnreadable";
        if (f->esp_free < FSP_ESP_MIN_FREE) return "install.manual.espFull";
    }
    return NULL;
}

int mp_changes_table(const MpPlan *pl) { return pl->ndel > 0 || pl->nadd > 0; }

int mp_sgdisk_argv(const MpPlan *pl, const char *sgdisk, const char *dev, const char *root_code,
                   char *buf, size_t n, const char *argv[MP_ARGV_MAX]) {
    int c = 0;
    size_t used = 0;
#define PUT(...) do { \
        if (c >= MP_ARGV_MAX - 2 || used >= n) return -1; \
        int w_ = snprintf(buf + used, n - used, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= n - used) return -1; \
        argv[c++] = buf + used; used += (size_t)w_ + 1; \
    } while (0)
    PUT("%s", sgdisk);
    for (int i = 0; i < pl->ndel; i++) { PUT("-d"); PUT("%u", pl->del[i]); }
    for (int i = 0; i < pl->nadd; i++) {
        MpRole r = pl->add[i].role;
        unsigned k = pl->add[i].num;
        if (!k || r >= MP_ROLES) return -1;
        PUT("-n"); PUT("%u:%llu:%llu", k, (unsigned long long)pl->add[i].start, (unsigned long long)pl->add[i].end);
        PUT("-t"); PUT("%u:%s", k, r == MP_ROOT ? root_code : role_codes[r]);
        PUT("-c"); PUT("%u:%s", k, role_labels[r]);
    }
    PUT("%s", dev);
#undef PUT
    argv[c] = NULL;
    return c;
}

int mp_entries_proven(const uint8_t *before, const uint8_t *after, uint32_t count, const MpPlan *pl,
                      const uint8_t *root_type) {
    static const uint8_t empty[FSP_ENTRY_SIZE];
    if (!count || count > FSP_MAX_PARTS) return -1;
    for (int i = 0; i < pl->ndel; i++) if (pl->del[i] < 1 || pl->del[i] > count) return -1;
    for (int i = 0; i < pl->nadd; i++) if (pl->add[i].num < 1 || pl->add[i].num > count) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = before + (size_t)i * FSP_ENTRY_SIZE, *b = after + (size_t)i * FSP_ENTRY_SIZE;
        unsigned num = i + 1;
        int k = -1;
        for (int j = 0; j < pl->nadd; j++) if (pl->add[j].num == num) k = j;
        if (k >= 0) {
            MpRole r = pl->add[k].role;
            const uint8_t *type = r == MP_ROOT ? root_type : r == MP_HOME ? home_guid : r == MP_SWAP ? swap_guid : esp_guid;
            if (memcmp(a, empty, FSP_ENTRY_SIZE) || deleted(pl, num)) return -1;
            if (memcmp(b, type, 16) || rd64(b + 32) != pl->add[k].start || rd64(b + 40) != pl->add[k].end) return -1;
            if (!memcmp(b + 16, empty, 16)) return -1;          /* a partition without its own GUID */
        } else if (deleted(pl, num)) {
            if (!memcmp(a, empty, 16) || memcmp(b, empty, FSP_ENTRY_SIZE)) return -1;
        } else if (memcmp(a, b, FSP_ENTRY_SIZE)) return -1;
    }
    return 0;
}

void mp_table_tag(const FspGpt *g, char *out, size_t n) {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", g->guid[i]);
    snprintf(out, n, "%s-%08x", hex, (unsigned)g->entries_crc);
}

int mp_home_fs_ok(const char *s) {
    return !strcmp(s, "ext4") || !strcmp(s, "ext3") || !strcmp(s, "ext2") || !strcmp(s, "xfs") || !strcmp(s, "btrfs");
}

int mp_fsck_pass(const char *s) { return !strncmp(s, "ext", 3) ? 2 : 0; }

int mp_loader_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 32 || s[0] == '.') return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) return 0;
    return 1;
}
