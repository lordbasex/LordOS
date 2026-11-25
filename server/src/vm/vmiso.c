/* vmiso.c — what an ISO boots on, read from its boot files (vmiso.h). Every structure here comes from an untrusted
 * file: each offset and length is checked before it is used, reads go through rd() (bounded, counted, inside the file),
 * paths are fixed (EFI/BOOT, sources) so no directory is visited twice, and every loop has a fixed upper bound. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vmiso.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEC 2048u
#define VD_MAX 32              /* volume descriptors looked at */
#define ESP_MAX 4              /* El Torito UEFI images looked at */
#define BOOTFILES_MAX 8        /* BOOT*.EFI files whose header is read, per directory */
#define FAT_CHAIN_MAX 64       /* clusters followed in one chain */
#define UDF_AD_MAX 64          /* allocation descriptors of one file */
#define UDF_VDS_MAX 64         /* sectors of the volume descriptor sequence */
#define PE_LFANEW_MAX 65536u   /* where a PE header may start */

typedef struct {
    int fd;
    const uint8_t *map;     /* the file mapped (vmiso_probe_map, in lord-vm's probe under seccomp strict mode), or NULL */
    uint64_t size;
    unsigned reads;
    uint64_t bytes;
    int over;
    unsigned max;
} Rd;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

