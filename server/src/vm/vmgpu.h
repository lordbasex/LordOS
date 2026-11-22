/* vmgpu.h — Machines' graphics cards (docs/MACHINES-GPU.md, Part 2 Mode A and Settings → Graphics cards; AGENTS.md
 * "Machines' graphics cards"): the pure parts, shared by the admin helper (vmgpu_admin.c: gpu-role, vm-gvt-create,
 * vm-gvt-remove, gpu-min-freq), lord-server (lord.vm.gpus) and lord-vm. No root here. Every reader takes a root folder:
 * "" for this computer, a test's folder otherwise (<root>/sys/bus/pci/devices/..., <root>/etc/modprobe.d/...). */
#ifndef LORD_VM_VMGPU_H
#define LORD_VM_VMGPU_H

#include <stddef.h>
#include <sys/types.h>

#define VMGPU_MAX 8                     /* GPUs listed */
#define VMGPU_TYPES 8                   /* mdev types per GPU */
#define VMGPU_TYPE_MAX 40               /* an mdev type's name, "i915-GVTg_V5_4" */

/* the fixed files gpu-role writes (and "unused" removes), and the two a person may have written by hand before
 * (docs/MACHINES-GPU.md's feasibility test): adopted by machines-shared and removed by unused, only when what they
 * say is exactly what LordOS would write */
#define VMGPU_MODPROBE        "/etc/modprobe.d/lordos-gpu.conf"
#define VMGPU_MODLOAD         "/etc/modules-load.d/lordos-gpu.conf"
#define VMGPU_LEGACY_MODPROBE "/etc/modprobe.d/i915-gvt.conf"
#define VMGPU_LEGACY_MODLOAD  "/etc/modules-load.d/kvmgt.conf"
extern const char VMGPU_MODPROBE_TEXT[], VMGPU_MODLOAD_TEXT[];

/* where root keeps who asked for each vGPU (vm-gvt-create): /run/lordos/vm-gvt/<machine id> holds "<uid>\n"; and the
 * iGPU's minimum frequency before gpu-min-freq first changed it: /run/lordos/vm-gvt/minfreq-<pci address> */
#define VMGPU_RUN "/run/lordos/vm-gvt"

/* our GVT-g GOP option ROM (firmware/gvt-gop), from the signed package */
#define VMGPU_ROM "/usr/lib/lordos/firmware/lordos-gvt-gop.rom"

typedef struct {
    char name[VMGPU_TYPE_MAX + 1];      /* "i915-GVTg_V5_4" (the folder's name) */
    int low_mb, high_mb, fence;         /* its share of the iGPU's memory (description) */
    int max_w, max_h;                   /* the largest picture it can show ("resolution: 1920x1200") */
    int weight;                         /* its share of the iGPU's time */
    int available;                      /* available_instances */
} VmGpuType;

typedef struct {
    char addr[16];                      /* "0000:00:02.0" */
    unsigned cls, vendor, device;       /* PCI class (0x03xxxx), ids */
    char driver[32];                    /* "i915", "amdgpu", "" when none */
    int iommu_group;                    /* -1 when the IOMMU is off */
    int boot_vga;                       /* the firmware's primary display */
    int card, render;                   /* /dev/dri/card<N>, /dev/dri/renderD<N>; -1 when none */
    int desktop;                        /* drives this desktop (vmgpu_mark_desktop): never offered for anything else */
    int gvt_capable;                    /* an Intel iGPU of a family GVT-g supports (Broadwell to Comet Lake) */
    int gvt_on;                         /* i915 runs it with GVT-g: it lists mdev types */
    int n_types;
    VmGpuType types[VMGPU_TYPES];       /* largest first */
} VmGpu;

/* the GPUs (PCI class 0x03) of <root>/sys/bus/pci/devices, sorted by address; how many (≤ max) */
int vmgpu_list(const char *root, VmGpu *out, int max);
/* marks which GPUs drive this desktop: the cards (/dev/dri/card<N>) a process holds open, read from its fd folder
 * (/proc/self/fd in lord-server, /proc/<lord-server>/fd in the admin helper; a test's folder of symbolic links). When
 * none is held (a headless or test session), the firmware's primary display counts as the desktop's. How many marked */
int vmgpu_mark_desktop(const char *root, const char *fd_dir, VmGpu *g, int n);
/* the one GPU of the list with GVT-g on that isn't the desktop's, or NULL */
const VmGpu *vmgpu_gvt(const VmGpu *g, int n);

/* an Intel iGPU GVT-g supports, by its device id's family (gen8 Broadwell, gen9 Skylake to Comet Lake, Broxton) */
int vmgpu_gvt_device(unsigned vendor, unsigned device);
/* "0000:00:02.0": domain, bus, slot and function in lowercase hex. 1 valid */
int vmgpu_addr_valid(const char *a);
/* an mdev type's name: [A-Za-z0-9_-], 1..VMGPU_TYPE_MAX, not "." or "..". 1 valid */
int vmgpu_type_valid(const char *t);
/* an mdev type's description (i915's: "low_gm_size: 128MB\nhigh_gm_size: 512MB\nfence: 4\nresolution: 1920x1200\n
 * weight: 4\n"). 0 when it has at least a resolution */
int vmgpu_type_desc(const char *text, VmGpuType *t);

/* the type for a machine: whole (1), the largest; shared (0), the smallest that shows w × h (0 × 0: any). Only types
 * with an instance available unless any_state. Its index or -1 */
int vmgpu_pick_type(const VmGpu *g, int whole, int w, int h, int any_state);

enum { VMGPU_ROLE_UNUSED = 0, VMGPU_ROLE_SHARED, VMGPU_ROLE_DEDICATED, VMGPU_ROLE_AI };
/* gpu-role's argument, "<pci address> <unused|machines-shared|machines-dedicated|ai>". 0 ok */
int vmgpu_role_arg(const char *arg, size_t len, char addr[16], int *role);
/* vm-gvt-create's, "<machine id> <mdev type>"; vm-gvt-remove's, "<machine id>" (a lowercase UUID). 0 ok */
int vmgpu_create_arg(const char *arg, size_t len, char id[37], char type[VMGPU_TYPE_MAX + 1]);
int vmgpu_remove_arg(const char *arg, size_t len, char id[37]);
/* gpu-min-freq's, "<pci address> <MHz|default>": *mhz is 0 for default, else 1..10000. 0 ok */
int vmgpu_freq_arg(const char *arg, size_t len, char addr[16], int *mhz);

/* whether a modprobe.d (which 0) or modules-load.d (which 1) file's text says exactly what LordOS writes for
 * machines-shared, comments and blank lines aside, options in any order */
int vmgpu_conf_ours(const char *text, int which);
/* the role written on this computer: VMGPU_ROLE_SHARED when both of LordOS's files (or both hand-written ones that
 * say the same) are there, else VMGPU_ROLE_UNUSED */
int vmgpu_configured(const char *root);

/* Settings → Graphics cards and Machines' Configure: a JSON object { "gpus": [{ "address", "vendor", "device",
 * "vendorName", "driver", "iommuGroup", "desktop", "bootVga", "render", "gvtCapable", "gvtOn",
 * "types": [{ "name", "lowMb", "highMb", "maxW", "maxH", "weight", "available" }] }], "configured":
 * "unused"|"machines-shared", "restart": bool }. malloc'd, NULL on out of memory */
char *vmgpu_json(const VmGpu *g, int n, int configured);

#endif
