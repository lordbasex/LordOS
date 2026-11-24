/* vmiso.h — what an ISO a person chose boots on (docs/VIRTUALIZATION.md 5.2): its volume label, the El Torito boot
 * catalog's platforms, the architecture of its UEFI boot files and whether it is a virtio-win guest's install disc.
 *
 * The ISO is untrusted. vmiso_probe reads it only through the descriptor it is given, with pread of bounded sizes at
 * offsets checked against the file's size; it follows fixed paths only (EFI/BOOT, sources), never a whole tree, so a
 * directory that points at itself can't loop; it counts its reads and stops at a budget. No allocation is larger than
 * VMISO_DIR_MAX. Pure: no globals, no other I/O, so tests/vmiso_test.c fuzzes it under ASan and UBSan. */
#ifndef VMISO_H
#define VMISO_H

#include <stddef.h>
#include <stdint.h>

/* UEFI boot files (EFI/BOOT/BOOT<arch>.EFI), by the machine their PE header names */
enum { VMISO_X64 = 1, VMISO_AA64 = 2, VMISO_IA32 = 4, VMISO_RISCV64 = 8 };

#define VMISO_DIR_MAX (1u << 20)       /* a directory's bytes read at most */
#define VMISO_READS 4096               /* reads at most, all of them */
#define VMISO_BYTES (24u << 20)        /* bytes read at most, all of them */

typedef struct {
    int iso9660;          /* a primary volume descriptor */
    int udf;              /* a UDF volume (its anchor at sector 256) that we could read */
    char label[33];       /* the primary volume's label, printable ASCII, trailing spaces gone */
    int bios;             /* El Torito: a bootable entry for x86 BIOS (platform 0x00) */
    int efi;              /* El Torito: a bootable entry for UEFI (platform 0xEF) */
    unsigned efi_named;   /* BOOT<arch>.EFI files found by their names (VMISO_*) */
    unsigned efi_pe;      /* the machines their PE headers name: the tree's files and the El Torito ESP image's */
    int windows;          /* sources/install.wim, .esd or .swm */
    unsigned reads;       /* how many reads it took (tests) */
} VmIso;

/* 0 when fd is an ISO 9660 or UDF image (out filled in as far as it could be read), -1 otherwise. max_reads (0 for
 * VMISO_READS) caps the reads further: a file on a network mount gets a few, so a slow server can't hold the caller */
int vmiso_probe(int fd, VmIso *out, unsigned max_reads);

/* the same over the whole file mapped in memory (size bytes): what lord-vm's probe runs under seccomp strict mode, where
 * no pread is allowed (lord-vm --iso-probe) */
int vmiso_probe_map(const void *map, uint64_t size, VmIso *out, unsigned max_reads);

/* The probe runs out of lord-server (docs/VIRTUALIZATION.md 5.2): `lord-vm --iso-probe MAX` with the ISO on fd 0 maps it,
 * enters seccomp strict mode (read, write, exit) and writes one fixed answer of VMISO_ANSWER bytes on fd 1:
 *   0  "LVMISO1\0"   8  flags (VMISO_A_*)   12 efi_named   16 efi_pe   20 reads   24 label, 32 bytes + 8 zero, NUL-padded
 * (little-endian). lord-server reads only that: vmiso_answer_decode refuses anything else whole. */
#define VMISO_ANSWER 64
#define VMISO_MAGIC "LVMISO1"
enum { VMISO_A_OK = 1, VMISO_A_ISO = 2, VMISO_A_UDF = 4, VMISO_A_BIOS = 8, VMISO_A_EFI = 16, VMISO_A_WINDOWS = 32, VMISO_A_ALL = 63 };
#define VMISO_ARCH_ALL (VMISO_X64 | VMISO_AA64 | VMISO_IA32 | VMISO_RISCV64)
/* rc: vmiso_probe's */
void vmiso_answer_encode(int rc, const VmIso *v, uint8_t out[VMISO_ANSWER]);
/* 0 an ISO (v filled), 1 the probe said not an ISO, -1 not a valid answer (v zeroed) */
int vmiso_answer_decode(const uint8_t *in, size_t n, VmIso *v);

/* lord-server's side: lordvm (the installed lord-vm's path) --iso-probe max_reads with fd as its ISO, its answer read
 * within timeout_ms (then it is killed either way) and decoded. vmiso_answer_decode's return; -1 when it couldn't run */
int vmiso_run(const char *lordvm, int fd, unsigned max_reads, int timeout_ms, VmIso *v);

/* the architecture Machines names ("amd64", "arm64", "riscv64") its boot files are for: the PE headers first, then the
 * files' names, then El Torito's x86 BIOS entry; "" when nothing says, or when they name both amd64 and arm64 */
const char *vmiso_arch(const VmIso *v);

/* a PE image's machine (the 6 bytes after "PE\0\0") as VMISO_*, 0 for another */
unsigned vmiso_pe_machine(unsigned machine);

#endif
