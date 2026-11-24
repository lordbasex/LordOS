/* vmiso.c — what an installer disc starts on (see vmiso.h). */
#include "vm/vmiso.h"

#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a sector's bytes, or NULL when it isn't whole inside the image */
static const uint8_t *sector(const uint8_t *img, size_t len, uint64_t lba) {
    uint64_t off = lba * VMISO_SECTOR;
    if (lba > (uint64_t)len / VMISO_SECTOR || off + VMISO_SECTOR > (uint64_t)len) return NULL;
    return img + off;
}

/* a directory record's identifier, without ";1" and a trailing '.', upper case, into out (at most 31 bytes); 0 ok */
static int ident(const uint8_t *rec, size_t rl, char out[32]) {
    size_t nl = rec[32];
    if (nl == 0 || 33 + nl > rl) return -1;
    size_t k = 0;
    for (size_t i = 0; i < nl && k < 31; i++) {
        unsigned char ch = rec[33 + i];
        if (ch == ';') break;
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        out[k++] = (char)ch;
    }
    while (k && out[k - 1] == '.') k--;
    out[k] = 0;
    return k ? 0 : -1;
}

typedef struct { uint64_t lba; uint32_t size; } Ext;

/* calls fn for each record of the directory at e (names only; '.' and '..' are left out). Stops early when fn returns 1. */
typedef int (*RecFn)(void *u, const char *name, int dir, Ext child);
static void each_record(const uint8_t *img, size_t len, Ext e, RecFn fn, void *u) {
    uint64_t start = e.lba * VMISO_SECTOR;
    if (e.lba > (uint64_t)len / VMISO_SECTOR || start >= (uint64_t)len) return;
    uint64_t size = e.size > VMISO_DIR_MAX ? VMISO_DIR_MAX : e.size;
    if (size > (uint64_t)len - start) size = (uint64_t)len - start;
    const uint8_t *d = img + start;
    uint64_t pos = 0;
    int seen = 0;
    while (pos < size && seen < VMISO_RECORDS) {
        unsigned rl = d[pos];
        if (rl == 0) {                                   /* the rest of this sector is padding */
            uint64_t next = (pos / VMISO_SECTOR + 1) * VMISO_SECTOR;
            if (next <= pos) return;
            pos = next;
            continue;
        }
        if (rl < 34 || pos + rl > size) return;          /* malformed: stop here */
        const uint8_t *rec = d + pos;
        pos += rl;
        seen++;
        if (rec[32] == 1 && (rec[33] == 0 || rec[33] == 1)) continue;    /* '.' and '..' */
        char name[32];
        if (ident(rec, rl, name)) continue;
        Ext child = { le32(rec + 2), le32(rec + 10) };
        if (fn(u, name, (rec[25] & 2) != 0, child)) return;
    }
}

typedef struct { const char *want; int found; Ext e; } Find;
static int find_dir(void *u, const char *name, int dir, Ext child) {
    Find *f = u;
    if (!dir || strcmp(name, f->want)) return 0;
    f->found = 1;
    f->e = child;
    return 1;
}

static int boot_file(void *u, const char *name, int dir, Ext child) {
    (void)child;
    unsigned *efi = u;
    if (dir) return 0;
    static const struct { const char *n; unsigned bit; } F[] = {
        { "BOOTX64.EFI", VMISO_EFI_AMD64 }, { "BOOTAA64.EFI", VMISO_EFI_ARM64 }, { "BOOTIA32.EFI", VMISO_EFI_IA32 },
        { "BOOTARM.EFI", VMISO_EFI_ARM }, { "BOOTRISCV64.EFI", VMISO_EFI_RISCV64 },
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) if (!strcmp(name, F[i].n)) *efi |= F[i].bit;
    return 0;
}

/* El Torito's boot catalog (its sector only): the validation entry's platform and the default entry, then each section
 * header's platform with a bootable entry after it */
