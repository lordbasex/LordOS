/* fanhw.h — lord-fand's hardware side: finds the CPU temperature and the fans it may drive, and
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
 * firmware" and never written by the curve.
 *
 * GPU fans (0.8.8.6, amdgpu only): a card whose hwmon is "amdgpu" with pwm1, pwm1_enable and fan1_input is also listed
 * in `g`. The curve never drives it; an administrator may set it by hand (GPUFAN, fand.c): pwm1_enable=1 and a duty of
 * FAN_GPU_FLOOR..100 %. It goes back to automatic (pwm1_enable=2) when asked, when the card passes
 * FANHW_GPU_BACKSTOP_C, after FANHW_GPU_ERRORS failed ticks, when the session that set it ends, and on stop,
 * restart and crash (the restore notes). It is never saved: a reboot always starts in automatic. */
#ifndef LORD_FAN_HW_H
#define LORD_FAN_HW_H

#define FANHW_MAX        8
#define FANHW_MAX_RO     8
#define FANHW_MAX_TRIPS  8
#define FANHW_PATH       320
#define FANHW_BACKSTOP_C 78      /* kernel trips while we drive the board fan: 78, 80, 82, 84 °C */
#define FANHW_MAX_GPU    4
#define FANHW_GPU_BACKSTOP_C 85  /* a GPU fan set by hand goes back to automatic above this (edge, junction or memory) */
#define FANHW_GPU_ERRORS 3       /* … or after this many ticks in a row that couldn't read or write it */

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

typedef enum { GR_NONE, GR_HOT, GR_ERROR, GR_SESSION, GR_STOP } GpuRevert;

typedef struct {
    char dir[FANHW_PATH];        /* .../hwmonN of the card */
    char dev[FANHW_PATH];        /* realpath of hwmonN/device (identity across restarts) */
    char slot[32];               /* its base name: the PCI slot, "0000:01:00.0" */
    int pwm_min, pwm_max;        /* pwm1_min / pwm1_max, else 0 / 255 */
    int has_rpm;
    int manual;                  /* 1 while set by hand */
    int pct;                     /* the duty asked for (FAN_GPU_FLOOR..100), -1 none */
    int orig_enable, orig_pwm;   /* before we took it, -1 unknown */
    unsigned owner;              /* uid that set it (0: root, no session to follow) */
    int errors;
    GpuRevert reverted;          /* why it last went back to automatic by itself */
} GpuFan;

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
    GpuFan g[FANHW_MAX_GPU];
    int cpu_held;                /* fanhw_take until fanhw_release: the notes keep the curve's fans */
    int ng;
} FanHw;

/* Finds the temperature source and the fans. root: "" for the real system. Returns the number
 * of drivable fans. */
int  fanhw_scan(FanHw *h, const char *root);
/* CPU temperature in milli-°C, or -1000000 if unreadable. */
int  fanhw_temp(const FanHw *h);
/* Takes manual control of every fan (hwmon: pwmN_enable=1; board: raise the trips). Writes what it
 * changed to state_path first (if not NULL) so `lord-fand --restore` can undo it after a crash. */
int  fanhw_take(FanHw *h, const char *state_path);
/* Duty 0..100 to fan i (already through fan_output_pct). Rewrites only when the value read back
 * differs, so a kernel step in between is corrected on the next tick. */
int  fanhw_set(FanHw *h, int i, int pct);
int  fanhw_pct(const FanHw *h, int i);      /* duty read back, -1 unknown */
int  fanhw_rpm(const FanHw *h, int i);      /* -1 if the fan has no tachometer */
int  fanhw_ro_rpm(const FanHw *h, int i);
/* Gives every fan of the curve back to the kernel or firmware: original pwmN_enable and trip points, and the
 * board fan's cooling state for the current temperature. GPU fans set by hand are fanhw_gpu_auto's. */
void fanhw_release(FanHw *h);
/* After a crash: rescan and restore what state_path says (validated: only values, matched to the
 * fans found by the scan; a GPU fan in the notes goes back to automatic). Then removes the file. 0 ok. */
int  fanhw_restore_file(const char *root, const char *state_path);

const char *fanhw_kind_name(FanKind k);

/* ---- GPU fans (amdgpu) ---- */
/* The card's hottest sensor (temp1..3_input: edge, junction, memory) in milli-°C, or -1000000. */
int  fanhw_gpu_temp(const FanHw *h, int i);
int  fanhw_gpu_pct(const FanHw *h, int i);  /* duty read back (pwm1 within pwm1_min..max), -1 unknown */
int  fanhw_gpu_rpm(const FanHw *h, int i);
/* By hand: pct is clamped to FAN_GPU_FLOOR..100 (fan_gpu_pct). The first time, the original pwm1_enable and pwm1 go
 * into the restore notes before anything is written. owner: the uid that asked. 0 ok, -1 refused or failed (then
 * the card is put back to automatic). */
int  fanhw_gpu_manual(FanHw *h, int i, int pct, unsigned owner, const char *state_path);
/* Back to automatic (pwm1_enable=2); why: GR_NONE when asked. Rewrites the notes. */
void fanhw_gpu_auto(FanHw *h, int i, GpuRevert why, const char *state_path);
/* One tick for a fan set by hand: the backstop, read/write errors, a resume that reset it. session: whether the
 * owner still has a session (fanhw_uid_has_session). */
void fanhw_gpu_tick(FanHw *h, int i, int session, const char *state_path);
/* Does uid have a running lord-server (a desktop session) under root/proc? Uid 0 always does. */
int  fanhw_uid_has_session(const char *root, unsigned uid);
/* Rewrites the restore notes for what is held now (cpu: the curve's fans and trips too); removes the file when
 * nothing is held. */
void fanhw_notes(FanHw *h, const char *state_path, int cpu);
const char *fanhw_revert_name(GpuRevert r);

#endif
