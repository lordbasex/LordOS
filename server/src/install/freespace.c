/* freespace.c — see freespace.h. No I/O, nothing here can touch a disk. */
#include "freespace.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

uint32_t fsp_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

int fsp_mode(const char *s) {
    if (!s[0] || !strcmp(s, "erase")) return 0;
    if (!strcmp(s, "free")) return 1;
    if (!strcmp(s, "manual")) return 2;
    return -1;
}

/* ---- first sector ---- */

FspMbr fsp_mbr_kind(const uint8_t *s) {
    if (s[510] != 0x55 || s[511] != 0xaa) return FSP_MBR_NONE;
    int ee = 0, other = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        if (e[4] == 0xee) ee++;
        else if (e[4]) other++;
    }
    if (ee && other) return FSP_MBR_HYBRID;
    if (ee > 1) return FSP_MBR_HYBRID;
    if (ee) return FSP_MBR_PROTECTIVE;
    return other ? FSP_MBR_DOS : FSP_MBR_NONE;
}

const char *fsp_mbr_name(FspMbr k) {
    return k == FSP_MBR_PROTECTIVE ? "gpt" : k == FSP_MBR_HYBRID ? "hybrid" : k == FSP_MBR_DOS ? "dos" : "none";
}

int fsp_mbr_same(const uint8_t *a, const uint8_t *b) {
    return !memcmp(a, b, 440) && !memcmp(a + 510, b + 510, 2) && fsp_mbr_kind(b) == FSP_MBR_PROTECTIVE;
}

/* ---- raw GPT ---- */

int fsp_gpt_header(const uint8_t *sec, unsigned lsec, uint64_t total, int backup, FspGpt *g) {
    uint8_t copy[4096];
    if ((lsec != 512 && lsec != 4096) || total < 64 || total > FSP_MAX_TOTAL) return -1;
    if (memcmp(sec, "EFI PART", 8)) return -1;
    uint32_t hsize = rd32(sec + 12);
    if (hsize < 92 || hsize > lsec) return -1;
    memcpy(copy, sec, hsize);
    memset(copy + 16, 0, 4);
    if (fsp_crc32(copy, hsize) != rd32(sec + 16)) return -1;
    g->my = rd64(sec + 24);
    g->alt = rd64(sec + 32);
    g->first = rd64(sec + 40);
    g->last = rd64(sec + 48);
    memcpy(g->guid, sec + 56, 16);
    g->entries_lba = rd64(sec + 72);
    g->count = rd32(sec + 80);
    g->esize = rd32(sec + 84);
    g->entries_crc = rd32(sec + 88);
    if (g->esize != FSP_ENTRY_SIZE || !g->count || g->count > FSP_MAX_PARTS) return -1;
    if (backup ? g->my != total - 1 || g->alt != 1 : g->my != 1 || g->alt != total - 1) return -1;
    if (g->first < 2 || g->first > g->last || g->last >= total - 1) return -1;
    uint64_t bytes = (uint64_t)g->count * FSP_ENTRY_SIZE, sectors = (bytes + lsec - 1) / lsec;
    if (g->entries_lba < 2 || g->entries_lba > total) return -1;
    if (backup ? g->entries_lba <= g->last || g->entries_lba + sectors > total - 1
               : g->entries_lba + sectors > g->first) return -1;
    return 0;
}

