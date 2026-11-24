/* vmiso_test.c — what an ISO boots on (src/vm/vmiso.c), read from images built here byte by byte: ISO 9660 trees with
 * EFI/BOOT and sources, an El Torito catalog with x86 BIOS and UEFI entries, FAT12 ESP images (short and long names),
 * a UDF volume like an install disc's (sources/install.wim, efi/boot/bootx64.efi), PE headers of each machine. Then
 * hostile images: directories and FAT chains that point at themselves, huge sizes and counts, truncated files, and a
 * fuzz pass of random changes over every image. ASan and UBSan.
 *
 *   build/vmiso-test [N]            N fuzz rounds per image (default 2000)
 *   build/vmiso-test --file ISO...  prints what real ISOs say (not a test) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/vm/vmiso.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int pass, fail;
#define T(name, cond) do { if (cond) pass++; else { fail++; printf("FAIL  %s\n", name); } } while (0)

#define SEC 2048
#define IMG_SECS 340
static uint8_t img[IMG_SECS * SEC];

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w32b(uint8_t *p, uint32_t v) { w32(p, v); for (int i = 0; i < 4; i++) p[4 + i] = (uint8_t)(v >> (24 - 8 * i)); }    /* both-endian */
static uint8_t *sec(unsigned n) { return img + (size_t)n * SEC; }

/* a PE image's first 512 bytes for this machine */
static void pe(uint8_t *p, unsigned machine) {
    memset(p, 0, 512);
    p[0] = 'M'; p[1] = 'Z';
    w32(p + 0x3C, 0x80);
    memcpy(p + 0x80, "PE\0\0", 4);
    w16(p + 0x84, machine);
}

/* ---- ISO 9660 ---- */

/* a directory record at p: its length */
static size_t rec(uint8_t *p, uint32_t ext, uint32_t size, int dir, const char *name, size_t nl) {
    size_t len = 33 + nl + (nl % 2 ? 0 : 1);
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    w32b(p + 2, ext);
    w32b(p + 10, size);
    p[25] = dir ? 2 : 0;
    w16(p + 28, 1);
    p[32] = (uint8_t)nl;
    memcpy(p + 33, name, nl);
    return len;
}
static size_t dot(uint8_t *p, uint32_t ext, int which) { return rec(p, ext, SEC, 1, which ? "\1" : "\0", 1); }

/* a directory sector at `at` with . and .. and these entries ({name, ext, size, dir}) */
typedef struct { const char *name; uint32_t ext, size; int dir; } Ent;
static void dirsec(unsigned at, unsigned parent, const Ent *e, int n) {
    uint8_t *p = sec(at);
    memset(p, 0, SEC);
    size_t o = dot(p, at, 0);
    o += dot(p + o, parent, 1);
    for (int i = 0; i < n; i++) o += rec(p + o, e[i].ext, e[i].size, e[i].dir, e[i].name, strlen(e[i].name));
}

static void pvd(unsigned root, const char *label) {
    uint8_t *p = sec(16);
    memset(p, 0, SEC);
    p[0] = 1; memcpy(p + 1, "CD001", 5); p[6] = 1;
    memset(p + 40, ' ', 32);
    memcpy(p + 40, label, strlen(label));
    w32b(p + 80, IMG_SECS);
    rec(p + 156, root, SEC, 1, "\0", 1);
}
static void boot_record(unsigned at, unsigned catalog) {
    uint8_t *p = sec(at);
    memset(p, 0, SEC);
    p[0] = 0; memcpy(p + 1, "CD001", 5); p[6] = 1;
    memcpy(p + 7, "EL TORITO SPECIFICATION", 23);
    w32(p + 0x47, catalog);
}
static void terminator(unsigned at) { uint8_t *p = sec(at); memset(p, 0, SEC); p[0] = 255; memcpy(p + 1, "CD001", 5); p[6] = 1; }

/* the catalog: a validation entry (platform), a default entry, then sections ({platform, lba} each with one entry) */
static void catalog(unsigned at, int platform, uint32_t deflba, const int *plat, const uint32_t *lba, int n) {
    uint8_t *p = sec(at);
    memset(p, 0, SEC);
    p[0] = 1; p[1] = (uint8_t)platform; p[30] = 0x55; p[31] = 0xAA;
    uint16_t sum = 0;
    for (int i = 0; i < 32; i += 2) sum = (uint16_t)(sum + (p[i] | p[i + 1] << 8));
    w16(p + 28, (uint16_t)(0x10000 - sum));
    p[32] = 0x88; w16(p + 38, 4); w32(p + 40, deflba);
    size_t o = 64;
    for (int i = 0; i < n; i++) {
        p[o] = i == n - 1 ? 0x91 : 0x90; p[o + 1] = (uint8_t)plat[i]; w16(p + o + 2, 1); o += 32;
        p[o] = 0x88; w16(p + o + 6, 1); w32(p + o + 8, lba[i]); o += 32;
    }
}

