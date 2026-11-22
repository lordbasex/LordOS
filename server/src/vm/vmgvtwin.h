/* vmgvtwin.h — Intel's driver for a Windows machine's GVT-g vGPU (docs/MACHINES-GPU.md, Mode A; AGENTS.md "LordOS guest
 * tools for Windows"). lord-vm runs GVTWIN_PS once per run, as SYSTEM through qemu-guest-agent, when the machine has a
 * vGPU (machine.json "gpu") and the LordOS tools say "gvt" in their hello (w6 and later). The script's only variable
 * parts are the constants below: nothing from the guest's person, from JS or from machine.json reaches it.
 *
 * Windows on a GVT-g vGPU needs Intel's 27.20.100.8681 (newer DCH drivers stop with code 43), Windows Update must not
 * replace it (it offers 30.0.101.1273), and the guest must not hibernate or Fast-Start (a resumed session runs the iGPU
 * at 350 MHz and GVT logs "fail to find ppgtt instance"). The script:
 *   1. turns hibernation off (powercfg /hibernate off, which also ends Fast Startup) and HiberbootEnabled 0;
 *   2. finds the vGPU (a present PCI\VEN_8086 device whose compatible IDs say display controller, PCI\CC_0300/0380);
 *      if its driver is GVTWIN_DRV_VERSION already, nothing is installed;
 *   3. otherwise downloads GVTWIN_DRV_URL (the Microsoft Update Catalog's own file) into a fresh SYSTEM-and-
 *      Administrators-only folder in %SystemRoot%\Temp (checked: not a reparse point, owner and every entry ours),
 *      compares its SHA-256 with GVTWIN_DRV_SHA256, expands it, checks the INF's DriverVer, the catalog's Authenticode
 *      signature (Valid, Microsoft Windows Hardware Compatibility Publisher) and igdkmd64.sys's (Valid, Intel
 *      Corporation), then pnputil /add-driver /install; when Windows keeps a driver it ranks higher (Windows Update's
 *      newer one), UpdateDriverForPlugAndPlayDevices with INSTALLFLAG_FORCE puts this one on that device's ID. Anything
 *      that doesn't check out stops it; the folder goes whatever happens;
 *   4. only with GVTWIN_DRV_VERSION on the device: the Device Installation Restrictions policy denies any later driver
 *      for that device's ID (PCI\VEN_8086&DEV_xxxx, added once to DenyDeviceIDs' list; DenyDeviceIDs 1,
 *      DenyDeviceIDsRetroactive 0 so the installed driver stays). That blocks Windows Update's driver for this device
 *      only (any install for it, whatever offers it), where ExcludeWUDriversInQualityUpdate would stop Windows Update's
 *      drivers for every device (virtio-win's included) and not a driver installed another way. Our own entry is lifted
 *      just for the next install of our driver, after every check passed;
 *   5. prints one line: "<driver> <block> <power> <device id or -> <why>", read by gvtwin_parse.
 * A named mutex keeps two runs from overlapping ("busy"). */
#ifndef LORD_VM_VMGVTWIN_H
#define LORD_VM_VMGVTWIN_H

#include <stddef.h>

/* Intel Corporation - Display - 27.20.100.8681, on the Microsoft Update Catalog (search "27.20.100.8681"; the same file
 * is listed under several of its update IDs, e.g. b754e0bc-f9e9-43b8-b9d0-20041db0afeb). 246 891 113 bytes; the name's
 * last part is its SHA-1, 80a4157b037be2e8a539744ceee56669f3a3b9ce. Checked 2026-11-21: the URL serves exactly this
 * SHA-256. Changing the driver means changing these four together. */
#define GVTWIN_DRV_VERSION "27.20.100.8681"
#define GVTWIN_DRV_URL "https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2021/05/31ad2fb8-af44-4c78-a4b7-09b492620f0a_80a4157b037be2e8a539744ceee56669f3a3b9ce.cab"
#define GVTWIN_DRV_SHA256 "B5DB0C7196C3968736F021A660ECF74905D60480D9AF7B2953024B34586021A0"
#define GVTWIN_DRV_INF "iigd_dch.inf"

extern const char GVTWIN_PS[];

enum { GW_D_ALREADY = 1, GW_D_INSTALLED, GW_D_RESTART, GW_D_REFUSED, GW_D_FAILED, GW_D_NODEVICE, GW_D_BUSY };
enum { GW_B_KEPT = 1, GW_B_ADDED, GW_B_NONE, GW_B_FAILED };
enum { GW_P_OFF = 1, GW_P_TURNED, GW_P_FAILED };
#define GVTWIN_ID_MAX 21                /* "PCI\VEN_8086&DEV_xxxx" */
#define GVTWIN_WHY_MAX 150
typedef struct {
    int driver, block, power;           /* GW_D_*, GW_B_*, GW_P_* */
    char id[GVTWIN_ID_MAX + 1];         /* "" when the script said "-" */
    char why[GVTWIN_WHY_MAX + 1];       /* printable ASCII, maybe "" */
} GvtWinResult;

/* the script's line (as ag_exec_text gives it): 0, or -1 when it isn't one (an unknown word, a malformed device ID, a
 * device ID missing where the driver is said to be there, a reason that isn't printable ASCII or is too long) */
int gvtwin_parse(const char *said, GvtWinResult *r);
/* the line lord-vm logs, "intel driver: ..." */
void gvtwin_describe(const GvtWinResult *r, char *out, size_t n);

#endif
