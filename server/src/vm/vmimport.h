/* vmimport.h — reading a machine made by another desktop virtualization app (docs/VIRTUALIZATION.md 6.22, Import).
 *
 * Sources, by their settings file (the format names are file-format facts; the app says "other virtualization apps"):
 *   VMI_VMX   a .vmx (key = "value" lines), alone or in a .vmwarevm folder; disks are VMDK (descriptor + extents, or
 *             one sparse file with its own descriptor); snapshots are delta VMDKs naming their parent
 *   VMI_VBOX  a .vbox (XML); disks VDI or VMDK, snapshots are differencing images in the media registry's tree
 *   VMI_PVS   config.pvs (XML) in a .pvm folder; each disk is a .hdd folder with DiskDescriptor.xml and .hds data files
 *   VMI_OVF   an .ovf descriptor, or an .ova (a tar holding the descriptor and stream-optimized VMDKs, read in place)
 *   VMI_DISK  one disk image: VMDK, VDI, VHDX, qcow2, raw
 *
 * Everything here is pure: text in, a VmiMachine out. Paths stay as the file wrote them; lordvm.c resolves them
 * inside the bundle (vmi_rel_ok, then realpath and a prefix check), opens every disk only through the jailed qemu-img
 * with an explicit driver, and never follows a name the image itself carries.
 *
 * What the guest saw is carried over as it saw it (the plan sheet lists what can't be):
 *   - every network card's MAC address and model (where QEMU has the same one);
 *   - the SMBIOS system UUID, derived from how that app writes it into its tables and how the guest reads it back
 *     (vmi_guest_uuid); manufacturer, product, version, serial, SKU and family where the source sets them;
 *   - BIOS or UEFI, the vCPU count and topology, memory, the disks' order, virtual sizes and serials, the RTC's base. */
#ifndef LORD_VM_IMPORT_H
#define LORD_VM_IMPORT_H

#include <stddef.h>
#include <stdint.h>

enum { VMI_NONE = 0, VMI_VMX, VMI_VBOX, VMI_PVS, VMI_OVF, VMI_DISK };

/* disk image formats, by qemu-img driver */
enum { VMF_UNKNOWN = 0, VMF_VMDK, VMF_VDI, VMF_VHDX, VMF_VPC, VMF_QCOW2, VMF_RAW, VMF_PARALLELS };

#define VMI_MAX_NICS   8
#define VMI_MAX_DISKS  5             /* the system disk and four data disks (6.4) */
#define VMI_MAX_CHAIN  32            /* a disk's current state and the snapshots under it */
#define VMI_MAX_EXTENTS 64           /* a split VMDK: 2 GB pieces, so 128 GB at most */
#define VMI_PATH 1024
#define VMI_STR 65

typedef struct {
    char mac[18];                    /* lowercase aa:bb:cc:dd:ee:ff; "" when the source doesn't keep one */
    char model[16];                  /* QEMU's device: e1000e, e1000, vmxnet3, virtio, rtl8139, pcnet; "" unknown */
    char src_model[24];              /* as the source named it, for the plan */
    int connected;                   /* attached to a network in the source */
} VmiNic;

/* one layer of a disk: its file (relative to the bundle, or an absolute path the source wrote) and its format.
 * chain[0] is the base, chain[n-1] the current state. */
typedef struct {
    char path[VMI_PATH];
    int fmt;
    char uuid[37];                   /* VDI / .vbox medium, .hdd snapshot GUID; "" */
} VmiLayer;

typedef struct {
    VmiLayer chain[VMI_MAX_CHAIN];
    int nchain;
    char bus[8];                     /* sata, ide, scsi, nvme, virtio, "" */
    char serial[41];                 /* what the guest read as the disk's serial; "" none */
    long long vsize;                 /* virtual size in bytes, if the source says (0: ask qemu-img) */
    int boot;                        /* 1: the source boots from it first */
} VmiDisk;

