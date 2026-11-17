/* battery_admin.h — the admin helper's charge control actions (Settings → Battery, docs/BATTERY.md; maintainer's
 * approval 2026-11-17, AGENTS.md). Closed: the battery is chosen here (a system battery under /sys/class/power_supply,
 * type Battery, scope not Device, a real device under /sys/devices), the values are numbers and a word from a fixed
 * list, written to the kernel's standard attributes and nothing else.
 *
 *   battery-thresholds <start> <end>   charge_control_start_threshold / charge_control_end_threshold: integers
 *                                      50..100, start below end (start written only where the battery has it)  admin
 *   battery-behaviour <mode>           charge_behaviour: auto | inhibit-charge | force-discharge, only one the
 *                                      battery lists                                                          admin */
#ifndef LORD_HW_BATTERY_ADMIN_H
#define LORD_HW_BATTERY_ADMIN_H

#include "../admin.h"
#include <stddef.h>

void hw_act_battery_thresholds(const AdminReq *r, const char *arg, size_t len);
void hw_act_battery_behaviour(const AdminReq *r, const char *arg, size_t len);

/* Pure parts, exposed for tests (root: the power_supply folder; devices: the prefix a battery's real path must have,
 * "/sys/devices/" on this machine). */
int hw_battery_pick(const char *root, const char *devices, char *name, size_t n);
int hw_battery_thresholds_parse(const char *arg, int *start, int *end);      /* 0 if valid */
int hw_battery_behaviour_parse(const char *arg);                             /* POWER_BEH_* or 0 */
/* Writes the thresholds in an order the kernel accepts (an end below the current start would be refused).
 * 0 ok; -1 no such battery or attribute; -2 the kernel refused (errno set). */
int hw_battery_set_thresholds(const char *root, const char *devices, int start, int end);
int hw_battery_set_behaviour(const char *root, const char *devices, int beh);

#endif
