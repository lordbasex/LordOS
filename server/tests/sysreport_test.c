/* sysreport_test.c — System Information's collector (src/sysreport) over fake trees: an AMD desktop,
 * an Intel laptop, an Nvidia desktop, a Raspberry Pi 5 (with the LordOS kernel) and a VM, and the Pi
 * firmware's power and throttling on small trees (that kernel, an older one with get_throttled, both,
 * neither, not a Pi). Each tree is full of things a report must never carry (serial numbers, UUIDs, MAC addresses, a host name, a user name, a monitor's serial,
 * a MAC inside an interface name); the test checks what each report finds, that its JSON is valid,
 * and that none of those values reaches the JSON or the Markdown.
 *
 * The computer's serial numbers (hw-serial-numbers) and the peripherals' names (sysreport/periph.c, including a
 * person's name inside one) are on screen only: the trees hold them all, and no report may carry them.
 *
 *   build/sysreport-test                       the tests (make test)
 * lordos-fetch's summary is checked on the same trees: with --share it carries none of those values
 * either, and without color it has no escape codes.
 *
 *   build/sysreport-test smbios                the memory module lines of this machine, as the
 *                                              hw-memory-modules action gives them (root)
 *   build/sysreport-test fixture <dir> <kind>  writes a fake tree (amd, intel, nvidia, rpi5, vm), for
 *                                              `lordos-sysreport report --root <dir>` */
#include "sysreport/sysreport.h"
#include "sysreport/periph.h"
#include "sysreport/ids.h"
#include "sysreport/fetch.h"
#include "sysreport/smbios.h"
#include "sysreport/facts.h"
#include "sec/json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int n_ok, n_bad;
#define CHECK(cond, ...) do { if (cond) n_ok++; else { n_bad++; printf("✗ " __VA_ARGS__); printf("\n"); } } while (0)

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

/* <root>/<rel> with data (parent folders created); len < 0: strlen. */
static void putn(const char *root, const char *rel, const void *data, int len) {
    char p[1024], d[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    FILE *f = fopen(p, "wb");
    if (!f) { printf("cannot write %s: %s\n", p, strerror(errno)); exit(2); }
    fwrite(data, 1, len < 0 ? strlen(data) : (size_t)len, f);
    fclose(f);
}
static void put(const char *root, const char *rel, const char *text) { putn(root, rel, text, -1); }
static void mkdirs_in(const char *root, const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdirs(p);
}

static void lnk(const char *root, const char *rel, const char *target) {
    char p[1024], d[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    unlink(p);
    if (symlink(target, p)) printf("symlink %s: %s\n", p, strerror(errno));
}

static void driver(const char *root, const char *dir, const char *name) {
    char rel[512], target[256];
    snprintf(rel, sizeof rel, "%s/driver", dir);
    snprintf(target, sizeof target, "../../../bus/drivers/%s", name);
    lnk(root, rel, target);
}

/* A 128-byte EDID 1.4 with a name, a serial descriptor and a native mode. */
static void edid(uint8_t *e, const char *pnp, uint16_t product, const char *name, const char *serial, int w, int h, int wcm, int hcm, int year) {
    memset(e, 0, 128);
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    memcpy(e, hdr, 8);
    uint16_t m = (uint16_t)(((pnp[0] - 'A' + 1) << 10) | ((pnp[1] - 'A' + 1) << 5) | (pnp[2] - 'A' + 1));
    e[8] = (uint8_t)(m >> 8); e[9] = (uint8_t)m;
    e[10] = (uint8_t)product; e[11] = (uint8_t)(product >> 8);
    e[12] = 0xef; e[13] = 0xbe; e[14] = 0xad; e[15] = 0xde;      /* serial 0xdeadbeef */
    e[16] = 10; e[17] = (uint8_t)(year - 1990);
    e[18] = 1; e[19] = 4; e[20] = 0x80; e[21] = (uint8_t)wcm; e[22] = (uint8_t)hcm;
    uint8_t *t = e + 54;                                          /* detailed timing */
    t[0] = 0x56; t[1] = 0x5e;
    t[2] = (uint8_t)w; t[4] = (uint8_t)((w >> 8) << 4);
    t[5] = (uint8_t)h; t[7] = (uint8_t)((h >> 8) << 4);
    const char *txt[2] = { name, serial };
    uint8_t tag[2] = { 0xfc, 0xff };
    for (int k = 0; k < 2; k++) {
        uint8_t *d = e + 72 + 18 * k;
        d[3] = tag[k];
        size_t n = strlen(txt[k]);
        for (int i = 0; i < 13; i++) d[5 + i] = (size_t)i < n ? (uint8_t)txt[k][i] : (size_t)i == n ? 0x0a : 0x20;
    }
    e[108 + 3] = 0x10;                                            /* dummy descriptor */
    unsigned sum = 0;
    for (int i = 0; i < 127; i++) sum += e[i];
    e[127] = (uint8_t)(256 - sum % 256);
}

static const char *PCI_IDS =
    "# pci.ids (a few lines)\n"
    "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
    "\t67df  Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]\n"
    "\t\t1002 0b31  Radeon RX 580\n"
    "\tab38  Navi 10 HDMI Audio\n"
    "10de  NVIDIA Corporation\n"
    "\t2684  AD102 [GeForce RTX 4090]\n"
    "144d  Samsung Electronics Co Ltd\n"
    "\ta808  NVMe SSD Controller SM981/PM981/PM983\n"
    "1af4  Red Hat, Inc.\n"
    "\t1050  Virtio 1.0 GPU\n"
    "8086  Intel Corporation\n"
    "\t9a49  TigerLake-LP GT2 [Iris Xe Graphics]\n"
    "\t2725  Wi-Fi 6E(802.11ax) AX210/AX1675* 2x2 [Typhoon Peak]\n"
    "C 02  Network controller\n"
    "\t80  Network controller\n"
    "C 03  Display controller\n"
    "\t00  VGA compatible controller\n"
    "\t80  Display controller\n"
    "C 01  Mass storage controller\n"
    "\t08  Non-Volatile memory controller\n";

static const char *USB_IDS =
    "046d  Logitech, Inc.\n"
    "\tc52b  Unifying Receiver\n"
    "0d8c  C-Media Electronics, Inc.\n"
    "\t0014  Audio Adapter (Unitek Y-247A)\n"
    "0bda  Realtek Semiconductor Corp.\n"
    "\t8153  RTL8153 Gigabit Ethernet Adapter\n"
    "C 00  (Defined at Interface level)\n";

/* the things no report may carry */
static const char *SECRETS[] = { "SECRET", "secret", "deadbeef", "DEADBEEF", "de:ad:be:ef", "4c4c4544", "enxdeadbeef", "SECRETMON", "secretuser", "/home/" };

static void common_secrets(const char *r) {
    put(r, "proc/sys/kernel/hostname", "secret-host\n");
    put(r, "sys/class/dmi/id/product_serial", "SECRET-SERIAL-123\n");
    put(r, "sys/class/dmi/id/product_uuid", "4c4c4544-0042-3510-8051-b7c04f4d4d32\n");
    put(r, "sys/class/dmi/id/board_serial", "SECRET-BOARD\n");
    put(r, "sys/class/dmi/id/chassis_serial", "SECRET-CHASSIS\n");
    put(r, "etc/hostname", "secret-host\n");
    put(r, "etc/passwd", "secretuser:x:1000:1000:Secret User,,,:/home/secretuser:/bin/bash\n");
}

static void kernel(const char *r, const char *rel) {
    char p[256];
    put(r, "proc/sys/kernel/osrelease", rel);
    snprintf(p, sizeof p, "boot/config-%s", rel);
    put(r, p, "#\n# Linux kernel config\n#\nCONFIG_SECURITY_LANDLOCK=y\nCONFIG_BPF_SYSCALL=y\n# CONFIG_BPF_LSM is not set\nCONFIG_ZRAM=m\nCONFIG_USER_NS=y\n");
    put(r, "sys/kernel/security/lsm", "lockdown,capability,landlock,yama,apparmor,bpf");
    put(r, "sys/fs/cgroup/cgroup.controllers", "cpu memory io pids");
    put(r, "etc/os-release", "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\nID=debian\nVERSION_ID=\"13\"\n");
    put(r, "usr/share/misc/pci.ids", PCI_IDS);
    put(r, "usr/share/misc/usb.ids", USB_IDS);
}

static void cpus(const char *r, int threads, int per_core) {
    for (int i = 0; i < threads; i++) {
        char p[256], v[16];
        snprintf(p, sizeof p, "sys/devices/system/cpu/cpu%d/topology/core_id", i);
        snprintf(v, sizeof v, "%d\n", i / per_core);
        put(r, p, v);
        snprintf(p, sizeof p, "sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        put(r, p, "0\n");
    }
    put(r, "sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", "800000\n");
    put(r, "sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "4200000\n");
    put(r, "sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "schedutil\n");
    put(r, "sys/devices/system/cpu/cpu0/cache/index0/level", "1"); put(r, "sys/devices/system/cpu/cpu0/cache/index0/type", "Data");
    put(r, "sys/devices/system/cpu/cpu0/cache/index0/size", "32K");
    put(r, "sys/devices/system/cpu/cpu0/cache/index1/level", "1"); put(r, "sys/devices/system/cpu/cpu0/cache/index1/type", "Instruction");
    put(r, "sys/devices/system/cpu/cpu0/cache/index1/size", "32K");
    put(r, "sys/devices/system/cpu/cpu0/cache/index2/level", "2"); put(r, "sys/devices/system/cpu/cpu0/cache/index2/type", "Unified");
    put(r, "sys/devices/system/cpu/cpu0/cache/index2/size", "512K");
    put(r, "sys/devices/system/cpu/cpu0/cache/index3/level", "3"); put(r, "sys/devices/system/cpu/cpu0/cache/index3/type", "Unified");
    put(r, "sys/devices/system/cpu/cpu0/cache/index3/size", "32768K");
}

/* a PCI device under /sys/bus/pci/devices */
static void pcidev(const char *r, const char *slot, const char *ven, const char *dev, const char *cls, const char *drv) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/bus/pci/devices/%s", slot);
    snprintf(f, sizeof f, "%s/vendor", d); put(r, f, ven);
    snprintf(f, sizeof f, "%s/device", d); put(r, f, dev);
    snprintf(f, sizeof f, "%s/class", d); put(r, f, cls);
    if (drv) driver(r, d, drv);
}

/* a DRM card: device ids (or none for a platform GPU), driver, a render node and connectors */
static void card(const char *r, int n, const char *ven, const char *dev, const char *drv, int render, int boot) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/class/drm/card%d/device", n);
    if (ven) { snprintf(f, sizeof f, "%s/vendor", d); put(r, f, ven); snprintf(f, sizeof f, "%s/device", d); put(r, f, dev); }
    if (boot) { snprintf(f, sizeof f, "%s/boot_vga", d); put(r, f, "1\n"); }
    driver(r, d, drv);
    if (render) { snprintf(f, sizeof f, "%s/drm/renderD%d/dev", d, 128 + n); put(r, f, "226:128\n"); }
    snprintf(f, sizeof f, "%s/serial", d); put(r, f, "SECRET-GPU\n");
}

static void conn(const char *r, int n, const char *name, int connected, const uint8_t *ed, const char *modes) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/class/drm/card%d-%s", n, name);
    snprintf(f, sizeof f, "%s/status", d); put(r, f, connected ? "connected\n" : "disconnected\n");
    snprintf(f, sizeof f, "%s/enabled", d); put(r, f, connected ? "enabled\n" : "disabled\n");
    if (ed) { snprintf(f, sizeof f, "%s/edid", d); putn(r, f, ed, 128); }
    if (modes) { snprintf(f, sizeof f, "%s/modes", d); put(r, f, modes); }
}

static void netif(const char *r, const char *name, const char *drv, int wifi, const char *speed) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/class/net/%s", name);
    snprintf(f, sizeof f, "%s/address", d); put(r, f, "de:ad:be:ef:00:01\n");
    snprintf(f, sizeof f, "%s/type", d); put(r, f, "1\n");
    snprintf(f, sizeof f, "%s/operstate", d); put(r, f, speed ? "up\n" : "down\n");
    if (speed) { snprintf(f, sizeof f, "%s/speed", d); put(r, f, speed); }
    if (drv) { snprintf(f, sizeof f, "%s/device", d); driver(r, f, drv); }
    if (wifi) { snprintf(f, sizeof f, "%s/wireless/x", d); put(r, f, ""); }
}

static void disk(const char *r, const char *name, const char *model, const char *sectors, int rot, const char *link) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/block/%s", name);
    if (link) {                         /* sdX is a link into the device tree: its path says the bus */
        char real[256];
        snprintf(real, sizeof real, "sys/devices/%s/block/%s", link, name);
        snprintf(f, sizeof f, "%s/size", real); put(r, f, sectors);
        snprintf(f, sizeof f, "../devices/%s/block/%s", link, name);
        lnk(r, d, f);
    } else {
        snprintf(f, sizeof f, "%s/size", d); put(r, f, sectors);
    }
    snprintf(f, sizeof f, "%s/queue/rotational", d); put(r, f, rot ? "1\n" : "0\n");
    snprintf(f, sizeof f, "%s/removable", d); put(r, f, "0\n");
    snprintf(f, sizeof f, "%s/device/model", d); put(r, f, model);
    snprintf(f, sizeof f, "%s/device/serial", d); put(r, f, "SECRET-DISK\n");
    snprintf(f, sizeof f, "%s/device/wwid", d); put(r, f, "eui.SECRET0000\n");
}