/* ---- FAT12 (an ESP image: 256 sectors of 512, one FAT sector each, 16 root entries, data from sector 4) ---- */

static void fat12_set(uint8_t *fat, unsigned n, unsigned v) {
    unsigned off = n * 3 / 2;
    if (n & 1) { fat[off] = (uint8_t)((fat[off] & 0x0F) | ((v << 4) & 0xF0)); fat[off + 1] = (uint8_t)(v >> 4); }
    else { fat[off] = (uint8_t)v; fat[off + 1] = (uint8_t)((fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
}
static void sfn(uint8_t *e, const char *eleven, int attr, unsigned cl, uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, eleven, 11);
    e[11] = (uint8_t)attr;
    w16(e + 26, cl);
    w32(e + 28, size);
}
/* a long name's one part (names up to 13) before the short entry e[1]: returns entries written */
static int lfn(uint8_t *e, const char *name, const char *eleven) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)eleven[i]);
    memset(e, 0, 32);
    e[0] = 0x41; e[11] = 0x0F; e[13] = sum;
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    size_t nl = strlen(name);
    for (size_t c = 0; c < 13; c++) w16(e + pos[c], c < nl ? (unsigned char)name[c] : c == nl ? 0 : 0xFFFF);
    return 1;
}
/* an ESP at sector `at` (in 2048-byte sectors) with EFI/BOOT/<file> holding a PE of machine; long names when longnames */
static void esp(unsigned at, const char *file11, const char *longname, unsigned machine, int longnames) {
    uint8_t *b = sec(at);
    memset(b, 0, 256 * 512);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90; memcpy(b + 3, "MSWIN4.1", 8);
    w16(b + 11, 512); b[13] = 1; w16(b + 14, 1); b[16] = 2; w16(b + 17, 16); w16(b + 19, 256); b[21] = 0xF8; w16(b + 22, 1);
    b[510] = 0x55; b[511] = 0xAA;
    for (int f = 0; f < 2; f++) {
        uint8_t *fat = b + 512 * (1 + f);
        fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF;
        fat12_set(fat, 2, 0xFFF); fat12_set(fat, 3, 0xFFF); fat12_set(fat, 4, 0xFFF);
    }
    uint8_t *root = b + 512 * 3, *c2 = b + 512 * 4, *c3 = b + 512 * 5, *c4 = b + 512 * 6;
    int k = 0;
    sfn(root + 32 * k++, "ESPLABEL   ", 0x08, 0, 0);
    if (longnames) k += lfn(root + 32 * k, "EFI", "EFI~1      ");
    sfn(root + 32 * k++, longnames ? "EFI~1      " : "EFI        ", 0x10, 2, 0);
    k = 0;
    sfn(c2 + 32 * k++, ".          ", 0x10, 2, 0);
    sfn(c2 + 32 * k++, "..         ", 0x10, 0, 0);
    sfn(c2 + 32 * k++, "DELETED    ", 0x10, 3, 0); c2[32 * (k - 1)] = 0xE5;
    if (longnames) k += lfn(c2 + 32 * k, "Boot", "BOOT~1     ");
    sfn(c2 + 32 * k++, longnames ? "BOOT~1     " : "BOOT       ", 0x10, 3, 0);
    k = 0;
    if (longnames) k += lfn(c3 + 32 * k, longname, "BOOTRI~1EFI");
    sfn(c3 + 32 * k++, longnames ? "BOOTRI~1EFI" : file11, 0x20, 4, 512);
    pe(c4, machine);
}

/* ---- UDF (2048-byte blocks; the partition at sector 300) ---- */

