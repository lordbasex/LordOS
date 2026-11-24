/* guesttools_test.c — the LordOS Guest Tools disc (tools/guest/mkguestiso.c, mk/vm.mk; docs/VIRTUALIZATION.md 6.17),
 * under ASan and UBSan:
 *   - build/lordos-guest-tools.iso reads back with lord-vm's own ISO 9660 reader (vmunattend.c): label LORDOS_TOOLS,
 *     install.sh, lguest, arch.txt, sha256.txt and readme.txt, each the bytes of its source;
 *   - its SHA-256 is the one compiled into lord-vm (build/gen/guesttools.h), and sha256.txt is lguest's;
 *   - mkguestiso makes the same bytes twice, and refuses a bad label, a name that isn't 8.3 of A-Z 0-9 _, a name given
 *     twice and a missing file.
 * Run from server/ (make test). */
#include "vm/vmunattend.h"
#include "pkg/crypto.h"
#include "guesttools.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned char *slurp(const char *p, size_t *n) {
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return NULL; }
    unsigned char *b = malloc((size_t)st.st_size + 1);
    if (b && read(fd, b, (size_t)st.st_size) != st.st_size) { free(b); b = NULL; }
    close(fd);
    *n = (size_t)st.st_size;
    return b;
}

static void hex(const unsigned char d[32], char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }

/* the file at path on the disc has exactly the bytes of src */
static void same_file(int fd, const char *path, const char *src) {
    uint64_t off = 0, size = 0;
    size_t n = 0;
    unsigned char *want = slurp(src, &n);
    CHECK(want != NULL, "can't read %s", src);
    CHECK(!vmu_iso_find(fd, path, &off, &size), "%s is on the disc", path);
    if (want && size == n) {
        unsigned char *got = malloc(n ? n : 1);
        CHECK(got && pread(fd, got, n, (off_t)off) == (ssize_t)n && !memcmp(got, want, n), "%s has the bytes of %s", path, src);
        free(got);
    } else CHECK(0, "%s: %llu bytes, %s has %zu", path, (unsigned long long)size, src, n);
    free(want);
}

static int run(char *const argv[]) {
    pid_t p = fork();
    if (!p) { int null = open("/dev/null", O_WRONLY); dup2(null, 2); execv(argv[0], argv); _exit(127); }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

int main(void) {
    const char *iso = "build/lordos-guest-tools.iso";
    int fd = open(iso, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0, "%s is there (make build/lordos-guest-tools.iso)", iso);
    if (fd < 0) return 1;
    char label[33];
    CHECK(!vmu_iso_label(fd, label) && !strcmp(label, "LORDOS_TOOLS"), "the label is LORDOS_TOOLS (%s)", label);
    same_file(fd, "install.sh", "../tools/guest/install.sh");
    same_file(fd, "readme.txt", "../tools/guest/guest-tools-readme.txt");
    same_file(fd, "lguest", "build/guesttools/lguest");
    same_file(fd, "arch.txt", "build/guesttools/arch.txt");
    same_file(fd, "sha256.txt", "build/guesttools/sha256.txt");
    uint64_t off, size;
    CHECK(vmu_iso_find(fd, "lordos-guest", &off, &size), "nothing else on the disc (lordos-guest)");
    CHECK(vmu_iso_find(fd, "../install.sh", &off, &size), "no path outside the root");
    close(fd);

    /* the hash lord-vm checks */
    size_t n;
    unsigned char *img = slurp(iso, &n), d[32];
    char h[65];
    sha256(img, n, d);
    hex(d, h);
    CHECK(!strcmp(h, GUEST_TOOLS_SHA256), "the image's SHA-256 is the one compiled into lord-vm (%.16s…)", h);
    free(img);
    unsigned char *arch = slurp("build/guesttools/arch.txt", &n);
    CHECK(arch && n == strlen(GUEST_TOOLS_ARCH) + 1 && !memcmp(arch, GUEST_TOOLS_ARCH, n - 1), "arch.txt says %s", GUEST_TOOLS_ARCH);
    free(arch);
    unsigned char *lg = slurp("build/guesttools/lguest", &n), *sums = slurp("build/guesttools/sha256.txt", &n);
    if (lg && sums) {
        size_t ln;
        free(lg);
        lg = slurp("build/guesttools/lguest", &ln);
        sha256(lg, ln, d);
        hex(d, h);
        CHECK(!strncmp((char *)sums, h, 64) && !strncmp((char *)sums + 64, "  lguest\n", 9), "sha256.txt is lguest's");
    }
    free(lg); free(sums);

    /* the writer: the same bytes twice; refusals */
    char *a1[] = { "build/mkguestiso", "build/guesttools/t1.iso", "LORDOS_TOOLS", "INSTALL.SH=../tools/guest/install.sh", "LGUEST=build/guesttools/lguest", NULL };
    char *a2[] = { "build/mkguestiso", "build/guesttools/t2.iso", "LORDOS_TOOLS", "LGUEST=build/guesttools/lguest", "INSTALL.SH=../tools/guest/install.sh", NULL };
    CHECK(run(a1) == 0 && run(a2) == 0, "mkguestiso makes two images");
    size_t n1, n2;
    unsigned char *i1 = slurp("build/guesttools/t1.iso", &n1), *i2 = slurp("build/guesttools/t2.iso", &n2);
    CHECK(i1 && i2 && n1 == n2 && !memcmp(i1, i2, n1), "the same files in another order make the same bytes");
    free(i1); free(i2);
    unlink("build/guesttools/t1.iso"); unlink("build/guesttools/t2.iso");
    const char *bad[][3] = {
        { "lordos_tools", "INSTALL.SH=../tools/guest/install.sh", "a label in lower case" },
        { "LORDOS TOOLS", "INSTALL.SH=../tools/guest/install.sh", "a label with a space" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", "INSTALL.SH=../tools/guest/install.sh", "a label of 36" },
        { "LORDOS_TOOLS", "install.sh=../tools/guest/install.sh", "a name in lower case" },
        { "LORDOS_TOOLS", "LONGNAMEXX.SH=../tools/guest/install.sh", "a name of 10" },
        { "LORDOS_TOOLS", "A.LONG=../tools/guest/install.sh", "an extension of 4" },
        { "LORDOS_TOOLS", "../X.SH=../tools/guest/install.sh", "a name with a path" },
        { "LORDOS_TOOLS", "X.SH=build/guesttools/no-such-file", "a missing file" },
        { "LORDOS_TOOLS", "NOEQUALS", "an argument without =" },
        { "LORDOS_TOOLS", ".SH=../tools/guest/install.sh", "an empty name" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char *a[] = { "build/mkguestiso", "build/guesttools/bad.iso", (char *)bad[i][0], (char *)bad[i][1], NULL };
        CHECK(run(a) != 0 && access("build/guesttools/bad.iso", F_OK), "refused: %s", bad[i][2]);
        unlink("build/guesttools/bad.iso");
    }
    char *twice[] = { "build/mkguestiso", "build/guesttools/bad.iso", "LORDOS_TOOLS", "A.SH=../tools/guest/install.sh", "A.SH=../tools/guest/install.sh", NULL };
    CHECK(run(twice) != 0, "refused: a name given twice");
    unlink("build/guesttools/bad.iso");
    printf("guesttools-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
