/* vmkboot-test: direct kernel boot's checks (src/vm/vmkboot.h; docs/VIRTUALIZATION.md 6.1, "Start Linux directly"): the
 * command line, its cleaning, a kernel's start on amd64 and arm64, and a random pass over both. Under ASan and UBSan */
#include "vm/vmkboot.h"
#include <stdio.h>
#include <stdlib.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

int main(void) {
    /* the command line */
    CHECK(kboot_cmdline_ok("root=UUID=0b1e ro quiet"));
    CHECK(kboot_cmdline_ok("x"));
    CHECK(!kboot_cmdline_ok(""));
    CHECK(!kboot_cmdline_ok(NULL));
    CHECK(!kboot_cmdline_ok("root=/dev/vda1\n"));
    CHECK(!kboot_cmdline_ok("a\tb"));
    CHECK(!kboot_cmdline_ok("caf\xc3\xa9"));
    CHECK(!kboot_cmdline_ok("a\x7f"));
    char big[1100];
    memset(big, 'a', sizeof big); big[1023] = 0;
    CHECK(kboot_cmdline_ok(big));
    memset(big, 'a', sizeof big); big[1024] = 0;
    CHECK(!kboot_cmdline_ok(big));
    char c1[] = "  BOOT_IMAGE=/vmlinuz root=/dev/sda2 ro quiet\n";
    CHECK(kboot_cmdline_clean(c1) && !strcmp(c1, "BOOT_IMAGE=/vmlinuz root=/dev/sda2 ro quiet"));
    char c2[] = "\n\r\n";
    CHECK(!kboot_cmdline_clean(c2) && !c2[0]);
    char c3[] = "a\nb";
    CHECK(!kboot_cmdline_clean(c3));
    CHECK(!kboot_cmdline_clean(NULL));
    /* a kernel's start */
    uint8_t h[VMKBOOT_HEAD];
    memset(h, 0, sizeof h);
    memcpy(h + 0x202, "HdrS", 4);
    CHECK(kboot_kernel_ok(h, sizeof h, 1));
    CHECK(!kboot_kernel_ok(h, 0x205, 1));          /* too short to hold the magic */
    CHECK(!kboot_kernel_ok(h, sizeof h, 0));       /* a bzImage isn't an arm64 kernel */
    memset(h, 0, sizeof h);
    memcpy(h + 0x38, "ARM\x64", 4);
    CHECK(kboot_kernel_ok(h, sizeof h, 0) && !kboot_kernel_ok(h, sizeof h, 1));
    CHECK(!kboot_kernel_ok(h, 0x3b, 0));
    memset(h, 0, sizeof h);
    memcpy(h, "MZ\0\0zimg", 8);
    CHECK(kboot_kernel_ok(h, 8, 0) && !kboot_kernel_ok(h, 8, 1));
    CHECK(!kboot_kernel_ok(h, 7, 0));
    h[0] = 0x1f; h[1] = 0x8b;
    CHECK(kboot_kernel_ok(h, 2, 0) && !kboot_kernel_ok(h, 1, 0) && !kboot_kernel_ok(h, 2, 1));
    memset(h, 0, sizeof h);
    CHECK(!kboot_kernel_ok(h, sizeof h, 0) && !kboot_kernel_ok(h, sizeof h, 1));
    CHECK(!kboot_kernel_ok(NULL, 100, 1));
    /* random bytes and lengths: never read past n (ASan), and a command line with a control byte never passes */
    srand(21);
    for (int i = 0; i < 200000; i++) {
        size_t n = (size_t)(rand() % (VMKBOOT_HEAD + 1));
        uint8_t *b = malloc(n ? n : 1);
        for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rand();
        if (rand() % 4 == 0 && n >= 0x206) memcpy(b + 0x202, "HdrS", 4);
        (void)kboot_kernel_ok(b, n, rand() & 1);
        free(b);
        size_t L = (size_t)(rand() % 1100);
        char *s = malloc(L + 1);
        int bad = 0;
        for (size_t k = 0; k < L; k++) { s[k] = (char)(0x20 + rand() % 0x5f); if (rand() % 50 == 0) { s[k] = (char)(1 + rand() % 0x1f); bad = 1; } }
        s[L] = 0;
        int ok = kboot_cmdline_ok(s);
        CHECK(ok == (!bad && L >= 1 && L <= 1023));
        (void)kboot_cmdline_clean(s);
        free(s);
    }
    printf("vmkboot-test: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
