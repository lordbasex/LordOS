# Devices: the battery of everything connected

The Devices desk widget (Workspaces), the panel of the bar's battery item (every theme) and Monitor's Devices
tab show one list: the computer's battery, mice and keyboards, headphones and the rest. No external app is needed.

## Where each number comes from

| Source | What | How it reaches LordOS |
|---|---|---|
| `/sys/class/power_supply`, scope System | the computer's battery | `lord.power.state()`, refreshed on power_supply uevents (`hw/power.c`) |
| `/sys/class/power_supply`, scope Device | Logitech devices (`hidpp_battery_N`) and Bluetooth HID devices (`hid-<address>-battery`) | the same, with `level` (capacity_level) for a device that gives no percentage |
| BlueZ `org.bluez.Battery1` | Bluetooth headphones and the rest | `lord.bt.state()`, refreshed on BlueZ signals (`hw/bt.c`) |
| lord-hidpp | the exact percentage of a Logitech device the kernel reports only as a level | `lord.hidpp.batteries()`, at most once a minute (`hw/hidppsvc.c`) |

`sdk/devicesmodel.js` joins them (tested by `server/tests/devices_model_test.mjs`): a Bluetooth device both the
kernel and BlueZ report is listed once (matched by address); its kind (laptop, mouse, keyboard, headset, gamepad,
pen, phone) comes from BlueZ's icon name or words in its name; lord-hidpp's percentage fills the device whose power
supply it names. Colours: green, yellow under 30 %, red under 15 % (never while charging).

## lord-hidpp

The kernel's HID++ driver asks a Logitech device for its battery with the first of Battery Voltage (0x1001), Unified
Battery (0x1004) and Battery Status (0x1000) it has. With Unified Battery a device may give only levels (Critical,
Low, Normal, Full), and the kernel then never asks Battery Status, which on some devices has the percentage: an MX
Master 3 shows "Normal" in `capacity_level` and no `capacity`.

lord-hidpp asks each device for 0x1004 with a state of charge, then 0x1000, then 0x1001 (millivolts on a Li-ion
curve). A device with none of them keeps the kernel's level; no number is made up.

Its privileges (maintainer's approval 2026-11-17, battery reads only; AGENTS.md):

- `packaging/udev/72-lordos-hidpp.rules` gives group `lordos-hidpp` the hidraw nodes of the `logitech-hidpp-device`
  driver only (one per device, behind a receiver or over Bluetooth), never a receiver's own node: pairing and
  unpairing go through the receiver, so they can't be reached.
- The binary is `root:lordos-hidpp 2755`, set by postinst with `dpkg-statoverride`; no person is in the group.
- It takes no arguments and reads no environment. It opens the nodes (each checked against the device number in
  sysfs), drops the group for good, applies the helpers' sandbox (no network, no files, no ioctl) and only then
  talks to the devices: the root feature's lookup of the three battery features and their read functions, nothing
  else (`tests/hidpp_test.c` checks it against a fake device, with notifications and other programs' answers mixed
  in).
- It prints numbers only, `<hidraw> <percent> <status>`. lord-server checks each line and finds the device's power
  supply in sysfs itself.

## Peripherals and charge limits

A mouse or keyboard decides its own charging in firmware; neither HID++ nor Bluetooth has a standard way to set a
limit, and lord-hidpp only reads. The equivalent is a notice: unplug it at the top limit while it charges, charge it
at the bottom one (Settings → Battery, "Also for devices").