/* n bytes at off, wholly inside the file, within the budget */
static int rd(Rd *r, uint64_t off, void *buf, size_t n) {
    if (r->over || !n || n > VMISO_DIR_MAX || off > r->size || n > r->size - off) return -1;
    if (r->reads >= r->max || r->bytes + n > VMISO_BYTES) { r->over = 1; return -1; }
    r->reads++;
    r->bytes += n;
    if (r->map) { memcpy(buf, r->map + off, n); return 0; }
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(r->fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

unsigned vmiso_pe_machine(unsigned machine) {
    switch (machine) {
    case 0x8664: return VMISO_X64;
    case 0xAA64: return VMISO_AA64;
    case 0x014C: return VMISO_IA32;
    case 0x5064: return VMISO_RISCV64;
    }
    return 0;
}

/* BOOT<arch>.EFI by its name: VMISO_*, 0 for another name */
static unsigned boot_name(const char *n) {
    static const struct { const char *name; unsigned bit; } B[] = {
        { "bootx64.efi", VMISO_X64 }, { "bootaa64.efi", VMISO_AA64 }, { "bootia32.efi", VMISO_IA32 }, { "bootriscv64.efi", VMISO_RISCV64 },
    };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) if (name_eq(n, B[i].name)) return B[i].bit;
    return 0;
}
static int install_name(const char *n) { return name_eq(n, "install.wim") || name_eq(n, "install.esd") || name_eq(n, "install.swm"); }

/* a file's bytes, however it is stored (contiguous on ISO 9660, a cluster chain on FAT, extents on UDF) */
typedef int (*ReadAt)(void *ctx, uint64_t off, void *buf, size_t n);

/* the machine a PE image of fsize bytes is for (VMISO_*), 0 when it isn't one */
static unsigned pe_of(ReadAt get, void *ctx, uint64_t fsize) {
    uint8_t mz[64], pe[6];
    if (fsize < sizeof mz || get(ctx, 0, mz, sizeof mz) || mz[0] != 'M' || mz[1] != 'Z') return 0;
    uint32_t lfanew = le32(mz + 0x3C);
    if (lfanew < sizeof mz || lfanew > PE_LFANEW_MAX || (uint64_t)lfanew + sizeof pe > fsize) return 0;
    if (get(ctx, lfanew, pe, sizeof pe) || memcmp(pe, "PE\0\0", 4)) return 0;
    return vmiso_pe_machine(le16(pe + 4));
}

/* ---- ISO 9660 ---- */

typedef struct { Rd *r; uint64_t off, size; } Span;
static int span_get(void *ctx, uint64_t off, void *buf, size_t n) {
    Span *s = ctx;
    if (off > s->size || n > s->size - off) return -1;
    return rd(s->r, s->off + off, buf, n);
}

/* a directory record's name: Rock Ridge's NM when there is one, Joliet's UCS-2 as ASCII ('?' for the rest), or the plain
 * name without ";1" and a final '.'. rec has len bytes, 33 + rec[32] <= len (checked by the caller) */
static void rec_name(const uint8_t *rec, size_t len, int joliet, char *out, size_t on) {
    size_t nl = rec[32], o = 0;
    const uint8_t *nm = rec + 33;
    out[0] = 0;
    if (!joliet) {
        int got = 0;
        for (size_t su = 33 + nl + (nl % 2 ? 0 : 1), k = 0; su + 4 <= len && k < 64; k++) {
            const uint8_t *e = rec + su;
            size_t el = e[2];
            if (el < 4 || el > len - su) break;
            if (e[0] == 'N' && e[1] == 'M' && el >= 5 && !(e[4] & 6)) {
                for (size_t i = 5; i < el && o + 1 < on; i++) out[o++] = (char)e[i];
                got = 1;
            }
            su += el;
        }
        if (got) { out[o] = 0; return; }
    }
    if (joliet) for (size_t i = 0; i + 1 < nl && o + 1 < on; i += 2) out[o++] = (char)(nm[i] || nm[i + 1] >= 0x80 || !nm[i + 1] ? '?' : nm[i + 1]);
    else for (size_t i = 0; i < nl && o + 1 < on; i++) out[o++] = (char)nm[i];
    out[o] = 0;
    char *semi = strchr(out, ';');
    if (semi) *semi = 0;
    size_t l = strlen(out);
    if (l && out[l - 1] == '.') out[l - 1] = 0;
}

typedef struct {
    const char *want;      /* a name to find (dir or file), or NULL to look at every entry */
    int want_dir;
    uint32_t ext, size;    /* what was found */
    int found;
    VmIso *v;
    Rd *r;
    int mode;              /* 0 find, 1 boot files, 2 install image */
    int pe_left;
} IsoWalk;

static void iso_visit(IsoWalk *w, const char *name, int isdir, uint32_t ext, uint32_t size) {
    if (w->mode == 0) {
        if (!w->found && isdir == w->want_dir && name_eq(name, w->want)) { w->found = 1; w->ext = ext; w->size = size; }
    } else if (w->mode == 1 && !isdir) {
        unsigned b = boot_name(name);
        if (!b) return;
        w->v->efi_named |= b;
        if (w->pe_left > 0) {
            w->pe_left--;
            Span s = { w->r, (uint64_t)ext * SEC, size };
            w->v->efi_pe |= pe_of(span_get, &s, size);
        }
    } else if (w->mode == 2 && !isdir && install_name(name)) w->v->windows = 1;
}

/* every record of the directory at ext (size bytes), sector by sector: records never cross a sector */
static void iso_dir(Rd *r, uint32_t ext, uint32_t size, int joliet, IsoWalk *w) {
    if (!size || size > VMISO_DIR_MAX) return;
    uint8_t s[SEC];
    uint32_t secs = (size + SEC - 1) / SEC;
    for (uint32_t i = 0; i < secs && !(w->mode == 0 && w->found); i++) {
        uint32_t n = size - i * SEC < SEC ? size - i * SEC : SEC;
        if (rd(r, ((uint64_t)ext + i) * SEC, s, n)) return;
        for (size_t pos = 0; pos < n;) {
            size_t len = s[pos];
            if (!len) break;                                   /* the rest of this sector is padding */
            if (len < 34 || len > n - pos || 33 + (size_t)s[pos + 32] > len) break;
            const uint8_t *rec = s + pos;
            pos += len;
            if (rec[32] == 1 && (rec[33] == 0 || rec[33] == 1)) continue;         /* . and .. */
            if (rec[25] & 0x80) continue;                                         /* multi-extent: not ours */
            char name[256];
            rec_name(rec, len, joliet, name, sizeof name);
            iso_visit(w, name, (rec[25] & 2) != 0, le32(rec + 2), le32(rec + 10));
        }
    }
}

/* the directory under (ext, size) named `name`: 0 and its place, -1 when it isn't there */
static int iso_sub(Rd *r, uint32_t *ext, uint32_t *size, const char *name, int joliet) {
    IsoWalk w = { .want = name, .want_dir = 1, .mode = 0 };
    iso_dir(r, *ext, *size, joliet, &w);
    if (!w.found) return -1;
    *ext = w.ext;
    *size = w.size;
    return 0;
}

static void iso_tree(Rd *r, const uint8_t *root, int joliet, VmIso *v) {
    if (root[0] < 34) return;
    uint32_t rext = le32(root + 2), rsize = le32(root + 10);
    uint32_t e = rext, s = rsize;
    if (!iso_sub(r, &e, &s, "efi", joliet) && !iso_sub(r, &e, &s, "boot", joliet)) {
        IsoWalk w = { .mode = 1, .v = v, .r = r, .pe_left = BOOTFILES_MAX };
        iso_dir(r, e, s, joliet, &w);
    }
    e = rext; s = rsize;
    if (!iso_sub(r, &e, &s, "sources", joliet)) {
        IsoWalk w = { .mode = 2, .v = v, .r = r };
        iso_dir(r, e, s, joliet, &w);
    }
}

/* ---- FAT (El Torito's UEFI image: the ESP) ---- */

typedef struct {
    Rd *r;
    int type;                  /* 12, 16, 32 */
    uint32_t clsize, nclusters, root_cl, root_ents;
    uint64_t fat_off, root_off, data_off;
} Fat;

static int fat_open(Rd *r, uint64_t base, Fat *f) {
    uint8_t b[512];
    if (rd(r, base, b, sizeof b) || b[510] != 0x55 || b[511] != 0xAA) return -1;
    unsigned bps = le16(b + 11), spc = b[13], rsv = le16(b + 14), nf = b[16], ents = le16(b + 17);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1)) || bps * spc > 65536 || !rsv || !nf || nf > 4) return -1;
    uint64_t tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint64_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    uint64_t rootsecs = ((uint64_t)ents * 32 + bps - 1) / bps;
    uint64_t datasec = rsv + nf * fatsz + rootsecs;
    if (!fatsz || tot <= datasec) return -1;
    uint64_t ncl = (tot - datasec) / spc;
    if (!ncl || ncl > 0x0FFFFFF0) return -1;
    *f = (Fat){ .r = r, .clsize = bps * spc, .nclusters = (uint32_t)ncl, .root_ents = ents };
    f->type = ncl < 4085 ? 12 : ncl < 65525 ? 16 : 32;
    if (f->type == 32) {
        if (ents) return -1;
        f->root_cl = le32(b + 44);
    } else if (!ents) return -1;
    f->fat_off = base + (uint64_t)rsv * bps;
    f->root_off = f->fat_off + nf * fatsz * bps;
    f->data_off = f->root_off + rootsecs * bps;
    return 0;
}

