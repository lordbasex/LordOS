/* vmiso_test.c — the Detect step's reader of installer discs (vm/vmiso.c), under ASan and UBSan: made-up ISO 9660 images
 * with UEFI boot files and El Torito catalogs, hostile ones (offsets past the end, huge sizes, records that overrun, a
 * loop of directories), and random mutations of a good image. With a path argument it also reads that real image. */
#include "vm/vmiso.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "vmiso-test: %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define S 2048u
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* one directory record at d: returns its length */
static unsigned rec(uint8_t *d, const char *name, uint32_t lba, uint32_t size, int dir) {
    size_t nl = strlen(name);
    unsigned rl = (unsigned)(33 + nl + ((33 + nl) & 1));
    memset(d, 0, rl);
    d[0] = (uint8_t)rl;
    put32(d + 2, lba);
    put32(d + 10, size);
    d[25] = dir ? 2 : 0;
    d[32] = (uint8_t)nl;
    memcpy(d + 33, name, nl);
    return rl;
}
static unsigned dots(uint8_t *d, uint32_t self, uint32_t parent) {
    unsigned o = rec(d, "x", self, S, 1);
    d[33] = 0;
    unsigned p = rec(d + o, "x", parent, S, 1);
    d[o + 33] = 1;
    return o + p;
}

/* an image: PVD at 16, El Torito record at 17, terminator at 18, catalog at 19, root at 20, EFI at 21, BOOT at 22 */
static uint8_t *image(size_t *len, const char *label, const char *const *files, int bios, int efi_entry) {
    *len = 32 * S;
    uint8_t *img = calloc(1, *len);
    uint8_t *pvd = img + 16 * S;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    memset(pvd + 40, ' ', 32);
    memcpy(pvd + 40, label, strlen(label));
    rec(pvd + 156, "x", 20, S, 1);
    pvd[156 + 33] = 0;
    uint8_t *br = img + 17 * S;
    br[0] = 0; memcpy(br + 1, "CD001", 5); br[6] = 1; memcpy(br + 7, "EL TORITO SPECIFICATION", 23); put32(br + 0x47, 19);
    uint8_t *t = img + 18 * S;
    t[0] = 255; memcpy(t + 1, "CD001", 5);
    uint8_t *c = img + 19 * S;
    c[0] = 1; c[1] = bios ? 0 : 0xEF; c[30] = 0x55; c[31] = 0xAA;
    unsigned sum = 0;
    for (int i = 0; i < 32; i += 2) sum += (unsigned)c[i] | (unsigned)c[i + 1] << 8;
    unsigned fix = (0x10000 - (sum & 0xffff)) & 0xffff;
    c[28] = (uint8_t)fix; c[29] = (uint8_t)(fix >> 8);
    c[32] = 0x88;
    if (efi_entry && bios) { c[64] = 0x91; c[65] = 0xEF; c[66] = 1; c[96] = 0x88; }
    unsigned o = dots(img + 20 * S, 20, 20);
    o += rec(img + 20 * S + o, "BOOT", 23, S, 1);           /* a decoy: /BOOT isn't /EFI/BOOT */
    rec(img + 20 * S + o, "efi", 21, S, 1);                  /* lower case, as some makers write it */
    o = dots(img + 21 * S, 21, 20);
    rec(img + 21 * S + o, "BOOT", 22, S, 1);
    o = dots(img + 22 * S, 22, 21);
    for (int i = 0; files && files[i]; i++) o += rec(img + 22 * S + o, files[i], 24, 100, 0);
    o = dots(img + 23 * S, 23, 20);
    rec(img + 23 * S + o, "BOOTAA64.EFI;1", 24, 100, 0);    /* in the decoy: must not count */
    return img;
}

static void real_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { fprintf(stderr, "vmiso-test: can't read %s\n", path); fails++; return; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { fails++; return; }
    VmIsoInfo in;
    char js[256];
    CHECK(!vmiso_detect(m, (size_t)st.st_size, &in));
    CHECK(vmiso_json(&in, js, sizeof js) > 0);
    printf("%s: %s\n", path, js);
    munmap(m, (size_t)st.st_size);
}

