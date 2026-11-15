/* tabletpower.c — the power button's defaults on a tablet (see tabletpower.h). */
#include "tabletpower.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return 0;
}

int tablet_chassis(const char *root) {
    char p[512], b[16];
    snprintf(p, sizeof p, "%s/sys/class/dmi/id/chassis_type", root ? root : "");
    if (read_small(p, b, sizeof b)) return 0;
    /* a decimal number and a newline, nothing else */
    char *end;
    errno = 0;
    long t = strtol(b, &end, 10);
    if (end == b || b[0] < '0' || b[0] > '9' || errno || (*end && strcmp(end, "\n"))) return 0;
    return t == 30 || t == 32;
}

int logind_buttons_text(char *text, size_t n, const char *power, const char *hold, const char *lid, const char *lidac) {
    char line[48] = "";
    if (hold && hold[0]) snprintf(line, sizeof line, "HandlePowerKeyLongPress=%s\n", hold);
    int w = snprintf(text, n,
                     "# Written by LordOS (Settings → Battery). Changes here are overwritten.\n"
                     "[Login]\nHandlePowerKey=%s\n%sHandleLidSwitch=%s\nHandleLidSwitchExternalPower=%s\n", power, line, lid, lidac);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* One line of logind.conf: 0 if it doesn't set key, 1 and the value in out if it sets it to one of
 * Settings' choices, -1 if it sets it to anything else. */
static int conf_value(const char *line, const char *key, char *out, size_t n) {
    size_t k = strlen(key);
    if (strncmp(line, key, k) || line[k] != '=') return 0;
    const char *v = line + k + 1;
    size_t l = strcspn(v, " \t\r\n");
    static const char *const ok[] = { "poweroff", "suspend", "hibernate", "ignore", NULL };
    for (int i = 0; ok[i]; i++)
        if (strlen(ok[i]) == l && !strncmp(v, ok[i], l) && l < n) { memcpy(out, v, l); out[l] = 0; return 1; }
    return -1;
}

int tablet_power_install(const char *sysroot, const char *target) {
    if (!tablet_chassis(sysroot)) return 0;
    char p[512], buf[8192], lid[16] = "suspend", lidac[16] = "", v[16];
    struct stat st;
    snprintf(p, sizeof p, "%s" LOGIND_LORDOS_CONF, target);
    if (lstat(p, &st) == 0) return 0;                           /* a choice is already there */
    snprintf(p, sizeof p, "%s/etc/systemd/logind.conf", target);
    if (read_small(p, buf, sizeof buf) == 0) {
        for (char *s = buf, *nl; *s; s = nl ? nl + 1 : s + strlen(s)) {
            nl = strchr(s, '\n');
            if (nl) *nl = 0;
            if (conf_value(s, "HandlePowerKey", v, sizeof v) || conf_value(s, "HandlePowerKeyLongPress", v, sizeof v))
                return 0;                                       /* the administrator's own setting */
            int r = conf_value(s, "HandleLidSwitchExternalPower", v, sizeof v);
            if (r > 0) snprintf(lidac, sizeof lidac, "%s", v);
            else if (!r && (r = conf_value(s, "HandleLidSwitch", v, sizeof v)) > 0) snprintf(lid, sizeof lid, "%s", v);
            if (r < 0) return 0;                                /* a lid value Settings can't show */
        }
    }
    if (!lidac[0]) snprintf(lidac, sizeof lidac, "%s", lid);     /* logind: unset = same as HandleLidSwitch */
    char text[400];
    if (logind_buttons_text(text, sizeof text, "suspend", "poweroff", lid, lidac)) { errno = EOVERFLOW; return -1; }
    snprintf(p, sizeof p, "%s/etc/systemd/logind.conf.d", target);
    if (mkdir(p, 0755) && errno != EEXIST) return -1;
    snprintf(p, sizeof p, "%s" LOGIND_LORDOS_CONF, target);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    ok = fsync(fd) == 0 && ok;
    int e = errno;
    close(fd);
    if (!ok) { unlink(p); errno = e; return -1; }
    return 1;
}
