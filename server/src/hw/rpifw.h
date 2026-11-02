/* rpifw.h — what the Raspberry Pi firmware says about power and throttling, as the kernel shows it to an
 * unprivileged process (docs/BENCHMARK.md, "The Raspberry Pi firmware's throttling"). Shared by LordBench's
 * guards (bench/lbcond.c) and System Information (sysreport/sysreport.c). World-readable sysfs only: no
 * mailbox (/dev/vcio), no vcgencmd, no exec. Every reader takes a root folder ("" for the real one) so tests
 * can feed fake trees.
 *
 * Two sources, either, both or none:
 *  - the hwmon "rpi_volt" (drivers/hwmon/raspberrypi-hwmon.c), the only one the LordOS rpi-6.18.y kernel has:
 *    it asks GET_THROTTLED every 2 s with the sticky bits cleared each time and exposes in0_lcrit_alarm, the
 *    "under-voltage has occurred" bit of the last answer, so "under-voltage in the last 2 s window";
 *  - the firmware node's get_throttled of older Raspberry Pi kernels (the whole GET_THROTTLED word). With
 *    rpi_volt loaded too, its sticky bits only cover the last 2 s, not the time since boot. */
#ifndef LORD_HW_RPIFW_H
#define LORD_HW_RPIFW_H

/* GET_THROTTLED: bit 0 under-voltage now, 1 ARM frequency capped now, 2 throttled now, 3 soft temperature
 * limit now; bits 16-19 the same four "has occurred" (sticky until someone asks the firmware to clear them) */
#define RPIFW_NOW_MASK    0x0000fu
#define RPIFW_STICKY_MASK 0xf0000u

#include <stddef.h>

/* Is this device tree "compatible" list (NUL-separated strings) a BCM2712 board (Pi 5, Pi 500, CM5)? */
int hw_is_bcm2712(const char *compat, size_t n);

typedef struct {
    int hwmon;          /* 1 if the rpi_volt hwmon exists (even if its alarm can't be read) */
    int uv_alarm;       /* its in0_lcrit_alarm: 1 under-voltage in the last report, 0 none, -1 unknown */
    int throttled;      /* get_throttled masked to RPIFW_NOW_MASK | RPIFW_STICKY_MASK, -1 no such file or not
                         * a strict number */
} RpiFw;

/* get_throttled's text, strictly: 1-8 hex digits (an optional 0x), an optional newline, nothing else; the
 * value masked to the eight known bits, or -1 */
int rpifw_parse_throttled(const char *s);

/* both sources; never fails (unknown fields stay -1, hwmon 0) */
void rpifw_read(const char *root, RpiFw *f);

#endif
