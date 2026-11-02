/* fancurve.h — fan profiles, curve evaluation with hysteresis, the configuration file and the
 * socket requests of axia-fand (docs/FANS.md). Pure code: no I/O, so it is tested by
 * tests/fan_test.c.
 *
 * A curve is a list of steps "from T °C, P %", sorted by temperature. Below the first step the fan
 * is off (or at the profile's floor). A step is entered when the temperature reaches it and left
 * only when it falls `hyst` °C below it, so the fan does not flap around a threshold. */
#ifndef LORD_FAN_CURVE_H
#define LORD_FAN_CURVE_H

#include <stddef.h>

#define FAN_MAX_POINTS   8
#define FAN_TEMP_MIN     30      /* custom curve limits, °C */
#define FAN_TEMP_MAX     90
#define FAN_HYST_DEFAULT 3       /* °C */
#define FAN_HYST_MIN     1
#define FAN_HYST_MAX     10
#define FAN_MIN_PCT      25      /* below this most small fans stall: a non-zero duty is raised to it */
#define FAN_KICK_BELOW   50      /* starting from 0 % to less than this: one tick at 100 % first */
#define FAN_REQ_MAX      256     /* longest request line accepted on the socket */
#define FAN_FORCE_MAX_MIN (24 * 60)

typedef enum { FP_AUTO, FP_QUIET, FP_BALANCED, FP_PERFORMANCE, FP_ALWAYS, FP_CUSTOM, FP__COUNT } FanProfile;

typedef struct { int temp; int pct; } FanPoint;          /* °C, % */

typedef struct {
    int n;
    FanPoint p[FAN_MAX_POINTS];
    int floor_pct;                                          /* duty below the first step (Always on) */
} FanCurve;

typedef enum { FF_OFF, FF_TIMED, FF_ON } FanForceMode;

typedef struct {
    FanProfile profile;
    FanCurve custom;            /* the user's curve (kept even while another profile is active) */
    int hyst;                   /* °C */
    FanForceMode force;
    long long force_until;      /* FF_TIMED: CLOCK_REALTIME seconds */
} FanConfig;

const char *fan_profile_name(FanProfile p);
int  fan_profile_parse(const char *s, FanProfile *out);  /* 0 ok, -1 unknown */

/* The curve a profile runs (custom → cfg->custom). FP_AUTO has no curve (n = 0, floor -1):
 * the fans are left to the kernel or the firmware. */
void fan_profile_curve(const FanConfig *cfg, FanCurve *out);

/* 0 if a user curve is acceptable: 1..FAN_MAX_POINTS points, temperatures FAN_TEMP_MIN..FAN_TEMP_MAX
 * strictly increasing, percentages 0..100 never decreasing. */
int  fan_curve_valid(const FanCurve *c);

/* Next step (-1 = below the first) for temperature temp_mc (milli-°C), given the previous step. */
int  fan_curve_step(const FanCurve *c, int hyst, int prev, int temp_mc);
/* Duty (%) of a step: the step's %, never below the floor. */
int  fan_curve_pct(const FanCurve *c, int step);
/* What reaches the hardware: 0, or at least FAN_MIN_PCT, at most 100. */
int  fan_output_pct(int pct);

void fan_config_default(FanConfig *cfg, int has_board_fan);

/* /etc/lordos/fan.json. The caller fills cfg with fan_config_default() first. Parse: 0 ok; -1 on any
 * error (cfg is left unchanged). Strict: unknown
 * keys, bad types or an invalid curve are refused. */
int    fan_config_parse(const char *text, size_t len, FanConfig *cfg, char *err, size_t errlen);
size_t fan_config_write(const FanConfig *cfg, char *out, size_t n);

/* "55:30,60:55,65:100" → curve; 0 ok. */
int  fan_curve_parse_text(const char *s, FanCurve *c);

/* Socket requests (one line, no trailing newline):
 *   STATUS
 *   PROFILE auto|quiet|balanced|performance|always|custom
 *   CURVE <t:p,...>          sets the custom curve and selects it
 *   FORCE off|on|<minutes>   minutes 1..FAN_FORCE_MAX_MIN */
typedef enum { FR_STATUS, FR_PROFILE, FR_CURVE, FR_FORCE } FanReqType;
typedef struct {
    FanReqType type;
    FanProfile profile;
    FanCurve curve;
    FanForceMode force;
    int minutes;
} FanReq;
int  fan_req_parse(const char *line, size_t len, FanReq *r);   /* 0 ok, -1 refused */

#endif