static int fat_valid(const Fat *f, uint32_t cl) { return cl >= 2 && cl - 2 < f->nclusters; }

/* the cluster after cl, 0 at the chain's end or on anything odd */
static uint32_t fat_next(const Fat *f, uint32_t cl) {
    if (!fat_valid(f, cl)) return 0;
    uint8_t b[4];
    uint32_t v;
    if (f->type == 12) {
        if (rd(f->r, f->fat_off + cl + cl / 2, b, 2)) return 0;
        v = le16(b);
        v = cl & 1 ? v >> 4 : v & 0xFFF;
    } else if (f->type == 16) {
        if (rd(f->r, f->fat_off + (uint64_t)cl * 2, b, 2)) return 0;
        v = le16(b);
    } else {
        if (rd(f->r, f->fat_off + (uint64_t)cl * 4, b, 4)) return 0;
        v = le32(b) & 0x0FFFFFFF;
    }
    return fat_valid(f, v) ? v : 0;
}

static uint64_t fat_cl_off(const Fat *f, uint32_t cl) { return f->data_off + (uint64_t)(cl - 2) * f->clsize; }

typedef struct { const Fat *f; uint32_t first; uint64_t size; } FatFile;
static int fat_get(void *ctx, uint64_t off, void *buf, size_t n) {
    FatFile *ff = ctx;
    const Fat *f = ff->f;
    if (off > ff->size || n > ff->size - off) return -1;
    uint64_t idx = off / f->clsize;
    if (idx >= FAT_CHAIN_MAX) return -1;
    uint32_t cl = ff->first;
    for (uint64_t i = 0; i < idx && cl; i++) cl = fat_next(f, cl);
    size_t done = 0;
    uint32_t in = (uint32_t)(off % f->clsize);
    for (int steps = 0; done < n && steps < FAT_CHAIN_MAX; steps++) {
        if (!fat_valid(f, cl)) return -1;
        size_t k = f->clsize - in < n - done ? f->clsize - in : n - done;
        if (rd(f->r, fat_cl_off(f, cl) + in, (char *)buf + done, k)) return -1;
        done += k;
        in = 0;
        if (done < n) cl = fat_next(f, cl);
    }
    return done == n ? 0 : -1;
}