static void fixture(const char *r, const char *kind) {
    uint8_t ed[128];
    common_secrets(r);
    kernel(r, "6.12.43+deb13-amd64");
    put(r, "proc/meminfo", "MemTotal:       32768000 kB\nMemFree:  1 kB\nSwapTotal:       2097148 kB\n");
    put(r, "proc/asound/cards", " 0 [Generic        ]: HDA-Intel - HD-Audio Generic\n                      HD-Audio Generic at 0xfcd00000 irq 89\n");
    put(r, "run/lordos/netperm.sock", "");
    if (!strcmp(kind, "amd")) {
        put(r, "sys/class/dmi/id/sys_vendor", "Micro-Star International Co., Ltd.\n");
        put(r, "sys/class/dmi/id/product_name", "MS-7C02\n");
        put(r, "sys/class/dmi/id/product_version", "To be filled by O.E.M.\n");
        put(r, "sys/class/dmi/id/board_vendor", "Micro-Star International Co., Ltd.\n");
        put(r, "sys/class/dmi/id/board_name", "B450 TOMAHAWK MAX (MS-7C02)\n");
        put(r, "sys/class/dmi/id/board_version", "1.0\n");
        put(r, "sys/class/dmi/id/chassis_type", "3\n");
        put(r, "sys/class/dmi/id/bios_vendor", "American Megatrends Inc.\n");
        put(r, "sys/class/dmi/id/bios_version", "3.J0\n");
        put(r, "sys/class/dmi/id/bios_date", "11/23/2023\n");
        uint8_t sb[5] = { 6, 0, 0, 0, 1 };
        putn(r, "sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", sb, 5);
        put(r, "proc/cpuinfo", "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Ryzen 7 5800X 8-Core Processor\n"
                               "microcode\t: 0xa201205\nflags\t\t: fpu sse4_2 avx avx2 fma aes sha_ni\n");
        cpus(r, 16, 2);
        card(r, 0, "0x1002", "0x67df", "amdgpu", 1, 1);
        edid(ed, "DEL", 0xa0c4, "DELL U2720Q", "SECRETMON01", 3840, 2160, 60, 34, 2021);
        conn(r, 0, "DP-1", 1, ed, "3840x2160\n2560x1440\n");
        conn(r, 0, "HDMI-A-1", 0, NULL, NULL);
        pcidev(r, "0000:0a:00.0", "0x1002", "0x67df", "0x030000", "amdgpu");
        pcidev(r, "0000:0a:00.1", "0x1002", "0xab38", "0x040300", "snd_hda_intel");
        pcidev(r, "0000:01:00.0", "0x144d", "0xa808", "0x010802", "nvme");
        disk(r, "nvme0n1", "Samsung SSD 970 EVO Plus 1TB\n", "1953525168\n", 0, NULL);
        put(r, "sys/block/nvme0n1/device/device/current_link_speed", "8.0 GT/s PCIe\n");
        put(r, "sys/block/nvme0n1/device/device/current_link_width", "4\n");
        disk(r, "sda", "WDC WD40EFRX-68N\n", "7814037168\n", 1, "pci0000:00/0000:00:01.3/ata1/host0/target0:0:0/0:0:0:0");
        disk(r, "loop0", "", "1000\n", 0, NULL);
        netif(r, "enp5s0", "r8169", 0, "1000\n");
        netif(r, "docker0", NULL, 0, NULL);
        put(r, "sys/bus/usb/devices/usb1/idVendor", "1d6b\n");
        put(r, "sys/bus/usb/devices/1-2/idVendor", "046d\n"); put(r, "sys/bus/usb/devices/1-2/idProduct", "c52b\n");
        put(r, "sys/bus/usb/devices/1-2/speed", "12\n"); put(r, "sys/bus/usb/devices/1-2/bDeviceClass", "00\n");
        put(r, "sys/bus/usb/devices/1-2/serial", "SECRET-USB\n"); put(r, "sys/bus/usb/devices/1-2/product", "secretuser's receiver\n");
        driver(r, "sys/bus/usb/devices/1-2/1-2:1.0", "usbhid");
        put(r, "sys/class/hwmon/hwmon0/name", "k10temp"); put(r, "sys/class/hwmon/hwmon0/temp1_input", "41250");
        put(r, "sys/class/hwmon/hwmon1/name", "nct6797"); put(r, "sys/class/hwmon/hwmon1/fan1_input", "900");
        put(r, "sys/class/hwmon/hwmon1/fan2_input", "0");
    } else if (!strcmp(kind, "intel")) {
        put(r, "sys/class/dmi/id/sys_vendor", "LENOVO\n");
        put(r, "sys/class/dmi/id/product_name", "20XW0026US\n");
        put(r, "sys/class/dmi/id/product_version", "ThinkPad X1 Carbon Gen 9\n");
        put(r, "sys/class/dmi/id/board_version", "Not Specified\n");
        put(r, "sys/class/dmi/id/chassis_type", "10\n");
        put(r, "sys/class/dmi/id/bios_vendor", "LENOVO\n");
        put(r, "sys/firmware/efi/x", "");
        put(r, "proc/cpuinfo", "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: 11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz\n"
                               "flags\t\t: fpu sse4_2 avx avx2 avx512f fma aes vaes\n");
        cpus(r, 8, 2);
        card(r, 0, "0x8086", "0x9a49", "i915", 1, 1);
        edid(ed, "AUO", 0x2336, "", "SECRETMON02", 1920, 1200, 30, 19, 2020);
        conn(r, 0, "eDP-1", 1, ed, "1920x1200\n");
        pcidev(r, "0000:00:02.0", "0x8086", "0x9a49", "0x030000", "i915");
        pcidev(r, "0000:00:14.3", "0x8086", "0x2725", "0x028000", "iwlwifi");
        netif(r, "wlp0s20f3", "iwlwifi", 1, "866\n");
        netif(r, "enxdeadbeef0001", "r8152", 0, "1000\n");   /* the name carries the MAC */
        put(r, "sys/class/power_supply/BAT0/type", "Battery\n");
        put(r, "sys/class/power_supply/BAT0/manufacturer", "SMP\n");
        put(r, "sys/class/power_supply/BAT0/model_name", "5B10W13975\n");
        put(r, "sys/class/power_supply/BAT0/technology", "Li-poly\n");
        put(r, "sys/class/power_supply/BAT0/capacity", "76\n");
        put(r, "sys/class/power_supply/BAT0/status", "Discharging\n");
        put(r, "sys/class/power_supply/BAT0/energy_full", "45000000\n");
        put(r, "sys/class/power_supply/BAT0/energy_full_design", "57000000\n");
        put(r, "sys/class/power_supply/BAT0/serial_number", "SECRET-BAT\n");
        put(r, "sys/class/power_supply/AC/type", "Mains\n");
        put(r, "sys/class/power_supply/AC/online", "0\n");
        put(r, "sys/class/power_supply/hidpp_battery_0/type", "Battery\n");
        put(r, "sys/class/power_supply/hidpp_battery_0/scope", "Device\n");
        put(r, "sys/class/bluetooth/hci0/address", "de:ad:be:ef:00:02\n");
        driver(r, "sys/class/bluetooth/hci0/device", "btusb");
        put(r, "sys/class/bluetooth/hci0:256/x", "");
        put(r, "sys/class/thermal/thermal_zone0/type", "x86_pkg_temp"); put(r, "sys/class/thermal/thermal_zone0/temp", "52000");
    } else if (!strcmp(kind, "nvidia")) {
        put(r, "sys/class/dmi/id/sys_vendor", "ASUS\n");
        put(r, "sys/class/dmi/id/chassis_type", "7\n");
        put(r, "proc/cpuinfo", "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Core(TM) i9-10900\nflags\t\t: avx2\n");
        cpus(r, 20, 2);
        card(r, 0, "0x10de", "0x2684", "nouveau", 1, 1);
        conn(r, 0, "DP-1", 0, NULL, NULL);
        pcidev(r, "0000:01:00.0", "0x10de", "0x2684", "0x030000", "nouveau");
    } else if (!strcmp(kind, "rpi5")) {
        putn(r, "proc/device-tree/model", "Raspberry Pi 5 Model B Rev 1.0", 31);
        putn(r, "proc/device-tree/compatible", "raspberrypi,5-model-b\0brcm,bcm2712\0", 36);
        putn(r, "proc/device-tree/serial-number", "10000000deadbeef", 17);
        putn(r, "proc/device-tree/chosen/bootloader/version", "1accd6659983613cd104973ce2432da14f1a7682", 41);
        putn(r, "proc/device-tree/chosen/bootloader/build-timestamp", "\x6a\xb6\x9a\x4d", 4);
        /* the cards of a Pi 5 in an Argon ONE V5 case, as ALSA lists them */
        put(r, "proc/asound/cards", " 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi-0\n                      vc4-hdmi-0\n"
                                    " 1 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi-1\n                      vc4-hdmi-1\n"
                                    " 2 [Device         ]: USB-Audio - USB Audio Device\n"
                                    "                      C-Media Electronics Inc. USB Audio Device at usb-1000480000.usb-1.4, full speed\n");
        put(r, "proc/asound/card2/usbid", "0d8c:0014\n");
        put(r, "proc/cpuinfo", "processor\t: 0\nBogoMIPS\t: 108.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n"
                               "CPU implementer\t: 0x41\nCPU part\t: 0xd0b\n\nprocessor\t: 1\n\n"
                               "Revision\t: d04170\nSerial\t\t: 10000000deadbeef\nModel\t\t: Raspberry Pi 5 Model B Rev 1.0\n");
        cpus(r, 4, 1);
        put(r, "proc/sys/kernel/arch", "aarch64\n");
        card(r, 0, NULL, NULL, "v3d", 1, 0);
        card(r, 1, NULL, NULL, "vc4-drm", 0, 0);
        edid(ed, "SAM", 0x7155, "LS27A600U", "SECRETMON03", 2560, 1440, 60, 34, 2022);
        conn(r, 1, "HDMI-A-1", 1, ed, "2560x1440\n");
        conn(r, 1, "HDMI-A-2", 0, NULL, NULL);
        conn(r, 1, "Writeback-1", 0, NULL, NULL);
        disk(r, "mmcblk0", "", "62333952\n", 0, NULL);
        disk(r, "mmcblk0boot0", "", "8192\n", 0, NULL);
        /* the LordOS rpi-6.18.y kernel: no get_throttled under soc@107c000000:firmware, only rpi_volt's alarm */
        mkdirs_in(r, "sys/devices/platform/soc@107c000000/soc@107c000000:firmware");
        put(r, "sys/class/hwmon/hwmon1/name", "rpi_volt\n"); put(r, "sys/class/hwmon/hwmon1/in0_lcrit_alarm", "0\n");
        put(r, "sys/block/zram0/disksize", "2147483648\n");
        put(r, "sys/block/zram0/comp_algorithm", "lzo lzo-rle lz4 [zstd]\n");
        put(r, "sys/class/hwmon/hwmon2/name", "pwmfan"); put(r, "sys/class/hwmon/hwmon2/fan1_input", "2900");
    } else if (!strcmp(kind, "vm")) {
        put(r, "sys/class/dmi/id/sys_vendor", "QEMU\n");
        put(r, "sys/class/dmi/id/product_name", "Standard PC (Q35 + ICH9, 2009)\n");
        put(r, "sys/class/dmi/id/chassis_type", "1\n");
        put(r, "sys/class/dmi/id/bios_vendor", "SeaBIOS\n");
        put(r, "proc/cpuinfo", "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel Xeon Processor (Skylake, IBRS)\n"
                               "flags\t\t: fpu sse4_2 avx avx2 hypervisor\n");
        cpus(r, 4, 1);
        card(r, 0, "0x1af4", "0x1050", "virtio-pci", 0, 1);
        conn(r, 0, "Virtual-1", 1, NULL, "1280x800\n1024x768\n");
        disk(r, "vda", "", "67108864\n", 1, NULL);
        netif(r, "ens18", "virtio_net", 0, "-1\n");
    }
}

