/* hw_admin.h — the admin helper's actions for Settings (displays, battery, date and time,
 * Bluetooth). Every action is closed: validated arguments, fixed program paths and config files
 * generated here, never user text passed to a shell. Listed in AGENTS.md and docs/SETTINGS.md.
 *
 *   display-brightness <name> <value>  /sys/class/backlight/<name>/brightness (0..max)     anyone
 *   power-profile <p>                  performance|balanced|power-saver: powerprofilesctl if
 *                                      installed, else the cpufreq governor (and EPP)         anyone
 *   power-suspend                      systemctl suspend                                      anyone
 *   power-buttons <power> <lid> <lidac> /etc/systemd/logind.conf.d/lordos.conf + reload logind admin
 *   time-ntp on|off                    timedatectl set-ntp                                    admin
 *   time-set YYYY-MM-DD HH:MM:SS       timedatectl set-time (automatic time must be off)       admin
 *   bt-unblock                         rfkill: unblock Bluetooth (/dev/rfkill)               anyone
 *   rpi-case none|argon-one-v5         Raspberry Pi 5 only: writes /boot/firmware/lordos-case.txt
 *                                      (fixed text, included from config.txt; takes effect
 *                                      after a restart)                                      admin
 *   rpi-case-detect                    (no arguments) Raspberry Pi 5 only: turns the USB-C controller into a host
 *                                      until the next restart and looks for the Argon ONE V5's chips; left on if found
 *   rpi-eeprom-status                  (no arguments) BCM2712 boards (Pi 5, 500, CM5) only: runs
 *                                      /usr/bin/rpi-eeprom-update (no options), parses its report
 *                                      strictly and answers "OK <uptodate|available|pending>
 *                                      <current> <latest> <release>"; read-only            anyone
 *   rpi-eeprom-update                  (no arguments) BCM2712 only: /usr/bin/rpi-eeprom-update -A
 *                                      bootloader (the newest bootloader of the configured
 *                                      release; finished by a restart), then the same report admin
 *   rpi-pmic                           (no arguments) BCM2712 only: runs /usr/bin/vcgencmd pmic_read_adc, parses it
 *                                      strictly (hw_pmic_parse) and counts the kernel log's under-voltage alarms
 *                                      (hw_uv_log). Answers "OK uv <count|-1> <last_s|-1> <RAIL>=<uV>,<uA|-1> ...",
 *                                      numbers and a closed list of rail names only (Monitor → Voltage); read-only anyone
 *   hw-firmware-missing                (no arguments) reads the kernel log (/dev/kmsg) and returns
 *                                      only the validated names of firmware files a driver asked
 *                                      for and that are still missing, as "LOG fw <file> <driver>"
 *                                      lines (the Store's suggestions, hwscan.c); read-only        anyone
 *   hw-memory-modules                  (no arguments) the memory modules from the SMBIOS tables
 *                                      (/sys/firmware/dmi/entries/17-<n>/raw, root only), parsed
 *                                      strictly in C (sysreport/smbios.c): one "LOG mem <size_mb>|
 *                                      <type>|<speed>|<configured>|<slot>|<bank>|<maker>" line per slot,
 *                                      never serials, asset tags or part numbers; read-only     anyone
 *   hw-serial-numbers                  (no arguments) this computer's serial numbers, for the Show button in System
 *                                      Information and nothing else: only the fixed files product_serial and
 *                                      board_serial (/sys/class/dmi/id, root only) and the device tree's
 *                                      serial-number, each validated (hw_serial_clean), one "LOG serial
 *                                      <system|board|device> <value>" line; read-only                    admin
 *   video-optimize <mode> <lower> <vp9> <av1>   auto|always|never, on|off, yes|no, yes|no (the
 *                                      last two: hardware VP9/AV1 decoding found this boot).
 *                                      Writes the browsers' video settings from fixed text:
 *                                      /etc/chromium/policies/managed/lordos-video.json and
 *                                      /etc/firefox-esr/lordos-video.js (docs/VIDEO.md)      anyone
 *   browser-restore on|off             on: each browser continues where it left off, from fixed text:
 *                                      /etc/chromium/policies/recommended/lordos-session.json and
 *                                      /etc/firefox-esr/lordos-session.js; off: removes both
 *                                      (docs/SESSIONS.md, "Session restore")                 anyone
 *   boot-test <n>                      LordBench's boot time test (bench/boottest.h): n restarts
 *                                      in a row, 2..10; writes only the fixed-format state file
 *                                      /var/lib/lordos/boot-test, which lord-sessiond follows at
 *                                      each boot; refused in the live session                  admin
 *   boot-test-cancel                   (no arguments) removes that file: no more restarts      anyone
 */
#ifndef LORD_HW_ADMIN_H
#define LORD_HW_ADMIN_H

#include "../admin.h"
#include "rpifw.h"

void hw_act_brightness(const AdminReq *r, const char *arg, size_t len);
void hw_act_power_profile(const AdminReq *r, const char *arg, size_t len);
void hw_act_suspend(const AdminReq *r, const char *arg, size_t len);
void hw_act_buttons(const AdminReq *r, const char *arg, size_t len);
void hw_act_ntp(const AdminReq *r, const char *arg, size_t len);
void hw_act_time_set(const AdminReq *r, const char *arg, size_t len);
void hw_act_bt_unblock(const AdminReq *r, const char *arg, size_t len);
void hw_act_rpi_case(const AdminReq *r, const char *arg, size_t len);
void hw_act_rpi_case_detect(const AdminReq *r, const char *arg, size_t len);
void hw_act_firmware_missing(const AdminReq *r, const char *arg, size_t len);
void hw_act_memory_modules(const AdminReq *r, const char *arg, size_t len);
void hw_act_serial_numbers(const AdminReq *r, const char *arg, size_t len);
void hw_act_eeprom_status(const AdminReq *r, const char *arg, size_t len);
void hw_act_eeprom_update(const AdminReq *r, const char *arg, size_t len);
/* rpi-eeprom-update's report, parsed (exposed for tests) */
typedef struct {
    long long current, latest;          /* build time stamps (0: current unknown) */
    char release[16];                   /* default, latest… ([a-z]) */
    int update;                         /* the program says an update is available */
} HwEeprom;
int hw_eeprom_parse(const char *out, HwEeprom *e);
void hw_act_rpi_pmic(const AdminReq *r, const char *arg, size_t len);
/* vcgencmd pmic_read_adc, parsed (exposed for tests). Rails are from a closed list of names; a well-formed line of
 * an unknown rail is left out; anything malformed, repeated, out of range or cut fails the whole reading. */
