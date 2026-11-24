/* vmexport.h — Export and its import check (docs/VIRTUALIZATION.md 3.6, "Moving a machine", and 6.4): the manifest of
 * a `.lordvm` copy, the OVF descriptor and `.mf` of an OVA, and the tar writer that packs the OVA. libc and LordOS's
 * own SHA-256 (pkg/crypto.c) and JSON reader (sec/json.c) only.
 *
 * A `.lordvm` copy holds the machine's files plus manifest.json: the SHA-256 and size of every other file. An import
 * gets the folder from a USB stick or a download, so the manifest and the folder are hostile: the parser is strict,
 * every path is one of the names a machine has (vmx_path_ok), files are opened one component at a time without ever
 * following a symlink, and the folder may hold nothing the manifest doesn't list. */
#ifndef LORD_VM_VMEXPORT_H
#define LORD_VM_VMEXPORT_H

#include <stddef.h>
#include <stdint.h>

#define VMX_MAX_FILES     64
#define VMX_PATH_MAX      64                  /* the longest allowed name is "disks/tpm/snap-<32>.permall" (51) */
#define VMX_NAME_MAX      64                  /* the machine's name, UTF-8 bytes */
#define VMX_MANIFEST_MAX  (1u << 20)          /* manifest.json's text */
#define VMX_SIZE_MAX      (1ull << 50)        /* one file */
#define VMX_FORMAT        "lordvm-export"
#define VMX_VERSION       1

/* 1 when rel is a file a machine folder may carry, else 0. Exactly: machine.json, snapshots.json, cpu-usage.json,
 * firmware-code.fd, disks/system.{qcow2,raw}, disks/data-{1..4}.{qcow2,raw}, disks/nvram.qcow2,
 * disks/tpm/tpm2-00.permall and disks/tpm/snap-<tag>.permall with a tag as vm_snap_tag_valid takes it ('s', then
 * a-z 0-9 '-', 2 to 32 bytes). manifest.json itself is not one: it is never listed in the manifest. */
int vmx_path_ok(const char *rel);

typedef struct {
    char path[VMX_PATH_MAX];
    uint64_t size;
    char sha256[65];                          /* 64 lowercase hex digits */
} VmxEntry;

typedef struct {
    char id[37];                              /* the machine's UUID, lowercase as machine.c writes it */
    char name[VMX_NAME_MAX + 1];              /* UTF-8, no control characters */
    char created[21];                         /* "YYYY-MM-DDTHH:MM:SSZ" */
    int encrypted, snapshots;                 /* 0 or 1 */
    int n;
    VmxEntry e[VMX_MAX_FILES];
} VmxManifest;

/* progress of a hashing pass: bytes read so far of all the files, and their total */
typedef void (*VmxProgress)(void *ctx, uint64_t done, uint64_t total);

/* 1 when the fields are well formed (the same checks vmx_manifest_parse makes on them) */
int vmx_uuid_ok(const char *s);
int vmx_name_ok(const char *s);
int vmx_created_ok(const char *s);

/* Fills m->n and m->e from the n relative names under dirfd (each vmx_path_ok, no repeats, n <= VMX_MAX_FILES):
 * each opened without following a symlink at any component, a regular file, hashed. The caller sets id, name,
 * created and the flags before or after. 0, or -1 with a short English reason in why. */
int vmx_manifest_build(int dirfd, const char *const *rel, int n, VmxManifest *m,
                       VmxProgress progress, void *pctx, char *why, size_t wn);

/* manifest.json's text (malloc'd, ends with a newline; NULL when a field isn't valid or out of memory). Keys in a
 * fixed order and the entries in m's order, so the same manifest always gives the same bytes. */
char *vmx_manifest_json(const VmxManifest *m);

/* Reads manifest.json's text, strictly: at most VMX_MANIFEST_MAX bytes, exactly the keys format ("lordvm-export"),
 * version (1), id, name, created, encrypted, snapshots and files (1..VMX_MAX_FILES objects of exactly path, size,
 * sha256); unknown keys, repeated paths, a path vmx_path_ok refuses, a size that isn't a whole number 0..2^50 and a
 * hash that isn't 64 lowercase hex digits are refused. 0, or -1 with a reason. */
int vmx_manifest_parse(const char *text, size_t len, VmxManifest *m, char *why, size_t wn);

/* Checks the folder under dirfd against m: every listed file is a regular file with one link, reached without
 * following a symlink, of the listed size and SHA-256; and the folder holds nothing else but manifest.json and the
 * folders disks/ and disks/tpm/ (no symlinks, no other files or folders, at any depth). When fds is not NULL it gets
 * m->n descriptors (read-only, at offset 0) of the very files that were hashed, so the caller copies what was checked
 * and not what a name points to later; the caller closes them. On failure none are left open. 0, or -1 with a reason. */
int vmx_manifest_verify(int dirfd, const VmxManifest *m, int *fds, VmxProgress progress, void *pctx,
                        char *why, size_t wn);

/* ---- OVA (OVF 1.0, DMTF DSP0243 1.0.0) ---- */

#define VMX_OVF_DISKS 5

typedef struct {
    const char *file;                         /* the VMDK's name in the OVA: [A-Za-z0-9._-], not starting with '.', < 100 bytes */
    uint64_t capacity;                        /* the virtual disk's size in bytes */
    uint64_t populated;                       /* bytes the guest has written (qemu-img's "disk size" estimate; 0 = unknown) */
    uint64_t file_size;                       /* the VMDK file's size */
} VmxOvfDisk;

typedef struct {
    const char *name;                         /* the machine's name (vmx_name_ok) */
    int vcpus;                                /* 1..256 */
    uint64_t memory_mb;                       /* 1..4 TiB in MiB */
    int efi;                                  /* 1 UEFI, 0 BIOS */
    const char *os;                           /* "linux", "debian", "windows", anything else: other (64-bit) */
    int ndisks;                               /* 1..VMX_OVF_DISKS, the first is the system disk */
    VmxOvfDisk disk[VMX_OVF_DISKS];
} VmxOvfInfo;

/* the CIM_OperatingSystem id vmx_ovf uses for an os hint */
int vmx_ovf_os_id(const char *os);
/* the OVF descriptor (malloc'd), or NULL when info isn't valid */
char *vmx_ovf(const VmxOvfInfo *info);
/* the .mf: "SHA256(<name>)= <hex>\n" per file (malloc'd), or NULL when a name or hash isn't valid */
char *vmx_mf(const char *const *names, const char *const *sha256hex, int n);

/* ---- the tar writer (POSIX ustar; pax extended header when needed) ----
 * Members are regular files: mode 0644, uid and gid 0, no user or group names, typeflag '0'. A name longer than 99
 * bytes or a size of 8 GiB or more gets a pax 'x' header first (path, size); the ustar size field then also carries
 * the size in GNU's base-256 form, so readers that know only one of the two still get it right. A name is relative,
 * 1..1024 bytes, without control characters, empty or ".." components. Each returns 0, or -1 (errno set). */

/* copies exactly size bytes from in (refuses a file that ends sooner) and pads to 512. Into a regular file, runs of
 * zeros become holes, so a big empty disk costs no space. */
int vmx_tar_file(int out, const char *name, int in, long long size, long long mtime);
/* the same from memory (the .ovf and the .mf) */
int vmx_tar_bytes(int out, const char *name, const void *p, size_t n, long long mtime);
/* the two zero blocks that end the archive */
int vmx_tar_end(int out);

#endif
