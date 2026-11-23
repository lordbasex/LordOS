/* vmmem_test.c — how a machine's memory is backed (src/vm/vmmem.h): /proc/meminfo's hugetlb fields, and hugetlb only
 * when all of the memory fits in free, unreserved 2 MB pages */
#include "../src/vm/vmmem.h"
#include <stdio.h>

static int n, bad;
#define CHECK(c, ...) do { n++; if (!(c)) { bad++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void) {
    VmHuge h;
    const char *none = "MemTotal:       65738652 kB\nAnonHugePages:    405504 kB\nHugePages_Total:       0\nHugePages_Free:        0\n"
                       "HugePages_Rsvd:        0\nHugePages_Surp:        0\nHugepagesize:       2048 kB\nHugetlb:               0 kB\n";
    vmmem_parse(none, &h);
    CHECK(h.free == 0 && h.rsvd == 0 && h.size_kb == 2048, "a computer with no hugetlb pages: %ld %ld %ld", h.free, h.rsvd, h.size_kb);
    CHECK(vmmem_choose(8192, 0, &h) == VMMEM_ANON, "no pages, no shared folder: QEMU's own memory (THP)");
    CHECK(vmmem_choose(8192, 1, &h) == VMMEM_SHARED, "no pages, a shared folder: the shared memfd");
    const char *some = "HugePages_Total:    4200\nHugePages_Free:     4200\nHugePages_Rsvd:        0\nHugepagesize:       2048 kB\n";
    vmmem_parse(some, &h);
    CHECK(h.free == 4200 && h.size_kb == 2048, "4200 free pages");
    CHECK(vmmem_choose(8192, 1, &h) == VMMEM_HUGETLB, "8 GB in 4200 free 2 MB pages: hugetlb");
    CHECK(vmmem_choose(8192, 0, &h) == VMMEM_HUGETLB, "hugetlb without a shared folder too");
    CHECK(vmmem_choose(8400, 0, &h) == VMMEM_HUGETLB && vmmem_choose(8402, 0, &h) == VMMEM_ANON, "exactly the pages there are, not one more");
    CHECK(vmmem_choose(8191, 0, &h) == VMMEM_ANON, "a size that isn't a whole number of 2 MB pages: not hugetlb");
    const char *taken = "HugePages_Free:     4200\nHugePages_Rsvd:     1000\nHugepagesize:       2048 kB\n";
    vmmem_parse(taken, &h);
    CHECK(vmmem_choose(8192, 1, &h) == VMMEM_SHARED, "pages reserved by another machine don't count");
    CHECK(vmmem_choose(6400, 1, &h) == VMMEM_HUGETLB, "what's left unreserved does");
    const char *gig = "HugePages_Free:     16\nHugePages_Rsvd:     0\nHugepagesize:    1048576 kB\n";
    vmmem_parse(gig, &h);
    CHECK(vmmem_choose(8192, 0, &h) == VMMEM_ANON, "1 GB default pages: not used");
    vmmem_parse("", &h);
    CHECK(vmmem_choose(4096, 0, &h) == VMMEM_ANON && vmmem_choose(4096, 1, &h) == VMMEM_SHARED, "no /proc/meminfo: the usual backing");
    CHECK(vmmem_choose(4096, 1, NULL) == VMMEM_SHARED, "nothing read: the usual backing");
    vmmem_parse("HugePages_Free: -5\nHugePages_Rsvd: -1\nHugepagesize: 2048 kB\n", &h);
    CHECK(h.free == 0 && h.rsvd == 0 && vmmem_choose(2, 0, &h) == VMMEM_ANON, "nonsense numbers count as none");
    if (bad) { fprintf(stderr, "vmmem-test: %d of %d failed\n", bad, n); return 1; }
    printf("vmmem-test: %d checks passed\n", n);
    return 0;
}
