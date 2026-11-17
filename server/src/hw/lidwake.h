/* lidwake.h — the lid wake source around suspend (GitHub #172). Pure: parsing and decisions only,
 * header only, libc only; the root hook lidwake_tool.c reads the kernel files and does the writes,
 * tests/lidwake_test.c checks everything here (docs/SETTINGS.md, "Power button and lid").
 *
 * Some laptops (a MacBookPro12,1 in #172) wake about a second after they go to sleep with the lid
 * open: the firmware's lid device (LID0 in /proc/acpi/wakeup) is a wake source and fires although
 * the lid never moved. Writing a device's name to /proc/acpi/wakeup toggles it. The hook, run by
 * systemd-sleep before and after every sleep, turns the lid's wake source off only when all of
 * these hold, and turns back on, after waking, only what it turned off:
 *
 *   - the computer is portable (SMBIOS chassis type, hw/chassis.h; desktops are never touched);
 *   - /proc/acpi/wakeup lists a LID* device, enabled and able to wake ('*');
 *   - every lid in /proc/acpi/button/lid/<name>/state says open. A closed lid, or one that can't be
 *     read, keeps its wake source: opening the lid must still wake the computer.
 *
 * Every input is a kernel file and is parsed strictly; anything unexpected means "do nothing". The
 * only name ever written back is one read from /proc/acpi/wakeup itself, [A-Z0-9_]{1,8}. */
#ifndef LORD_LIDWAKE_H
#define LORD_LIDWAKE_H

#include <stddef.h>
#include <string.h>

#include "chassis.h"

#define LIDWAKE_MAX      4      /* LID* devices handled at most; more = malformed, nothing done */
#define LIDWAKE_NAME     8      /* longest device name accepted (ACPI names are 4) */
#define LIDWAKE_LID_OPEN    1
#define LIDWAKE_LID_CLOSED  0
#define LIDWAKE_LID_UNKNOWN (-1)

typedef struct {
    char name[LIDWAKE_NAME + 1];
    int enabled;                /* "enabled" in the status column */
    int valid;                  /* '*' before the status: the kernel lets it wake (and toggle) */
} LidWakeEntry;

/* [A-Z0-9_]{1,8} */
static inline int lidwake_name_valid(const char *s, size_t n) {
    if (n < 1 || n > LIDWAKE_NAME) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

/* /sys/class/dmi/id/chassis_type: 1 to 3 digits and an optional final newline, nothing else.
 * The type, or -1. */
static inline int lidwake_chassis(const char *b, size_t n) {
    if (n && b[n - 1] == '\n') n--;
    if (n < 1 || n > 3) return -1;
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return -1;
        v = v * 10 + (b[i] - '0');
    }
    return v;
}

/* /proc/acpi/button/lid/<name>/state: "state:", spaces or tabs, "open" or "closed", an optional
 * final newline. LIDWAKE_LID_OPEN, _CLOSED or _UNKNOWN. */
static inline int lidwake_lid_state(const char *b, size_t n) {
    if (n && b[n - 1] == '\n') n--;
    if (n < 6 || memcmp(b, "state:", 6)) return LIDWAKE_LID_UNKNOWN;
    size_t i = 6;
    if (i >= n || (b[i] != ' ' && b[i] != '\t')) return LIDWAKE_LID_UNKNOWN;
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    if (n - i == 4 && !memcmp(b + i, "open", 4)) return LIDWAKE_LID_OPEN;
    if (n - i == 6 && !memcmp(b + i, "closed", 6)) return LIDWAKE_LID_CLOSED;
    return LIDWAKE_LID_UNKNOWN;
}

/* The lids together: start from LIDWAKE_LID_UNKNOWN with seen = 0 and fold each one in. Open only
 * when every lid read says open; one closed lid = closed; one unreadable = unknown (unless another
 * is closed: closed wins, both mean "leave it alone"). */
static inline int lidwake_lid_fold(int acc, int seen, int s) {
    if (acc == LIDWAKE_LID_CLOSED || s == LIDWAKE_LID_CLOSED) return LIDWAKE_LID_CLOSED;
    if (s == LIDWAKE_LID_UNKNOWN) return LIDWAKE_LID_UNKNOWN;
    if (!seen) return s;
    return acc;  /* open so far and this one open: open; unknown so far: unknown */
}

static inline int lidwake_space(char c) { return c == ' ' || c == '\t'; }

/* /proc/acpi/wakeup (drivers/acpi/proc.c):
 *
 *   Device	S-state	  Status   Sysfs node
 *   LID0	  S4	*enabled   platform:PNP0C0D:00
 *   XHC1	  S3	*disabled  pci:0000:00:14.0
 *   		*enabled   ...                    (a device's further physical nodes)
 *
 * Fills out[] with the LID* devices (name starting "LID"), in file order. Each device line must be
 * a valid name, blanks, "S" and one digit, blanks, an optional '*' and "enabled" or "disabled", then
 * a blank or the end of the line. Lines that start with a blank (further nodes) and the header on
 * the first line are skipped. The count of LID* devices (0 = none), or -1 when anything doesn't
 * match, a name repeats, or there are more than max. */