#define PART 300
static void tag(uint8_t *p, unsigned id, uint32_t loc, size_t len) {
    w16(p, id); w16(p + 2, 2); w32(p + 12, loc);
    w16(p + 8, (unsigned)(len > 16 ? len - 16 : 0));
    uint8_t s = 0;
    for (int i = 0; i < 16; i++) if (i != 4) s = (uint8_t)(s + p[i]);
    p[4] = s;
}
static uint8_t *ublk(unsigned lbn) { return sec(PART + lbn); }
/* a file entry at lbn: a directory or file of len bytes, one short_ad to data_lbn */
static void ufe(unsigned lbn, int dir, uint32_t len, unsigned data_lbn) {
    uint8_t *p = ublk(lbn);
    memset(p, 0, SEC);
    p[16 + 11] = dir ? 4 : 5;
    w16(p + 16 + 18, 0);
    w32(p + 56, len); w32(p + 60, 0);
    w32(p + 168, 0); w32(p + 172, 8);
    w32(p + 176, len); w32(p + 180, data_lbn);
    tag(p, 261, lbn, 184);
}
/* a file identifier at p: returns its padded length */
static size_t fid(uint8_t *p, const char *name, int dir, int parent, unsigned icb) {
    size_t nl = parent ? 0 : strlen(name), lfi = parent ? 0 : nl + 1, total = 38 + lfi, pad = (total + 3) & ~(size_t)3;
    memset(p, 0, pad);
    p[18] = (uint8_t)((dir ? 2 : 0) | (parent ? 8 : 0));
    p[19] = (uint8_t)lfi;
    w32(p + 20, SEC); w32(p + 24, icb);
    if (!parent) { p[38] = 8; memcpy(p + 39, name, nl); }
    tag(p, 257, 0, total);
    return pad;
}
typedef struct { const char *name; int dir; unsigned icb; } UEnt;
static uint32_t udir(unsigned data_lbn, unsigned parent, const UEnt *e, int n) {
    uint8_t *p = ublk(data_lbn);
    memset(p, 0, SEC);
    size_t o = fid(p, "", 1, 1, parent);
    for (int i = 0; i < n; i++) o += fid(p + o, e[i].name, e[i].dir, 0, e[i].icb);
    return (uint32_t)o;
}
static void udf(unsigned machine) {
    uint8_t *a = sec(256);
    memset(a, 0, SEC);
    w32(a + 16, 3 * SEC); w32(a + 20, 260);
    tag(a, 2, 256, 512);
    uint8_t *pd = sec(260);
    memset(pd, 0, SEC);
    w16(pd + 22, 0); w32(pd + 188, PART); w32(pd + 192, 40);
    tag(pd, 5, 260, 512);
    uint8_t *lv = sec(261);
    memset(lv, 0, SEC);
    w32(lv + 212, SEC); w32(lv + 248, SEC); w32(lv + 252, 0); w16(lv + 256, 0);
    w32(lv + 264, 6); w32(lv + 268, 1);
    lv[440] = 1; lv[441] = 6; w16(lv + 442, 1); w16(lv + 444, 0);
    tag(lv, 6, 261, 446);
    uint8_t *td = sec(262);
    memset(td, 0, SEC);
    tag(td, 8, 262, 512);
    uint8_t *fsd = ublk(0);
    memset(fsd, 0, SEC);
    w32(fsd + 400, SEC); w32(fsd + 404, 1); w16(fsd + 408, 0);
    tag(fsd, 256, 0, 512);
    /* root (1, data 2): efi, sources; sources (3, data 4): install.wim (5); efi (6, data 7): boot (8, data 9): bootx64.efi (10, data 11) */
    UEnt root[] = { { "efi", 1, 6 }, { "sources", 1, 3 } };
    ufe(1, 1, udir(2, 1, root, 2), 2);
    UEnt src[] = { { "boot.wim", 0, 5 }, { "install.wim", 0, 5 } };
    ufe(3, 1, udir(4, 1, src, 2), 4);
    ufe(5, 0, 512, 12);
    UEnt efi[] = { { "boot", 1, 8 } };
    ufe(6, 1, udir(7, 1, efi, 1), 7);
    UEnt boot[] = { { "bootx64.efi", 0, 10 } };
    ufe(8, 1, udir(9, 6, boot, 1), 9);
    ufe(10, 0, 512, 11);
    pe(ublk(11), machine);
}

/* ---- images ---- */

enum { IMG_ARM, IMG_AMD, IMG_WIN, IMG_BOTH, IMG_RISCV, IMG_COUNT };
static const char *const IMG_NAME[] = { "arm64 Linux", "amd64 hybrid", "install disc (UDF)", "two architectures", "riscv64 by name" };