#define HW_PMIC_RAILS 14
#define HW_PMIC_LIMIT 30000000L                 /* 30 V or 30 A, in micro units: no such rail on these boards */
typedef struct {
    const char *name;                           /* from the closed list, static */
    long uv, ua;                                /* microvolts, microamps; -1 not reported */
} HwPmicRail;
typedef struct { HwPmicRail r[HW_PMIC_RAILS]; int n; } HwPmic;
int hw_pmic_parse(const char *out, HwPmic *p);
/* "uv <count> <last_s> <RAIL>=<uV>,<uA> ..."; 0 if it fits */
int hw_pmic_line(const HwPmic *p, int uv_count, long long uv_last_s, char *out, size_t n);
/* The kernel's own records of the board's alarm in /dev/kmsg's text: how many "Undervoltage detected!" lines from a
 * kernel driver are in the log (the ring: older ones may be gone) and the last one's time in seconds since boot, -1
 * with none. Text from anything but the kernel facility is ignored. */
int hw_uv_log(const char *log, size_t n, long long *last_s);
const char *hw_rpi_case_text(const char *id);   /* fixed file text of a known case, NULL otherwise */
void hw_act_video(const AdminReq *r, const char *arg, size_t len);
void hw_act_browser_restore(const AdminReq *r, const char *arg, size_t len);
void hw_act_boot_test(const AdminReq *r, const char *arg, size_t len);          /* LordBench, bench/boottest.h */
void hw_act_boot_test_cancel(const AdminReq *r, const char *arg, size_t len);
/* The two files video-optimize writes, from its argument; 0 if the argument is valid. */
int hw_video_files(const char *arg, char *chromium, size_t cn, char *firefox, size_t fn);
#define HW_VIDEO_EXT_ID "kadhcpnkalbaminbojfmbmlgoheicmjo"     /* assets/browser/video (manifest "key") */

/* Validators, exposed for tests. */
/* n bytes of a serial number file into out: trimmed, [A-Za-z0-9 ._:/-] only, under size bytes. 0 if it is a real
 * serial; -1 for anything else, the placeholders firmware leaves in these fields included. */
int hw_serial_clean(const char *raw, size_t n, char *out, size_t size);
int hw_button_action_valid(const char *a, int lid);
int hw_time_valid(const char *t);       /* "YYYY-MM-DD HH:MM:SS", a real date from 2000 to 2099 */
int hw_backlight_name_valid(const char *n);

void hw_act_wallpaper_get(const AdminReq *r, const char *arg, size_t len);
void hw_act_wallpaper_remove(const AdminReq *r, const char *arg, size_t len);
void hw_act_wallpaper_login(const AdminReq *r, const char *arg, size_t len);
void hw_act_wallpaper_image(const AdminReq *r, const char *arg, size_t len);
void hw_act_wallpaper_login_image(const AdminReq *r, const char *arg, size_t len);

#define HW_ADMIN_ACTIONS \
    { "wallpaper-get", hw_act_wallpaper_get, A_ANYONE }, \
    { "wallpaper-remove", hw_act_wallpaper_remove, 0 }, \
    { "wallpaper-login", hw_act_wallpaper_login, 0 }, \
    { "wallpaper-image", hw_act_wallpaper_image, A_FORK | A_ANYONE }, \
    { "wallpaper-login-image", hw_act_wallpaper_login_image, A_FORK }, \
    { "display-brightness", hw_act_brightness, A_ANYONE }, \
    { "power-profile", hw_act_power_profile, A_ANYONE }, \
    { "power-suspend", hw_act_suspend, A_ANYONE }, \
    { "power-buttons", hw_act_buttons, 0 }, \
    { "time-ntp", hw_act_ntp, 0 }, \
    { "time-set", hw_act_time_set, 0 }, \
    { "bt-unblock", hw_act_bt_unblock, A_ANYONE }, \
    { "rpi-case", hw_act_rpi_case, 0 }, \
    { "rpi-case-detect", hw_act_rpi_case_detect, A_FORK }, \
    { "rpi-pmic", hw_act_rpi_pmic, A_ANYONE | A_FORK }, \
    { "hw-firmware-missing", hw_act_firmware_missing, A_ANYONE }, \
    { "hw-memory-modules", hw_act_memory_modules, A_ANYONE }, \
    { "hw-serial-numbers", hw_act_serial_numbers, 0 }, \
    { "rpi-eeprom-status", hw_act_eeprom_status, A_ANYONE | A_FORK }, \
    { "rpi-eeprom-update", hw_act_eeprom_update, A_FORK }, \
    { "video-optimize", hw_act_video, A_ANYONE }, \
    { "browser-restore", hw_act_browser_restore, A_ANYONE }, \
    { "boot-test", hw_act_boot_test, 0 }, \
    { "boot-test-cancel", hw_act_boot_test_cancel, A_ANYONE },

#endif
