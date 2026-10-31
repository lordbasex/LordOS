/* hw_admin.c — Settings' admin actions (see hw_admin.h). Runs as root in the admin helper. */
#include "hw_admin.h"
#include "hwscan.h"
#include "../sysreport/smbios.h"
#include "../bench/boottest.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/rfkill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* One argument line, trimmed, into out. 0 if it fits. */
static int arg_copy(const char *arg, size_t len, char *out, size_t n) {
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (len >= n || memchr(arg, '\n', len) || memchr(arg, 0, len)) return -1;
    memcpy(out, arg, len);
    out[len] = 0;
    return 0;
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t w = write(fd, text, strlen(text));
    int e = errno;
    close(fd);
    errno = e;
    return w == (ssize_t)strlen(text) ? 0 : -1;
}

static long read_long(const char *path) {
    char b[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r <= 0) return -1;
    b[r] = 0;
    char *e;
    long v = strtol(b, &e, 10);
    return (*e == 0 || *e == '\n') ? v : -1;
}

/* ---- backlight ---- */

int hw_backlight_name_valid(const char *n) {
    size_t l = n ? strlen(n) : 0;
    if (!l || l > 64 || n[0] == '.') return 0;
    for (const char *p = n; *p; p++) if (!(isalnum((unsigned char)*p) || strchr("_-.:", *p))) return 0;
    return 1;
}

void hw_act_brightness(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[128], name[72];
    long v;
    if (arg_copy(arg, len, a, sizeof a) || sscanf(a, "%71s %ld", name, &v) != 2 || !hw_backlight_name_valid(name)) {
        admin_err("admin.invalidArgument", NULL);
        return;
    }
    /* the entry has to be a real backlight device under /sys/devices */
    char link[PATH_MAX], real[PATH_MAX];
    snprintf(link, sizeof link, "/sys/class/backlight/%s", name);
    if (!realpath(link, real) || strncmp(real, "/sys/devices/", 13)) { admin_err("hw.noBacklight", NULL); return; }
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/max_brightness", real);
    long max = read_long(p);
    if (max <= 0 || v < 0 || v > max) { admin_err("hw.outOfRange", "max", lerr_int(max), NULL); return; }
    char val[32];
    snprintf(val, sizeof val, "%ld\n", v);
    snprintf(p, sizeof p, "%s/brightness", real);
    if (write_file(p, val)) admin_err("hw.brightnessFailed", "errno", lerr_errno(errno), NULL);
    else admin_say("OK", "%ld", v);   /* i18n-ok */
}

/* ---- power profile ---- */

static int file_ok(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
}

void hw_act_power_profile(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char p[24];
    if (arg_copy(arg, len, p, sizeof p) || (strcmp(p, "performance") && strcmp(p, "balanced") && strcmp(p, "power-saver"))) {
        admin_err("hw.badProfile", NULL);
        return;
    }
    if (file_ok("/usr/bin/powerprofilesctl")) {
        const char *const argv[] = { "/usr/bin/powerprofilesctl", "set", p, NULL };
        int code = admin_run(argv);
        if (code) admin_err("sys.programFailed", "program", "powerprofilesctl", "code", lerr_int(code), NULL);
        else admin_say("OK", "%s", p);   /* i18n-ok */
        return;
    }
    /* no daemon: the cpufreq governor of every policy (and the energy/performance preference) */
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (!d) { admin_err("hw.noCpufreq", NULL); return; }
    int done = 0, failed = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "policy", 6) || !isdigit((unsigned char)e->d_name[6])) continue;
        char path[400], avail[512] = "";
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/%s/scaling_available_governors", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { ssize_t n = read(fd, avail, sizeof avail - 1); if (n > 0) avail[n] = 0; close(fd); }
        const char *want = !strcmp(p, "performance") ? "performance" : !strcmp(p, "power-saver") ? "powersave"
                         : strstr(avail, "schedutil") ? "schedutil" : strstr(avail, "ondemand") ? "ondemand" : "powersave";
        if (!strstr(avail, want)) { failed++; continue; }
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/%s/scaling_governor", e->d_name);
        char line[32];
        snprintf(line, sizeof line, "%s\n", want);
        if (write_file(path, line)) { failed++; continue; }
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/%s/energy_performance_preference", e->d_name);
        const char *epp = !strcmp(p, "performance") ? "performance\n" : !strcmp(p, "power-saver") ? "power\n" : "balance_performance\n";
        if (access(path, F_OK) == 0) write_file(path, epp);         /* only on intel_pstate/amd-pstate */
        done++;
    }
    closedir(d);
    if (!done) admin_err("hw.profileRefused", NULL);
    else admin_ok("hw.profileSet", "profile", p, "count", lerr_int(done), "refused", failed ? "1" : "0", NULL);
}

