/* battery_admin.c — charge control actions (see battery_admin.h). Runs as root in the admin helper. */
#include "battery_admin.h"
#include "power.h"
#include "../errcode.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_text(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return 0;
}

static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t w = write(fd, text, strlen(text));
    int e = errno;
    close(fd);
    errno = e;
    return w == (ssize_t)strlen(text) ? 0 : -1;
}

static int name_ok(const char *s) {
    size_t l = strlen(s);
    if (!l || l > 31 || s[0] == '.') return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

int hw_battery_pick(const char *root, const char *devices, char *name, size_t n) {
    DIR *d = opendir(root);
    if (!d) return -1;
    struct dirent *e;
    char best[32] = "";
    while ((e = readdir(d))) {
        if (!name_ok(e->d_name)) continue;
        char p[PATH_MAX], v[32], real[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s/type", root, e->d_name);
        if (read_text(p, v, sizeof v) || strcmp(v, "Battery")) continue;
        snprintf(p, sizeof p, "%s/%s/scope", root, e->d_name);
        if (!read_text(p, v, sizeof v) && !strcmp(v, "Device")) continue;     /* a mouse's, not the computer's */
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (!realpath(p, real) || strncmp(real, devices, strlen(devices))) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0] || strlen(best) >= n) return -1;
    snprintf(name, n, "%s", best);
    return 0;
}

int hw_battery_thresholds_parse(const char *arg, int *start, int *end) {
    if (!arg) return -1;
    char extra;
    int s, e;
    if (sscanf(arg, "%d %d %c", &s, &e, &extra) != 2) return -1;
    for (const char *p = arg; *p; p++) if (!isdigit((unsigned char)*p) && *p != ' ' && *p != '\n') return -1;
    if (s < 50 || s > 100 || e < 50 || e > 100 || s >= e) return -1;
    *start = s; *end = e;
    return 0;
}

int hw_battery_behaviour_parse(const char *arg) {
    if (!arg) return 0;
    char a[32];
    size_t l = strlen(arg);
    while (l && (arg[l - 1] == '\n' || arg[l - 1] == ' ')) l--;
    if (!l || l >= sizeof a) return 0;
    memcpy(a, arg, l);
    a[l] = 0;
    if (!strcmp(a, "auto")) return POWER_BEH_AUTO;
    if (!strcmp(a, "inhibit-charge")) return POWER_BEH_INHIBIT;
    if (!strcmp(a, "force-discharge")) return POWER_BEH_DISCHARGE;
    return 0;
}

static int attr_path(const char *root, const char *devices, const char *attr, char *out, size_t n) {
    char bat[32];
    if (hw_battery_pick(root, devices, bat, sizeof bat)) return -1;
    if ((size_t)snprintf(out, n, "%s/%s/%s", root, bat, attr) >= n) return -1;
    return access(out, F_OK) ? -1 : 0;
}

int hw_battery_set_thresholds(const char *root, const char *devices, int start, int end) {
    char pe[PATH_MAX], ps[PATH_MAX], v[16];
    if (start < 50 || end > 100 || start >= end) return -1;
    if (attr_path(root, devices, "charge_control_end_threshold", pe, sizeof pe)) return -1;
    int has_start = !attr_path(root, devices, "charge_control_start_threshold", ps, sizeof ps);
    /* the kernel keeps start below end at every step: lowering the end below the current start needs start first */
    int cur_start = -1;
    if (has_start && !read_text(ps, v, sizeof v)) cur_start = atoi(v);
    char se[16], ss[16];
    snprintf(se, sizeof se, "%d\n", end);
    snprintf(ss, sizeof ss, "%d\n", start);
    if (has_start && cur_start >= end) {
        if (write_text(ps, ss) || write_text(pe, se)) return -2;
    } else {
        if (write_text(pe, se)) return -2;
        if (has_start && write_text(ps, ss)) return -2;
    }
    return 0;
}

int hw_battery_set_behaviour(const char *root, const char *devices, int beh) {
    char p[PATH_MAX], v[96];
    if (attr_path(root, devices, "charge_behaviour", p, sizeof p) || read_text(p, v, sizeof v)) return -1;
    int listed = 0;
    power_parse_behaviour(v, &listed);
    if (!(listed & beh)) return -1;
    const char *w = beh == POWER_BEH_AUTO ? "auto\n" : beh == POWER_BEH_INHIBIT ? "inhibit-charge\n" : beh == POWER_BEH_DISCHARGE ? "force-discharge\n" : NULL;
    if (!w) return -1;
    return write_text(p, w) ? -2 : 0;
}

#define PS_ROOT "/sys/class/power_supply"
#define PS_DEVICES "/sys/devices/"

static int arg_line(const char *arg, size_t len, char *out, size_t n) {
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (len >= n || memchr(arg, '\n', len) || memchr(arg, 0, len)) return -1;
    memcpy(out, arg, len);
    out[len] = 0;
    return 0;
}

void hw_act_battery_thresholds(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[32];
    int s, e;
    if (arg_line(arg, len, a, sizeof a) || hw_battery_thresholds_parse(a, &s, &e)) { admin_err("admin.invalidArgument", NULL); return; }
    int rc = hw_battery_set_thresholds(PS_ROOT, PS_DEVICES, s, e);
    if (rc == -1) admin_err("hw.noChargeControl", NULL);
    else if (rc) admin_err("hw.chargeControlFailed", "errno", lerr_errno(errno), NULL);
    else admin_say("OK", "%d %d", s, e);   /* i18n-ok */
}

void hw_act_battery_behaviour(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[32];
    int b = arg_line(arg, len, a, sizeof a) ? 0 : hw_battery_behaviour_parse(a);
    if (!b) { admin_err("admin.invalidArgument", NULL); return; }
    int rc = hw_battery_set_behaviour(PS_ROOT, PS_DEVICES, b);
    if (rc == -1) admin_err("hw.noChargeControl", NULL);
    else if (rc) admin_err("hw.chargeControlFailed", "errno", lerr_errno(errno), NULL);
    else admin_say("OK", "%s", a);   /* i18n-ok */
}