typedef struct {
    const char *want;
    uint32_t found_cl;
    int found;
    VmIso *v;
    int boot;           /* looking at EFI/BOOT's files */
    int pe_left;
} FatWalk;

static void fat_visit(const Fat *f, FatWalk *w, const char *name, int isdir, uint32_t cl, uint32_t size) {
    if (!w->boot) {
        if (!w->found && isdir && name_eq(name, w->want)) { w->found = 1; w->found_cl = cl; }
        return;
    }
    if (isdir) return;
    unsigned b = boot_name(name);
    if (!b) return;
    w->v->efi_named |= b;
    if (w->pe_left > 0 && fat_valid(f, cl)) {
        w->pe_left--;
        FatFile ff = { f, cl, size };
        w->v->efi_pe |= pe_of(fat_get, &ff, size);
    }
}

/* one 32-byte entry; lfn keeps a long name being put together (its parts come last to first) */
typedef struct { char name[261]; int next; uint8_t sum; int ok; } Lfn;

static int fat_entry(const Fat *f, FatWalk *w, const uint8_t *e, Lfn *l) {
    if (!e[0]) return 1;                                      /* the end of the directory */
    if (e[0] == 0xE5) { l->ok = 0; return 0; }
    if (e[11] == 0x0F) {
        int ord = e[0] & 0x3F;
        if (ord < 1 || ord > 20) { l->ok = 0; return 0; }
        if (e[0] & 0x40) { memset(l->name, 0, sizeof l->name); l->ok = 1; l->sum = e[13]; l->next = ord; }
        else if (!l->ok || ord != l->next - 1 || e[13] != l->sum) { l->ok = 0; return 0; }
        l->next = ord;
        static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int c = 0; c < 13; c++) {
            unsigned ch = le16(e + pos[c]);
            if (!ch || ch == 0xFFFF) break;
            l->name[(ord - 1) * 13 + c] = (char)(ch < 0x80 && ch >= 0x20 ? ch : '?');
        }
        return 0;
    }
    if (e[11] & 0x08) { l->ok = 0; return 0; }               /* the volume's label */
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + e[i]);
    char name[261];
    if (l->ok && l->next == 1 && sum == l->sum && l->name[0]) memcpy(name, l->name, sizeof name);
    else {
        size_t o = 0;
        for (int i = 0; i < 8 && e[i] != ' '; i++) name[o++] = (char)(e[i] >= 0x20 && e[i] < 0x7f ? e[i] : '?');
        if (e[8] != ' ') { name[o++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) name[o++] = (char)(e[i] >= 0x20 && e[i] < 0x7f ? e[i] : '?'); }
        name[o] = 0;
    }
    name[260] = 0;
    l->ok = 0;
    uint32_t cl = le16(e + 26) | (f->type == 32 ? (uint32_t)le16(e + 20) << 16 : 0);
    fat_visit(f, w, name, (e[11] & 0x10) != 0, cl, le32(e + 28));
    return 0;
}