static int contains(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

/* Collects the tree, checks the JSON parses and holds no secret; returns the report and its texts. */
static SysReport *run(const char *root, char **json, char **md) {
    SysReport *r = calloc(1, sizeof *r);
    sysreport_collect(r, root, NULL, NULL);
    snprintf(r->sw.lordos, sizeof r->sw.lordos, "0.8.5-beta");
    *json = sysreport_json(r);
    *md = sysreport_markdown(r);
    JDoc doc;
    char err[128] = "";
    CHECK(*json && !json_parse(*json, strlen(*json), &doc, err, sizeof err), "%s: JSON invalid: %s", root, err);
    if (*json && !err[0]) json_free(&doc);
    for (size_t i = 0; i < sizeof SECRETS / sizeof SECRETS[0]; i++) {
        CHECK(!contains(*json, SECRETS[i]), "%s: the JSON carries \"%s\"", root, SECRETS[i]);
        CHECK(!contains(*md, SECRETS[i]), "%s: the Markdown carries \"%s\"", root, SECRETS[i]);
    }
    return r;
}

static void test_ids(void) {
    char path[] = "/tmp/sysreport-ids-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, PCI_IDS, strlen(PCI_IDS)) != (ssize_t)strlen(PCI_IDS)) { printf("mkstemp\n"); exit(2); }
    close(fd);
    IdsDb db;
    CHECK(!ids_open(&db, path), "ids_open");
    char v[96], d[96];
    ids_lookup(&db, "1002", "67df", v, sizeof v, d, sizeof d);
    CHECK(!strcmp(v, "Advanced Micro Devices, Inc. [AMD/ATI]") && !strcmp(d, "Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]"), "ids amd: %s / %s", v, d);
    ids_lookup(&db, "10DE", "2684", v, sizeof v, d, sizeof d);
    CHECK(!strcmp(v, "NVIDIA Corporation") && !strcmp(d, "AD102 [GeForce RTX 4090]"), "ids uppercase: %s / %s", v, d);
    ids_lookup(&db, "1002", "2684", v, sizeof v, d, sizeof d);     /* another vendor's device */
    CHECK(!strcmp(v, "Advanced Micro Devices, Inc. [AMD/ATI]") && !d[0], "ids no device across vendors: %s", d);
    ids_lookup(&db, "1002", "0b31", v, sizeof v, d, sizeof d);     /* a subsystem line, not a device */
    CHECK(!d[0], "ids subsystem is not a device: %s", d);
    ids_lookup(&db, "dead", "beef", v, sizeof v, d, sizeof d);
    CHECK(!v[0] && !d[0], "ids unknown");
    ids_lookup(&db, "10d", "2684", v, sizeof v, d, sizeof d);
    CHECK(!v[0], "ids short vendor id");
    char cn[48], sn[48];
    ids_pci_class(&db, "030000", cn, sizeof cn, sn, sizeof sn);
    CHECK(!strcmp(cn, "Display controller") && !strcmp(sn, "VGA compatible controller"), "class: %s / %s", cn, sn);
    ids_pci_class(&db, "028000", cn, sizeof cn, sn, sizeof sn);
    CHECK(!strcmp(cn, "Network controller") && !strcmp(sn, "Network controller"), "class 0280: %s / %s", cn, sn);
    ids_pci_class(&db, "0c0330", cn, sizeof cn, sn, sizeof sn);
    CHECK(!cn[0] && !sn[0], "class unknown");
    ids_close(&db);
    unlink(path);
    CHECK(ids_open(&db, "/nonexistent/pci.ids") == -1, "ids missing file");
    ids_lookup(&db, "1002", "67df", v, sizeof v, d, sizeof d);
    CHECK(!v[0] && !d[0], "ids lookups on an empty db");
}

static void rmtree(const char *dir) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("cannot remove %s\n", dir);
}