int fsp_gpt_pair(const FspGpt *a, const FspGpt *b, const uint8_t *ea, const uint8_t *eb) {
    if (memcmp(a->guid, b->guid, 16) || a->first != b->first || a->last != b->last) return -1;
    if (a->count != b->count || a->entries_crc != b->entries_crc) return -1;
    size_t bytes = (size_t)a->count * FSP_ENTRY_SIZE;
    if (fsp_crc32(ea, bytes) != a->entries_crc || fsp_crc32(eb, bytes) != b->entries_crc) return -1;
    return memcmp(ea, eb, bytes) ? -1 : 0;
}

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B as it lies on disk */
static const uint8_t esp_guid[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t zero16[16];

int fsp_raw_table(const FspGpt *g, unsigned lsec, uint64_t total, const uint8_t *entries, FspTable *t) {
    memset(t, 0, sizeof *t);
    t->lsec = lsec;
    t->max_entries = g->count;
    t->total = total;
    t->first = g->first;
    t->last = g->last;
    for (uint32_t i = 0; i < g->count; i++) {
        const uint8_t *e = entries + (size_t)i * FSP_ENTRY_SIZE;
        if (!memcmp(e, zero16, 16)) continue;
        FspPart *p = &t->p[t->n++];
        p->num = i + 1;
        p->start = rd64(e + 32);
        p->end = rd64(e + 40);
        p->esp = !memcmp(e, esp_guid, 16);
        if (p->start > p->end) return -1;
    }
    return 0;
}

/* ---- sgdisk's text ---- */

/* A decimal number that fits, and nothing cute: no sign, no spaces, no hex. */
static int number(const char *s, const char **end, uint64_t *out) {
    if (!isdigit((unsigned char)*s)) return -1;
    char *e;
    errno = 0;
    unsigned long long v = strtoull(s, &e, 10);
    if (errno || e - s > 19) return -1;
    *out = v;
    if (end) *end = e;
    return 0;
}

static int prefixed(const char *line, const char *pre, const char **rest) {
    size_t n = strlen(pre);
    if (strncmp(line, pre, n)) return 0;
    *rest = line + n;
    return 1;
}

/* Matches a line of the shape "<head><n><tail>" exactly. */
static int number_line(const char *line, const char *head, const char *tail, uint64_t *out) {
    const char *r;
    if (!prefixed(line, head, &r) || number(r, &r, out)) return 0;
    return !strcmp(r, tail);
}

static int parse_row(const char *l, FspPart *p) {
    uint64_t num;
    const char *r = l;
    while (*r == ' ') r++;
    if (number(r, &r, &num) || num < 1 || num > FSP_MAX_PARTS || *r != ' ') return -1;
    while (*r == ' ') r++;
    if (number(r, &r, &p->start) || *r != ' ') return -1;
    while (*r == ' ') r++;
    if (number(r, &r, &p->end) || *r != ' ') return -1;
    while (*r == ' ') r++;
    /* size: "20.0 MiB" */
    while (isdigit((unsigned char)*r) || *r == '.') r++;
    if (*r != ' ' || !r[1]) return -1;
    r++;
    if (strncmp(r, "bytes", 5) && strncmp(r + 1, "iB", 2)) return -1;
    r += strncmp(r, "bytes", 5) ? 3 : 5;
    if (*r != ' ') return -1;
    while (*r == ' ') r++;
    /* the code: four hex digits; the rest is the partition's name, which says nothing to us */
    for (int i = 0; i < 4; i++) if (!isxdigit((unsigned char)r[i])) return -1;
    if (r[4] && r[4] != ' ') return -1;
    p->num = (unsigned)num;
    p->esp = !strncmp(r, "EF00", 4);
    return p->start > p->end ? -1 : 0;
}

int fsp_parse_print(const char *text, FspTable *t) {
    enum { DISK = 1, SECTOR = 2, ENTRIES = 4, LIMITS = 8, HEADER = 16 };
    unsigned seen = 0;
    memset(t, 0, sizeof *t);
    const char *p = text;
    char line[512];
    while (*p) {
        size_t n = strcspn(p, "\n");
        if (n >= sizeof line) return -1;
        memcpy(line, p, n);
        line[n] = 0;
        p += n + (p[n] == '\n');
        const char *r;
        uint64_t a, b;
        if (seen & HEADER) {
            if (t->n >= FSP_MAX_PARTS || parse_row(line, &t->p[t->n])) return -1;
            t->n++;
            continue;
        }
        if (!line[0]) continue;
        if (prefixed(line, "Disk identifier (GUID): ", &r) || prefixed(line, "Model: ", &r)
            || prefixed(line, "Total free space is ", &r) || prefixed(line, "Partitions will be aligned on ", &r)) continue;
        if (prefixed(line, "Main partition table begins at sector ", &r)) continue;
        if (prefixed(line, "Disk ", &r)) {
            const char *c = strrchr(line, ':');
            if ((seen & DISK) || !c || c[1] != ' ' || number(c + 2, &r, &a) || strncmp(r, " sectors, ", 10)) return -1;
            t->total = a;
            seen |= DISK;
        } else if (prefixed(line, "Sector size (logical): ", &r)) {
            if ((seen & SECTOR) || number(r, &r, &a) || strcmp(r, " bytes")) return -1;
            t->lsec = (unsigned)a;
            seen |= SECTOR;
        } else if (prefixed(line, "Sector size (logical/physical): ", &r)) {
            if ((seen & SECTOR) || number(r, &r, &a) || *r != '/') return -1;
            t->lsec = (unsigned)a;
            if (number(r + 1, &r, &b) || strcmp(r, " bytes")) return -1;
            seen |= SECTOR;
        } else if (prefixed(line, "First usable sector is ", &r)) {
            if ((seen & LIMITS) || number(r, &r, &a) || strncmp(r, ", last usable sector is ", 24) || number(r + 24, &r, &b) || *r) return -1;
            t->first = a;
            t->last = b;
            seen |= LIMITS;
        } else if (number_line(line, "Partition table holds up to ", " entries", &a)) {
            if ((seen & ENTRIES) || !a || a > FSP_MAX_PARTS) return -1;
            t->max_entries = (unsigned)a;
            seen |= ENTRIES;
        } else if (!strncmp(line, "Number  Start (sector)", 22)) {
            seen |= HEADER;
        } else return -1;                       /* a warning, or something new: not a healthy table */
    }
    if (seen != (DISK | SECTOR | ENTRIES | LIMITS | HEADER)) return -1;
    if ((t->lsec != 512 && t->lsec != 4096) || !t->total || t->total > FSP_MAX_TOTAL) return -1;
    if (t->first > t->last || t->last >= t->total) return -1;
    for (int i = 0; i < t->n; i++)
        for (int j = i + 1; j < t->n; j++) if (t->p[i].num == t->p[j].num) return -1;
    return 0;
}

/* sgdisk -v's note that a partition doesn't end on the alignment says nothing about the table, and a
 * disk filled to its last sector always has one. Exactly that text and no other: returns what follows
 * it, or NULL. */
static const char *alignment_note(const char *s) {
    uint64_t n;
    const char *r;
    if (!prefixed(s, "Caution: Partition ", &r) || number(r, &r, &n) || !prefixed(r, " doesn't end on a ", &r)) return NULL;
    if (number(r, &r, &n) || !prefixed(r, "-sector boundary. This may\n", &r)) return NULL;
    return prefixed(r, "result in problems with some disk encryption tools.\n", &r) ? r : NULL;
}

int fsp_verify_clean(const char *text) {
    for (;;) {
        while (*text == '\n' || *text == ' ') text++;
        const char *after = alignment_note(text);
        if (!after) break;
        text = after;
    }
    if (strncmp(text, "No problems found.", 18)) return -1;
    if (strstr(text, "Caution") || strstr(text, "Warning") || strstr(text, "Problem:")) return -1;
    return 0;
}

static const FspPart *by_num(const FspTable *t, unsigned num) {
    for (int i = 0; i < t->n; i++) if (t->p[i].num == num) return &t->p[i];
    return NULL;
}

int fsp_tables_agree(const FspTable *a, const FspTable *b) {
    if (a->lsec != b->lsec || a->total != b->total || a->first != b->first || a->last != b->last) return 0;
    if (a->max_entries != b->max_entries || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        const FspPart *q = by_num(b, a->p[i].num);
        if (!q || q->start != a->p[i].start || q->end != a->p[i].end || q->esp != a->p[i].esp) return 0;
    }
    return 1;
}

/* ---- the region ---- */

static int cmp_start(const void *a, const void *b) {
    const FspPart *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void consider(uint64_t from, uint64_t to, uint64_t align, unsigned lsec, FspRegion *r) {
    uint64_t s = (from + align - 1) / align * align;
    uint64_t e = (to + 1) / align * align;      /* one past the end */
    if (s >= e) return;
    uint64_t bytes = (e - s) * lsec;
    if (bytes > r->bytes) { r->start = s; r->end = e - 1; r->bytes = bytes; }
}

int fsp_largest_free(const FspTable *t, FspRegion *r) {
    memset(r, 0, sizeof *r);
    if ((t->lsec != 512 && t->lsec != 4096) || t->last > FSP_MAX_TOTAL || t->first > t->last) return -1;
    FspPart sorted[FSP_MAX_PARTS];
    if (t->n < 0 || t->n > FSP_MAX_PARTS) return -1;
    memcpy(sorted, t->p, (size_t)t->n * sizeof *sorted);
    qsort(sorted, (size_t)t->n, sizeof *sorted, cmp_start);
    uint64_t align = FSP_ALIGN_BYTES / t->lsec, cursor = t->first;
    for (int i = 0; i < t->n; i++) {
        const FspPart *p = &sorted[i];
        if (p->start < cursor || p->end > t->last || p->start > p->end) return -1;     /* overlap or outside */
        if (p->start > cursor) consider(cursor, p->start - 1, align, t->lsec, r);
        cursor = p->end + 1;
    }
    if (cursor <= t->last) consider(cursor, t->last, align, t->lsec, r);
    return 0;
}

unsigned fsp_free_number(const FspTable *t) {
    for (unsigned n = 1; n <= t->max_entries && n <= FSP_MAX_PARTS; n++) if (!by_num(t, n)) return n;
    return 0;
}

int fsp_esp_count(const FspTable *t, unsigned *num) {
    int c = 0;
    if (num) *num = 0;
    for (int i = 0; i < t->n; i++) if (t->p[i].esp) { if (!c && num) *num = t->p[i].num; c++; }
    return c;
}

const char *fsp_decide(const FspFacts *f, FspChoice *c) {
    memset(c, 0, sizeof *c);
    if (!f->uefi) return "install.free.needUefi";
    if (f->busy) return "install.free.busy";
    if (f->mbr == FSP_MBR_DOS) return "install.free.mbr";
    if (f->mbr == FSP_MBR_HYBRID) return "install.free.hybridMbr";
    if (f->mbr != FSP_MBR_PROTECTIVE) return "install.free.noTable";
    if (!f->table_ok || !f->table) return "install.free.tableDamaged";
    const FspTable *t = f->table;
    int esps = fsp_esp_count(t, &c->esp_num);
    if (!esps) return "install.free.noEsp";
    if (esps > 1) return "install.free.manyEsps";
    if (!f->esp_fat) return "install.free.espNotFat";
    if (!f->esp_read) return "install.free.espUnreadable";
    if (f->esp_free < FSP_ESP_MIN_FREE) return "install.free.espFull";
    if (f->esp_has_ours) return "install.free.espHasLordOS";
    if (fsp_largest_free(t, &c->region)) return "install.free.tableDamaged";
    if (c->region.bytes < FSP_MIN_BYTES) return "install.free.noRoom";
    if (!(c->num = fsp_free_number(t))) return "install.free.tableFull";
    if (!f->boot_tool) return "install.free.noBootTool";
    return NULL;
}

/* ---- after the write ---- */

int fsp_entries_changed_only(const uint8_t *before, const uint8_t *after, uint32_t count, uint32_t idx,
                             uint64_t start, uint64_t end, const uint8_t *type) {
    if (idx >= count) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = before + (size_t)i * FSP_ENTRY_SIZE, *b = after + (size_t)i * FSP_ENTRY_SIZE;
        if (i != idx) { if (memcmp(a, b, FSP_ENTRY_SIZE)) return -1; continue; }
        static const uint8_t empty[FSP_ENTRY_SIZE];
        if (memcmp(a, empty, FSP_ENTRY_SIZE)) return -1;
        if (memcmp(b, type, 16) || rd64(b + 32) != start || rd64(b + 40) != end) return -1;
    }
    return 0;
}

int fsp_header_kept(const FspGpt *a, const FspGpt *b) {
    return !memcmp(a->guid, b->guid, 16) && a->first == b->first && a->last == b->last && a->my == b->my && a->alt == b->alt
        && a->entries_lba == b->entries_lba && a->count == b->count && a->esize == b->esize;
}

void fsp_part_path(const char *disk, unsigned num, char *out, size_t n) {
    size_t l = strlen(disk);
    snprintf(out, n, "/dev/%s%s%u", disk, l && isdigit((unsigned char)disk[l - 1]) ? "p" : "", num);
}