/* the root (cl 0 on FAT12/16) or the directory at cluster cl */
static void fat_dir(const Fat *f, uint32_t cl, FatWalk *w) {
    uint8_t b[4096];
    Lfn l = { .ok = 0 };
    if (!cl && f->type != 32) {
        uint64_t len = (uint64_t)f->root_ents * 32;
        for (uint64_t o = 0; o < len; o += sizeof b) {
            size_t n = len - o < sizeof b ? (size_t)(len - o) : sizeof b;
            if (rd(f->r, f->root_off + o, b, n)) return;
            for (size_t i = 0; i + 32 <= n; i += 32) if (fat_entry(f, w, b + i, &l) || (!w->boot && w->found)) return;
        }
        return;
    }
    if (!cl) cl = f->root_cl;
    for (int steps = 0; steps < FAT_CHAIN_MAX && fat_valid(f, cl); steps++) {
        for (uint32_t o = 0; o < f->clsize; o += sizeof b) {
            size_t n = f->clsize - o < sizeof b ? f->clsize - o : sizeof b;
            if (rd(f->r, fat_cl_off(f, cl) + o, b, n)) return;
            for (size_t i = 0; i + 32 <= n; i += 32) if (fat_entry(f, w, b + i, &l) || (!w->boot && w->found)) return;
        }
        cl = fat_next(f, cl);
    }
}

static void esp_image(Rd *r, uint64_t base, VmIso *v) {
    Fat f;
    if (fat_open(r, base, &f)) return;
    FatWalk w = { .want = "efi", .v = v };
    fat_dir(&f, 0, &w);
    if (!w.found || !fat_valid(&f, w.found_cl)) return;
    uint32_t efi = w.found_cl;
    w = (FatWalk){ .want = "boot", .v = v };
    fat_dir(&f, efi, &w);
    if (!w.found || !fat_valid(&f, w.found_cl) || w.found_cl == efi) return;
    uint32_t boot = w.found_cl;
    w = (FatWalk){ .boot = 1, .v = v, .pe_left = BOOTFILES_MAX };
    fat_dir(&f, boot, &w);
}

/* ---- El Torito ---- */

static void el_torito(Rd *r, uint32_t cat, VmIso *v) {
    uint8_t b[SEC];
    if (rd(r, (uint64_t)cat * SEC, b, sizeof b)) return;
    /* the validation entry: header 1, key 55 AA, its 16-bit words summing to 0 */
    if (b[0] != 1 || b[30] != 0x55 || b[31] != 0xAA) return;
    uint16_t sum = 0;
    for (int i = 0; i < 32; i += 2) sum = (uint16_t)(sum + le16(b + i));
    if (sum) return;
    uint32_t esp[ESP_MAX];
    int nesp = 0;
    int platform = b[1];
    for (size_t i = 32, k = 0; i + 32 <= sizeof b; i += 32, k++) {
        const uint8_t *e = b + i;
        if (k > 0 && (e[0] == 0x90 || e[0] == 0x91)) { platform = e[1]; continue; }       /* a section header */
        if (e[0] == 0x44) continue;                                                     /* an extension */
        if (e[0] != 0x88 && e[0] != 0x00) break;
        if (e[0] == 0x00) { int zero = 1; for (int j = 0; j < 32; j++) if (e[j]) zero = 0; if (zero) break; continue; }
        if (platform == 0x00) v->bios = 1;
        else if (platform == 0xEF) {
            v->efi = 1;
            uint32_t lba = le32(e + 8);
            int seen = 0;
            for (int j = 0; j < nesp; j++) if (esp[j] == lba) seen = 1;
            if (!seen && nesp < ESP_MAX) esp[nesp++] = lba;
        }
    }
    for (int i = 0; i < nesp; i++) esp_image(r, (uint64_t)esp[i] * SEC, v);
}