static void test_kind(const char *kind) {
    char root[] = "/tmp/sysreport-test-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    fixture(root, kind);
    char *json, *md;
    SysReport *r = run(root, &json, &md);
    CHECK(r->schema == 1 && strlen(r->date) == 10, "%s: schema/date", kind);
    CHECK(!strcmp(r->sw.os, "Debian GNU/Linux 13 (trixie)") && !strcmp(r->sw.os_version, "13"), "%s: os %s", kind, r->sw.os);
    CHECK(r->sw.landlock_abi == -1, "%s: landlock unknown under a fake root", kind);
    CHECK(r->sw.bpf_lsm && r->sw.cgroup2 && r->sw.netperm && !r->sw.fand, "%s: lsm/cgroup/services", kind);
    int ll = 0, bl = 0, zr = 0;
    for (int i = 0; i < r->sw.nkconf; i++) {
        if (!strcmp(r->sw.kconf[i].name, "CONFIG_SECURITY_LANDLOCK")) ll = !strcmp(r->sw.kconf[i].value, "y");
        if (!strcmp(r->sw.kconf[i].name, "CONFIG_BPF_LSM")) bl = !strcmp(r->sw.kconf[i].value, "n");
        if (!strcmp(r->sw.kconf[i].name, "CONFIG_ZRAM")) zr = !strcmp(r->sw.kconf[i].value, "m");
    }
    CHECK(ll && bl && zr, "%s: kernel config", kind);
    if (strcmp(kind, "rpi5")) CHECK(r->naudio == 1 && !strcmp(r->audio[0], "HD-Audio Generic"), "%s: audio %s", kind, r->audio[0]);
    CHECK(contains(md, "# System report") && contains(md, "| LordOS | 0.8.5-beta |"), "%s: markdown head", kind);

    if (!strcmp(kind, "amd")) {
        CHECK(!strcmp(r->machine.vendor, "Micro-Star International Co., Ltd.") && !r->machine.version[0], "amd: dmi (placeholder dropped)");
        CHECK(!strcmp(r->machine.chassis, "desktop") && !strcmp(r->machine.firmware, "uefi") && r->machine.secure_boot == 1, "amd: chassis/firmware/sb");
        CHECK(!strcmp(r->machine.board_version, "1.0") && contains(json, "\"boardVersion\":\"1.0\"") && contains(md, "(MS-7C02), 1.0 |"), "amd: board version");
        CHECK(!strcmp(r->machine.bios_vendor, "American Megatrends Inc.") && !strcmp(r->machine.bios_version, "3.J0") && !strcmp(r->machine.bios_date, "11/23/2023"), "amd: firmware fields");
        CHECK(!r->machine.virt[0], "amd: bare metal");
        CHECK(r->cpu.threads == 16 && r->cpu.cores == 8 && r->cpu.sockets == 1, "amd: topology %d/%d/%d", r->cpu.sockets, r->cpu.cores, r->cpu.threads);
        CHECK(r->cpu.max_mhz == 4200 && r->cpu.min_mhz == 800 && !strcmp(r->cpu.governor, "schedutil"), "amd: freq");
        CHECK(r->cpu.l1d_kb == 32 && r->cpu.l2_kb == 512 && r->cpu.l3_kb == 32768, "amd: caches");
        CHECK(!strcmp(r->cpu.flags, "sse4_2 avx avx2 fma aes sha_ni"), "amd: flags \"%s\"", r->cpu.flags);
        CHECK(r->mem.total_kb == 32768000 && r->mem.swap_kb == 2097148 && !r->mem.zram_n, "amd: memory");
        CHECK(r->ngpu == 1 && !strcmp(r->gpu[0].driver, "amdgpu") && r->gpu[0].render_node && r->gpu[0].boot_vga
              && r->gpu[0].connectors == 2 && r->gpu[0].connected == 1 && !strcmp(r->gpu[0].bus, "pci"), "amd: gpu");
        CHECK(contains(r->gpu[0].device, "Radeon RX 470/480/570"), "amd: gpu name %s", r->gpu[0].device);
        CHECK(r->ndisplay == 1 && !strcmp(r->display[0].name, "DELL U2720Q") && !strcmp(r->display[0].pnp, "DEL")
              && r->display[0].pref_w == 3840 && r->display[0].year == 2021 && r->display[0].diag_tenths > 270
              && !strcmp(r->display[0].connector, "DP-1") && !strcmp(r->display[0].mode, "3840x2160"), "amd: display");
        CHECK(r->ndisk == 2 && !strcmp(r->disk[0].name, "nvme0n1") && !strcmp(r->disk[0].transport, "nvme")
              && !strcmp(r->disk[0].link, "8.0 GT/s PCIe x4") && !strcmp(r->disk[1].transport, "sata") && r->disk[1].rotational, "amd: disks");
        CHECK(r->npci == 3 && !strcmp(r->pci[0].class_name, "Non-Volatile memory controller") && !strcmp(r->pci[0].driver, "nvme"), "amd: pci");
        CHECK(r->nusb == 1 && !strcmp(r->usb[0].product, "Unifying Receiver") && !strcmp(r->usb[0].driver, "usbhid") && r->usb[0].speed_mbps == 12, "amd: usb");
        CHECK(r->nnet == 2 && !strcmp(r->net[0].kind, "virtual") && !strcmp(r->net[1].kind, "ethernet") && r->net[1].speed_mbps == 1000, "amd: net");
        int fans = 0;
        for (int i = 0; i < r->nthermal; i++) fans += r->thermal[i].fans;
        CHECK(r->nthermal == 2 && r->thermal[0].milli_c == 41250 && fans == 2, "amd: thermal");
        CHECK(!r->pi.is_pi && !contains(json, "raspberryPi"), "amd: no Pi");
    } else if (!strcmp(kind, "intel")) {
        CHECK(!strcmp(r->machine.chassis, "laptop") && !strcmp(r->machine.version, "ThinkPad X1 Carbon Gen 9") && r->machine.secure_boot == -1, "intel: machine");
        CHECK(!r->machine.board_version[0], "intel: a placeholder board version is dropped");
        CHECK(!strcmp(r->cpu.flags, "sse4_2 avx avx2 fma avx512f aes vaes"), "intel: flags %s", r->cpu.flags);
        CHECK(r->ndisplay == 1 && !r->display[0].name[0] && !strcmp(r->display[0].pnp, "AUO") && contains(md, "| eDP-1 | AUO, 2020 |"), "intel: panel");
        CHECK(r->nnet == 2 && !strcmp(r->net[0].kind, "ethernet") && !strcmp(r->net[0].driver, "r8152")
              && !strcmp(r->net[1].kind, "wifi") && !strcmp(r->net[1].driver, "iwlwifi"), "intel: net %s %s", r->net[0].kind, r->net[1].kind);
        CHECK(r->npower == 2 && !strcmp(r->power[0].kind, "ac") && r->power[0].online == 0
              && !strcmp(r->power[1].kind, "battery") && r->power[1].capacity == 76 && r->power[1].health == 78, "intel: power");
        CHECK(contains(md, "holds 78 % of its design charge"), "intel: markdown battery");
        CHECK(r->nbt == 1 && !strcmp(r->bt_driver[0], "btusb"), "intel: bluetooth");
        CHECK(contains(r->pci[1].class_name, "Network controller") && contains(r->pci[1].device, "AX210"), "intel: wifi pci");
    } else if (!strcmp(kind, "nvidia")) {
        CHECK(r->ngpu == 1 && !strcmp(r->gpu[0].vendor, "NVIDIA Corporation") && !strcmp(r->gpu[0].driver, "nouveau") && !r->gpu[0].connected, "nvidia: gpu");
        CHECK(r->cpu.cores == 10 && r->cpu.threads == 20 && !strcmp(r->machine.chassis, "desktop"), "nvidia: cpu/chassis");
        CHECK(!r->ndisplay && !contains(md, "## Displays"), "nvidia: no displays");
    } else if (!strcmp(kind, "rpi5")) {
        CHECK(r->pi.is_pi && !strcmp(r->pi.revision, "d04170") && r->pi.throttled == -1 && r->pi.uv_alarm == 0,
              "rpi5: pi %s %x %d", r->pi.revision, r->pi.throttled, r->pi.uv_alarm);
        CHECK(!strcmp(r->machine.firmware, "dt") && !strcmp(r->machine.model, "Raspberry Pi 5 Model B Rev 1.0"), "rpi5: machine");
        CHECK(!strcmp(r->pi.bootloader_version, "1accd6659983613cd104973ce2432da14f1a7682") && !strcmp(r->pi.bootloader_date, "2026-09-25"),
              "rpi5: bootloader %s %s", r->pi.bootloader_version, r->pi.bootloader_date);
        CHECK(contains(json, "\"bootloaderDate\":\"2026-09-25\"") && contains(md, ", bootloader of 2026-09-25"), "rpi5: bootloader in the report");
        CHECK(r->naudio == 3 && !strcmp(r->audio[0], "Raspberry Pi HDMI 0 audio") && !strcmp(r->audio[1], "Raspberry Pi HDMI 1 audio")
              && !strcmp(r->audio[2], "C-Media Electronics, Inc. Audio Adapter (Unitek Y-247A)"), "rpi5: audio %s | %s | %s", r->audio[0], r->audio[1], r->audio[2]);
        CHECK(!contains(json, "usb-1000480000"), "rpi5: no USB path in the audio names");
        SysReport *no_ids = calloc(1, sizeof *no_ids);      /* the Pi image has no usb.ids */
        sysreport_collect(no_ids, root, NULL, "/nonexistent/usb.ids");
        CHECK(no_ids->naudio == 3 && !strcmp(no_ids->audio[2], "C-Media Electronics Inc. USB Audio Device"), "rpi5: audio without usb.ids: %s", no_ids->audio[2]);
        free(no_ids);
        CHECK(!strcmp(r->cpu.vendor, "ARM") && !strcmp(r->cpu.model, "Cortex-A76") && !strcmp(r->cpu.arch, "aarch64"), "rpi5: cpu %s %s", r->cpu.vendor, r->cpu.model);
        CHECK(!strcmp(r->cpu.flags, "asimd aes sha2 crc32 atomics"), "rpi5: features %s", r->cpu.flags);
        CHECK(r->ngpu == 2 && !strcmp(r->gpu[0].bus, "platform") && !strcmp(r->gpu[0].driver, "v3d") && r->gpu[0].render_node
              && !strcmp(r->gpu[1].driver, "vc4-drm") && r->gpu[1].connectors == 2 && r->gpu[1].connected == 1, "rpi5: gpus");
        CHECK(r->ndisplay == 1 && !strcmp(r->display[0].card, "card1") && !strcmp(r->display[0].name, "LS27A600U"), "rpi5: display");
        CHECK(r->ndisk == 1 && !strcmp(r->disk[0].transport, "mmc"), "rpi5: sd card (boot partitions skipped)");
        CHECK(r->mem.zram_n == 1 && r->mem.zram_kb[0] == 2097152 && !strcmp(r->mem.zram_algo[0], "zstd"), "rpi5: zram");
        CHECK(contains(json, "\"raspberryPi\":{") && contains(json, "\"throttled\":null,\"underVoltageAlarm\":false}"),
              "rpi5: report json");
        CHECK(contains(md, "no under-voltage in the last 2 s (rpi_volt); throttling not reported by this kernel"), "rpi5: report markdown");
    } else if (!strcmp(kind, "vm")) {
        CHECK(!strcmp(r->machine.virt, "kvm") && !strcmp(r->machine.chassis, "vm") && !strcmp(r->machine.firmware, "bios"), "vm: virt %s %s", r->machine.virt, r->machine.chassis);
        CHECK(r->ngpu == 1 && !strcmp(r->gpu[0].device, "Virtio 1.0 GPU") && !r->gpu[0].render_node, "vm: gpu");
        CHECK(r->ndisplay == 1 && !r->display[0].pref_w && contains(md, "| Virtual-1 | — | — | 1280x800 |"), "vm: display without EDID");
        CHECK(r->ndisk == 1 && !strcmp(r->disk[0].transport, "virtio") && r->nnet == 1 && r->net[0].speed_mbps == -1, "vm: disk/net");
        CHECK(contains(json, "\"speedMbps\":null"), "vm: unknown speed is null");
    }
    free(json);
    free(md);
    free(r);
    rmtree(root);
}