void hw_act_suspend(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    const char *const argv[] = { "/usr/bin/systemctl", "suspend", NULL };
    int code = admin_run(argv);
    if (code) admin_err("hw.suspendFailed", "code", lerr_int(code), NULL);
    else admin_ok("hw.suspended", NULL);
}

/* ---- power button and lid (logind) ---- */

/* The same short list for the power button and the lid (lid = 1): what logind can do without a
 * session lock of its own (LordOS locks from the theme). */
int hw_button_action_valid(const char *a, int lid) {
    static const char *const ok[] = { "poweroff", "suspend", "hibernate", "ignore", NULL };
    (void)lid;
    for (int i = 0; a && ok[i]; i++) if (!strcmp(a, ok[i])) return 1;
    return 0;
}

void hw_act_buttons(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[128], power[16], lid[16], lidac[16];
    if (arg_copy(arg, len, a, sizeof a) || sscanf(a, "%15s %15s %15s", power, lid, lidac) != 3 ||
        !hw_button_action_valid(power, 0) || !hw_button_action_valid(lid, 1) || !hw_button_action_valid(lidac, 1)) {
        admin_err("admin.invalidArgument", NULL);
        return;
    }
    mkdir("/etc/systemd/logind.conf.d", 0755);
    char text[400];
    snprintf(text, sizeof text,
             "# Written by LordOS (Settings → Battery). Changes here are overwritten.\n"
             "[Login]\nHandlePowerKey=%s\nHandleLidSwitch=%s\nHandleLidSwitchExternalPower=%s\n", power, lid, lidac);
    const char *tmp = "/etc/systemd/logind.conf.d/.lordos.conf.tmp", *dst = "/etc/systemd/logind.conf.d/lordos.conf";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    int ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    if (fd >= 0) { ok = fsync(fd) == 0 && ok; close(fd); }
    if (!ok || rename(tmp, dst)) { unlink(tmp); admin_err("sys.writeFailed", "path", dst, NULL); return; }
    const char *const argv[] = { "/usr/bin/systemctl", "reload", "systemd-logind", NULL };
    int code = admin_run(argv);
    if (code) admin_err("hw.logindFailed", "code", lerr_int(code), NULL);
    else admin_say("OK", "%s %s %s", power, lid, lidac);   /* i18n-ok */
}

/* ---- date and time ---- */

void hw_act_ntp(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[8];
    if (arg_copy(arg, len, a, sizeof a) || (strcmp(a, "on") && strcmp(a, "off"))) { admin_err("admin.invalidArgument", NULL); return; }
    const char *const argv[] = { "/usr/bin/timedatectl", "set-ntp", !strcmp(a, "on") ? "true" : "false", NULL };
    int code = admin_run(argv);
    if (code) admin_err("hw.ntpFailed", "code", lerr_int(code), NULL);
    else admin_say("OK", "%s", a);   /* i18n-ok */
}

