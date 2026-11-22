/* gpureport.h — the graphics compatibility report (Settings → Graphics cards → "Send a compatibility report",
 * `lordos-sysreport gpu`, `lordos-issue --gpu`; docs/NVIDIA-PLAN.md). What the graphics cards are, which kernel driver
 * runs each and which draws the desktop, NVIDIA's driver (its version, open or proprietary kernel modules,
 * nvidia-drm.modeset, CUDA's userspace), Mesa's version, the Vulkan, VA-API and VDPAU drivers installed, Secure Boot,
 * and, inside a session, how lord-server composes the screen.
 *
 * Read without root from world-readable files only (sysfs, /proc, the library folders, the efivars); nothing is
 * loaded, executed, installed or sent. Privacy by construction, as sysreport.h: the report is made of fixed fields
 * with no place for serial numbers, MAC addresses, user or host names or paths in the home; gpureport_redacted() then
 * scrubs the text once more (tests/gpureport_test.c plants such values and checks none comes out).
 *
 * `root` prefixes every path ("" for this machine, a test's fake tree otherwise). */
#ifndef LORD_SYSREPORT_GPUREPORT_H
#define LORD_SYSREPORT_GPUREPORT_H

#include <stddef.h>

#define GR_MAX_CARD 8
#define GR_LIST 12
#define GR_TEXT_MAX 6000                /* the report's text is cut here (a GitHub form takes much less) */

typedef struct {
    char card[12];                      /* "card0", "" without a DRM node */
    char slot[16];                      /* "0000:01:00.0", "" when not on PCI */
    char vendor_id[5], device_id[5];    /* "10de", "2504"; "" on a platform GPU */
    char vendor[96], device[128];       /* pci.ids names, "" unknown */
    char driver[32];                    /* the kernel driver: "nvidia", "nouveau", "amdgpu", "i915"…; "" none */
    int boot_vga;                       /* the firmware's console GPU */
    int render_node;                    /* has a renderD* node */
    int desktop;                        /* 1 draws this desktop, 0 doesn't, -1 unknown */
} GrCard;

typedef struct {
    int ncard;
    GrCard card[GR_MAX_CARD];
    char lordos[48];                    /* LordOS's version, filled by the caller */
    char kernel[64], arch[16];
    char firmware[8];                   /* "uefi", "bios", "" */
    int secure_boot;                    /* 1 on, 0 off, -1 unknown */
    char lockdown[20];                  /* the kernel's lockdown mode, "" unknown */
    char cmdline[200];                  /* only the graphics options of the kernel command line */
    /* NVIDIA's driver */
    int nv_loaded;                      /* the nvidia module is loaded */
    char nv_version[24];                /* "550.163.01", "" unknown */
    int nv_open;                        /* 1 open kernel modules, 0 proprietary ones, -1 unknown */
    int nv_modeset, nv_fbdev;           /* nvidia_drm's modeset and fbdev: 1, 0, -1 module not loaded */
    int nouveau_loaded;
    int cuda;                           /* libcuda.so.1 is installed */
    int nv_smi;                         /* nvidia-smi is installed */
    int nv_uvm;                         /* /dev/nvidia-uvm exists (CUDA needs it) */
    /* Mesa and the graphics APIs installed */
    char mesa[24];                      /* "25.0.7-2" (from libgallium-<version>.so), "" unknown */
    int nvulkan; char vulkan[GR_LIST][40];      /* "radeon 1.4.305" (an ICD's driver and Vulkan version) */
    int nva;     char va[GR_LIST][24];          /* VA-API drivers: "radeonsi", "iHD" */
    int nvdpau;  char vdpau[GR_LIST][24];       /* VDPAU drivers: "radeonsi", "nvidia" */
    int opencl;                         /* OpenCL vendor files */
    /* this session, filled by lord-server (session 0 outside one: the command line) */
    int session;
    char composition[16];               /* main.c lord_composition(): "gpu", "cpu", "gpu-off"… */
    char gpu_init[16];                  /* gpu.h GpuInfo.why */
    char renderer[128];                 /* the GL renderer string of the desktop's context, "" none */
} GpuReport;

/* Fills r from the tree at root. pci_ids: the name list (NULL: <root>/usr/share/misc/pci.ids). Always succeeds:
 * what can't be read stays empty or unknown. Every card's desktop starts unknown (-1). */
void gpureport_collect(GpuReport *r, const char *root, const char *pci_ids);

/* Which card draws this desktop: the cards (/dev/dri/cardN) the processes hold open. fd_dir is lord-server's own
 * (/proc/self/fd); NULL looks for the user's lord-server under <root>/proc (by its name, only processes whose fd folder
 * can be read: the user's own). When a card is found every other card is marked 0. How many were marked 1. */
int gpureport_mark_desktop(GpuReport *r, const char *root, const char *fd_dir);

/* "nvidia", "amd", "intel" or "other", by the PCI vendor id */
const char *gpureport_kind(const GrCard *c);

/* The report as plain text (English: GitHub issues are), malloc'd, NULL on out of memory. Not yet redacted. */
char *gpureport_text(const GpuReport *r);
/* The same facts as JSON for Settings: { cards: [{ card, slot, vendorId, deviceId, vendor, device, driver, kind,
 * bootVga, renderNode, desktop }], nvidia: { loaded, version, open, modeset, fbdev, nouveau, cuda, smi, uvm },
 * secureBoot, lockdown, mesa, vulkan: [], va: [], vdpau: [], opencl, kernel, arch, session: {...} | null }. */
char *gpureport_json(const GpuReport *r);

/* A copy of text with the person's traces replaced: home (the home folder's path) → "~", any other /home/<name> →
 * /home/<user>, user and host (whole words; NULL or "" skipped, as are "root", "localhost" and names under 2
 * characters) → <user>, <host>; MAC addresses → <mac>; UUIDs → <uuid>; e-mail addresses → <email>; the value after
 * "serial" → <hidden>. Cut at GR_TEXT_MAX bytes on a line's end. malloc'd, NULL on out of memory. */
char *gpureport_redacted(const char *text, const char *user, const char *host, const char *home);

/* The NVIDIA kernel module's version from /proc/driver/nvidia/version's text ("NVRM version: NVIDIA UNIX Open Kernel
 * Module for x86_64  560.35.03  Release Build ..."): 0 when found, with *open 1 for the open kernel modules */
int gpureport_nv_version(const char *text, char *ver, size_t n, int *open);

#endif