static void test_fetch(const char *kind) {
    char root[] = "/tmp/sysreport-fetch-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    fixture(root, kind);
    SysReport *r = calloc(1, sizeof *r);
    sysreport_collect(r, root, NULL, NULL);
    snprintf(r->sw.lordos, sizeof r->sw.lordos, "0.8.5-beta");
    FetchLive l;
    fetch_live(&l, root, 1);
    CHECK(!l.user[0] && !l.host[0], "%s: --share has no names", kind);
    FetchColor modes[3] = { FETCH_PLAIN, FETCH_256, FETCH_TRUECOLOR };
    for (int m = 0; m < 3; m++) {
        char *out = fetch_render(r, &l, modes[m]);
        CHECK(out && contains(out, "LordOS 0.8.5-beta (Debian GNU/Linux 13 (trixie))") && contains(out, "CPU"), "%s/%d: fetch lines", kind, m);
        for (size_t i = 0; i < sizeof SECRETS / sizeof SECRETS[0]; i++)
            CHECK(!contains(out, SECRETS[i]), "%s/%d: fetch --share carries \"%s\"", kind, m, SECRETS[i]);
        CHECK(m != FETCH_PLAIN || !strchr(out, '\033'), "%s: plain fetch has escape codes", kind);
        CHECK(m != FETCH_TRUECOLOR || contains(out, "\033[38;2;255;214;10m"), "%s: true color gem", kind);
        CHECK(m != FETCH_256 || contains(out, "\033[38;5;220m"), "%s: 256-color gem", kind);
        free(out);
    }
    fetch_live(&l, root, 0);
    char *out = fetch_render(r, &l, FETCH_PLAIN);
    CHECK(contains(out, "@secret-host"), "%s: without --share the host is on screen", kind);
    free(out);
    free(r);
    rmtree(root);
}

/* A type-17 structure: formatted area of fl bytes, then the strings. */
static size_t smb17(uint8_t *b, int fl, unsigned size, unsigned type, unsigned speed, unsigned cfg, const char **strs, int nstr) {
    memset(b, 0, 512);
    b[0] = 17; b[1] = (uint8_t)fl; b[2] = 0x10;
    b[0x0C] = (uint8_t)size; b[0x0D] = (uint8_t)(size >> 8);
    b[0x10] = 1; b[0x11] = 2; b[0x12] = (uint8_t)type;
    b[0x15] = (uint8_t)speed; b[0x16] = (uint8_t)(speed >> 8);
    b[0x17] = 3; b[0x18] = 4; b[0x19] = 5; b[0x1A] = 6;      /* manufacturer, serial, asset tag, part number */
    if (fl >= 0x22) { b[0x20] = (uint8_t)cfg; b[0x21] = (uint8_t)(cfg >> 8); }
    size_t o = (size_t)fl;
    for (int i = 0; i < nstr; i++) { size_t l = strlen(strs[i]); memcpy(b + o, strs[i], l); o += l + 1; }
    b[o++] = 0;
    return o;
}

static void test_smbios(void) {
    uint8_t b[512];
    SmMem m;
    const char *s1[] = { "DIMM_A1", "BANK 0", "Kingston", "SECRET-SERIAL-9", "SECRET-ASSET", "SECRETPART-1" };
    size_t n = smb17(b, 0x28, 16384, 0x1A, 3200, 2666, s1, 6);
    CHECK(!smbios_memory_parse(b, n, &m), "smbios: parse");
    CHECK(m.size_mb == 16384 && !strcmp(m.type, "DDR4") && m.speed == 3200 && m.configured == 2666, "smbios: size/type/speed %u %s %u %u", m.size_mb, m.type, m.speed, m.configured);
    CHECK(!strcmp(m.slot, "DIMM_A1") && !strcmp(m.bank, "BANK 0") && !strcmp(m.maker, "Kingston"), "smbios: strings %s|%s|%s", m.slot, m.bank, m.maker);
    char line[256];
    smbios_memory_line(&m, line, sizeof line);
    CHECK(!strcmp(line, "mem 16384|DDR4|3200|2666|DIMM_A1|BANK 0|Kingston"), "smbios: line %s", line);
    CHECK(!strstr(line, "SECRET"), "smbios: no serial, asset tag or part number");
    const char *s2[] = { "ChannelA-DIMM1", "BANK 1", "Not Specified" };
    n = smb17(b, 0x22, 0x7FFF, 0x22, 0xFFFF, 0, s2, 3);
    b[0x1C] = 0x00; b[0x1D] = 0x80; b[0x1E] = 0; b[0x1F] = 0;        /* extended size: 32768 MB */
    CHECK(!smbios_memory_parse(b, n, &m) && m.size_mb == 32768 && !strcmp(m.type, "DDR5") && !m.maker[0], "smbios: extended size, placeholder maker");
    const char *s3[] = { "DIMM 1", "", "Unknown" };
    n = smb17(b, 0x1B, 0, 0x02, 0, 0, s3, 3);
    CHECK(!smbios_memory_parse(b, n, &m) && m.size_mb == 0 && !m.type[0] && !strcmp(m.slot, "DIMM 1"), "smbios: an empty slot");
    n = smb17(b, 0x22, 0x8000 | 2048, 0x1E, 4266, 4266, s1, 3);
    CHECK(!smbios_memory_parse(b, n, &m) && m.size_mb == 2 && !strcmp(m.type, "LPDDR4"), "smbios: size in KB");
    const char *s4[] = { "A|B", "x" };
    n = smb17(b, 0x1B, 8192, 0x1A, 0, 0, s4, 2);
    b[0x17] = 9;                                                        /* a string index past the set */
    CHECK(!smbios_memory_parse(b, n, &m) && !strcmp(m.slot, "A?B") && !m.maker[0], "smbios: '|' replaced, missing string empty");
    /* malformed */
    n = smb17(b, 0x28, 8192, 0x1A, 0, 0, s1, 6);
    CHECK(smbios_memory_parse(b, n - 1, &m) == -1, "smbios: string set without its double NUL");
    CHECK(smbios_memory_parse(b, 0x10, &m) == -1, "smbios: too short");
    b[0] = 16;
    CHECK(smbios_memory_parse(b, n, &m) == -1, "smbios: another type");
    b[0] = 17; b[1] = 0xFF;
    CHECK(smbios_memory_parse(b, 0x40, &m) == -1, "smbios: formatted length past the buffer");
    CHECK(smbios_memory_parse(NULL, 100, &m) == -1, "smbios: NULL");
}

static void test_facts(void) {
    char root[] = "/tmp/sysreport-facts-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    put(root, "proc/self/status", "Name:\tlord-server\nUmask:\t0022\nState:\tS\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\n");
    put(root, "proc/self/cgroup", "0::/system.slice/lordos.service/lordos-session-1000/server\n");
    char p[1024];
    snprintf(p, sizeof p, "%s/sys/fs/cgroup/system.slice/lordos.service/lordos-session-1000/server", root); mkdirs(p);
    snprintf(p, sizeof p, "%s/sys/fs/cgroup/system.slice/lordos.service/lordos-session-1000/app-lordos-media.scope", root); mkdirs(p);
    snprintf(p, sizeof p, "%s/sys/fs/cgroup/system.slice/lordos.service/lordos-session-1000/app-lordos-chromium.scope", root); mkdirs(p);
    put(root, "sys/fs/cgroup/system.slice/lordos.service/lordos-session-1000/cgroup.procs", "");
    put(root, "proc/net/tcp", "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1 1\n"
        "   1: 3400A8C0:1629 0500A8C0:C350 01 00000000:00000000 00:00000000 00000000     0        0 2 1\n");
    put(root, "proc/net/tcp6", "  sl  local_address remote_address st\n"
        "   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A 0 0\n");
    SrFacts f;
    sr_facts(&f, root);
    int me_root = getuid() == 0;
    CHECK(f.uid == 1000 && f.euid == 1000, "facts: uid %d/%d", f.uid, f.euid);
    CHECK(!strcmp(f.cgroup, "/system.slice/lordos.service/lordos-session-1000/server"), "facts: cgroup %s", f.cgroup);
    CHECK(f.leaf_root == me_root && f.parent_root == me_root && f.app_scopes == 2, "facts: owners %d %d scopes %d", f.leaf_root, f.parent_root, f.app_scopes);
    CHECK(sr_tcp_listening(root, 5673) == 0, "facts: 5673 only connected, not listening");
    CHECK(sr_tcp_listening(root, 631) == 1 && sr_tcp_listening(root, 22) == 1, "facts: listening v4 and v6");
    put(root, "proc/net/tcp6", "  sl  local_address remote_address st\n   0: 00000000000000000000000000000000:1629 00000000000000000000000000000000:0000 0A 0 0\n");
    CHECK(sr_tcp_listening(root, 5673) == 1, "facts: 5673 listening (v6)");
    put(root, "proc/self/cgroup", "0::/../../etc\n");
    sr_facts(&f, root);
    CHECK(!f.cgroup[0] && f.leaf_root == -1, "facts: a path with .. is refused");
    rmtree(root);
    CHECK(sr_tcp_listening("/nonexistent", 5673) == -1, "facts: no /proc/net");
}

/* The Raspberry Pi firmware's power and throttling (hw/rpifw.c, shared with LordBench) on small trees: what the
 * kernel shows is reported, what it doesn't is left out, never a false "no throttling". */