int hw_time_valid(const char *t) {
    if (!t || strlen(t) != 19) return 0;
    for (int i = 0; i < 19; i++) {
        char c = t[i];
        int sep = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
        if (sep ? c != sep : !isdigit((unsigned char)c)) return 0;
    }
    int y, mo, d, h, mi, s;
    if (sscanf(t, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days[mo - 1] + (mo == 2 && leap)) return 0;
    return h <= 23 && mi <= 59 && s <= 59;
}

void hw_act_time_set(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char t[32];
    if (arg_copy(arg, len, t, sizeof t) || !hw_time_valid(t)) { admin_err("hw.badTime", NULL); return; }
    const char *const argv[] = { "/usr/bin/timedatectl", "set-time", t, NULL };
    int code = admin_run(argv);
    if (code) admin_err("hw.setTimeFailed", "code", lerr_int(code), NULL);
    else admin_say("OK", "%s", t);   /* i18n-ok */
}

/* ---- Bluetooth rfkill ---- */

void hw_act_bt_unblock(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { admin_err("hw.noRfkill", NULL); return; }
    struct rfkill_event ev = { .type = RFKILL_TYPE_BLUETOOTH, .op = RFKILL_OP_CHANGE_ALL, .soft = 0 };
    ssize_t w = write(fd, &ev, RFKILL_EVENT_SIZE_V1);
    close(fd);
    if (w != RFKILL_EVENT_SIZE_V1) admin_err("hw.rfkillFailed", "errno", lerr_errno(errno), NULL);
    else admin_ok("hw.unblocked", NULL);
}

/* ---- Raspberry Pi 5 case accessories (docs/FANS.md, "Argon ONE V5 case") ---- */

/* The cases LordOS knows. The text is fixed here; the file is included from config.txt
 * ("include lordos-case.txt", tools/rpi). Never user text. */
const char *hw_rpi_case_text(const char *id) {
    if (!strcmp(id, "none"))
        return "# lordos-case.txt: written by LordOS (Settings, Cooling). No case accessories.\n";
    if (!strcmp(id, "argon-one-v5"))
        return "# lordos-case.txt: written by LordOS (Settings, Cooling). Do not edit by hand.\n"
               "# Argon ONE V5 case: its board puts the two front USB 2.0 ports and the audio chip\n"
               "# (USB) on the USB-C controller, which has to work as a host. Its fan is on the fan\n"
               "# header and is driven by lord-fand: nothing to set for it.\n"
               "[pi5]\n"
               "dtoverlay=dwc2,dr_mode=host\n"
               "[all]\n";
    return NULL;
}

/* The boot partition is root's alone: this copy is what Settings reads (js_fan.c). */
static void case_note(const char *id) {
    mkdir("/var/lib/lordos", 0755);
    const char *tmp = "/var/lib/lordos/case.tmp", *dst = "/var/lib/lordos/case";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    size_t l = strlen(id);
    int ok = write(fd, id, l) == (ssize_t)l;
    close(fd);
    if (!ok || rename(tmp, dst)) unlink(tmp);
}

void hw_act_rpi_case(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char id[24], model[96] = "";
    const char *text;
    if (arg_copy(arg, len, id, sizeof id) || !(text = hw_rpi_case_text(id))) { admin_err("admin.invalidArgument", NULL); return; }
    int fd = open("/proc/device-tree/model", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ssize_t n = read(fd, model, sizeof model - 1); if (n > 0) model[n] = 0; close(fd); }
    struct stat st;
    if (!strstr(model, "Raspberry Pi 5") || stat("/boot/firmware/config.txt", &st) || !S_ISREG(st.st_mode)) {
        admin_err("hw.notRaspberryPi5", NULL);
        return;
    }
    /* written next to config.txt: temporary file, then rename (the FAT partition has no links) */
    const char *tmp = "/boot/firmware/lordos-case.tmp", *dst = "/boot/firmware/lordos-case.txt";
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) { admin_err("hw.caseFailed", "errno", lerr_errno(errno), NULL); return; }
    size_t l = strlen(text);
    int ok = write(fd, text, l) == (ssize_t)l && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, dst)) { unlink(tmp); admin_err("hw.caseFailed", "errno", lerr_errno(errno), NULL); return; }
    case_note(id);
    admin_ok("hw.caseSet", "case", id, NULL);
}

/* ---- Raspberry Pi 5 bootloader (Settings → Board firmware, docs/SETTINGS.md) ---- */

static int board_is_bcm2712(void) {
    char b[512];
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    return n > 0 && hw_is_bcm2712(b, (size_t)n);
}

/* "<words> (<seconds>)" at the end of a CURRENT:/LATEST: line: the build time stamp. */
static int eeprom_stamp(const char *v, long long *out) {
    const char *open_p = strrchr(v, '(');
    if (!open_p) return -1;
    char *e;
    errno = 0;
    long long t = strtoll(open_p + 1, &e, 10);
    if (errno || e == open_p + 1 || *e != ')' || e[1]) return -1;
    if (t < 1000000000LL || t > 4000000000LL) return -1;      /* 2001 .. 2096 */
    *out = t;
    return 0;
}

/* rpi-eeprom-update's report (no options) → e. Only the bootloader block is read: the first
 * "BOOTLOADER:" line, then its CURRENT, LATEST and RELEASE lines, up to an empty line or the VL805
 * block (Pi 4). Anything unexpected makes it fail: the UI never shows the program's text. */
