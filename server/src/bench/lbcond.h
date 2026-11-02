/* lbcond.h — what LordBench records about the machine around a run, and its thermal and power guards
 * (docs/BENCHMARK.md, "Thermal and power safety", "Recorded conditions"). World-readable sysfs and
 * procfs only, read by lord-server (never by lord-bench). Every reader takes a root folder ("" for the
 * real one) so tests can feed fake trees. */
#ifndef LORD_BENCH_LBCOND_H
#define LORD_BENCH_LBCOND_H

#include <stdint.h>

#include "../hw/rpifw.h"

typedef struct {
    double temp_c;                  /* the hottest thermal zone or CPU hwmon sensor, °C; < -270 unknown */
    double passive_c, critical_c;   /* the lowest passive and critical trip points, °C; 0 unknown */
    char   zone[24];                /* the hottest zone's type ("x86_pkg_temp", "cpu-thermal", …) */
    unsigned long throttle_events;  /* x86 core + package thermal throttle counters, summed */
    int    throttle_known;          /* 1 if those counters exist */
    int    rpi;                     /* 1 on a Raspberry Pi: its firmware's hwmon or get_throttled, or the device tree */
    int    rpi_throttled;           /* get_throttled, masked to LB_RPI_NOW_MASK | LB_RPI_STICKY_MASK (-1: no such file,
                                     * or not a 1-8 digit hex number) */
    int    rpi_uv_alarm;            /* rpi_volt's in0_lcrit_alarm: 1 under-voltage in the firmware's last report,
                                     * 0 none, -1 no such sensor */
    double freq_mhz, freq_max_mhz;  /* the average current and the highest maximum cpufreq, 0 unknown */
    char   governor[24];
    char   platform_profile[24];    /* /sys/firmware/acpi/platform_profile, "" without one */
    int    on_battery;              /* 1 battery, 0 mains, -1 unknown (no power_supply: a desktop or VM) */
    int    battery_pct;             /* -1 without a battery */
} LbCond;

void lb_cond_read(const char *root, LbCond *c);

/* The Pi firmware's GET_THROTTLED word (docs/BENCHMARK.md, "The Raspberry Pi firmware's throttling"): bit 0
 * under-voltage now, 1 ARM frequency capped now, 2 throttled now, 3 soft temperature limit now; bits 16-19 the
 * same four "has occurred", sticky until someone asks the firmware to clear them (the kernel's rpi_volt hwmon
 * does, every 2 s). LB_RPI_HOT is ours: on a Pi, the hottest zone at or above LB_RPI_FW_TEMP_C, where Raspberry
 * Pi's documentation says the firmware starts slowing the ARM cores down. */
enum { LB_RPI_UV = 1, LB_RPI_CAPPED = 2, LB_RPI_THROTTLED = 4, LB_RPI_SOFTTEMP = 8, LB_RPI_HOT = 16 };
#define LB_RPI_NOW_MASK    RPIFW_NOW_MASK
#define LB_RPI_STICKY_MASK RPIFW_STICKY_MASK
#define LB_RPI_SLOWED      (LB_RPI_CAPPED | LB_RPI_THROTTLED | LB_RPI_SOFTTEMP | LB_RPI_HOT)
#define LB_RPI_FW_TEMP_C   80.0

/* What the Pi firmware reported during a run, sample by sample. A sticky bit (or the hwmon alarm) already set
 * at the start may predate the run, so it counts only once it was seen clear and then set again; the "now"
 * bits and LB_RPI_HOT always count. lb_rpi_watch returns this sample's LB_RPI_* events and adds them to seen. */
typedef struct {
    unsigned base;                  /* sticky bits (>> 16) and the alarm (LB_RPI_UV << 4) set at the start */
    unsigned seen;                  /* LB_RPI_* seen during the run */
    int      rpi, bits, alarm;      /* a Pi; get_throttled was readable; rpi_volt was there (at any sample) */
} LbRpiWatch;
void     lb_rpi_watch_start(LbRpiWatch *w, const LbCond *c0);
unsigned lb_rpi_watch(LbRpiWatch *w, const LbCond *cn);

/* CPU busy share between two reads of /proc/stat: lb_cpu_ticks fills busy and total */
int    lb_cpu_ticks(const char *root, unsigned long long *busy, unsigned long long *total);

/* the kind of device holding the folder `path` is on: "nvme", "sd" (an SD card or eMMC through
 * mmcblk), "ssd", "hdd", "usb", "virtual", "ram" (tmpfs, overlay of the live session) or "unknown" */
const char *lb_storage_kind(const char *root, const char *path);

/* why a run must stop now, or NULL: "thermal" when the temperature is within 2 °C of the passive trip
 * point (or within 10 °C of the critical one when there's no passive), "throttled" when the throttle
 * counters grew since `start` or `rpi_events` (lb_rpi_watch for `now`) has LB_RPI_SLOWED (only checked when
 * peak = 1). Under-voltage alone is recorded, never a stop. */
const char *lb_cond_stop(const LbCond *start, const LbCond *now, unsigned rpi_events, int peak);

/* refused before starting: on battery below 30 % */
int lb_cond_battery_low(const LbCond *c);

#endif