typedef struct {
    int kind;                        /* VMI_* */
    char name[VMI_STR];
    char guest_os[VMI_STR];          /* the source's guest type, as written ("windows9-64", "Windows11_64", ...) */
    int windows;                     /* the guest type is a Windows family: RTC in local time, UUID read little-endian */
    int arch;                        /* VM_ARCH_*, -1 unknown */
    int firmware;                    /* VM_FW_UEFI / VM_FW_BIOS */
    int secure_boot;
    int vcpus, sockets, cores, threads;
    int memory_mb;
    int rtc_local;                   /* the guest's clock is local time */
    /* identity */
    char uuid[37];                   /* the SMBIOS UUID as the guest read it ("" none: a new one is made) */
    char uuid_how[96];               /* how it was derived, for the plan and the log */
    char sm_manufacturer[VMI_STR], sm_product[VMI_STR], sm_version[VMI_STR], sm_serial[VMI_STR], sm_sku[VMI_STR], sm_family[VMI_STR];
    VmiNic nic[VMI_MAX_NICS];
    int nnic;
    VmiDisk disk[VMI_MAX_DISKS];
    int ndisk;
    /* what can't come along, or stops the import */
    int snapshots;                   /* snapshots found (flattened) */
    int saved_state;                 /* a suspended session: refused until it's shut down */
    int tpm;                         /* a virtual TPM: its keys can't be carried */
    int encrypted;                   /* an encrypted disk or machine: refused */
    int more_disks;                  /* disks beyond VMI_MAX_DISKS: refused */
    int more_nics;                   /* cards beyond VMI_MAX_NICS */
    int cdrom_files;                 /* CD/DVD images attached in the source (not carried) */
    int shared_folders;
    int usb_filters;
    char problem[160];               /* a reason the source can't be imported at all ("" none) */
} VmiMachine;

/* the parsers: 0 ok (m filled; m->problem may still say why it can't be imported), -1 not that format (err says) */
int vmi_parse_vmx(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen);
int vmi_parse_vbox(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen);
int vmi_parse_pvs(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen);
int vmi_parse_ovf(const char *text, size_t len, VmiMachine *m, char *err, size_t errlen);

/* A VMDK text descriptor (a .vmdk of a few KB, or the one inside a monolithic sparse file) */
typedef struct {
    char create_type[32];            /* monolithicSparse, monolithicFlat, twoGbMaxExtentSparse, twoGbMaxExtentFlat,
                                      * streamOptimized, vmfs, vmfsSparse, ... */
    char parent[VMI_PATH];           /* parentFileNameHint: a snapshot's parent; "" for a base */
    int nextents;
    struct { char kind[12]; long long sectors; char file[VMI_PATH]; long long offset; } ext[VMI_MAX_EXTENTS];
    char uuid[48];                   /* ddb.uuid ("60 00 C2 ..."), the disk serial a guest reads with disk.EnableUUID */
    int encrypted;                   /* an encryption key in the descriptor */
} VmiVmdk;
int vmi_parse_vmdk(const char *text, size_t len, VmiVmdk *d, char *err, size_t errlen);

/* A .hdd folder's DiskDescriptor.xml: the data files of the current state, base first */
typedef struct {
    long long sectors;
    int nlayers;
    char file[VMI_MAX_CHAIN][VMI_PATH];
    int snapshots;
    int encrypted;
} VmiHdd;
int vmi_parse_hdd(const char *text, size_t len, VmiHdd *d, char *err, size_t errlen);

/* An OVA: a ustar archive. Members are read in place (offset, size); names must be plain (no '/', no ".."). */
#define VMI_OVA_MAX 64
typedef struct { char name[101]; long long offset, size; } VmiTarEntry;
/* reads headers from `read_at` (a callback over the file); returns the number of members, -1 bad (err says) */
typedef long (*vmi_read_fn)(void *ctx, void *buf, size_t n, long long off);
int vmi_tar_list(vmi_read_fn rd, void *ctx, long long file_size, VmiTarEntry *out, int max, char *err, size_t errlen);

/* first bytes of a disk image: its format (VMF_*), never QEMU's probing. A VMDK text descriptor is VMF_VMDK too */
int vmi_disk_format(const uint8_t *head, size_t n, const char *name);
/* a VDI header: its image type (1 dynamic, 2 static, 4 differencing), its own and its parent's UUID, its size */
typedef struct { int type; char uuid[37], parent[37]; long long size; } VmiVdi;
int vmi_vdi_header(const uint8_t *head, size_t n, VmiVdi *v);

/* a relative name a bundle may use: no absolute path, no "..", no control characters, no backslash, at most
 * VMI_PATH bytes; "a/b.vmdk" is fine. 1 ok */
int vmi_rel_ok(const char *s);
/* MAC normalization: "00:0C:29:AA:BB:CC", "000c29aabbcc", "00-0c-29-aa-bb-cc" → "00:0c:29:aa:bb:cc"; a multicast, all
 * zero or malformed one gives -1 */
int vmi_mac(const char *in, char out[18]);
/* a UUID from "{8C...}", "8c...-...", or VMware's "56 4d 0e ad 1b 2c 3d 4e-5f 60 71 82 93 a4 b5 c6": its 16 bytes in
 * the order written. 0 ok */
int vmi_uuid_bytes(const char *in, uint8_t out[16]);
void vmi_uuid_str(const uint8_t b[16], char out[37]);
/* the first three fields byte-swapped (a UUID read little-endian from bytes written in network order, or back) */
void vmi_uuid_swap(uint8_t b[16]);

#endif
