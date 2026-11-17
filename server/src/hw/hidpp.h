/* hidpp.h — the battery questions of Logitech's HID++ 2.0, for lord-hidpp (docs/DEVICES.md; maintainer's approval
 * 2026-11-17: read the battery only).
 *
 * Only three features are ever asked, each through the root feature (0x0000) to find its index:
 *   0x1004 Unified Battery: get_capabilities, then get_status (state of charge, when the device gives one)
 *   0x1000 Battery Status:  get_level_status (a percentage)
 *   0x1001 Battery Voltage: get_battery_voltage (millivolts, turned into a percentage with a Li-ion curve)
 * Nothing that changes a device, pairs it or reads anything else is in this file. The transport is a file
 * descriptor that reads and writes whole reports (a hidraw node; a socketpair in tests/hidpp_test.c). */
#ifndef LORD_HW_HIDPP_H
#define LORD_HW_HIDPP_H

#include <stdint.h>

enum { HIDPP_SHORT = 0x10, HIDPP_LONG = 0x11, HIDPP_LONG_LEN = 20 };
enum { HIDPP_SWID = 0x0A };          /* our software id: replies to the kernel's own questions are not ours */

/* percent: 0..100, or -1 for a device charging that gives no level then (an MX Keys on its cable);
 * status: 0 discharging, 1 charging, 2 full, -1 unknown */
typedef struct { int percent, status, source; } HidppBattery;   /* source: 0x1004, 0x1000 or 0x1001 */

/* A long request for feature index feat, function fn, with up to 16 parameter bytes. */
void hidpp_request(uint8_t out[HIDPP_LONG_LEN], uint8_t feat, uint8_t fn, const uint8_t *params, int nparams);
/* Classifies a report read back for that request: 1 our answer (its parameters, up to 16 bytes, to params),
 * -1 the device's error answer to it, 0 something else (a notification, another program's answer). */
int hidpp_match(const uint8_t *rep, int len, uint8_t feat, uint8_t fn, uint8_t params[16]);
/* Millivolts of a Li-ion cell to a percentage (Logitech's curve, as Solaar uses it), 0..100. */
int hidpp_voltage_percent(int mv);
/* The status byte of each feature to our status. */
int hidpp_status_1004(uint8_t charging);
int hidpp_status_1000(uint8_t status);

/* Asks the device on fd for its battery, each answer waited for at most timeout_ms. 0 with *b filled, -1 when
 * none of the three features gives a percentage (asleep, out of range, or levels only). */
int hidpp_battery(int fd, int timeout_ms, HidppBattery *b);

#endif
