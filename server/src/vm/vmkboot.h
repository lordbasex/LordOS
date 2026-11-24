/* vmkboot.h — direct kernel boot of an installed Linux guest (docs/VIRTUALIZATION.md 6.1, "Start Linux directly";
 * machine.json schema 22 "kernel_boot"). Pure checks, shared by lord-vm (--set's copy and the start) and tested by
 * vmkboot-test under ASan and UBSan.
 *
 * The person picks a kernel, an optional initrd and a command line (Configure → Boot, or `lord-vm --set`). lord-vm copies
 * the files into the machine's disks/boot/ (QEMU's Landlock reads only disks/), so the machine is whole on its own and the
 * person's file can change or go without touching it. The files are the guest's own: what they could do, they do to the
 * guest, inside QEMU's sandbox, like any disk. These checks only stop a wrong pick early with a clear sentence (QEMU's
 * own message for a non-kernel is cryptic) and keep the command line one printable argument. */
#ifndef LORD_VM_KBOOT_H
#define LORD_VM_KBOOT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* how many bytes of a kernel's start kboot_kernel_ok looks at */
#define VMKBOOT_HEAD 0x240

/* the command line: 1 to VM_KBOOT_CMDLINE_MAX (1023) printable ASCII characters, with no trailing newline. It goes to
 * QEMU as one argument of its own (-append), never through a shell, so quotes and commas are the guest's business */
static inline int kboot_cmdline_ok(const char *s) {
    size_t n = 0;
    if (!s || !*s) return 0;
    for (; s[n]; n++) {
        if (n >= 1023) return 0;
        unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 1;
}

/* a command line read from a file the person picked or typed: a final newline (an editor's, `cat /proc/cmdline > f`'s)
 * and spaces at either end are dropped in place; then kboot_cmdline_ok */
static inline int kboot_cmdline_clean(char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
    return kboot_cmdline_ok(s);
}

/* the start of a kernel file, for the machine's architecture (amd64: 1, arm64: 0, as VM_ARCH_*): amd64 takes a bzImage
 * (the setup header's "HdrS" at 0x202, what QEMU's -kernel loads on x86); arm64 an Image ("ARM\x64" at 0x38), a gzip'd
 * Image (1f 8b) or an EFI zboot image ("MZ", "zimg" at 4: Debian's arm64 kernels since 6.x), each of which QEMU's arm
 * loader takes. Returns 1 when it looks like one */
static inline int kboot_kernel_ok(const uint8_t *h, size_t n, int amd64) {
    if (!h) return 0;
    if (amd64) return n >= 0x206 && !memcmp(h + 0x202, "HdrS", 4);
    if (n >= 0x3c && !memcmp(h + 0x38, "ARM\x64", 4)) return 1;
    if (n >= 8 && h[0] == 'M' && h[1] == 'Z' && !memcmp(h + 4, "zimg", 4)) return 1;
    if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) return 1;
    return 0;
}

#endif
