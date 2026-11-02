/* sysreport.h — System Information's collector (0.8.5, see docs/BENCHMARK.md): what hardware this
 * machine has and what the kernel made of it, read without root from world-readable files (sysfs,
 * /proc, the device tree, os-release, the kernel config). Used by `lordos-sysreport` and, later, by
 * `lord.sysinfo`; nothing is written, executed or sent anywhere.
 *
 * Privacy by construction: the report is built from a fixed list of fields, and the structures below
 * have no place for serial numbers, UUIDs, MAC or IP addresses, host or user names, Wi-Fi network
 * names or home-folder paths (network interfaces are reported by kind and driver, not by name, as a
 * name can carry the MAC). tests/sysreport_test.c fills a fake system with such values and checks that
 * none reaches the JSON or the Markdown.
 *
 * `root` prefixes every path ("" for this machine; tests point it at a fake tree). Facts that only
 * the running system can give (the Landlock ABI, which LordOS services are up) are left unknown
 * under a fake root. */
#ifndef LORD_SYSREPORT_H
#define LORD_SYSREPORT_H

#include <stdint.h>

#define SR_NAME 96
#define SR_MAX_GPU 8
#define SR_MAX_DISPLAY 12
#define SR_MAX_DISK 32
#define SR_MAX_PCI 96
#define SR_MAX_USB 64
#define SR_MAX_NET 16
#define SR_MAX_AUDIO 8
#define SR_MAX_POWER 8
#define SR_MAX_THERMAL 16
#define SR_MAX_KCONF 16
#define SR_MAX_ZRAM 4

typedef struct {
    char vendor[SR_NAME], product[SR_NAME], version[SR_NAME];
    char board_vendor[SR_NAME], board_name[SR_NAME], board_version[SR_NAME];
    char chassis[24];                   /* "desktop", "laptop", "server", "vm"…, "" unknown */
    char bios_vendor[SR_NAME], bios_version[SR_NAME], bios_date[24];
    char model[SR_NAME];                /* device tree model (ARM boards) */
    char firmware[8];                   /* "uefi", "bios", "dt" (device tree), "" */
    int secure_boot;                    /* 1 on, 0 off, -1 unknown */
    char virt[24];                      /* hypervisor ("kvm", "vmware"…), "" bare metal or unknown */
} SrMachine;

typedef struct {
    char model[SR_NAME], vendor[32], arch[16];
    int sockets, cores, threads;
    int min_mhz, max_mhz;
    char governor[24];
    int l1d_kb, l1i_kb, l2_kb, l3_kb;   /* per core (L1, L2 on most) or shared (L3); 0 unknown */
    char flags[160];                    /* the instruction sets that matter, space-separated */
    char microcode[24];
} SrCpu;

typedef struct {
    uint64_t total_kb, swap_kb;
    int zram_n;
    uint64_t zram_kb[SR_MAX_ZRAM];
    char zram_algo[SR_MAX_ZRAM][16];
} SrMemory;

typedef struct {
    char card[12];                      /* "card0" */
    char bus[12];                       /* "pci", "platform" */
    char vendor_id[5], device_id[5];
    char vendor[SR_NAME], device[SR_NAME];
    char driver[32];
    int boot_vga;                       /* the firmware's console GPU */
    int render_node;                    /* a renderD* node belongs to it */
    int connectors, connected;          /* KMS connectors */
} SrGpu;

typedef struct {
    char connector[32];                 /* "HDMI-A-1", "eDP-1" (the card is dropped) */
    char card[12];
    int connected, enabled;
    char name[16], pnp[4];              /* EDID monitor name and manufacturer code */
    char product[5];
    int year, width_cm, height_cm, diag_tenths;
    int pref_w, pref_h;                 /* native mode */
    char mode[24];                      /* first mode the kernel lists */
} SrDisplay;

typedef struct {
    char name[16];                      /* "nvme0n1", "sda" */
    char model[SR_NAME], vendor[SR_NAME];
    uint64_t bytes;
    int rotational, removable;
    char transport[12];                 /* "nvme", "sata", "usb", "mmc", "virtio", "scsi", "" */
    char link[32];                      /* NVMe PCIe link ("8.0 GT/s PCIe x4") */
} SrDisk;

typedef struct {
    char slot_class[7];                 /* class code "030000" */
    char vendor_id[5], device_id[5];
    char vendor[SR_NAME], device[SR_NAME], class_name[48];
    char driver[32];
} SrPci;

