/* fanhw.h — axia-fand's hardware side: finds the CPU temperature and the fans it may drive, and
 * writes their duty (docs/FANS.md). Everything is found under a sysfs root (normally "", so the
 * real /sys; tests/fan_test.c builds a fake tree). Paths never come from a client: a client only
 * picks a profile, and the daemon drives every fan it found.
 *
 * Kinds of fan:
 *   FK_BOARD  the Raspberry Pi 5 fan header (hwmon "pwmfan", cooling device "pwm-fan"): the official
 *             Active Cooler and case fans plugged there (the Argon ONE V5 case's blower too).
 *             pwm1 is written directly; the kernel's active trip points for this fan are raised
 *             to a backstop (FANHW_BACKSTOP_C) while we drive it, and put back when we stop.
 *   FK_HWMON  a motherboard Super I/O or fan controller with pwmN + pwmN_enable (allow-listed
 *             drivers). pwmN_enable is set to 1 (manual) while we drive it and put back after.
 * Fans that only report fanN_input (laptops, GPUs, unknown drivers) are listed as "managed by
 * firmware" and never written. */
#ifndef LORD_FAN_HW_H
#define LORD_FAN_HW_H

#define FANHW_MAX        8
#define FANHW_MAX_RO     8
#define FANHW_MAX_TRIPS  8
#define FANHW_PATH       320
#define FANHW_BACKSTOP_C 78      /* kernel trips while we drive the board fan: 78, 80, 82, 84 °C */

typedef enum { FK_BOARD, FK_HWMON } FanKind;

typedef struct {
    FanKind kind;
    char chip[32];               /* hwmon name */
    char dir[FANHW_PATH];        /* .../hwmonN */
    char dev[FANHW_PATH];        /* realpath of hwmonN/device (identity across restarts) */
    int ch;                      /* pwmN */
    int has_rpm;
    int orig_enable;             /* FK_HWMON: pwmN_enable before we took it (-1 unknown) */
    int taken;
    int last_pct;                /* last duty written, -1 none */
    int kicking;                 /* one tick at 100 % before a low duty */
} FanTarget;

typedef struct {
    char chip[32];
    char path[FANHW_PATH];       /* fanN_input */
} FanRo;

typedef struct {
    char root[128];
    FanTarget t[FANHW_MAX];
    int n;
    FanRo ro[FANHW_MAX_RO];
    int nro;
    char temp_path[FANHW_PATH];
    char temp_src[32];
    /* FK_BOARD: the kernel trips bound to its cooling device */
    char zone[FANHW_PATH];       /* .../thermal_zoneN */
    char cdev[FANHW_PATH];       /* .../cooling_deviceM (type pwm-fan) */
    int ntrips;
    int trip[FANHW_MAX_TRIPS];   /* trip index */
    int trip_orig[FANHW_MAX_TRIPS];
    int trips_raised;
    int board;                   /* 1: Raspberry Pi 5 (device-tree model) */
} FanHw;

/* Finds the temperature source and the fans. root: "" for the real system. Returns the number
 * of drivable fans. */
int  fanhw_scan(FanHw *h, const char *root);
/* CPU temperature in milli-°C, or -1000000 if unreadable. */
int  fanhw_temp(const FanHw *h);
/* Takes manual control of every fan (hwmon: pwmN_enable=1; board: raise the trips). Writes what it
 * changed to state_path first (if not NULL) so `axia-fand --restore` can undo it after a crash. */
int  fanhw_take(FanHw *h, const char *state_path);
/* Duty 0..100 to fan i (already through fan_output_pct). Rewrites only when the value read back
 * differs, so a kernel step in between is corrected on the next tick. */
int  fanhw_set(FanHw *h, int i, int pct);
int  fanhw_pct(const FanHw *h, int i);      /* duty read back, -1 unknown */
int  fanhw_rpm(const FanHw *h, int i);      /* -1 if the fan has no tachometer */
int  fanhw_ro_rpm(const FanHw *h, int i);
/* Gives every fan back to the kernel or firmware: original pwmN_enable and trip points, and the
 * board fan's cooling state for the current temperature. */
void fanhw_release(FanHw *h);
/* After a crash: rescan and restore what state_path says (validated: only values, matched to the
 * fans found by the scan). Then removes the file. 0 ok. */
int  fanhw_restore_file(const char *root, const char *state_path);

const char *fanhw_kind_name(FanKind k);

#endif