static void pi_case(const char *name, int pi, const char *throttled_path, const char *throttled, const char *alarm,
                    int want_thr, int want_uv, const char *want_json, const char *not_json, const char *want_md, const char *not_md) {
    char root[] = "/tmp/sysreport-pi-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    mkdirs_in(root, "sys/devices/platform/soc@107c000000/soc@107c000000:firmware");
    if (throttled) put(root, throttled_path, throttled);
    if (alarm) { put(root, "sys/class/hwmon/hwmon0/name", "rpi_volt\n"); if (*alarm) put(root, "sys/class/hwmon/hwmon0/in0_lcrit_alarm", alarm); }
    put(root, "sys/class/hwmon/hwmon1/name", "cpu_thermal\n");
    if (pi) putn(root, "proc/device-tree/compatible", "raspberrypi,5-model-b\0brcm,bcm2712\0", 36);
    char *json, *md;
    SysReport *r = run(root, &json, &md);
    CHECK(r->pi.is_pi == pi, "pi %s: is_pi %d", name, r->pi.is_pi);
    CHECK(r->pi.throttled == want_thr && r->pi.uv_alarm == want_uv, "pi %s: throttled %d alarm %d", name, r->pi.throttled, r->pi.uv_alarm);
    if (want_json) CHECK(contains(json, want_json), "pi %s: json has %s", name, want_json);
    if (not_json) CHECK(!contains(json, not_json), "pi %s: json lacks %s", name, not_json);
    if (want_md) CHECK(contains(md, want_md), "pi %s: markdown has \"%s\"", name, want_md);
    if (not_md) CHECK(!contains(md, not_md), "pi %s: markdown lacks \"%s\"", name, not_md);
    free(json); free(md); free(r);
    rmtree(root);
}
static void test_pi_firmware(void) {
    const char *p5 = "sys/devices/platform/soc@107c000000/soc@107c000000:firmware/get_throttled";
    const char *p4 = "sys/devices/platform/soc/soc:firmware/get_throttled";
    /* a Pi 5 with the LordOS kernel: rpi_volt only */
    pi_case("lordos ok", 1, NULL, NULL, "0\n", -1, 0, "\"throttled\":null,\"underVoltageAlarm\":false}", NULL,
            "no under-voltage in the last 2 s (rpi_volt); throttling not reported by this kernel", "since boot");
    pi_case("lordos alarm", 1, NULL, NULL, "1\n", -1, 1, "\"throttled\":null,\"underVoltageAlarm\":true}", NULL,
            "; under-voltage in the last 2 s (rpi_volt)", "slowed");
    pi_case("lordos bad alarm", 1, NULL, NULL, "yes\n", -1, -1, "\"throttled\":null,\"underVoltageAlarm\":null}", NULL, NULL, "voltage");
    pi_case("lordos alarm 01", 1, NULL, NULL, "01\n", -1, -1, "\"underVoltageAlarm\":null}", NULL, NULL, "voltage");
    pi_case("lordos no alarm file", 1, NULL, NULL, "", -1, -1, "\"underVoltageAlarm\":null}", NULL, NULL, "voltage");
    /* an older kernel with get_throttled and no rpi_volt: the sticky bits are since boot */
    pi_case("old pi4 sticky", 1, p4, "0x50000\n", NULL, 0x50000, -1, "\"throttled\":327680,\"underVoltageAlarm\":null}", NULL,
            "; under-voltage since boot; slowed down since boot (get_throttled 0x50000)", NULL);
    pi_case("old pi5 clear", 1, p5, "0\n", NULL, 0, -1, "\"throttled\":0", NULL,
            "; no under-voltage since boot; not slowed down since boot (get_throttled 0x0)", NULL);
    pi_case("old now bits", 1, p5, "5\n", NULL, 5, -1, NULL, NULL, "; under-voltage now; slowed down now (get_throttled 0x5)", "since boot");
    /* both: rpi_volt clears the sticky bits every 2 s, so they don't cover the time since boot */
    pi_case("old with rpi_volt", 1, p5, "0x20000\n", "0\n", 0x20000, 0, "\"underVoltageAlarm\":false", NULL,
            "; no under-voltage in the last 2 s; slowed down in the last 2 s (get_throttled 0x20000)", "since boot");
    pi_case("old bad value", 1, p5, "0x5 junk\n", NULL, -1, -1, "\"throttled\":null", NULL, NULL, "voltage");
    pi_case("old second line", 1, p5, "0\n0\n", NULL, -1, -1, "\"throttled\":null", NULL, NULL, "voltage");
    /* a Pi that shows neither: model and revision only */
    pi_case("pi nothing", 1, NULL, NULL, NULL, -1, -1, "\"throttled\":null,\"underVoltageAlarm\":null}", NULL, "## Raspberry Pi", "voltage");
    /* not a Pi (no raspberrypi in the device tree), even with the files: no Pi section */
    pi_case("not a pi", 0, p4, "0x50005\n", "1\n", -1, -1, NULL, "raspberryPi", NULL, "Raspberry Pi");
}

static void test_empty(void) {
    char root[] = "/tmp/sysreport-empty-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    char *json, *md;
    SysReport *r = run(root, &json, &md);
    CHECK(!r->ngpu && !r->ndisk && !r->cpu.threads && contains(md, "none found"), "empty tree");
    CHECK(!r->sw.live && contains(json, "\"live\":false"), "empty tree: not live");
    free(json); free(md); free(r);
    /* (0.8.7) the live session: live-boot's /run/live */
    mkdirs_in(root, "run/live");
    r = run(root, &json, &md);
    CHECK(r->sw.live && contains(json, "\"cgroup2\":false,\"live\":true"), "live session: live");
    free(json); free(md); free(r);
    rmtree(root);
}

/* An input node under /sys/class/input: a link into sysfs, as the real ones are. The address attributes are the
 * things nobody may read. */
static void input_node(const char *r, int n, const char *path, const char *bus, const char *name, const char *key, const char *rel) {
    char rel_p[128], real[512], f[600];
    snprintf(real, sizeof real, "sys/devices/%s/input/input%d", path, n);
    snprintf(f, sizeof f, "%s/name", real); put(r, f, name);
    snprintf(f, sizeof f, "%s/id/bustype", real); put(r, f, bus);
    snprintf(f, sizeof f, "%s/uniq", real); put(r, f, "de:ad:be:ef:00:03\n");
    snprintf(f, sizeof f, "%s/phys", real); put(r, f, "de:ad:be:ef:00:04\n");
    snprintf(f, sizeof f, "%s/capabilities/key", real); put(r, f, key);
    snprintf(f, sizeof f, "%s/capabilities/rel", real); put(r, f, rel);
    snprintf(rel_p, sizeof rel_p, "sys/class/input/input%d", n);
    snprintf(f, sizeof f, "../../devices/%s/input/input%d", path, n);
    lnk(r, rel_p, f);
}

static void usb_dev(const char *r, const char *dev, const char *ven, const char *prod, const char *cls, const char *maker, const char *product) {
    char f[256];
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/idVendor", dev); put(r, f, ven);
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/idProduct", dev); put(r, f, prod);
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/bDeviceClass", dev); put(r, f, cls);
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/serial", dev); put(r, f, "SECRET-USB\n");
    if (maker) { snprintf(f, sizeof f, "sys/bus/usb/devices/%s/manufacturer", dev); put(r, f, maker); }
    if (product) { snprintf(f, sizeof f, "sys/bus/usb/devices/%s/product", dev); put(r, f, product); }
}

static void usb_if(const char *r, const char *ifc, const char *cls, const char *proto) {
    char f[256];
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/bInterfaceClass", ifc); put(r, f, cls);
    snprintf(f, sizeof f, "sys/bus/usb/devices/%s/bInterfaceProtocol", ifc); put(r, f, proto);
}

/* (0.8.7) The peripherals, on screen only: what each device is and how it hangs off the machine, the battery of what
 * sits behind a receiver, and none of it (nor a serial or an address) in a report. The bit masks are real ones. */
