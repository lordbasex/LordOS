/* tabletpower.h — what the power button does on a tablet by default (issue #131; docs/SETTINGS.md,
 * "Power button and lid"). Pure and root-side: the disk installer (src/install/install.c) writes it
 * into a new system and the power-buttons admin action (hw_admin.c) shares its text; migration 008
 * (packaging/migrations/008-0.8.8.9-tablet-power.sh) applies the same rule, in shell, to systems
 * installed before it.
 *
 * The rule: SMBIOS chassis type 30 (Tablet) or 32 (Detachable), from /sys/class/dmi/id/chassis_type,
 * gets a tap = suspend and a 5 s hold = poweroff. 31 (Convertible: a laptop whose screen folds back)
 * keeps a laptop's defaults: it is used with its keyboard most of the time, its power key sits where a
 * laptop's does, and its lid switch already decides sleep. Any other value, a missing file or junk
 * means "not a tablet": a wrong guess only keeps the old behavior. */
#ifndef LORD_TABLETPOWER_H
#define LORD_TABLETPOWER_H

#include <stddef.h>

#define LOGIND_LORDOS_CONF "/etc/systemd/logind.conf.d/lordos.conf"

/* 1 if <root>/sys/class/dmi/id/chassis_type is 30 or 32 (root NULL or "" = this machine). */
int tablet_chassis(const char *root);

/* The text of lordos.conf for these values (already validated; hold NULL or "" = no
 * HandlePowerKeyLongPress line). 0 if it fits in n, never cut. */
int logind_buttons_text(char *text, size_t n, const char *power, const char *hold, const char *lid, const char *lidac);

/* The installer's step: when tablet_chassis(sysroot), writes <target>/etc/systemd/logind.conf.d/lordos.conf
 * with power suspend, hold poweroff and the lid values of <target>/etc/systemd/logind.conf (logind's
 * defaults: suspend, and the same on AC). Never replaces a lordos.conf that is there, and leaves it all
 * alone when that logind.conf sets HandlePowerKey or HandlePowerKeyLongPress (an administrator's own
 * choice) or a lid value outside Settings' list. 1 written, 0 nothing to do, -1 error (errno). */
int tablet_power_install(const char *sysroot, const char *target);

#endif
