/* power.h — batteries and chargers from /sys/class/power_supply (Settings → Battery, the menu
 * bar percentage and the low-battery notice). Read-only and unprivileged; refreshed on
 * power_supply uevents (hw/uevent.c), never polled. The root directory is a parameter so
 * tests/power_test.c can use a fake sysfs tree. */
#ifndef LORD_HW_POWER_H
#define LORD_HW_POWER_H

#include <stddef.h>

#define POWER_MAX 8

typedef struct {
    char name[32];              /* "BAT0", "AC", "hidpp_battery_0" */
    int battery;                /* type Battery (else a charger: Mains, USB...) */
    int device_scope;           /* scope Device: a mouse or keyboard battery, not the system's */
    int online;                 /* chargers: plugged in */
    int present;
    int percent;                /* 0..100, -1 unknown */
    char status[16];            /* Charging, Discharging, Full, Not charging, Unknown */
    char level[16];             /* capacity_level, for a battery that gives no percentage (a Logitech mouse over HID++:
                                 * Critical, Low, Normal, High, Full); "" when there is none */
    int minutes_left;           /* to empty (discharging) or to full (charging); -1 unknown */
    int health;                 /* full / design capacity, percent; -1 unknown */
    int cycles;                 /* -1 unknown */
    double watts;               /* current draw, -1 unknown */
    char model[48], vendor[48], technology[16];
    /* live readings and the battery's own facts (Monitor → Battery, Settings → Battery); -1 / 0 when not reported */
    double volts, amps, temp_c;           /* voltage_now, current_now (its size), temp (tenths of °C); temp_c -1000 unknown */
    double wh_now, wh_full, wh_design;    /* energy_*, or charge_* times the design (else present) voltage */
    int mfg_year, mfg_month;              /* manufacture_year / manufacture_month, 0 unknown */
    /* charge control (docs/BATTERY.md): the kernel's standard attributes, -1 where the battery has none */
    int start_threshold, end_threshold;
    int behaviours;                       /* POWER_BEH_* the battery lists in charge_behaviour */
    int behaviour;                        /* the selected one (POWER_BEH_*), 0 unknown */
} PowerSupply;

enum { POWER_BEH_AUTO = 1, POWER_BEH_INHIBIT = 2, POWER_BEH_DISCHARGE = 4 };
/* "[auto] inhibit-charge force-discharge" → the listed ones (mask) and the bracketed one; pure, tested */
int power_parse_behaviour(const char *text, int *listed);

typedef struct {
    PowerSupply s[POWER_MAX];
    int n;
    int on_ac;                  /* any charger online (or no system battery) */
    int has_battery;            /* a system battery (scope != Device) */
    int percent;                /* system batteries combined, -1 without one */
    int minutes_left;           /* combined, -1 unknown */
    int charging;
} PowerState;

/* Reads every supply under root ("/sys/class/power_supply"). Returns ps->n. */
int  power_read(const char *root, PowerState *ps);
/* JSON for lord.power.state(). Returns bytes written (truncated output is still valid JSON
 * only if n is large enough; 4 KB always is). */
size_t power_json(const PowerState *ps, char *out, size_t n);

#endif
