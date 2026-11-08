/* gpustat.h — Monitor → GPU (docs/MONITOR.md, "GPU"): what this machine's graphics cards report and which of the
 * user's processes use them. Read on the Monitor's thread about once per second, only while the GPU tab or the
 * Overview is on screen. World-readable sysfs and the user's own /proc only: no root, no exec, no new library.
 *
 *  - /sys/class/drm/cardN: the driver, the PCI ids (names from pci.ids, sysreport/ids.c), amdgpu's gpu_busy_percent,
 *    mem_info_{vram,vis_vram,gtt}_{used,total}, pp_dpm_sclk / pp_dpm_mclk, i915's gt_{act,cur,max}_freq_mhz, xe's
 *    tile0/gt0/freq0, and the card's hwmon: temp*_input and labels, fan1_input, fan1_max, pwm1 (+ pwm1_max),
 *    power1_average or power1_input, power1_cap, energy1_input, freq1/freq2 (sclk, mclk).
 *  - /proc/<pid>/fd and /proc/<pid>/fdinfo/<fd> of the user's own processes (the kernel's DRM usage stats:
 *    drm-driver, drm-pdev, drm-client-id, drm-engine-*, drm-engine-capacity-*, drm-cycles-*, drm-total-cycles-*,
 *    drm-memory-*, drm-total-*, drm-resident-*, drm-shared-*). Raw counters only: rates are made in JS
 *    (apps/monitor/gpumodel.js), which also dedupes clients shared between processes.
 *  - /proc/<pid>/stat of every process, for LordOS's AI helpers (lord-tts, lord-ocr, lord-say) and the virtual
 *    machines (qemu-system-*, the user's own), and lord-say's status note (~/.cache/lordos/say/<pid>, written by
 *    say.c: the backend, quality and language it chose).
 *
 * Every file is read with a bounded size and parsed strictly: a value that isn't a clean number in range is left out,
 * never guessed. The roots are parameters so tests can use fake trees. */
#ifndef LORD_MON_GPUSTAT_H
#define LORD_MON_GPUSTAT_H

#include <stddef.h>

#define GPU_CARDS    8
#define GPU_TEMPS    4
#define GPU_KEYS     16                 /* engines (and capacities, cycles) per client */
#define GPU_REGIONS  8                  /* memory regions per client */
#define GPU_NAME     24                 /* an engine's or a region's name, a driver's, a PCI slot */
#define GPU_FDINFO_MAX 8192             /* bytes read of one fdinfo file */

typedef struct { char name[GPU_NAME]; unsigned long long v; } GpuKV;

/* bytes; -1 when the client doesn't report that kind */
typedef struct { char name[GPU_NAME]; long long memory, total, resident, shared; } GpuMem;

typedef struct {
    char driver[GPU_NAME], pdev[GPU_NAME];
    unsigned long long id;
    int has_id;
    GpuKV eng[GPU_KEYS];  int neng;     /* drm-engine-<e>: ns */
    GpuKV cap[GPU_KEYS];  int ncap;     /* drm-engine-capacity-<e> */
    GpuKV cyc[GPU_KEYS];  int ncyc;     /* drm-cycles-<e> */
    GpuKV tcyc[GPU_KEYS]; int ntcyc;    /* drm-total-cycles-<e> */
    GpuMem mem[GPU_REGIONS]; int nmem;
} GpuClient;

typedef struct { char label[16]; int mc; } GpuTemp;

typedef struct {
    char card[12], driver[GPU_NAME], slot[GPU_NAME], vendor[8], device[8];
    char vname[96], dname[128];         /* pci.ids names, "" unknown */
    int render;                         /* has a render node */
    int busy;                           /* gpu_busy_percent, -1 */
    long long vram_used, vram_total, vis_used, vis_total, gtt_used, gtt_total;   /* bytes, -1 */
    GpuTemp temp[GPU_TEMPS];
    int ntemp;
    int fan_rpm, fan_max, fan_pct;      /* fan1_input, fan1_max, pwm1 as % of pwm1_max; -1 */
    long long power_uw, cap_uw, energy_uj;   /* -1 */
    int sclk_mhz, mclk_mhz, sclk_max, mclk_max;   /* -1 */
    int cur_mhz, max_mhz;               /* i915/xe: the GPU's actual and top frequency, -1 */
} GpuCard;

/* lord-say's status note: "lord-say 1 <vulkan|cpu> <hd|standard> <language> <speak|save>\n" */
typedef struct { char backend[8], quality[12], lang[8], mode[8]; } GpuSay;

/* Strict unsigned number: 1-20 digits, an optional trailing newline, no overflow. 0 if it is one. */
int gpu_parse_u64(const char *s, unsigned long long *out);
/* One fdinfo file. 0 if it is a DRM client (has drm-driver); unknown or malformed lines are skipped. */
int gpu_fdinfo_parse(const char *text, size_t len, GpuClient *c);
/* pp_dpm_sclk / pp_dpm_mclk: "0: 300Mhz \n1: 600Mhz *\n" → the starred level and the highest, in MHz (-1 unknown) */
void gpu_dpm_parse(const char *text, int *cur, int *max);
int gpu_say_parse(const char *text, GpuSay *s);
/* The libraries in a /proc/<pid>/maps text that tell which GPU API a process uses (libvulkan*, libEGL*, *_dri.so,
 * libOpenCL*, libcuda*, *_drv_video.so, ...): their base names, deduplicated, at most max. Their count. */
int gpu_maps_libs(const char *text, size_t len, char out[][64], int max);

/* The cards under root ("" = this machine); pci_ids: the name list's path or NULL. Returns how many. */
int gpu_cards_read(const char *root, const char *pci_ids, GpuCard *out, int max);

/* The whole picture as malloc'd JSON (NULL without memory):
 *   {"t","mono","hz","cards":[…],"list":[{pid,ppid,name,cmd,exe,self,clients:[…]}],
 *    "ai":[{pid,ppid,name,state,own}],"say":[{pid,state,backend,quality,lang,mode}],"vms":[{pid,ticks,rss}]}
 * root: "" for the real /proc and /sys; uid: the user's (only their processes' fds are read); self: lord-server's pid;
 * home: where lord-say's notes are (NULL: none). */
char *gpu_json(const char *root, const char *pci_ids, unsigned uid, int self, const char *home);

#endif