int hw_eeprom_parse(const char *out, HwEeprom *e) {
    memset(e, 0, sizeof *e);
    int state = -1, have_cur = 0, have_lat = 0, have_rel = 0;
    const char *p = out;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (len >= sizeof line) { if (state >= 0) return -1; p = nl ? nl + 1 : NULL; continue; }
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : NULL;
        const char *l = line;
        while (*l == ' ') l++;
        if (state < 0) {
            if (!strcmp(l, "BOOTLOADER: update available")) state = 1;
            else if (!strcmp(l, "BOOTLOADER: up to date")) state = 0;
            continue;
        }
        if (!*l || !strncmp(l, "VL805", 5)) break;
        if (!strncmp(l, "CURRENT: ", 9)) {
            if (have_cur++) return -1;
            if (strcmp(l + 9, "unknown") && eeprom_stamp(l + 9, &e->current)) return -1;
        } else if (!strncmp(l, "LATEST: ", 8)) {
            if (have_lat++ || eeprom_stamp(l + 8, &e->latest)) return -1;
        } else if (!strncmp(l, "RELEASE: ", 9)) {
            if (have_rel++) return -1;
            size_t k = 0;
            for (const char *r = l + 9; *r >= 'a' && *r <= 'z'; r++) if (k < sizeof e->release - 1) e->release[k++] = *r; else return -1;
            e->release[k] = 0;
            if (!k || (l[9 + k] && l[9 + k] != ' ')) return -1;
        }
        /* other lines (the hint to change the release) are ignored */
    }
    if (state < 0 || !have_cur || !have_lat || !have_rel) return -1;
    /* the program's verdict has to agree with the numbers it printed */
    if (state != (e->current && e->latest > e->current)) return -1;
    e->update = state;
    return 0;
}

#define RPI_EEPROM_UPDATE "/usr/bin/rpi-eeprom-update"
#define RPI_EEPROM_PENDING "/run/rpi-eeprom-update-pending"   /* the program's own marker (tmpfs: this boot) */

/* Runs the report and answers OK "<state> <current> <latest> <release>": state uptodate,
 * available or pending (an update was written or staged in this boot: restart to finish). */
static void eeprom_report(void) {
    const char *const argv[] = { RPI_EEPROM_UPDATE, NULL };
    char out[8192];
    int rc = admin_run_capture(argv, out, sizeof out);
    HwEeprom e;
    if ((rc != 0 && rc != 1) || hw_eeprom_parse(out, &e)) { admin_err("hw.eepromFailed", "code", lerr_int(rc), NULL); return; }
    struct stat st;
    int pending = lstat(RPI_EEPROM_PENDING, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0;
    admin_say("OK", "%s %lld %lld %s", pending ? "pending" : e.update ? "available" : "uptodate",   /* i18n-ok: parsed by apps/settings/Firmware.jsx */
              e.current, e.latest, e.release);
}

/* ---- finding the Argon ONE V5 case ---- */

#define DTOVERLAY "/usr/bin/dtoverlay"
#define CASE_USB  "/sys/bus/platform/drivers/dwc2/1000480000.usb"      /* the USB-C controller, in host mode */

static int usb_id(const char *dev, const char *attr, char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/%s", dev, attr);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 4) return -1;
    out[4] = 0;
    return 0;
}

/* The case's hub and audio chip, on that controller: generic chips, but nothing else puts them there */
static int case_chips(void) {
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return 0;
    int hub = 0, audio = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char link[PATH_MAX], target[PATH_MAX], v[8], m[8];
        snprintf(link, sizeof link, "/sys/bus/usb/devices/%s", e->d_name);
        ssize_t k = readlink(link, target, sizeof target - 1);
        if (k <= 0) continue;
        target[k] = 0;
        if (!strstr(target, "/1000480000.usb/") || usb_id(e->d_name, "idVendor", v, sizeof v) || usb_id(e->d_name, "idProduct", m, sizeof m)) continue;
        if (!strcmp(v, "1a86") && !strcmp(m, "8091")) hub = 1;
        if (!strcmp(v, "0d8c") && !strcmp(m, "0014")) audio = 1;
    }
    closedir(d);
    return hub && audio;
}

/* No arguments. The case can't be seen until the USB-C controller is a host, so it is made one until the next
 * restart (nothing is written to the boot partition) and its chips are looked for. Found: it stays on, and the
 * ports and the sound work at once. Not found: it goes back. Answers OK "argon-one-v5" or OK "none". */
void hw_act_rpi_case_detect(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    if (!board_is_bcm2712()) { admin_err("hw.notRaspberryPi5", NULL); return; }
    int loaded = 0;
    if (access(CASE_USB, F_OK)) {
        if (!file_ok(DTOVERLAY)) { admin_say("OK", "none"); return; }   /* i18n-ok: parsed by sdk/suggestions.js */
        const char *const argv[] = { DTOVERLAY, "dwc2", "dr_mode=host", NULL };
        if (admin_run(argv)) { admin_say("OK", "none"); return; }       /* i18n-ok: parsed by sdk/suggestions.js */
        loaded = 1;
    }
    int found = 0;
    for (int i = 0; i < 40 && !found; i++) {
        struct timespec ts = { 0, 200000000 };
        nanosleep(&ts, NULL);
        found = case_chips();
    }
    if (!found && loaded) {
        const char *const argv[] = { DTOVERLAY, "-r", "dwc2", NULL };
        admin_run(argv);
    }
    admin_say("OK", "%s", found ? "argon-one-v5" : "none");             /* i18n-ok: parsed by sdk/suggestions.js */
}

