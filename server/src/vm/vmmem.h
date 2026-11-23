/* vmmem.h — how a machine's memory is backed (docs/VIRTUALIZATION.md 3.7.2, "Guest memory and huge pages"). Pure logic,
 * used by lord-vm's QEMU command line (lordvm.c mem_args) and tested by vmmem-test.
 *
 *   - Without a shared folder or the transfer folder: QEMU's own anonymous memory. QEMU asks for transparent huge pages
 *     on all of it (MADV_HUGEPAGE), which the kernel gives with THP "always" or "madvise" (Debian's default): 2 MB pages
 *     with no setting and no privilege.
 *   - With one (virtiofsd shares the guest's memory over vhost-user): shared memory, memory-backend-ram with share=on,
 *     which QEMU 10.0 and later (every machine type lord-vm pins is 10.0 or later) back with a memfd it maps 2 MB
 *     aligned. Shared memory gets huge pages only with /sys/kernel/mm/transparent_hugepage/shmem_enabled at "advise"
 *     (what the lordos package sets: lordos-thp.conf in tmpfiles.d; the kernel's default is "never") or "always";
 *     QEMU's MADV_HUGEPAGE asks for them. Not memory-backend-memfd: QEMU maps that one only 4 KB aligned, so the
 *     kernel never gives it a 2 MB page whatever the setting (measured on QEMU 10.0.13, 3.7.2).
 *   - Either way, when the administrator reserved enough 2 MB hugetlb pages (vm.nr_hugepages) and enough of them are
 *     free and unreserved, the memory is a hugetlb memfd of 2 MB pages, allocated whole at start (prealloc: a guest
 *     never faults on a page that isn't there). Otherwise the backing above: never a failed start for want of pages. */
#ifndef LORD_VM_MEM_H
#define LORD_VM_MEM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VMMEM_ANON = 0, VMMEM_SHARED, VMMEM_HUGETLB };

typedef struct { long free, rsvd, size_kb; } VmHuge;

/* HugePages_Free, HugePages_Rsvd and Hugepagesize from /proc/meminfo's text (missing fields stay 0) */
static inline void vmmem_parse(const char *text, VmHuge *h) {
    memset(h, 0, sizeof *h);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, "HugePages_Free:", 15)) h->free = strtol(p + 15, NULL, 10);
        else if (!strncmp(p, "HugePages_Rsvd:", 15)) h->rsvd = strtol(p + 15, NULL, 10);
        else if (!strncmp(p, "Hugepagesize:", 13)) h->size_kb = strtol(p + 13, NULL, 10);
    }
    if (h->free < 0) h->free = 0;
    if (h->rsvd < 0) h->rsvd = 0;
}

/* the backing for a machine of memory_mb with (shared) or without a shared folder, given the hugetlb pages free now:
 * hugetlb only with 2 MB pages and the whole memory free and unreserved */
static inline int vmmem_choose(int memory_mb, int shared, const VmHuge *h) {
    if (h && h->size_kb == 2048 && memory_mb > 0 && h->free > h->rsvd && (h->free - h->rsvd) >= (long)memory_mb / 2 && memory_mb % 2 == 0)
        return VMMEM_HUGETLB;
    return shared ? VMMEM_SHARED : VMMEM_ANON;
}

/* the selected value of /sys/kernel/mm/transparent_hugepage/shmem_enabled ("always within_size [advise] never deny
 * force") into out (at most outlen - 1 characters); returns 1 when shared memory that asks for huge pages (QEMU's
 * MADV_HUGEPAGE) gets them, 0 when not or unknown */
static inline int vmmem_shmem_huge(const char *text, char *out, size_t outlen) {
    const char *a = text ? strchr(text, '[') : NULL, *b = a ? strchr(a, ']') : NULL;
    if (outlen) out[0] = 0;
    if (!a || !b || b - a - 1 <= 0 || (size_t)(b - a - 1) >= outlen) return 0;
    memcpy(out, a + 1, b - a - 1);
    out[b - a - 1] = 0;
    return !strcmp(out, "always") || !strcmp(out, "within_size") || !strcmp(out, "advise") || !strcmp(out, "force");
}

/* QEMU's -object for the backing (the machine's -machine names it "mem"; VMMEM_ANON has none: "" is returned) */
static inline const char *vmmem_object(char *buf, size_t len, int backing, int memory_mb, int shared) {
    if (!len) return buf;
    buf[0] = 0;
    if (backing == VMMEM_SHARED) snprintf(buf, len, "memory-backend-ram,id=mem,size=%dM,share=on", memory_mb);
    else if (backing == VMMEM_HUGETLB)
        snprintf(buf, len, "memory-backend-memfd,id=mem,size=%dM,share=%s,hugetlb=on,hugetlbsize=2M,prealloc=on", memory_mb, shared ? "on" : "off");
    return buf;
}

#endif
