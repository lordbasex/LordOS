/* voltage.h — what this machine really reports about voltages, for Monitor → Voltage (docs/MONITOR.md, "Voltage").
 * World-readable sysfs only, read on the Monitor's thread, no exec and no root:
 *
 *  - /sys/class/hwmon: every in*_input with its in*_label. A chip without labels gives the value at its pin, before
 *    the board's resistors, so it is marked raw and never scaled here. A chip that only mirrors a power_supply device
 *    is left to the next source.
 *  - /sys/class/power_supply: voltage_now of the system's batteries and of adapters that expose it.
 *  - the Raspberry Pi firmware's under-voltage alarm (hw/rpifw.c).
 *
 * The Raspberry Pi 5's power chip needs root: `board` only says that the session may ask the rpi-pmic admin action.
 * Every number is checked as a strict integer in range; a file that isn't one is left out, never guessed. The root
 * folder is a parameter so tests can use fake trees. */
#ifndef LORD_MON_VOLTAGE_H
#define LORD_MON_VOLTAGE_H

#define VOLT_CHIPS    12
#define VOLT_INPUTS   24                /* per chip */
#define VOLT_SUPPLIES 8
#define VOLT_LIMIT_MV 100000L           /* 100 V: nothing on a board or in a laptop; beyond it the file is wrong */

typedef struct {
    int index;                          /* the N of inN_input */
    long mv;
    char label[32];                     /* "" with none: a raw value */
} VoltInput;

typedef struct {
    char dev[16], name[32];             /* "hwmon3", "nct6798" */
    VoltInput in[VOLT_INPUTS];
    int n;
} VoltChip;

typedef struct {
    char name[32];
    int battery;
    long mv, min_mv;                    /* voltage_now; voltage_min_design, -1 unknown */
} VoltSupply;

typedef struct {
    int board;                          /* a BCM2712 board with vcgencmd: rpi-pmic can answer */
    int alarm;                          /* the firmware's under-voltage alarm: 1, 0, -1 no such source */
    int since_boot;                     /* the firmware's own "has occurred" bit: 1, 0, -1 unknown (rpifw.h: only
                                         * without rpi_volt, which clears it every 2 s) */
    VoltChip chips[VOLT_CHIPS];
    int nchips;
    VoltSupply sup[VOLT_SUPPLIES];
    int nsup;
} VoltState;

/* The strict number of a sysfs file: an optional '-', 1-9 digits, an optional newline. 0 if it is one. */
int volt_parse_int(const char *s, long *out);
void volt_read(const char *root, VoltState *v);
/* Is there anything to show? */
int volt_any(const VoltState *v);
/* malloc'd JSON: {"board","alarm","sinceBoot","chips":[{dev,name,in:[{i,v,label}]}],"supplies":[{name,battery,v,min}]},
 * volts as numbers; only checked characters in the strings. NULL without memory. */
char *volt_json(const VoltState *v);

#endif