typedef struct {
    char vendor_id[5], product_id[5];
    char vendor[SR_NAME], product[SR_NAME];
    char cls[3];                        /* device class, "00" when per interface */
    int speed_mbps;
    char driver[32];                    /* the first interface's */
} SrUsb;

typedef struct {
    char kind[12];                      /* "ethernet", "wifi", "virtual", "other" */
    char driver[32];
    int speed_mbps;                     /* -1 unknown or down */
    int up;
} SrNet;

typedef struct {
    char kind[12];                      /* "battery", "ac", "usb", "ups" */
    char vendor[SR_NAME], model[SR_NAME], technology[16];
    int capacity;                       /* %, -1 unknown */
    int health;                         /* full charge as % of the design capacity, -1 unknown */
    char status[16];
    int online;                         /* AC: 1 plugged */
} SrPower;

typedef struct {
    char type[32];                      /* thermal zone type or hwmon chip name */
    int milli_c;                        /* INT32_MIN unknown */
    int fans;                           /* hwmon: fan inputs */
} SrThermal;

typedef struct {
    char name[40];                      /* CONFIG_… */
    char value[8];                      /* "y", "m", "n" (not set), "" unknown */
} SrKconf;

typedef struct {
    char os[SR_NAME], os_id[24], os_version[24];
    char kernel[64];
    char lordos[48];                    /* LordOS version, filled by the caller */
    char lsm[96];                       /* /sys/kernel/security/lsm */
    int landlock_abi;                   /* -1 unknown, 0 unavailable */
    int bpf_lsm;                        /* "bpf" in the LSM list */
    int cgroup2;                        /* unified hierarchy mounted */
    int live;                           /* the live session (/run/live exists) */
    char boot_medium[12];               /* what the system started from (0.8.7): "internal", "usb", "sd", "emmc",
                                         * "optical", "network" or "" unknown; never a device name */
    int boot_link_mbps;                 /* a USB medium's link speed, -1 unknown or not USB */
    int boot_toram;                     /* live session copied to RAM (the "toram" boot option) */
    int nkconf;
    SrKconf kconf[SR_MAX_KCONF];
    int sessiond, netperm, fand, netd;  /* their sockets in /run/lordos (-1 unknown) */
} SrSoftware;

typedef struct {
    char model[SR_NAME], revision[16];  /* /proc/cpuinfo "Revision" (the board code) */
    char bootloader_version[44];        /* the running bootloader's git hash (device tree, chosen/bootloader), "" unknown */
    char bootloader_date[12];           /* its build date, YYYY-MM-DD, "" unknown */
    int throttled;                      /* get_throttled bits (hw/rpifw.h), -1 unknown (no such file: the
                                         * LordOS rpi-6.18.y kernel has none) */
    int uv_alarm;                       /* rpi_volt's under-voltage alarm: 1 in the last 2 s, 0 none, -1 unknown */
    int is_pi;
} SrPi;

typedef struct {
    int schema;
    char date[16];                      /* YYYY-MM-DD, filled by sysreport_collect */
    SrMachine machine;
    SrCpu cpu;
    SrMemory mem;
    int ngpu;     SrGpu gpu[SR_MAX_GPU];
    int ndisplay; SrDisplay display[SR_MAX_DISPLAY];
    int ndisk;    SrDisk disk[SR_MAX_DISK];
    int npci;     SrPci pci[SR_MAX_PCI];
    int nusb;     SrUsb usb[SR_MAX_USB];
    int nnet;     SrNet net[SR_MAX_NET];
    int naudio;   char audio[SR_MAX_AUDIO][SR_NAME];
    int nbt;      char bt_driver[4][32];
    int npower;   SrPower power[SR_MAX_POWER];
    int nthermal; SrThermal thermal[SR_MAX_THERMAL];
    SrPi pi;
    SrSoftware sw;
} SysReport;

/* Fills r from the tree at root ("" = this machine). pci_ids/usb_ids are the name lists (NULL: the
 * Debian paths under root). Always succeeds: what can't be read stays empty or unknown. */
void sysreport_collect(SysReport *r, const char *root, const char *pci_ids, const char *usb_ids);

/* The report as JSON (schema 1) or as a Markdown summary; malloc'd, NULL on out-of-memory. */
char *sysreport_json(const SysReport *r);
char *sysreport_markdown(const SysReport *r);

/* Whether the kernel command line holds the whole word w (or w=… when w ends in '='). */
int sysreport_cmdline_has(const char *cmdline, const char *w);

#endif