static int eeprom_ready(void) {
    if (!board_is_bcm2712()) { admin_err("hw.notRaspberryPi5", NULL); return 0; }
    if (!file_ok(RPI_EEPROM_UPDATE)) { admin_err("hw.noEeprom", NULL); return 0; }
    return 1;
}

void hw_act_eeprom_status(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    if (eeprom_ready()) eeprom_report();
}

/* Installs the newest bootloader of the configured release that is newer than the running one,
 * as rpi-eeprom-update decides: on a Pi 5 it stages it for the next boot (the bootloader flashes
 * itself, with its recovery path) or writes it at once with the A/B or flashrom update when those
 * are installed. Bootloader only (-A bootloader), no other option, no file. */
void hw_act_eeprom_update(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    if (!eeprom_ready()) return;
    const char *const argv[] = { RPI_EEPROM_UPDATE, "-A", "bootloader", NULL };
    char out[16384];
    int rc = admin_run_capture(argv, out, sizeof out);
    if (rc == 3) { admin_err("hw.eepromFrozen", NULL); return; }
    if (rc != 0) { admin_err("hw.eepromUpdateFailed", "code", lerr_int(rc), NULL); return; }
    eeprom_report();
}

/* ---- Monitor → Voltage: the Raspberry Pi 5 power chip (docs/MONITOR.md, "Voltage") ---- */

#define VCGENCMD "/usr/bin/vcgencmd"

/* The rails of the board's PMIC, as the firmware names them. Nothing else ever leaves the helper. */
static const char *const pmic_rails[HW_PMIC_RAILS] = {
    "EXT5V", "VDD_CORE", "3V3_SYS", "1V8_SYS", "1V1_SYS", "0V8_SW", "0V8_AON", "DDR_VDD2", "DDR_VDDQ",
    "3V7_WL_SW", "3V3_DAC", "3V3_ADC", "HDMI", "BATT",
};

/* "5.18312000" up to the unit letter, into micro units: 1-2 digits, a point, 1-12 digits. -1 if not. */
static long pmic_number(const char *s, const char *end) {
    const char *p = s;
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9' && p - s < 3) v = v * 10 + (*p++ - '0');
    if (p == s || p - s > 2 || p >= end || *p != '.') return -1;
    const char *f = ++p;
    long frac = 0;
    int k = 0;
    for (; p < end; p++, k++) {
        if (*p < '0' || *p > '9' || k >= 12) return -1;
        if (k < 6) frac = frac * 10 + (*p - '0');
    }
    if (p == f) return -1;
    for (; k < 6; k++) frac *= 10;
    return v * 1000000L + frac;
}

/* One line: [spaces]NAME_V volt(N)=D.DDDV or [spaces]NAME_A current(N)=D.DDDA, exactly. */
static int pmic_line(const char *line, size_t len, HwPmic *p) {
    const char *s = line, *end = line + len;
    while (s < end && *s == ' ') s++;
    const char *sp = memchr(s, ' ', (size_t)(end - s));
    if (!sp || sp - s < 3 || sp - s > 24 || sp[-2] != '_') return -1;
    for (const char *c = s; c < sp; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return -1;
    int volt = sp[-1] == 'V';
    if (!volt && sp[-1] != 'A') return -1;
    const char *word = volt ? "volt(" : "current(";
    size_t wl = strlen(word);
    const char *q = sp + 1;
    if ((size_t)(end - q) < wl || memcmp(q, word, wl)) return -1;
    q += wl;
    const char *d = q;
    while (q < end && *q >= '0' && *q <= '9' && q - d < 4) q++;
    if (q == d || q + 2 >= end || q[0] != ')' || q[1] != '=') return -1;
    q += 2;
    if (end[-1] != (volt ? 'V' : 'A')) return -1;
    long v = pmic_number(q, end - 1);
    if (v < 0 || v > HW_PMIC_LIMIT) return -1;
    size_t nl = (size_t)(sp - s) - 2;
    const char *name = NULL;
    for (int i = 0; i < HW_PMIC_RAILS; i++)
        if (strlen(pmic_rails[i]) == nl && !memcmp(pmic_rails[i], s, nl)) name = pmic_rails[i];
    if (!name) return 0;
    HwPmicRail *r = NULL;
    for (int i = 0; i < p->n; i++) if (p->r[i].name == name) r = &p->r[i];
    if (!r) { r = &p->r[p->n++]; r->name = name; r->uv = r->ua = -1; }
    long *slot = volt ? &r->uv : &r->ua;
    if (*slot >= 0) return -1;
    *slot = v;
    return 0;
}

int hw_pmic_parse(const char *out, HwPmic *p) {
    memset(p, 0, sizeof *p);
    int lines = 0;
    for (const char *s = out; *s;) {
        const char *nl = strchr(s, '\n');
        if (!nl || nl - s > 64 || ++lines > 64 || pmic_line(s, (size_t)(nl - s), p)) return -1;
        s = nl + 1;
    }
    int ok = 0, w = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->r[i].uv < 0) continue;                /* a current with no voltage says nothing */
        if (!strcmp(p->r[i].name, "EXT5V")) ok = 1;
        p->r[w++] = p->r[i];
    }
    p->n = w;
    return ok ? 0 : -1;
}