int main(int argc, char **argv) {
    VmIsoInfo in;
    char js[256];
    size_t len;

    const char *amd[] = { "BOOTX64.EFI;1", "GRUBX64.EFI;1", "MMX64.EFI;1", NULL };
    uint8_t *img = image(&len, "Debian 13.2.0 amd64 n", amd, 1, 1);
    CHECK(!vmiso_detect(img, len, &in));
    CHECK(in.iso && !strcmp(in.label, "Debian 13.2.0 amd64 n"));
    CHECK(in.efi == VMISO_EFI_AMD64 && in.bios && in.eltorito_efi);
    CHECK(!strcmp(vmiso_arch(&in), "amd64"));
    CHECK(vmiso_json(&in, js, sizeof js) > 0 && strstr(js, "\"efi\":[\"amd64\"]") && strstr(js, "\"arch\":\"amd64\""));

    /* an arm64 disc: UEFI only, no BIOS entry */
    const char *arm[] = { "bootaa64.efi", NULL };
    uint8_t *img2 = image(&len, "Ubuntu-Server 24.04 arm64", arm, 0, 0);
    CHECK(!vmiso_detect(img2, len, &in));
    CHECK(in.efi == VMISO_EFI_ARM64 && !in.bios && in.eltorito_efi);
    CHECK(!strcmp(vmiso_arch(&in), "arm64"));
    free(img2);

    /* both: no single answer */
    const char *both[] = { "BOOTX64.EFI", "BOOTAA64.EFI", "BOOTIA32.EFI", NULL };
    img2 = image(&len, "MULTI", both, 1, 0);
    CHECK(!vmiso_detect(img2, len, &in));
    CHECK(in.efi == (VMISO_EFI_AMD64 | VMISO_EFI_ARM64 | VMISO_EFI_IA32) && vmiso_arch(&in) == NULL);
    free(img2);

    /* BIOS only (an old disc): x86 */
    img2 = image(&len, "OLD", NULL, 1, 0);
    CHECK(!vmiso_detect(img2, len, &in));
    CHECK(!in.efi && in.bios && !strcmp(vmiso_arch(&in), "amd64"));
    free(img2);

    /* a label with a quote and a backslash, and control bytes: escaped, replaced */
    img2 = image(&len, "A\"B\\C\x01", NULL, 0, 0);
    CHECK(!vmiso_detect(img2, len, &in));
    CHECK(!strcmp(in.label, "A\"B\\C?"));
    CHECK(vmiso_json(&in, js, sizeof js) > 0 && strstr(js, "\"label\":\"A\\\"B\\\\C?\""));
    free(img2);

    /* not an ISO; too short; empty */
    uint8_t junk[40000];
    memset(junk, 0x41, sizeof junk);
    CHECK(!vmiso_detect(junk, sizeof junk, &in) && !in.iso);
    CHECK(!vmiso_detect(img, 16 * S + 100, &in) && !in.iso);
    CHECK(!vmiso_detect(NULL, 0, &in) && !in.iso);
    CHECK(vmiso_detect(NULL, 5, &in) == -1);

    /* hostile: the root past the end, a huge size, a directory pointing at itself, a record that overruns, a catalog
     * past the end, an image cut in the middle of a directory */
    uint8_t *h = malloc(len);
    memcpy(h, img, len); put32(h + 16 * S + 156 + 2, 0xfffffff0u);
    CHECK(!vmiso_detect(h, len, &in) && in.iso && !in.efi);
    memcpy(h, img, len); put32(h + 16 * S + 156 + 10, 0xffffffffu);
    CHECK(!vmiso_detect(h, len, &in) && in.efi == VMISO_EFI_AMD64);
    memcpy(h, img, len); put32(h + 21 * S + 34 + 34 + 2, 21);         /* EFI/BOOT → EFI itself */
    CHECK(!vmiso_detect(h, len, &in) && !in.efi);
    memcpy(h, img, len); h[20 * S + 68] = 255; put32(h + 16 * S + 156 + 10, 300);   /* the third record runs past the end */
    CHECK(!vmiso_detect(h, len, &in));
    memcpy(h, img, len); put32(h + 17 * S + 0x47, 0x7fffffffu);
    CHECK(!vmiso_detect(h, len, &in) && !in.bios);
    memcpy(h, img, len); h[19 * S + 28] ^= 1;                            /* a bad checksum: the catalog isn't read */
    CHECK(!vmiso_detect(h, len, &in) && !in.bios);
    memcpy(h, img, len); h[19 * S + 64] = 0x90; h[19 * S + 66] = 0xff; h[19 * S + 67] = 0xff;   /* 65535 entries */
    CHECK(!vmiso_detect(h, len, &in));
    CHECK(!vmiso_detect(img, 22 * S + 40, &in) && !in.efi);

    /* random mutations of the good image, and random sizes */
    srand(1234);
    for (int it = 0; it < 20000; it++) {
        memcpy(h, img, len);
        int n = 1 + rand() % 24;
        for (int k = 0; k < n; k++) {
            size_t at = 16 * S + (size_t)rand() % (len - 16 * S);
            h[at] = (uint8_t)rand();
        }
        size_t cut = it % 5 ? len : (size_t)rand() % len;
        CHECK(!vmiso_detect(h, cut, &in));
        CHECK(vmiso_json(&in, js, sizeof js) > 0);
    }
    free(h);
    free(img);
    for (int i = 1; i < argc; i++) real_file(argv[i]);
    if (fails) { fprintf(stderr, "vmiso-test: %d failed\n", fails); return 1; }
    printf("vmiso-test: ok\n");
    return 0;
}