/* ---- UDF (an install disc of virtio-win guests keeps its files there: its ISO 9660 side holds only a README) ---- */

static int tag_ok(const uint8_t *b, unsigned id) {
    if (le16(b) != id) return 0;
    uint8_t s = 0;
    for (int i = 0; i < 16; i++) if (i != 4) s = (uint8_t)(s + b[i]);
    return s == b[4];
}

typedef struct {
    Rd *r;
    uint64_t part;       /* the partition's first sector */
} Udf;

typedef struct {
    const Udf *u;
    uint8_t fe[SEC];
    uint64_t len;        /* information length */
    int dir;
    unsigned adtype;     /* 0 short_ad, 1 long_ad, 3 embedded */
    uint32_t adoff, adlen;
} UFile;

static int udf_file(const Udf *u, uint32_t lbn, UFile *f) {
    f->u = u;
    if (rd(u->r, (u->part + lbn) * SEC, f->fe, SEC)) return -1;
    uint32_t lea, lad, base;
    if (tag_ok(f->fe, 261)) { lea = le32(f->fe + 168); lad = le32(f->fe + 172); base = 176; }
    else if (tag_ok(f->fe, 266)) { lea = le32(f->fe + 208); lad = le32(f->fe + 212); base = 216; }
    else return -1;
    if (lea > SEC || lad > SEC || base + lea + lad > SEC) return -1;
    f->dir = f->fe[16 + 11] == 4;
    f->adtype = le16(f->fe + 16 + 18) & 7;
    if (f->adtype != 0 && f->adtype != 1 && f->adtype != 3) return -1;
    f->len = le64(f->fe + 56);
    f->adoff = base + lea;
    f->adlen = lad;
    return 0;
}

static int udf_get(void *ctx, uint64_t off, void *buf, size_t n) {
    UFile *f = ctx;
    if (off > f->len || n > f->len - off) return -1;
    if (f->adtype == 3) {
        if (off > f->adlen || n > f->adlen - off) return -1;
        memcpy(buf, f->fe + f->adoff + off, n);
        return 0;
    }
    unsigned adsz = f->adtype == 0 ? 8 : 16;
    uint64_t at = 0;
    size_t done = 0;
    for (uint32_t p = 0, k = 0; p + adsz <= f->adlen && k < UDF_AD_MAX && done < n; p += adsz, k++) {
        const uint8_t *ad = f->fe + f->adoff + p;
        uint32_t raw = le32(ad), elen = raw & 0x3FFFFFFF, lbn = le32(ad + 4);
        if (!elen || (raw >> 30) != 0) return -1;                      /* the end, or not recorded / continued */
        if (adsz == 16 && le16(ad + 8) != 0) return -1;                 /* another partition */
        uint64_t want = off + done;
        if (want < at + elen) {
            uint64_t in = want - at;
            size_t k2 = elen - in < n - done ? (size_t)(elen - in) : n - done;
            if (rd(f->u->r, (f->u->part + lbn) * SEC + in, (char *)buf + done, k2)) return -1;
            done += k2;
        }
        at += elen;
    }
    return done == n ? 0 : -1;
}

typedef struct {
    const char *want;
    int want_dir;
    uint32_t lbn;
    int found;
    VmIso *v;
    const Udf *u;
    int mode;            /* 0 find, 1 boot files, 2 install image */
    int pe_left;
} UdfWalk;