int hw_pmic_line(const HwPmic *p, int uv_count, long long uv_last_s, char *out, size_t n) {
    int w = snprintf(out, n, "uv %d %lld", uv_count, uv_last_s);
    if (w < 0 || (size_t)w >= n) return -1;
    size_t o = (size_t)w;
    for (int i = 0; i < p->n; i++) {
        w = snprintf(out + o, n - o, " %s=%ld,%ld", p->r[i].name, p->r[i].uv, p->r[i].ua);
        if (w < 0 || (size_t)w >= n - o) return -1;
        o += (size_t)w;
    }
    return 0;
}

int hw_uv_log(const char *log, size_t n, long long *last_s) {
    static const char tail[] = ": Undervoltage detected!";
    const size_t tl = sizeof tail - 1;
    int count = 0;
    *last_s = -1;
    const char *p = log, *end = log + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end, *line = p;
        p = nl ? nl + 1 : end;
        /* "<priority>,<seq>,<usec>,<flags>[,...];<text>" */
        const char *semi = memchr(line, ';', (size_t)(e - line));
        if (!semi || (size_t)(e - semi - 1) <= tl || memcmp(e - tl, tail, tl)) continue;
        char head[64];
        size_t hl = (size_t)(semi - line);
        if (hl >= sizeof head) continue;
        memcpy(head, line, hl);
        head[hl] = 0;
        int prio;
        unsigned long long seq, usec;
        if (sscanf(head, "%d,%llu,%llu,", &prio, &seq, &usec) != 3 || prio < 0 || prio > 7) continue;     /* kernel facility only */
        /* "<driver> <device>" before the colon, nothing else */
        int spaces = 0, clean = 1;
        for (const char *c = semi + 1; c < e - tl; c++) {
            if (*c == ' ') spaces++;
            else if (!isalnum((unsigned char)*c) && !strchr("_-.:@", *c)) clean = 0;
        }
        if (!clean || spaces != 1) continue;
        count++;
        *last_s = (long long)(usec / 1000000ULL);
    }
    return count;
}

/* Read-only, no arguments, on the boards that have this chip: the fixed program with one fixed word. The session
 * gets numbers and the closed rail names, never the program's text. */
void hw_act_rpi_pmic(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    if (!board_is_bcm2712()) { admin_err("hw.notRaspberryPi5", NULL); return; }
    if (!file_ok(VCGENCMD)) { admin_err("hw.noVcgencmd", NULL); return; }
    const char *const argv[] = { VCGENCMD, "pmic_read_adc", NULL };
    char out[4096], line[768];
    int rc = admin_run_capture(argv, out, sizeof out);
    HwPmic p;
    if (rc != 0 || hw_pmic_parse(out, &p)) { admin_err("hw.pmicFailed", "code", lerr_int(rc), NULL); return; }
    /* what the hwmon driver logged: with it loaded, the firmware's own "has occurred" bits only cover 2 s (rpifw.h) */
    size_t n;
    int denied, count = -1;
    long long last = -1;
    char *log = hwscan_read_kmsg("", &n, &denied);
    if (log) { count = hw_uv_log(log, n, &last); free(log); }
    if (hw_pmic_line(&p, count, last, line, sizeof line)) { admin_err("hw.pmicFailed", "code", "0", NULL); return; }
    admin_say("OK", "%s", line);   /* i18n-ok: numbers, parsed by apps/monitor/voltmodel.js */
}

/* ---- missing firmware (the Store's suggestions, hwscan.c) ---- */

/* Read-only: the session can't read the kernel log when kernel.dmesg_restrict=1. Takes no
 * argument and returns only firmware file names that pass hwscan_fw_name_valid() and the driver
 * name parsed from the same line (letters, digits, '_' and '-'), never other log text. */
