/* periph.h — the peripherals of this machine for System Information's Hardware tab (lord.sysinfo.peripherals()):
 * keyboards, mice, receivers and what else hangs off USB, read without root from sysfs. The devices behind a
 * wireless receiver come from the input class, with their battery from the HID++ power supplies.
 *
 * This is NOT part of the report. A device names itself, and a name can carry a person's ("Ana's keyboard"), so
 * the structures in sysreport.h have no place for these strings and the lordos-sysreport and lordos-fetch commands
 * don't link this file. Serial numbers (the USB "serial" attribute, a power supply's serial_number) and addresses
 * (an input device's "uniq" and "phys") are never read. Bluetooth devices come from BlueZ, in the app
 * (apps/sysinfo/model.js, from lord.bt). */
#ifndef LORD_SYSREPORT_PERIPH_H
#define LORD_SYSREPORT_PERIPH_H

/* [{ name, kind, connection, battery }], malloc'd, NULL on out-of-memory. kind: keyboard, mouse, receiver, touchpad,
 * gamepad, input, audio, camera, storage, printer, network, bluetooth, other; connection: usb, receiver, internal;
 * battery: 0..100, or -1 unknown. root prefixes every path ("" = this machine); usb_ids is the name list (NULL: the
 * Debian path under root). */
char *periph_json(const char *root, const char *usb_ids);

#endif
