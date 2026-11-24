/* vmiso.h — what an installer disc starts on, read from its ISO 9660 image (docs/VIRTUALIZATION.md 5.2, "Detect").
 *
 * The New Machine assistant's Detect step: the volume label names the system, the disc's UEFI boot files
 * (/EFI/BOOT/BOOT<arch>.EFI) say which processors it starts on, and El Torito's boot catalog whether a BIOS (or a UEFI
 * image) starts it. The image is hostile: every offset and length is checked against the mapping, directories are read
 * at most VMISO_DIR_MAX bytes and VMISO_RECORDS records, and nothing follows a pointer twice. lord-vm --iso-detect runs
 * this on a read-only mapping after it has dropped to seccomp's strict mode (read, write, exit only).
 */
#ifndef LORDOS_VMISO_H
#define LORDOS_VMISO_H

#include <stddef.h>
#include <stdint.h>

#define VMISO_SECTOR    2048u
#define VMISO_DIR_MAX   (256u * 1024u)   /* bytes of one directory read */
#define VMISO_RECORDS   4096             /* records looked at per directory */

enum {                                   /* bits of VmIsoInfo.efi: the UEFI boot files found */
    VMISO_EFI_AMD64 = 1, VMISO_EFI_ARM64 = 2, VMISO_EFI_IA32 = 4, VMISO_EFI_ARM = 8, VMISO_EFI_RISCV64 = 16,
};

typedef struct {
    int iso;                             /* 1: a primary volume descriptor was found ("CD001") */
    char label[33];                      /* its volume label, printable ASCII, trailing spaces cut */
    unsigned efi;                        /* VMISO_EFI_* */
    int bios;                            /* El Torito's catalog has a bootable x86 (BIOS) entry */
    int eltorito_efi;                    /* … and an EFI entry (platform 0xEF) */
} VmIsoInfo;

/* Reads img[0..len). 0 ok (info filled; info->iso may still be 0: not an ISO 9660 image), -1 bad arguments. Never reads
 * outside img. */
int vmiso_detect(const uint8_t *img, size_t len, VmIsoInfo *info);

/* The one architecture info says the disc starts on, as the app names it ("amd64", "arm64"), or NULL when it names none
 * or both */
const char *vmiso_arch(const VmIsoInfo *info);

/* info as one JSON line into out (n bytes): {"iso":true,"label":"…","efi":["amd64"],"bios":true,"eltoritoEfi":false}.
 * Returns the length, or -1 if it doesn't fit. */
int vmiso_json(const VmIsoInfo *info, char *out, size_t n);

#endif