void hw_act_firmware_missing(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    size_t n;
    int denied;
    char *log = hwscan_read_kmsg("", &n, &denied);
    if (!log) { admin_err("hw.noKernelLog", NULL); return; }
    HwFwMiss m[HWSCAN_FW_MAX];
    int k = hwscan_fw_missing("", log, n, m, HWSCAN_FW_MAX);
    free(log);
    for (int i = 0; i < k; i++)
        admin_say("LOG", "fw %s %s", m[i].file, m[i].driver[0] ? m[i].driver : "-");   /* i18n-ok: protocol line parsed by sdk/suggestengine.js */
    admin_ok("hw.firmwareChecked", NULL);
}

/* ---- System Information's memory modules (sysreport/smbios.c, docs/BENCHMARK.md) ---- */

void hw_act_memory_modules(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    DIR *d = opendir("/sys/firmware/dmi/entries");
    if (!d) { admin_ok("hw.memoryNoTables", NULL); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 64) {
        /* "17-<n>" only: the memory devices */
        if (strncmp(e->d_name, "17-", 3) || !e->d_name[3] || strspn(e->d_name + 3, "0123456789") != strlen(e->d_name + 3)) continue;
        char p[128];
        snprintf(p, sizeof p, "/sys/firmware/dmi/entries/%.32s/raw", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        uint8_t buf[4096];
        ssize_t got = read(fd, buf, sizeof buf);
        close(fd);
        SmMem m;
        if (got <= 0 || smbios_memory_parse(buf, (size_t)got, &m)) continue;
        char line[256];
        smbios_memory_line(&m, line, sizeof line);
        admin_say("LOG", "%s", line);  /* i18n-ok: protocol line parsed by apps/sysinfo/model.js */
        n++;
    }
    closedir(d);
    admin_ok("hw.memoryRead", NULL);
}

/* ---- System Information's serial numbers (docs/BENCHMARK.md, "Serial numbers") ---- */

int hw_serial_clean(const char *raw, size_t n, char *out, size_t size) {
    static const char *const junk[] = { "To be filled by O.E.M.", "Default string", "Not Specified", "Not Applicable", "None", "Unknown",
                                        "System Serial Number", "Base Board Serial Number", "0123456789", "123456789" };
    out[0] = 0;
    while (n && (raw[n - 1] == 0 || raw[n - 1] == '\n' || raw[n - 1] == ' ')) n--;     /* a device tree string ends in NUL */
    while (n && *raw == ' ') { raw++; n--; }
    if (!n || n >= size) return -1;
    int all_same = 1;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)raw[i]) && !memchr(" ._:/-", raw[i], 6)) return -1;
        if (raw[i] != raw[0]) all_same = 0;
    }
    if (all_same) return -1;                                /* "0000000000", "XXXXXXXX" */
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        if (strlen(junk[i]) == n && !strncasecmp(raw, junk[i], n)) return -1;
    memcpy(out, raw, n);
    out[n] = 0;
    return 0;
}

/* No arguments, read-only, and it answers only the request that asked: the kernel keeps product_serial and
 * board_serial for root, so this stays with administrators. The list of files is this table; nothing here reaches
 * sysreport/, and the app shows the answer and keeps it nowhere. */
void hw_act_serial_numbers(const AdminReq *r, const char *arg, size_t len) {
    static const struct { const char *kind, *path; } src[] = {
        { "system", "/sys/class/dmi/id/product_serial" },
        { "board", "/sys/class/dmi/id/board_serial" },
        { "device", "/proc/device-tree/serial-number" },
    };
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) {
        char raw[128], clean[64];
        int fd = open(src[i].path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        ssize_t got = read(fd, raw, sizeof raw);
        close(fd);
        if (got <= 0 || got == (ssize_t)sizeof raw || hw_serial_clean(raw, (size_t)got, clean, sizeof clean)) continue;
        admin_say("LOG", "serial %s %s", src[i].kind, clean);  /* i18n-ok: protocol line parsed by apps/sysinfo/model.js */
    }
    admin_ok("hw.serialsRead", NULL);
}

/* ---- video in the browser (docs/VIDEO.md) ---- */