static inline int lidwake_parse_wakeup(const char *b, size_t n, LidWakeEntry *out, int max) {
    int count = 0, line = 0;
    size_t p = 0;
    while (p < n) {
        size_t e = p;
        while (e < n && b[e] != '\n') e++;
        size_t len = e - p;
        const char *l = b + p;
        p = e < n ? e + 1 : e;
        if (line++ == 0 && len >= 6 && !memcmp(l, "Device", 6)) continue;
        if (len == 0 || lidwake_space(l[0])) continue;
        size_t i = 0;
        while (i < len && !lidwake_space(l[i])) i++;
        if (!lidwake_name_valid(l, i)) return -1;
        size_t nlen = i;
        while (i < len && lidwake_space(l[i])) i++;
        if (i + 2 > len || l[i] != 'S' || l[i + 1] < '0' || l[i + 1] > '9') return -1;
        i += 2;
        if (i >= len || !lidwake_space(l[i])) return -1;
        while (i < len && lidwake_space(l[i])) i++;
        int valid = 0, enabled;
        if (i < len && l[i] == '*') { valid = 1; i++; }
        if (len - i >= 7 && !memcmp(l + i, "enabled", 7)) { enabled = 1; i += 7; }
        else if (len - i >= 8 && !memcmp(l + i, "disabled", 8)) { enabled = 0; i += 8; }
        else return -1;
        if (i < len && !lidwake_space(l[i])) return -1;
        if (nlen < 3 || memcmp(l, "LID", 3)) continue;
        for (int k = 0; k < count; k++)
            if (strlen(out[k].name) == nlen && !memcmp(out[k].name, l, nlen)) return -1;
        if (count >= max) return -1;
        memcpy(out[count].name, l, nlen);
        out[count].name[nlen] = 0;
        out[count].enabled = enabled;
        out[count].valid = valid;
        count++;
    }
    return count;
}

/* Before sleep: which LID* devices to turn off, as a bit mask over e[0..n). Only on a portable
 * chassis, with every lid open, and only devices enabled and able to wake. */
static inline unsigned lidwake_plan_pre(int chassis_type, int lid, const LidWakeEntry *e, int n) {
    unsigned mask = 0;
    if (!chassis_portable(chassis_type) || lid != LIDWAKE_LID_OPEN || n <= 0) return 0;
    for (int i = 0; i < n && i < LIDWAKE_MAX; i++)
        if (e[i].valid && e[i].enabled) mask |= 1u << i;
    return mask;
}

/* The note in /run/lordos (what the hook turned off): one valid name per line, each line ended by
 * a newline, at most max names, no repeats. Fills names[]; the count, or -1. An empty note is 0. */
static inline int lidwake_parse_note(const char *b, size_t n, char names[][LIDWAKE_NAME + 1], int max) {
    int count = 0;
    size_t p = 0;
    while (p < n) {
        size_t e = p;
        while (e < n && b[e] != '\n') e++;
        if (e >= n) return -1;  /* no final newline: cut short */
        size_t len = e - p;
        if (!lidwake_name_valid(b + p, len) || count >= max) return -1;
        for (int k = 0; k < count; k++)
            if (strlen(names[k]) == len && !memcmp(names[k], b + p, len)) return -1;
        memcpy(names[count], b + p, len);
        names[count][len] = 0;
        count++;
        p = e + 1;
    }
    return count;
}

/* After waking: which LID* devices to turn back on, as a bit mask over e[0..n): those in the note,
 * still listed, able to wake and now disabled. Anything else (enabled again by someone, gone) is
 * left as it is. */
static inline unsigned lidwake_plan_post(char names[][LIDWAKE_NAME + 1], int nn, const LidWakeEntry *e, int n) {
    unsigned mask = 0;
    for (int i = 0; i < n && i < LIDWAKE_MAX; i++) {
        if (!e[i].valid || e[i].enabled) continue;
        for (int k = 0; k < nn; k++)
            if (!strcmp(names[k], e[i].name)) { mask |= 1u << i; break; }
    }
    return mask;
}

/* systemd-sleep's arguments: "pre" or "post", then the kind of sleep. 1 = pre, 2 = post, 0 = refused. */
static inline int lidwake_args(int argc, char *const *argv) {
    if (argc != 3) return 0;
    const char *kind = argv[2];
    if (strcmp(kind, "suspend") && strcmp(kind, "hibernate") && strcmp(kind, "hybrid-sleep") &&
        strcmp(kind, "suspend-then-hibernate")) return 0;
    if (!strcmp(argv[1], "pre")) return 1;
    if (!strcmp(argv[1], "post")) return 2;
    return 0;
}

#endif