static void build(int kind) {
    memset(img, 0, sizeof img);
    /* sectors: 16 PVD, 17 boot record, 18 terminator, 20 root, 21 EFI, 22 BOOT, 23 sources, 24 catalog, 25 a boot file,
     * 26 another, 40.. the ESP (128 KiB = 64 sectors), 256.. UDF */
    pvd(20, kind == IMG_WIN ? "CCCOMA_X64FRE_EN-US_DV9" : "MY_DISC");
    boot_record(17, 24);
    terminator(18);
    if (kind == IMG_WIN) {
        Ent r[] = { { "README.TXT;1", 25, 100, 0 } };
        dirsec(20, 20, r, 1);
        memcpy(sec(25), "This disc contains a UDF file system", 36);
        int plat[] = { 0xEF };
        uint32_t lba[] = { 40 };
        catalog(24, 0, 30, plat, lba, 1);
        esp(40, "BOOTX64 EFI", "", 0x8664, 0);
        udf(0x8664);
        return;
    }
    Ent r[] = { { "EFI", 21, SEC, 1 }, { "SOURCES", 23, SEC, 1 }, { "README.TXT;1", 27, 10, 0 } };
    dirsec(20, 20, r, 3);
    Ent e[] = { { "BOOT", 22, SEC, 1 } };
    dirsec(21, 20, e, 1);
    Ent s[] = { { "SETUP.EXE;1", 27, 10, 0 } };
    dirsec(23, 20, s, 1);
    if (kind == IMG_ARM) {
        Ent b[] = { { "BOOTAA64.EFI;1", 25, 512, 0 }, { "GRUBAA64.EFI;1", 26, 512, 0 } };
        dirsec(22, 21, b, 2);
        pe(sec(25), 0xAA64);
        pe(sec(26), 0xAA64);
        int plat[] = { 0xEF };
        uint32_t lba[] = { 40 };
        catalog(24, 0xEF, 40, plat, lba, 1);
        esp(40, "BOOTAA64EFI", "", 0xAA64, 0);
    } else if (kind == IMG_AMD) {
        Ent b[] = { { "BOOTX64.EFI;1", 25, 512, 0 }, { "BOOTIA32.EFI;1", 26, 512, 0 } };
        dirsec(22, 21, b, 2);
        pe(sec(25), 0x8664);
        pe(sec(26), 0x014C);
        int plat[] = { 0xEF };
        uint32_t lba[] = { 40 };
        catalog(24, 0, 30, plat, lba, 1);
        esp(40, "", "BOOTx64.efi", 0x8664, 1);
    } else if (kind == IMG_BOTH) {
        Ent b[] = { { "BOOTX64.EFI;1", 25, 512, 0 }, { "BOOTAA64.EFI;1", 26, 512, 0 } };
        dirsec(22, 21, b, 2);
        pe(sec(25), 0x8664);
        pe(sec(26), 0xAA64);
    } else if (kind == IMG_RISCV) {
        Ent b[] = { { "BOOTRISCV64.EFI;1", 25, 512, 0 } };
        dirsec(22, 21, b, 1);
        memcpy(sec(25), "not a PE image", 14);
    }
}