int hw_video_files(const char *arg, char *chromium, size_t cn, char *firefox, size_t fn) {
    char a[64], mode[16], lower[8], vp9[8], av1[8], extra[2];
    if (!arg || strlen(arg) >= sizeof a) return -1;
    snprintf(a, sizeof a, "%s", arg);
    a[strcspn(a, "\n")] = 0;
    if (sscanf(a, "%15s %7s %7s %7s %1s", mode, lower, vp9, av1, extra) != 4) return -1;
    if (strcmp(mode, "auto") && strcmp(mode, "always") && strcmp(mode, "never")) return -1;
    if (strcmp(lower, "on") && strcmp(lower, "off")) return -1;
    if ((strcmp(vp9, "yes") && strcmp(vp9, "no")) || (strcmp(av1, "yes") && strcmp(av1, "no"))) return -1;
    int hw_vp9 = !strcmp(vp9, "yes"), hw_av1 = !strcmp(av1, "yes");
    /* Chromium: the extension's managed storage ("3rdparty" policy); the extension decides */
    snprintf(chromium, cn,
             "{\n  \"3rdparty\": {\n    \"extensions\": {\n      \"%s\": {\n"
             "        \"mode\": \"%s\",\n        \"autoLower\": %s,\n        \"hwVp9\": %s,\n        \"hwAv1\": %s\n"
             "      }\n    }\n  }\n}\n",
             HW_VIDEO_EXT_ID, mode, !strcmp(lower, "on") ? "true" : "false", hw_vp9 ? "true" : "false", hw_av1 ? "true" : "false");
    /* Firefox ESR (no extension: it only loads signed ones): AV1 can be turned off on its own; VP9
     * has no such switch without turning off all of WebM, so it stays */
    int no_av1 = !strcmp(mode, "always") || (!strcmp(mode, "auto") && !hw_av1);
    snprintf(firefox, fn,
             "// Written by LordOS (Settings → Displays → Video in the browser). Changes here are overwritten.\n"
             "// See docs/VIDEO.md.\n%s",
             no_av1 ? "pref(\"media.av1.enabled\", false);\n" : "");
    return 0;
}

/* Writes text to path through a temporary file and rename (nothing if it is already there). */
static int write_atomic(const char *path, const char *text) {
    char cur[2048];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        ssize_t n = read(fd, cur, sizeof cur - 1);
        close(fd);
        if (n >= 0) { cur[n] = 0; if (!strcmp(cur, text)) return 0; }
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos-tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    ok = fsync(fd) == 0 && ok;
    fchmod(fd, 0644);
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

void hw_act_video(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char a[64], chromium[1024], firefox[512];
    if (arg_copy(arg, len, a, sizeof a) || hw_video_files(a, chromium, sizeof chromium, firefox, sizeof firefox)) {
        admin_err("admin.invalidArgument", NULL);
        return;
    }
    const char *cp = "/etc/chromium/policies/managed/lordos-video.json";
    mkdir("/etc/chromium", 0755);
    mkdir("/etc/chromium/policies", 0755);
    mkdir("/etc/chromium/policies/managed", 0755);
    if (write_atomic(cp, chromium)) { admin_err("sys.writeFailed", "path", cp, NULL); return; }
    struct stat st;
    const char *fp = "/etc/firefox-esr/lordos-video.js";
    if (stat("/etc/firefox-esr", &st) == 0 && S_ISDIR(st.st_mode) && write_atomic(fp, firefox)) {
        admin_err("sys.writeFailed", "path", fp, NULL);
        return;
    }
    admin_say("OK", "%s", a);   /* i18n-ok */
}

/* ---- LordBench's boot time test (bench/boottest.h, docs/BENCHMARK.md "Boot") ---- */

/* boot-test <n>: n restarts in a row (BT_MIN..BT_MAX), each boot's time recorded by lord-sessiond. Only
 * writes the fixed-format state file; the restart itself is asked by the session as any other (Restart). */
void hw_act_boot_test(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char v[4];
    if (arg_copy(arg, len, v, sizeof v) || !v[0] || strspn(v, "0123456789") != strlen(v)) { admin_err("admin.invalidArgument", NULL); return; }
    int n = atoi(v);
    if (n < BT_MIN || n > BT_MAX) { admin_err("admin.invalidArgument", NULL); return; }
    struct stat st;
    if (!stat("/run/live", &st)) { admin_err("bench.bootTestLive", NULL); return; }   /* nothing survives a restart there */
    mkdir("/var/lib/lordos", 0755);
    BootTest b = { n, n, (long long)time(NULL) };
    if (bt_write(BT_PATH, &b)) { admin_err("bench.bootTestFailed", "errno", lerr_errno(errno), NULL); return; }
    admin_ok("bench.bootTestSet", "n", v, NULL);
}

/* boot-test-cancel: no more restarts (the boots already measured stay in the history) */
void hw_act_boot_test_cancel(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    if (unlink(BT_PATH) && errno != ENOENT) { admin_err("bench.bootTestFailed", "errno", lerr_errno(errno), NULL); return; }
    admin_ok("bench.bootTestCancelled", NULL);
}