static void eltorito(const uint8_t *img, size_t len, uint64_t lba, VmIsoInfo *info) {
    const uint8_t *c = sector(img, len, lba);
    if (!c || c[0] != 1 || c[30] != 0x55 || c[31] != 0xAA) return;
    unsigned sum = 0;
    for (int i = 0; i < 32; i += 2) sum += (unsigned)c[i] | (unsigned)c[i + 1] << 8;
    if (sum & 0xffff) return;                            /* the validation entry's checksum */
    unsigned plat = c[1];
    if (c[32] == 0x88) { if (plat == 0) info->bios = 1; else if (plat == 0xEF) info->eltorito_efi = 1; }
    for (unsigned off = 64; off + 32 <= VMISO_SECTOR; off += 32) {
        const uint8_t *h = c + off;
        if (h[0] != 0x90 && h[0] != 0x91) break;         /* not a section header: the catalog ends */
        unsigned n = (unsigned)h[2] | (unsigned)h[3] << 8, p = h[1];
        for (unsigned k = 0; k < n && off + 32 <= VMISO_SECTOR - 32; k++) {
            off += 32;
            if (c[off] == 0x88) { if (p == 0) info->bios = 1; else if (p == 0xEF) info->eltorito_efi = 1; }
        }
        if (h[0] == 0x91) break;                         /* the last section */
    }
}

int vmiso_detect(const uint8_t *img, size_t len, VmIsoInfo *info) {
    if (!info || (!img && len)) return -1;
    memset(info, 0, sizeof *info);
    Ext root = { 0, 0 };
    /* the volume descriptors, from sector 16 until the terminator (at most 32 of them) */
    for (uint64_t s = 16; s < 16 + 32; s++) {
        const uint8_t *v = sector(img, len, s);
        if (!v || memcmp(v + 1, "CD001", 5)) break;
        if (v[0] == 255) break;
        if (v[0] == 1 && !info->iso) {
            info->iso = 1;
            memcpy(info->label, v + 40, 32);
            info->label[32] = 0;
            for (int i = 31; i >= 0 && (info->label[i] == ' ' || !info->label[i]); i--) info->label[i] = 0;
            for (int i = 0; info->label[i]; i++) if ((unsigned char)info->label[i] < 0x20 || (unsigned char)info->label[i] > 0x7e) info->label[i] = '?';
            root.lba = le32(v + 156 + 2);
            root.size = le32(v + 156 + 10);
        } else if (v[0] == 0 && !memcmp(v + 7, "EL TORITO SPECIFICATION", 23)) {
            eltorito(img, len, le32(v + 0x47), info);
        }
    }
    if (!info->iso) return 0;
    Find efi = { "EFI", 0, { 0, 0 } };
    each_record(img, len, root, find_dir, &efi);
    if (!efi.found) return 0;
    Find boot = { "BOOT", 0, { 0, 0 } };
    each_record(img, len, efi.e, find_dir, &boot);
    if (boot.found) each_record(img, len, boot.e, boot_file, &info->efi);
    return 0;
}

const char *vmiso_arch(const VmIsoInfo *info) {
    if (!info) return NULL;
    unsigned x = info->efi & (VMISO_EFI_AMD64 | VMISO_EFI_ARM64);
    if (x == VMISO_EFI_AMD64) return "amd64";
    if (x == VMISO_EFI_ARM64) return "arm64";
    if (!x && info->bios) return "amd64";               /* a BIOS boot entry is an x86 one */
    return NULL;
}

int vmiso_json(const VmIsoInfo *info, char *out, size_t n) {
    if (!info || !out || !n) return -1;
    char label[33 * 2] = "";
    size_t k = 0;
    for (const char *p = info->label; *p && k + 2 < sizeof label; p++) {
        if (*p == '"' || *p == '\\') label[k++] = '\\';
        label[k++] = *p;
    }
    label[k] = 0;
    static const char *const N[] = { "amd64", "arm64", "ia32", "arm", "riscv64" };
    char efi[64] = "";
    size_t e = 0;
    for (int i = 0; i < 5; i++) if (info->efi & (1u << i)) e += (size_t)snprintf(efi + e, sizeof efi - e, "%s\"%s\"", e ? "," : "", N[i]);
    const char *a = vmiso_arch(info);
    int w = snprintf(out, n, "{\"iso\":%s,\"label\":\"%s\",\"efi\":[%s],\"bios\":%s,\"eltoritoEfi\":%s,\"arch\":%s%s%s}",
                     info->iso ? "true" : "false", label, efi, info->bios ? "true" : "false", info->eltorito_efi ? "true" : "false",
                     a ? "\"" : "", a ? a : "null", a ? "\"" : "");
    return w < 0 || (size_t)w >= n ? -1 : w;
}