static void udf_dir(const Udf *u, uint32_t lbn, UdfWalk *w) {
    UFile *d = malloc(sizeof *d);
    if (!d) return;
    if (udf_file(u, lbn, d) || !d->dir || !d->len || d->len > VMISO_DIR_MAX) { free(d); return; }
    size_t len = (size_t)d->len;
    uint8_t *b = malloc(len);
    if (!b || udf_get(d, 0, b, len)) { free(b); free(d); return; }
    for (size_t pos = 0; pos + 38 <= len;) {
        const uint8_t *e = b + pos;
        if (!tag_ok(e, 257)) break;
        unsigned chars = e[18], lfi = e[19], liu = le16(e + 36);
        size_t total = 38 + (size_t)liu + lfi;
        if (total > len - pos) break;
        pos += (total + 3) & ~(size_t)3;
        if (chars & 0x0C || !lfi) continue;                               /* deleted, or the parent */
        const uint8_t *id = e + 38 + liu;
        char name[256];
        size_t o = 0;
        if (id[0] == 8) for (size_t i = 1; i < lfi && o + 1 < sizeof name; i++) name[o++] = (char)(id[i] >= 0x20 && id[i] < 0x7f ? id[i] : '?');
        else if (id[0] == 16) for (size_t i = 1; i + 1 < lfi && o + 1 < sizeof name; i += 2) name[o++] = (char)(!id[i] && id[i + 1] >= 0x20 && id[i + 1] < 0x7f ? id[i + 1] : '?');
        else continue;
        name[o] = 0;
        int isdir = (chars & 2) != 0;
        uint32_t flbn = le32(e + 24);
        if (le16(e + 28) != 0) continue;                                  /* another partition */
        if (w->mode == 0) {
            if (isdir == w->want_dir && name_eq(name, w->want)) { w->found = 1; w->lbn = flbn; break; }
        } else if (w->mode == 1 && !isdir) {
            unsigned bit = boot_name(name);
            if (!bit) continue;
            w->v->efi_named |= bit;
            if (w->pe_left > 0) {
                w->pe_left--;
                UFile *f = malloc(sizeof *f);
                if (f && !udf_file(u, flbn, f) && !f->dir) w->v->efi_pe |= pe_of(udf_get, f, f->len);
                free(f);
            }
        } else if (w->mode == 2 && !isdir && install_name(name)) w->v->windows = 1;
    }
    free(b);
    free(d);
}

static int udf_sub(const Udf *u, uint32_t *lbn, const char *name) {
    UdfWalk w = { .want = name, .want_dir = 1, .mode = 0 };
    udf_dir(u, *lbn, &w);
    if (!w.found || w.lbn == *lbn) return -1;
    *lbn = w.lbn;
    return 0;
}

/* the anchor at 256, the volume descriptor sequence's partition and logical volume (one type-1 map, 2048-byte blocks),
 * its file set and the root directory; UDF 2.5's metadata partition isn't read */