static int memfd_of(const uint8_t *b, size_t n) {
    int fd = memfd_create("iso", MFD_CLOEXEC);
    if (fd < 0 || write(fd, b, n) != (ssize_t)n) { perror("memfd"); exit(2); }
    return fd;
}
static int probe(VmIso *v, size_t n) {
    int fd = memfd_of(img, n);
    int rc = vmiso_probe(fd, v, 0);
    close(fd);
    return rc;
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--file")) {
        for (int i = 2; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
            VmIso v;
            int rc = fd >= 0 ? vmiso_probe(fd, &v, 0) : -1;
            if (fd >= 0) close(fd);
            if (rc) { printf("%s: not an ISO 9660 or UDF image\n", argv[i]); continue; }
            printf("%s: label \"%s\" iso9660 %d udf %d bios %d efi %d named %#x pe %#x windows %d arch \"%s\" reads %u\n", argv[i], v.label,
                   v.iso9660, v.udf, v.bios, v.efi, v.efi_named, v.efi_pe, v.windows, vmiso_arch(&v), v.reads);
        }
        return 0;
    }
    int rounds = argc > 1 ? atoi(argv[1]) : 2000;
    VmIso v;

    build(IMG_ARM);
    T("arm64: an ISO", !probe(&v, sizeof img) && v.iso9660 && !v.udf);
    T("arm64: label", !strcmp(v.label, "MY_DISC"));
    T("arm64: El Torito UEFI only", v.efi && !v.bios);
    T("arm64: named and PE", v.efi_named == VMISO_AA64 && v.efi_pe == VMISO_AA64);
    T("arm64: arch", !strcmp(vmiso_arch(&v), "arm64"));
    T("arm64: not an install disc", !v.windows);

    build(IMG_AMD);
    T("amd64: an ISO", !probe(&v, sizeof img));
    T("amd64: BIOS and UEFI", v.bios && v.efi);
    T("amd64: x64 and ia32 files", v.efi_named == (VMISO_X64 | VMISO_IA32) && v.efi_pe == (VMISO_X64 | VMISO_IA32));
    T("amd64: arch", !strcmp(vmiso_arch(&v), "amd64"));

    /* the ESP alone (long names): the tree's boot files gone */
    build(IMG_AMD);
    memset(sec(22) + 68, 0, SEC - 68);
    T("amd64: ESP with long names alone", !probe(&v, sizeof img) && v.efi_named == VMISO_X64 && v.efi_pe == VMISO_X64);

    build(IMG_WIN);
    T("install disc: ISO 9660 and UDF", !probe(&v, sizeof img) && v.iso9660 && v.udf);
    T("install disc: install.wim", v.windows);
    T("install disc: x64 from UDF and the ESP", v.efi_named == VMISO_X64 && v.efi_pe == VMISO_X64);
    T("install disc: arch", !strcmp(vmiso_arch(&v), "amd64"));
    /* an arm64 install disc: both boot files say so */
    build(IMG_WIN);
    esp(40, "BOOTAA64EFI", "", 0xAA64, 0);
    pe(ublk(11), 0xAA64);
    { UEnt b[] = { { "bootaa64.efi", 0, 10 } }; ufe(8, 1, udir(9, 6, b, 1), 9); }
    T("arm64 install disc: arch from PE", !probe(&v, sizeof img) && v.windows && !strcmp(vmiso_arch(&v), "arm64"));
    /* the UDF side alone, no ISO 9660 */
    build(IMG_WIN);
    memset(sec(16), 0, 3 * SEC);
    T("UDF alone", !probe(&v, sizeof img) && !v.iso9660 && v.udf && v.windows && v.efi_pe == VMISO_X64);

    build(IMG_BOTH);
    T("two architectures: no answer", !probe(&v, sizeof img) && v.efi_pe == (VMISO_X64 | VMISO_AA64) && !strcmp(vmiso_arch(&v), ""));
    build(IMG_RISCV);
    T("riscv64 by name only", !probe(&v, sizeof img) && v.efi_named == VMISO_RISCV64 && !v.efi_pe && !strcmp(vmiso_arch(&v), "riscv64"));

    /* BIOS only: amd64 */
    build(IMG_AMD);
    memset(sec(22), 0, SEC);
    { int plat[] = { 0 }; uint32_t lba[] = { 30 }; catalog(24, 0, 30, plat, lba, 1); }
    T("BIOS only: amd64", !probe(&v, sizeof img) && v.bios && !v.efi && !strcmp(vmiso_arch(&v), "amd64"));

    /* not ISOs */
    memset(img, 0, sizeof img);
    T("zeros: refused", probe(&v, sizeof img) == -1);
    T("short file: refused", probe(&v, 100) == -1);
    build(IMG_ARM);
    T("truncated after the PVD: label only", !probe(&v, 17 * SEC) && !strcmp(v.label, "MY_DISC") && !v.efi_named && !v.efi);

    /* hostile: a directory that is its own EFI and BOOT */
    build(IMG_ARM);
    { Ent r[] = { { "EFI", 20, SEC, 1 }, { "BOOT", 20, SEC, 1 }, { "BOOTAA64.EFI;1", 25, 512, 0 } }; dirsec(20, 20, r, 3); }
    T("self-referencing directory: ends", !probe(&v, sizeof img) && v.efi_named == VMISO_AA64);
    /* huge sizes and offsets */
    build(IMG_ARM);
    { Ent r[] = { { "EFI", 0xFFFFFFF0u, 0xFFFFFFFFu, 1 } }; dirsec(20, 20, r, 1); }
    sec(17)[7] = 'X';                                       /* no El Torito: only the tree could say */
    T("huge directory: skipped", !probe(&v, sizeof img) && !v.efi_named);
    build(IMG_ARM);
    { Ent b[] = { { "BOOTAA64.EFI;1", 0xFFFFFFFFu, 0xFFFFFFFFu, 0 } }; dirsec(22, 21, b, 1); }
    w32(sec(25) + 0x3C, 0xFFFFFFF0u);
    T("boot file out of the image: named only", !probe(&v, sizeof img) && v.efi_named == VMISO_AA64);
    /* a PE whose header is past the file's end */
    build(IMG_ARM);
    w32(sec(25) + 0x3C, 600);
    { uint8_t *e = sec(40) + 512 * 6; w32(e + 0x3C, 0x10000); }
    T("PE header past the end: no machine", !probe(&v, sizeof img) && v.efi_named == VMISO_AA64 && !v.efi_pe);
    /* a FAT chain that loops (cluster 3 → 3) and a FAT directory that is its own child */
    build(IMG_AMD);
    memset(sec(22), 0, SEC);
    {
        uint8_t *b = sec(40);
        for (int f = 0; f < 2; f++) { fat12_set(b + 512 * (1 + f), 3, 3); fat12_set(b + 512 * (1 + f), 2, 2); }
        uint8_t *c3 = b + 512 * 5;
        sfn(c3 + 32 * 3, "BOOT       ", 0x10, 3, 0);
    }
    T("looping FAT chain: ends", !probe(&v, sizeof img));
    /* a UDF directory that is its own sources, and a FID with a huge implementation use */
    build(IMG_WIN);
    { UEnt r[] = { { "sources", 1, 1 }, { "efi", 1, 1 } }; udir(2, 1, r, 2); ufe(1, 1, SEC, 2); }
    T("self-referencing UDF directory: ends", !probe(&v, sizeof img) && v.udf && !v.windows);
    build(IMG_WIN);
    { uint8_t *p = ublk(4) + 40; w16(p + 36, 0xFFFF); tag(p, 257, 0, 40); }
    T("FID past its directory: stops", !probe(&v, sizeof img) && v.udf);
    build(IMG_WIN);
    { uint8_t *p = ublk(3); w32(p + 56, 0xFFFFFFF0u); w32(p + 60, 7); tag(p, 261, 3, 184); }
    T("UDF directory too big: skipped", !probe(&v, sizeof img) && !v.windows);
    build(IMG_WIN);
    { uint8_t *p = ublk(3); w32(p + 172, 4096); tag(p, 261, 3, 184); }
    T("UDF descriptors past the entry: refused", !probe(&v, sizeof img) && !v.windows);
    /* a catalog with a broken checksum: no El Torito */
    build(IMG_ARM);
    sec(24)[28] ^= 1;
    T("bad catalog checksum: ignored", !probe(&v, sizeof img) && !v.efi && v.efi_named == VMISO_AA64);

    /* fuzz: random bytes changed in each image (the first 700 KiB hold every structure), under ASan and UBSan */
    unsigned maxreads = 0;
    for (int k = 0; k < IMG_COUNT; k++) {
        for (int i = 0; i < rounds; i++) {
            build(k);
            int changes = 1 + (int)(rnd() % 24);
            for (int c = 0; c < changes; c++) {
                size_t at;
                switch (rnd() % 4) {
                case 0: at = 16 * SEC + rnd() % (10 * SEC); break;                      /* descriptors, directories, catalog */
                case 1: at = 40 * SEC + rnd() % (8 * 512); break;                        /* the ESP's boot sector, FAT, dirs */
                case 2: at = 256 * SEC + rnd() % (84 * SEC); break;                       /* UDF */
                default: at = rnd() % sizeof img; break;
                }
                unsigned r = rnd();
                img[at] = (uint8_t)(r & 3 ? r >> 8 : (r & 4 ? 0xFF : 0));
            }
            VmIso f;
            size_t len = rnd() % 8 ? sizeof img : 16 * SEC + rnd() % (sizeof img - 16 * SEC);
            probe(&f, len);
            if (f.reads > maxreads) maxreads = f.reads;
            if (f.reads > VMISO_READS) { fail++; printf("FAIL  fuzz %s: %u reads\n", IMG_NAME[k], f.reads); }
            (void)vmiso_arch(&f);
        }
    }
    T("fuzz ran", rounds >= 0);
    printf("vmiso-test: %d passed, %d failed (fuzz: %d rounds per image, at most %u reads)\n", pass, fail, rounds, maxreads);
    return fail ? 1 : 0;
}