static void test_periph(void) {
    char root[] = "/tmp/sysreport-periph-XXXXXX";
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    common_secrets(root);
    put(root, "usr/share/misc/usb.ids", USB_IDS);
    usb_dev(root, "usb5", "1d6b", "0002", "09", "Linux xhci", "Root Hub");
    usb_dev(root, "5-1", "1a86", "8091", "09", NULL, "USB HUB");
    usb_dev(root, "5-1.2", "046d", "c534", "00", "Logitech", "USB Receiver");
    usb_if(root, "5-1.2:1.0", "03", "01"); usb_if(root, "5-1.2:1.1", "03", "02");
    usb_dev(root, "5-1.4", "0d8c", "0014", "00", "C-Media Electronics Inc.", "USB Audio Device");
    usb_if(root, "5-1.4:1.0", "01", "00"); usb_if(root, "5-1.4:1.3", "03", "00");
    usb_dev(root, "1-1", "04d9", "0024", "00", "secretuser", "secretuser's keyboard");
    usb_if(root, "1-1:1.0", "03", "01");
    usb_dev(root, "1-2", "046d", "c077", "00", "Logitech", "Logitech USB Optical Mouse");
    usb_if(root, "1-2:1.0", "03", "02");
    usb_dev(root, "1-3", "0781", "5583", "00", "SanDisk", "Ultra Fit");
    usb_if(root, "1-3:1.0", "08", "50");
    usb_dev(root, "1-4", "046d", "c52b", "00", NULL, NULL);          /* no strings: usb.ids; "Receiver" names it */
    usb_if(root, "1-4:1.0", "03", "01");
    /* behind the receiver: a mouse with a HID++ battery, and a keyboard without one */
    const char *mouse = "platform/axi/1000480000.usb/usb5/5-1/5-1.2/5-1.2:1.1/0003:046D:C534.000C/0003:046D:4054.000D";
    const char *keys = "platform/axi/1000480000.usb/usb5/5-1/5-1.2/5-1.2:1.1/0003:046D:C534.000C/0003:046D:408A.000E";
    /* key: BTN_LEFT..BTN_TASK in the 5th word from the right; rel: X, Y, WHEEL */
    input_node(root, 26, mouse, "0003\n", "Logitech Wireless Mouse\n", "ffff0000 0 0 0 0\n", "143\n");
    input_node(root, 27, keys, "0003\n", "Logitech Wireless Keyboard PID:408a\n", "fffffffffffffffe\n", "0\n");
    put(root, "sys/class/power_supply/hidpp_battery_0/scope", "Device\n");
    put(root, "sys/class/power_supply/hidpp_battery_0/capacity", "64\n");
    put(root, "sys/class/power_supply/hidpp_battery_0/model_name", "MX Master 3\n");
    put(root, "sys/class/power_supply/hidpp_battery_0/serial_number", "SECRET-BAT\n");
    lnk(root, "sys/class/power_supply/hidpp_battery_0/device", "../../../0003:046D:4054.000D");
    /* a direct USB device's node, a laptop's keyboard and touchpad, a Bluetooth keyboard (BlueZ's), the rest */
    input_node(root, 2, "pci0000:00/usb1/1-1/1-1:1.0/0003:04D9:0024.0001", "0003\n", "input-not-listed-twice\n", "fffffffffffffffe\n", "0\n");
    input_node(root, 3, "platform/i8042/serio0", "0011\n", "AT Translated Set 2 keyboard\n", "fffffffffffffffe\n", "0\n");
    input_node(root, 4, "pci0000:00/i2c-1/i2c-SYNA", "0018\n", "SYNA2B33:00 Touchpad\n", "420 0 0 0 0 0\n", "0\n");
    input_node(root, 5, "virtual/misc/uhid/0005:046D:B35B.0009", "0005\n", "MX Keys\n", "fffffffffffffffe\n", "0\n");
    input_node(root, 6, "platform/pwr_button", "0019\n", "pwr_button\n", "10000000000000 0\n", "0\n");
    input_node(root, 7, "platform/soc/107c701400.hdmi", "001e\n", "vc4-hdmi-0\n", "fffffffffffffffe\n", "3\n");
    char *j = periph_json(root, NULL);
    JDoc doc;
    char err[128] = "";
    CHECK(j && !json_parse(j, strlen(j), &doc, err, sizeof err), "periph: JSON invalid: %s", err);
    if (j && !err[0]) json_free(&doc);
    CHECK(contains(j, "{\"name\":\"Logitech USB Receiver\",\"kind\":\"receiver\",\"connection\":\"usb\",\"battery\":-1}"), "periph: receiver: %s", j);
    CHECK(contains(j, "{\"name\":\"Logitech, Inc. Unifying Receiver\",\"kind\":\"receiver\",\"connection\":\"usb\""), "periph: usb.ids names a device with no strings: %s", j);
    CHECK(contains(j, "{\"name\":\"C-Media Electronics Inc. USB Audio Device\",\"kind\":\"audio\""), "periph: a sound card with buttons is audio");
    CHECK(contains(j, "\"secretuser's keyboard\",\"kind\":\"keyboard\",\"connection\":\"usb\""), "periph: a keyboard");
    CHECK(contains(j, "{\"name\":\"Logitech USB Optical Mouse\",\"kind\":\"mouse\""), "periph: a mouse, its maker not doubled");
    CHECK(contains(j, "{\"name\":\"SanDisk Ultra Fit\",\"kind\":\"storage\""), "periph: storage");
    CHECK(contains(j, "{\"name\":\"MX Master 3\",\"kind\":\"mouse\",\"connection\":\"receiver\",\"battery\":64}"), "periph: behind the receiver, with its battery: %s", j);
    CHECK(contains(j, "{\"name\":\"Logitech Wireless Keyboard PID:408a\",\"kind\":\"keyboard\",\"connection\":\"receiver\",\"battery\":-1}"), "periph: a keyboard behind the receiver");
    CHECK(contains(j, "{\"name\":\"AT Translated Set 2 keyboard\",\"kind\":\"keyboard\",\"connection\":\"internal\""), "periph: a laptop keyboard");
    CHECK(contains(j, "{\"name\":\"SYNA2B33:00 Touchpad\",\"kind\":\"touchpad\",\"connection\":\"internal\""), "periph: a touchpad: %s", j);
    static const char *const out[] = { "Root Hub", "USB HUB", "input-not-listed-twice", "MX Keys", "pwr_button", "vc4-hdmi", "Logitech Wireless Mouse" };
    for (size_t i = 0; i < sizeof out / sizeof out[0]; i++) CHECK(!contains(j, out[i]), "periph: \"%s\" is listed", out[i]);
    static const char *const never[] = { "SECRET", "de:ad:be:ef", "1000480000" };
    for (size_t i = 0; i < sizeof never / sizeof never[0]; i++) CHECK(!contains(j, never[i]), "periph: carries \"%s\"", never[i]);
    free(j);
    /* the report of that same tree (run() also checks every secret): no device's own strings */
    char *json, *md;
    SysReport *r = run(root, &json, &md);
    static const char *const shown[] = { "MX Master 3", "MX Keys", "secretuser", "USB Optical Mouse", "Wireless Keyboard" };
    for (size_t i = 0; i < sizeof shown / sizeof shown[0]; i++) {
        CHECK(!contains(json, shown[i]), "periph: the JSON report carries \"%s\"", shown[i]);
        CHECK(!contains(md, shown[i]), "periph: the Markdown report carries \"%s\"", shown[i]);
    }
    free(json); free(md); free(r);
    rmtree(root);
    j = periph_json("/nonexistent", "/nonexistent/usb.ids");
    CHECK(j && !strcmp(j, "[]"), "periph: an empty tree");
    free(j);
}

/* (0.8.7) The boot medium: one fake tree per case, built from mountinfo, the /sys/dev/block links and the
 * sysfs attributes the collector reads. The report may carry the class (and a USB link speed), never the
 * device's name, its model, a mount point or the kernel command line. */
static const char *BOOT_NEVER[] = { "sdb", "nvme0n1", "mmcblk0", "dm-0", "sr0", "SECRET-STICK", "/run/live/medium", "BOOT_IMAGE", "server.lan" };

static char *boot_root(void) {
    static char root[64];
    snprintf(root, sizeof root, "/tmp/sysreport-boot-XXXXXX");
    if (!mkdtemp(root)) { printf("mkdtemp\n"); exit(2); }
    return root;
}

/* A block device <dev> ("devices/…/block/<disk>[/<part>]") with major:minor; a partition gets its file. */
static void boot_dev(const char *root, const char *majmin, const char *dev, int part) {
    char f[512], t[512];
    snprintf(f, sizeof f, "sys/%s/dev", dev); put(root, f, majmin);
    if (part) { snprintf(f, sizeof f, "sys/%s/partition", dev); put(root, f, "1\n"); }
    snprintf(f, sizeof f, "sys/dev/block/%s", majmin);
    snprintf(t, sizeof t, "../../%s", dev);
    lnk(root, f, t);
    const char *name = strrchr(dev, '/') + 1;
    snprintf(f, sizeof f, "sys/class/block/%s", name);
    lnk(root, f, t);
}

static void boot_check(char *root, const char *what, int live, const char *medium, int mbps, int toram) {
    if (live) mkdirs_in(root, "run/live");
    char *json, *md;
    SysReport *r = run(root, &json, &md);
    char want[96];
    snprintf(want, sizeof want, "\"bootMedium\":\"%s\"", medium);
    CHECK(r->sw.live == live && !strcmp(r->sw.boot_medium, medium) && r->sw.boot_link_mbps == mbps && r->sw.boot_toram == toram,
          "boot %s: \"%s\" %d %d (want \"%s\" %d %d)", what, r->sw.boot_medium, r->sw.boot_link_mbps, r->sw.boot_toram, medium, mbps, toram);
    CHECK(contains(json, want), "boot %s: JSON %s", what, want);
    if (mbps < 0) CHECK(contains(json, "\"bootLinkMbps\":null"), "boot %s: no link speed", what);
    CHECK(contains(json, toram ? "\"bootToRam\":true" : "\"bootToRam\":false"), "boot %s: toram in the JSON", what);
    for (size_t i = 0; i < sizeof BOOT_NEVER / sizeof BOOT_NEVER[0]; i++) {
        CHECK(!contains(json, BOOT_NEVER[i]), "boot %s: the JSON carries \"%s\"", what, BOOT_NEVER[i]);
        CHECK(!contains(md, BOOT_NEVER[i]), "boot %s: the Markdown carries \"%s\"", what, BOOT_NEVER[i]);
    }
    if (live || medium[0]) CHECK(contains(md, live ? "| Started | live session" : "| Started | installed"), "boot %s: Markdown row", what);
    if (mbps > 0) {
        char m[64];
        snprintf(m, sizeof m, "from a USB drive (%d Mbps)", mbps);
        CHECK(contains(md, m), "boot %s: Markdown \"%s\"", what, m);
    }
    CHECK(contains(md, "copied to memory") == toram, "boot %s: Markdown copy to memory", what);
    if (!strcmp(medium, "internal")) CHECK(contains(md, "from an internal disk"), "boot %s: Markdown internal", what);
    free(json); free(md); free(r);
    rmtree(root);
}

