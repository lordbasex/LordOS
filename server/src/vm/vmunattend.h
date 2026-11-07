/* vmunattend.h — unattended installs from the New Machine assistant's cards (docs/VIRTUALIZATION.md 4.1 and 3.9,
 * "Unattended install"): the answer file of the system's own installer (Debian preseed, Ubuntu autoinstall, Fedora
 * kickstart) made from what the person typed, and how it reaches the installer without a network server.
 *
 *   Debian (preseed)       preseed.cfg inside the installer's initrd (a second cpio archive after the maker's one,
 *                          which the kernel unpacks too); no seed disk
 *   Ubuntu (autoinstall)   user-data and meta-data on a FAT seed disk labelled CIDATA (cloud-init's NoCloud)
 *   Fedora (kickstart)     ks.cfg on a FAT seed disk labelled OEMDRV
 *
 * The first start boots the installer's own kernel and initrd, read from the ISO, with the kernel command line of the
 * catalog entry (QEMU -kernel/-initrd/-append; no boot menu to edit) and the ISO in the CD drive as usual. Its files are
 * in <machine>/disks/unattended: vmlinuz, initrd, cmdline and (Ubuntu, Fedora) seed.img. When the installer restarts
 * the machine at its end, lord-vm removes that folder and the ISO, and the next start is the installed system. The
 * password is in these files only as a SHA-512 crypt hash; the person's text never reaches them.
 *
 * Pure functions over bytes and one file reader (the ISO), apart for their test (tests/vmunattend_test.c). */
#ifndef LORD_VM_UNATTEND_H
#define LORD_VM_UNATTEND_H

#include <stddef.h>
#include <stdint.h>

enum { VMU_PRESEED = 1, VMU_AUTOINSTALL, VMU_KICKSTART };

typedef struct {
    int kind;
    char user[33];          /* [a-z_][a-z0-9_-]{0,31}, not a system name */
    char fullname[65];      /* letters, digits, spaces and . ' - (UTF-8 letters allowed): no quotes, no control chars */
    char hash[128];         /* crypt(3) hash: $id$salt$hash */
    char hostname[64];      /* one RFC 1123 label */
    char timezone[64];      /* Area/City */
    char keymap[32];        /* xkb layout */
    char variant[32];       /* xkb variant, may be "" */
    char locale[32];        /* ll_CC.UTF-8 */
    char release[16];       /* the system's release (Fedora's mirror list: "45") */
    char arch[8];           /* amd64 | arm64 */
    char kernel[128];       /* in the ISO, relative: install.amd/vmlinuz */
    char initrd[128];
    char append[512];       /* the kernel command line; {label} is the ISO's volume label */
    int desktop;            /* install the system's own desktop (default 1) */
} VmUnattend;

/* the spec lord-server writes (JSON, see the top of lordvm.c): 0, or -1 with why in err */
int vmu_parse(const char *json, size_t n, VmUnattend *u, char *err, size_t errlen);

/* the answer file's text (malloc, NUL-terminated) and its name in the guest: "preseed.cfg", "user-data" or "ks.cfg" */
char *vmu_answer(const VmUnattend *u, const char **name);
/* cloud-init's meta-data (autoinstall only; malloc) */
char *vmu_meta_data(const VmUnattend *u, const char *instance);

/* the kernel command line with {label} replaced; 0, or -1 if it doesn't fit or the label isn't a plain word */
int vmu_cmdline(const VmUnattend *u, const char *label, char *out, size_t n);

typedef struct { const char *name; const void *data; size_t len; } VmuFile;

/* a FAT12 image (2 MiB) with this volume label (up to 11 of A-Z 0-9 _ -) and these files in its root, each with a long
 * (VFAT) name so "user-data" and "ks.cfg" read as such: 0 and *img (malloc), or -1 */
int vmu_fat_image(const char *label, const VmuFile *files, int n, uint8_t **img, size_t *len);
/* a cpio "newc" archive of these files (mode 0644, in the root) with its trailer: 0 and *out (malloc), or -1 */
int vmu_cpio(const VmuFile *files, int n, uint8_t **out, size_t *len);

/* ISO 9660: the volume label (PVD's volume id, trimmed) and a file found by its path (Rock Ridge names, then Joliet,
 * then the plain names without ";1"): 0 with its byte offset and size, or -1. Directories are read with bounds (1 MiB
 * each, 8 levels), multi-extent files are refused. */
int vmu_iso_label(int fd, char out[33]);
int vmu_iso_find(int fd, const char *path, uint64_t *off, uint64_t *size);

/* what --create --unattended does: <disks>/unattended/{vmlinuz,initrd,cmdline[,seed.img]} from the ISO and the spec;
 * 0, or -1 with why in err (the folder is removed then) */
int vmu_prepare(const char *disks, const char *iso, const VmUnattend *u, const char *instance, char *err, size_t errlen);

#endif
