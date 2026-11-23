/* vmmem.h — how a machine's memory is backed (docs/VIRTUALIZATION.md 3.7.2, "Guest memory and huge pages"). Pure logic,
 * used by lord-vm's QEMU command line (lordvm.c mem_args) and tested by vmmem-test.
 *
 *   - Without a shared folder or the transfer folder: QEMU's own anonymous memory. QEMU asks for transparent huge pages
 *     on all of it (MADV_HUGEPAGE), which the kernel gives with THP "always" or "madvise" (Debian's default): 2 MB pages
 *     with no setting and no privilege.
 *   - With one (virtiofsd shares the guest's memory over vhost-user): a memfd with share=on. Shared memory gets huge
 *     pages only with /sys/kernel/mm/transparent_hugepage/shmem_enabled ("never" by default), a system setting lord-vm
 *     doesn't change.
 *   - Either way, when the administrator reserved enough 2 MB hugetlb pages (vm.nr_hugepages) and enough of them are
 *     free and unreserved, the memory is a hugetlb memfd of 2 MB pages, allocated whole at start (prealloc: a guest
 *     never faults on a page that isn't there). Otherwise the backing above: never a failed start for want of pages. */
#ifndef LORD_VM_MEM_H
#define LORD_VM_MEM_H

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

#endif