static int udf_probe(Rd *r, VmIso *v) {
    uint8_t b[SEC];
    if (rd(r, 256u * SEC, b, sizeof b) || !tag_ok(b, 2)) return -1;
    uint32_t vlen = le32(b + 16), vloc = le32(b + 20);
    int have_pd = 0, have_lv = 0;
    uint32_t pnum = 0, pstart = 0, fsd = 0, mapnum = 0;
    for (uint32_t i = 0; i < vlen / SEC && i < UDF_VDS_MAX; i++) {
        if (rd(r, ((uint64_t)vloc + i) * SEC, b, sizeof b)) return -1;
        if (tag_ok(b, 8)) break;
        if (tag_ok(b, 5) && !have_pd) { pnum = le16(b + 22); pstart = le32(b + 188); have_pd = 1; }
        else if (tag_ok(b, 6) && !have_lv) {
            if (le32(b + 212) != SEC || le32(b + 268) != 1 || b[440] != 1 || b[441] != 6 || le16(b + 248 + 8) != 0) return -1;
            fsd = le32(b + 248 + 4);
            mapnum = le16(b + 440 + 4);
            have_lv = 1;
        }
    }
    if (!have_pd || !have_lv || pnum != mapnum) return -1;
    Udf u = { r, pstart };
    if (rd(r, ((uint64_t)pstart + fsd) * SEC, b, sizeof b) || !tag_ok(b, 256) || le16(b + 400 + 8) != 0) return -1;
    uint32_t root = le32(b + 400 + 4);
    v->udf = 1;
    uint32_t l = root;
    if (!udf_sub(&u, &l, "efi") && !udf_sub(&u, &l, "boot")) {
        UdfWalk w = { .mode = 1, .v = v, .u = &u, .pe_left = BOOTFILES_MAX };
        udf_dir(&u, l, &w);
    }
    l = root;
    if (!udf_sub(&u, &l, "sources")) {
        UdfWalk w = { .mode = 2, .v = v, .u = &u };
        udf_dir(&u, l, &w);
    }
    return 0;
}

/* ---- the whole ---- */

static int probe(Rd *rp, VmIso *v);
int vmiso_probe(int fd, VmIso *v, unsigned max_reads) {
    memset(v, 0, sizeof *v);
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 17 * (off_t)SEC) return -1;
    Rd r = { .fd = fd, .size = (uint64_t)st.st_size, .max = max_reads && max_reads < VMISO_READS ? max_reads : VMISO_READS };
    return probe(&r, v);
}

int vmiso_probe_map(const void *map, uint64_t size, VmIso *v, unsigned max_reads) {
    memset(v, 0, sizeof *v);
    if (!map || size < 17 * (uint64_t)SEC) return -1;
    Rd r = { .fd = -1, .map = map, .size = size, .max = max_reads && max_reads < VMISO_READS ? max_reads : VMISO_READS };
    return probe(&r, v);
}

static int probe(Rd *rp, VmIso *v) {
    Rd r = *rp;
    uint8_t s[SEC], pvd_root[34], jol_root[34];
    int pvd = 0, jol = 0;
    uint32_t cat = 0;
    for (int i = 16; i < 16 + VD_MAX; i++) {
        if (rd(&r, (uint64_t)i * SEC, s, sizeof s) || memcmp(s + 1, "CD001", 5) || s[0] == 255) break;
        if (s[0] == 1 && !pvd) {
            pvd = 1;
            memcpy(pvd_root, s + 156, sizeof pvd_root);
            memcpy(v->label, s + 40, 32);
            v->label[32] = 0;
            for (int k = 31; k >= 0 && (v->label[k] == ' ' || !v->label[k]); k--) v->label[k] = 0;
            for (int k = 0; v->label[k]; k++) if ((unsigned char)v->label[k] < 0x20 || (unsigned char)v->label[k] > 0x7e) v->label[k] = '?';
        } else if (s[0] == 2 && !jol && s[88] == 0x25 && s[89] == 0x2F && (s[90] == 0x40 || s[90] == 0x43 || s[90] == 0x45)) {
            jol = 1;
            memcpy(jol_root, s + 156, sizeof jol_root);
        } else if (s[0] == 0 && !cat && !memcmp(s + 7, "EL TORITO SPECIFICATION", 23)) cat = le32(s + 0x47);
    }
    v->iso9660 = pvd;
    udf_probe(&r, v);
    if (!pvd && !v->udf) { v->reads = r.reads; return -1; }
    if (pvd) iso_tree(&r, pvd_root, 0, v);
    if (jol && !v->efi_named && !v->windows) iso_tree(&r, jol_root, 1, v);
    if (cat) el_torito(&r, cat, v);
    v->reads = r.reads;
    return 0;
}