static void test_boot(void) {
    const char *nvme = "devices/pci0000:00/0000:00:1d.0/0000:3d:00.0/nvme/nvme0/nvme0n1";
    const char *usb3 = "devices/pci0000:00/0000:00:14.0/usb2/2-3";
    char d[512];
    char *r;

    /* installed on an NVMe disk */
    r = boot_root();
    put(r, "proc/self/mountinfo", "22 1 0:21 / /proc rw - proc proc rw\n29 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n");
    put(r, "proc/cmdline", "BOOT_IMAGE=/boot/vmlinuz root=UUID=SECRET-UUID ro quiet\n");
    snprintf(d, sizeof d, "%s/nvme0n1p2", nvme); boot_dev(r, "259:2", d, 1);
    boot_check(r, "nvme", 0, "internal", -1, 0);

    /* live from a USB 3 stick */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 0:30 / / rw - overlay overlay rw\n41 29 8:17 / /run/live/medium ro - iso9660 /dev/sdb1 ro\n");
    put(r, "proc/cmdline", "BOOT_IMAGE=/live/vmlinuz boot=live components quiet\n");
    snprintf(d, sizeof d, "%s/2-3:1.0/host4/target4:0:0/4:0:0:0/block/sdb/sdb1", usb3); boot_dev(r, "8:17", d, 1);
    snprintf(d, sizeof d, "sys/%s/speed", usb3); put(r, d, "5000\n");
    snprintf(d, sizeof d, "sys/%s/serial", usb3); put(r, d, "SECRET-STICK\n");
    boot_check(r, "live usb3", 1, "usb", 5000, 0);

    /* live from a USB 2 stick behind a hub, copied to RAM (medium still mounted) */
    r = boot_root();
    put(r, "proc/self/mountinfo", "41 29 8:17 / /run/live/medium ro - iso9660 /dev/sdb1 ro\n");
    put(r, "proc/cmdline", "BOOT_IMAGE=/live/vmlinuz boot=live toram lordos.lang=en\n");
    boot_dev(r, "8:17", "devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2.4/1-2.4:1.0/host5/target5:0:0/5:0:0:0/block/sdb/sdb1", 1);
    put(r, "sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/speed", "480\n");
    put(r, "sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2.4/speed", "480\n");
    boot_check(r, "live usb2 toram", 1, "usb", 480, 1);

    /* copied to RAM and the medium let go: the class is unknown, the copy is not */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 0:30 / / rw - overlay overlay rw\n");
    put(r, "proc/cmdline", "boot=live toram\n");
    boot_check(r, "toram, medium gone", 1, "", -1, 1);

    /* "notoram" and toram on an installed system are not the live copy */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 259:2 / / rw - ext4 /dev/nvme0n1p2 rw\n");
    put(r, "proc/cmdline", "notoram toram\n");
    snprintf(d, sizeof d, "%s/nvme0n1p2", nvme); boot_dev(r, "259:2", d, 1);
    boot_check(r, "toram when installed", 0, "internal", -1, 0);
    r = boot_root();
    put(r, "proc/cmdline", "boot=live notoram toramx\n");
    boot_check(r, "notoram", 1, "", -1, 0);

    /* a Raspberry Pi's SD card, and an eMMC */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 179:2 / / rw - ext4 /dev/mmcblk0p2 rw\n");
    boot_dev(r, "179:2", "devices/platform/axi/1000fff000.mmc/mmc_host/mmc0/mmc0:aaaa/block/mmcblk0/mmcblk0p2", 1);
    lnk(r, "sys/devices/platform/axi/1000fff000.mmc/mmc_host/mmc0/mmc0:aaaa/block/mmcblk0/device", "../../../mmc0:aaaa");
    put(r, "sys/devices/platform/axi/1000fff000.mmc/mmc_host/mmc0/mmc0:aaaa/type", "SD\n");
    put(r, "sys/devices/platform/axi/1000fff000.mmc/mmc_host/mmc0/mmc0:aaaa/serial", "SECRET-STICK\n");
    boot_check(r, "sd", 0, "sd", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 179:2 / / rw - ext4 /dev/mmcblk0p2 rw\n");
    boot_dev(r, "179:2", "devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/block/mmcblk0/mmcblk0p2", 1);
    lnk(r, "sys/devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/block/mmcblk0/device", "../../../mmc0:0001");
    put(r, "sys/devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/type", "MMC\n");
    boot_check(r, "emmc", 0, "emmc", -1, 0);
    r = boot_root();                    /* a card whose type can't be read: unknown, not a guess */
    put(r, "proc/self/mountinfo", "29 1 179:2 / / rw - ext4 /dev/mmcblk0p2 rw\n");
    boot_dev(r, "179:2", "devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/block/mmcblk0/mmcblk0p2", 1);
    boot_check(r, "mmc unknown", 0, "", -1, 0);

    /* encrypted root (device mapper) on the NVMe disk, and on a USB disk */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 254:0 / / rw - ext4 /dev/mapper/root rw\n");
    boot_dev(r, "254:0", "devices/virtual/block/dm-0", 0);
    mkdirs_in(r, "sys/devices/virtual/block/dm-0/slaves/nvme0n1p3");
    snprintf(d, sizeof d, "%s/nvme0n1p3", nvme); boot_dev(r, "259:3", d, 1);
    boot_check(r, "luks nvme", 0, "internal", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 254:1 / / rw - ext4 /dev/mapper/root rw\n");
    boot_dev(r, "254:1", "devices/virtual/block/dm-1", 0);
    mkdirs_in(r, "sys/devices/virtual/block/dm-1/slaves/dm-0");
    boot_dev(r, "254:0", "devices/virtual/block/dm-0", 0);
    mkdirs_in(r, "sys/devices/virtual/block/dm-0/slaves/sdb2");
    snprintf(d, sizeof d, "%s/2-3:1.0/host4/target4:0:0/4:0:0:0/block/sdb/sdb2", usb3); boot_dev(r, "8:18", d, 1);
    snprintf(d, sizeof d, "sys/%s/speed", usb3); put(r, d, "10000\n");
    boot_check(r, "lvm on luks on usb", 0, "usb", 10000, 0);
    r = boot_root();                    /* a loop of device mapper layers ends, unknown */
    put(r, "proc/self/mountinfo", "29 1 254:0 / / rw - ext4 /dev/mapper/root rw\n");
    boot_dev(r, "254:0", "devices/virtual/block/dm-0", 0);
    mkdirs_in(r, "sys/devices/virtual/block/dm-0/slaves/dm-0");
    boot_check(r, "dm loop", 0, "", -1, 0);

    /* btrfs shows an anonymous device: the source names the disk */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 0:31 /@ / rw - btrfs /dev/sdb2 rw\n");
    boot_dev(r, "8:18", "devices/pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0/block/sdb/sdb2", 1);
    boot_check(r, "btrfs sata", 0, "internal", -1, 0);

    /* the last mount on / wins */
    r = boot_root();
    put(r, "proc/self/mountinfo", "1 0 0:2 / / rw - rootfs rootfs rw\n29 1 179:2 / / rw - ext4 /dev/mmcblk0p2 rw\n");
    boot_dev(r, "179:2", "devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/block/mmcblk0/mmcblk0p2", 1);
    lnk(r, "sys/devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/block/mmcblk0/device", "../../../mmc0:0001");
    put(r, "sys/devices/platform/soc/mmc0/mmc_host/mmc0/mmc0:0001/type", "SD\n");
    boot_check(r, "stacked mounts", 0, "sd", -1, 0);

    /* a live DVD (or a virtual machine's CD drive) */
    r = boot_root();
    put(r, "proc/self/mountinfo", "41 29 11:0 / /run/live/medium ro - iso9660 /dev/sr0 ro\n");
    boot_dev(r, "11:0", "devices/pci0000:00/0000:00:1f.2/ata2/host1/target1:0:0/1:0:0:0/block/sr0", 0);
    boot_check(r, "optical", 1, "optical", -1, 0);

    /* network: an NFS root, iSCSI, NBD, and a live system fetched over the network */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 0:23 / / rw - nfs4 server.lan:/export/root rw\n");
    boot_check(r, "nfs", 0, "network", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 8:33 / / rw - ext4 /dev/sdc1 rw\n");
    boot_dev(r, "8:33", "devices/platform/host5/session1/target5:0:0/5:0:0:0/block/sdc/sdc1", 1);
    boot_check(r, "iscsi", 0, "network", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 43:0 / / rw - ext4 /dev/nbd0 rw\n");
    boot_dev(r, "43:0", "devices/virtual/block/nbd0", 0);
    boot_check(r, "nbd", 0, "network", -1, 0);
    r = boot_root();
    put(r, "proc/cmdline", "boot=live fetch=http://server.lan/live/filesystem.squashfs\n");
    boot_check(r, "live fetch", 1, "network", -1, 0);

    /* a virtual machine's disk, a live ISO on a loop device, and a link that climbs out of /sys */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 253:1 / / rw - ext4 /dev/vda1 rw\n");
    boot_dev(r, "253:1", "devices/pci0000:00/0000:00:04.0/virtio1/block/vda/vda1", 1);
    boot_check(r, "virtio", 0, "internal", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "41 29 7:0 / /run/live/medium ro - iso9660 /dev/loop0 ro\n");
    boot_dev(r, "7:0", "devices/virtual/block/loop0", 0);
    boot_check(r, "loop", 1, "", -1, 0);
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 8:1 / / rw - ext4 /dev/sda1 rw\n");
    lnk(r, "sys/dev/block/8:1", "../../devices/../../../etc/block/sda/sda1");
    boot_check(r, "escaping link", 0, "", -1, 0);
    /* USB without a readable speed: the class, no number */
    r = boot_root();
    put(r, "proc/self/mountinfo", "29 1 8:17 / / rw - ext4 /dev/sdb1 rw\n");
    snprintf(d, sizeof d, "%s/2-3:1.0/host4/target4:0:0/4:0:0:0/block/sdb/sdb1", usb3); boot_dev(r, "8:17", d, 1);
    snprintf(d, sizeof d, "sys/%s/speed", usb3); put(r, d, "fast\n");
    boot_check(r, "usb no speed", 0, "usb", -1, 0);
}

/* The kernel command line, by whole words (the live ISO's "Check this computer": lordos.check=1). */
static void test_cmdline_has(void) {
    const char *w = "lordos.check=1";
    CHECK(sysreport_cmdline_has("boot=live lordos.check=1 lordos.lang=en\n", w), "cmdline: in the middle");
    CHECK(sysreport_cmdline_has("boot=live lordos.check=1\n", w), "cmdline: at the end");
    CHECK(sysreport_cmdline_has("lordos.check=1", w), "cmdline: alone");
    CHECK(!sysreport_cmdline_has("boot=live lordos.lang=en\n", w), "cmdline: not there");
    CHECK(!sysreport_cmdline_has("xlordos.check=1 lordos.check=10 lordos.check=0\n", w), "cmdline: only the whole word");
    CHECK(sysreport_cmdline_has("fetch=http://x/y boot=live", "fetch="), "cmdline: a word that ends in =");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "smbios")) {
        for (int i = 0; i < 64; i++) {
            char p[96], line[256];
            snprintf(p, sizeof p, "/sys/firmware/dmi/entries/17-%d/raw", i);
            FILE *f = fopen(p, "rb");
            if (!f) break;
            uint8_t b[4096];
            size_t n = fread(b, 1, sizeof b, f);
            fclose(f);
            SmMem m;
            if (smbios_memory_parse(b, n, &m)) { printf("17-%d: malformed\n", i); continue; }
            smbios_memory_line(&m, line, sizeof line);
            printf("%s\n", line);
        }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "fixture")) {
        mkdirs(argv[2]);
        fixture(argv[2], argv[3]);
        return 0;
    }
    test_ids();
    test_smbios();
    test_facts();
    const char *kinds[] = { "amd", "intel", "nvidia", "rpi5", "vm" };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) { test_kind(kinds[i]); test_fetch(kinds[i]); }
    setenv("NO_COLOR", "1", 1);
    CHECK(fetch_color_mode("auto", 1) == FETCH_PLAIN && fetch_color_mode("always", 0) != FETCH_PLAIN, "NO_COLOR and --color=always");
    unsetenv("NO_COLOR");
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    CHECK(fetch_color_mode("auto", 1) == FETCH_TRUECOLOR && fetch_color_mode("auto", 0) == FETCH_PLAIN && fetch_color_mode("never", 1) == FETCH_PLAIN, "color modes");
    test_pi_firmware();
    test_periph();
    test_empty();
    test_boot();
    test_cmdline_has();
    printf("sysreport-test: %d ok, %d failed\n", n_ok, n_bad);
    return n_bad ? 1 : 0;
}
