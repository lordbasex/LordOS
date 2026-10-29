# Fans and cooling

LordOS drives the fans by temperature from the first boot, before anyone logs in, and keeps doing
it if a session crashes. A small root service, `lord-fand`, reads the CPU temperature every two
seconds and writes each fan's duty from a profile. **Settings → Cooling** picks the profile, edits a
custom curve and runs the fans at full speed for a while; **Monitor → Cooling** shows the same
hardware live, with the curve and a temperature/fan history.

![Settings → Cooling on a Raspberry Pi 5 in the Argon ONE V5 case (fake sysfs, VM 116)](fans-settings.png)

| Piece | Code |
|---|---|
| Root service | `server/src/fan/fand.c` (`lord-fand`), unit `tools/iso/rootfs/etc/systemd/system/lord-fand.service` |
| Curves, presets, `fan.json`, socket requests | `server/src/fan/fancurve.c` (pure, tested) |
| Temperature and fan discovery, duty writes, giving control back | `server/src/fan/fanhw.c` |
| Session API `lord.fan` | `server/src/ui/js_fan.c` (registered from `js_hw.c`) |
| Shared UI: status store, illustrations, curve chart | `sdk/cooling.jsx` |
| Settings → Cooling | `apps/settings/Cooling.jsx` |
| Monitor → Cooling | `apps/monitor/Cooling.jsx` |
| Case accessories (Raspberry Pi 5) | admin action `rpi-case` in `server/src/hw/hw_admin.c` |
| Tests | `server/tests/fan_test.c` (`build/fan-test`, in `tests/run_suites.sh`) |

## What it drives

`lord-fand` scans sysfs once at start (`fanhw_scan`). A client never names a path or a device: it
only picks a profile, and the daemon drives every fan it found.

| Kind | Found as | How it is driven |
|---|---|---|
| **Board** | hwmon `pwmfan` with `pwm1` + `pwm1_enable` (the Raspberry Pi 5 fan header: the official Active Cooler, or a case fan plugged there, such as the Argon ONE V5 case's blower) | `pwm1` written directly; see "The board fan and the kernel" |
| **Motherboard** | hwmon with `pwmN` + `pwmN_enable` from an allow-listed Super I/O or fan controller driver: names starting with `nct6`, `nct7`, `it8`, `f71`, `f8000`, `w83`, `sch5`, `emc2`, `adt7`, `amc6821`, `max6650`, `max31790` | `pwmN_enable` = 1 (manual) while we drive it, the original value back afterwards |
| **Firmware** | any other hwmon with `fanN_input` (laptop embedded controllers, GPUs, unknown drivers) | never written; listed as "managed by the firmware" with their RPM |

Temperature source, first match: a thermal zone of type `cpu-thermal`, `x86_pkg_temp` or
`cpu_thermal`; then hwmon `k10temp`, `coretemp`, `zenpower` or `cpu_thermal` (`temp1_input`);
then `thermal_zone0`. Without a readable temperature the fans run at 100 %.

`lord-fand --detect` prints what it found. On the Raspberry Pi 5 used for testing:

```
board: Raspberry Pi 5
temperature: cpu-thermal (/sys/class/thermal/thermal_zone0/temp) 50.1 C
fan 0: board pwmfan pwm1 at 29 %, 2499 rpm (/sys/class/hwmon/hwmon2)
kernel trip 1: 50000 mC (/sys/class/thermal/thermal_zone0)
kernel trip 2: 60000 mC (/sys/class/thermal/thermal_zone0)
kernel trip 3: 67500 mC (/sys/class/thermal/thermal_zone0)
kernel trip 4: 75000 mC (/sys/class/thermal/thermal_zone0)
```

### PC support status

- **Desktop motherboards** with `nct67xx`/`nct6687` (the `nct6775`/`nct6683` drivers) or ITE
  `it87xx` (`it87`, often out of tree) chips expose `pwmN` + `pwmN_enable`; mode 1 is manual on all
  of them. The driver must be loaded (some boards need `acpi_enforce_resources=lax` for `nct6775`);
  LordOS does not load modules or change kernel parameters. Covered by the fake-sysfs test only:
  **not tried on a real motherboard yet.**
- Some headers drive a pump or are wired in DC mode; LordOS drives all allow-listed channels with
  the same curve. That is why a PC starts with the **System** profile (the motherboard firmware
  keeps control) and LordOS only takes over when the user picks a profile. The Raspberry Pi starts
  with **Balanced**.
- **Laptops** usually have no writable PWM (the embedded controller owns the fan), or a vendor
  driver with its own semantics: they show "Fans managed by the firmware" with the RPM, and are
  never written.
- **GPU** fans (`amdgpu`, `nouveau`, `radeon`) belong to their driver and are never written.

## Profiles

A curve is a list of steps "from T °C, P %". Below the first step the fan is off (or at the
profile's floor). A step is entered when the temperature reaches it and left only when it falls
`hyst` °C (3 by default) below it, so the fan does not flap around a threshold (`fan_curve_step`).

| Profile | Steps | Floor |
|---|---|---|
| System (`auto`) | none: the kernel or the firmware decides | — |
| Quiet | 60 °C → 30 %, 67 °C → 60 %, 72 °C → 100 % | off |
| **Balanced** (default on boards) | 55 °C → 30 %, 60 °C → 55 %, 65 °C → 100 % | off |
| Performance | 45 °C → 40 %, 55 °C → 70 %, 60 °C → 100 % | off |
| Always on | 55 °C → 60 %, 65 °C → 100 % | 40 % |
| Custom | 1 to 8 points, 30–90 °C strictly rising, 0–100 % never falling | off |

What reaches the hardware is never between 1 % and 24 % (raised to 25 %: small fans stall below
that), and a stopped fan given less than 50 % first gets one tick at 100 % so it starts.

**Full speed** ("Heavy work"): 30 minutes, 1 hour, 2 hours or until turned off. It is stored with
its end time, survives a restart of the service, and the profile comes back by itself.

## The board fan and the kernel

On a Raspberry Pi 5 the kernel already drives the fan header: the device tree binds the `pwm-fan`
cooling device to four active trip points of `cpu-thermal` (50, 60, 67.5 and 75 °C, cooling levels
0/75/125/175/250). Two ways to run our curve were possible:

1. **Rewrite the kernel's trip points** to match the curve. It survives a crash by itself, but the
   duty of each level is fixed by the device tree (only 5 levels, not our percentages), and the
   number of steps is fixed too.
2. **Write `pwm1` directly** and keep the kernel out of the way.

LordOS does (2), with the kernel as a **backstop**: while `lord-fand` drives the fan it moves the
four active trips to 78, 80, 82 and 84 °C (the critical 110 °C trip is never touched). Below that
the kernel's `step_wise` governor leaves `pwm1` alone; if `lord-fand` ever hangs and the board gets
hot, the kernel still spins the fan up by itself (the firmware throttles at 85 °C). Each tick the
daemon reads `pwm1` back and rewrites it if it differs, so a kernel step in between (seen for a few
seconds after a large change, while the governor's state settles) is corrected within 2 s.
`user_space` would be the cleaner governor, but the Raspberry Pi kernel only offers `step_wise`.

**Giving control back** (`fanhw_release`): the original trip temperatures are written back and
the cooling device's `cur_state` is set to the level the kernel would pick for the current
temperature, so there is no gap. This happens:

- on `SIGTERM`/`SIGINT` (stop, shutdown, package removal);
- in `lord-fand --restore`, the unit's `ExecStopPost`, which also runs after `SIGKILL` or a crash.
  Before taking a fan the daemon writes what it is about to change to `/run/lordos/fand.restore`
  (`trip <index> <m°C>`, `enable <channel> <value> <device path>`); `--restore` rescans, applies only
  values that match a fan the scan found (range-checked) and deletes the file;
- at start, if that file exists (a previous run died): restore first, then scan, so the raised
  trips are never mistaken for the originals.

A motherboard channel gets its original `pwmN_enable` (usually 2 or 5, the chip's automatic mode).

## Configuration and protocol

`/etc/lordos/fan.json` is written only by `lord-fand` (temporary file, `fsync`, `rename`), and read
only if it is a root-owned regular file nobody else can write. Strict reader (`sec/json.c`):
unknown keys, wrong types or an invalid curve make it fall back to the defaults.

```json
{"version":1,"profile":"balanced","hyst":3,"custom":[[55,30],[60,55],[65,100]],"force":"off","forceUntil":0}
```

`/run/lordos/fand.sock` (0666), one line per connection, at most 255 bytes, printable ASCII:

| Request | Who | Answer |
|---|---|---|
| `STATUS` | anyone | one JSON line: `temp`, `tempSource`, `board`, `profile`, `curve`, `custom`, `floor`, `hyst`, `step`, `target`, `force`, `forceLeft`, `fans` (`id`, `kind`, `chip`, `ch`, `pct`, `rpm`), `firmwareFans`, `canChange` |
| `PROFILE auto\|quiet\|balanced\|performance\|always\|custom` | administrators | `OK` or `ERR <code>` |
| `CURVE 55:30,60:55,65:100` | administrators | sets the custom curve and selects it |
| `FORCE off\|on\|<1..1440 minutes>` | administrators | full speed |

Administrators: `SO_PEERCRED` uid 0, or a user in `sudo`, `admin` or `wheel` (the same rule as the
admin helper). Errors are codes (`fan.notAdmin`, `fan.badRequest`, `fan.saveFailed`); the UI
translates them. A client that does not finish its line in one second is dropped; at most eight at
a time.

On the session side `lord.fan` (`js_fan.c`) validates everything again before sending, never waits
more than 300 ms for an answer, and `sdk/cooling.jsx` only polls (every 2 s) while a Cooling view
is on screen.

## The service

`lord-fand.service` starts early (`sysinit.target`, before `lordos.service`), `Restart=always`,
and is enabled by the package's `postinst` (also in the ISO and Raspberry Pi image chroots). It is
hardened like `lord-netperm.service`, and more:

- no capabilities at all (`CapabilityBoundingSet=` empty: root owns the sysfs files it writes);
- `RestrictAddressFamilies=AF_UNIX`, `PrivateNetwork=yes`, `IPAddressDeny=any`;
- `ProtectSystem=strict` with only `/etc/lordos`, `/run/lordos`, `/sys/devices/platform`,
  `/sys/devices/virtual/thermal` and `/sys/devices/pci0000:00` writable; `ProtectHome=yes`,
  `PrivateDevices=yes` (it uses no device node);
- `SystemCallFilter=@system-service` minus `@privileged @resources @mount @clock @debug @module
  @reboot @swap @raw-io`, `MemoryDenyWriteExecute`, `NoNewPrivileges`.

It executes nothing and links only libc. Cost: one sysfs read and at most one write every 2 s; 0.0 %
CPU and 1.5 MB RSS on the Raspberry Pi 5.

`--root DIR`, `--config FILE` and `--run DIR` exist for tests (a fake sysfs tree, a temporary
config and socket); the unit never passes them.

## Settings → Cooling and Monitor → Cooling

- At the top, our own drawing of the detected hardware (views only, no product photos or
  renders): the Argon ONE V5 case (aluminium body, fins, the blower, front ports), a Raspberry Pi 5
  with a heatsink and fan, a PC tower with a CPU cooler and case fans, or a passive heatsink when no
  fan can be driven. Light and dark variants (`docs/polish/fans-*.png`).
- The fan turns at a speed proportional to the real duty (eased; still at 0 %), and the fins warm
  from blue (40 °C) through amber (65 °C) to orange-red (80 °C). The rotors are turned with
  `lord.setStyle(id, 'rotate', degrees)` on their nodes at 20 frames per second, with no React render per
  frame (until 0.8.6, in development, they were sent as a `transform` key the engine doesn't have, so they
  stood still between React renders and the session log got `js: unsupported style 'transform'`); the timer
  stops at 0 %, while the window is minimized and with **Appearance → Reduce motion** (a new
  setting, `reduceMotion`). Frames: `docs/polish/fans-anim-1.png` … `-5.png`.
- The curve chart (`FanCurveChart`) shows the steps, the hysteresis band of each step (orange,
  stronger for the active one) and the live point (temperature, duty). In the custom profile the
  points can be dragged (temperature between their neighbours, duty in 5 % steps, never below the
  previous point); Add step / Remove step / Reset.
- Monitor shows the curve next to a three-minute temperature/fan history (Monitor's own chart), the
  profile, the step, every fan, and a **Full speed for 30 min** button (same `lord.fan.force`).
- Controls are enabled when `lord-fand` says the user may change them (`canChange`).

Measured on VM 116, headless `lord-server` with Monitor → Cooling open and the fan at 55 %, 30 s
with nobody watching: **2.4 % CPU** with the animation, **1.9 %** with Reduce motion (the
difference, about 0.5 % of one core, is the spinning fan). A first version that re-rendered React
on every frame cost 8.9 %.

![Monitor → Cooling](fans-monitor.png)

## Argon ONE V5 case

What the case is, from its printed manual (instruction manual AR1 V5, 2025-01-03) and a reading of
the vendor's install script `argon1v5.sh` and the scripts it downloads (`argononed.py`,
`argonregister.py`, `argonregister-v1.py`, `argonpowerbutton-libgpiod.py`, `argonone-irconfig.sh`,
`argon-blstrdac.sh`, `argon-rpi-eeprom-config-psu.py`, `argon-shutdown.sh`). They were downloaded
into a scratch folder and read; **none of them was executed**.

- **Fan:** a 30 mm PWM blower plugged into the **Raspberry Pi 5 fan header** (manual, assembly
  step 4: "Connect the blower FAN cable … to the exposed Raspberry Pi 5 FAN Port"; features table:
  "actively controlled by the Raspberry Pi OS"). So it is the `pwmfan` board fan above, driven by
  `lord-fand` like the Active Cooler. Measured on the user's case: 0 RPM stopped, about 2 700 RPM at
  30 %, 4 500–4 700 at 55 %, 6 700–7 400 at 100 %.
- **No microcontroller, no HAT EEPROM.** On the user's Raspberry Pi 5 in this case there is no
  `/proc/device-tree/hat`, and with I2C bus 1 enabled at runtime (`dtparam i2c_arm=on`, removed
  afterwards) `i2cdetect -y 1` shows **no device at all**, and reads of 0x1a fail.
- **The older Argon ONE protocol** (for reference; it is what the scripts still contain, for the
  cases with a microcontroller): I2C bus 1, address 0x1a. New firmware: register 0x80 = fan duty
  0–100, 0x81 = firmware version, 0x82 = IR code block, 0x86 = control (1 = cut the power after
  shutdown). Old firmware: a single byte written to 0x1a is the duty, 0xFF = power off. Their daemon
  probes "register support" by **writing** 0x80 and reading it back, uses 30 s delays before slowing
  down, raises any duty below 25 % to 25 %, kicks the fan at 100 % first, and its default curve is
  55 °C → 30 %, 60 °C → 55 %, 65 °C → 100 % (our Balanced). The power button reaches the board as
  pulses on GPIO 4 (20–30 ms = reboot, 40–50 ms = shut down), read with libgpiod, and the IR receiver
  uses `dtoverlay=gpio-ir,gpio_pin=23` with LIRC. LordOS implements none of this because the V5 has
  none of it, and blindly writing to address 0x1a could hit an audio codec on another board.
- **Its board's extras:** an HDMI-USB2-audio board adds two front USB 2.0 ports (an FE1.1s hub) and
  a microphone and headphone DAC (HS100B, USB audio). Both sit on the Raspberry Pi 5's USB-C
  controller, which has to run as a **host**: `dtoverlay=dwc2,dr_mode=host` (manual: "Enabling the
  USB ports and mic in and audio out ports"). The script also adds `usb_max_current_enable=1`,
  `dtparam=nvme`/`pciex1_gen=3`, and `PSU_MAX_CURRENT=5000` in the bootloader EEPROM.

**Detection.** The case cannot be detected from software: it has no EEPROM, no I2C device, and its
USB hub and DAC only appear after the USB-C controller is switched to host mode, and even then they
are generic chips used by other products. So LordOS does **not** guess. Settings → Cooling on a
Raspberry Pi 5 shows **Case: None or another case / Argon ONE V5 case**; choosing the Argon runs
the admin action `rpi-case argon-one-v5`, which writes `/boot/firmware/lordos-case.txt` (fixed
text, included at the end of `config.txt` by the image) and takes effect after a restart. Any other
case gets nothing Argon-specific. The illustration shows the Argon only after it is chosen.

| The vendor script | LordOS |
|---|---|
| Fan daemon (Python, I2C or GPIO) | `lord-fand` (C, root, hardened); the V5's fan is on the fan header |
| Fan curve config `/etc/argononed.conf` | Settings → Cooling, `/etc/lordos/fan.json` |
| `dtoverlay=dwc2,dr_mode=host` (front USB, audio) | `rpi-case argon-one-v5` → `lordos-case.txt` |
| `dtparam=nvme`, `pciex1_gen=3` | already in the LordOS image's `config.txt` |
| `usb_max_current_enable=1` | **not set**: it lets the board draw 1.6 A for USB, which is only safe with a 5 A supply; documented here instead |
| `PSU_MAX_CURRENT=5000` in the bootloader EEPROM | **not done**: LordOS does not change the bootloader configuration (Settings → Board firmware, 0.8.4.1, only applies Raspberry Pi's bootloader updates, never `rpi-eeprom-config`) |
| Power button daemon (GPIO 4 pulses) | not needed on the V5: the Raspberry Pi 5's own power button is handled by systemd-logind (Settings → Energy) |
| IR remote (LIRC, `gpio-ir` on GPIO 23) | not on the V5 (not in its feature list); left for a case that has it |
| OLED module, ZigBee module (optional add-ons) | not supported |
| Shutdown hook (`FANOFF`, power cut) | not needed: the fan header loses power with the board |

## Tests

- `build/fan-test` (`make test`, `tests/run_suites.sh`), 4 183 checks: curve steps and hysteresis
  (thresholds, jumps, oscillation, clamping), presets, output clamping, `fan.json` (valid files,
  round trip, 37 broken files, 4 000 random mutations and truncations), socket requests (28
  refused lines), and the sysfs driver on fake trees: a Raspberry Pi 5 (trips raised and restored,
  the kick, read-back correction, RPM, crash restore file), a PC (`nct6798` taken and given back,
  the `amdgpu` fan never touched, a tampered restore file ignored) and a passive machine.
  `build/fan-test mkfake DIR pi5|pc|none` builds a tree for manual runs and screenshots.
- On the Raspberry Pi 5 in the Argon ONE V5 case (as root from the build folder, temporary config
  and socket under `/run/lordos-test`): detection; Balanced kept the fan off at 50–53 °C during a
  4-thread `yes` load (the aluminium case is a large heatsink); test curves drove it to 55 % (4 500
  RPM) and full speed (7 400 RPM); `FORCE 30` and `FORCE off`; `PROFILE turbo` and garbage refused;
  `SIGTERM` and `kill -9` + `--restore` gave the trips back (50/60/67.5/75 °C, kernel state 1 at
  51 °C). The same binary under the unit's full hardening (`systemd-run` with the unit's
  properties): trips raised, `CURVE` applied and saved, and after killing only the main process
  `ExecStopPost` restored everything.
- On VM 116: `make -C server test` passes; headless `lord-server` with a fake-sysfs `lord-fand`
  for every variant, dark and light; the custom profile selected and a point dragged with the
  pointer (the daemon logged `CURVE 55:30,60:55,72:85`).

## Not done or untested

- The fan curve under the packaged `lord-fand.service` on the real Raspberry Pi: the packaged unit
  runs there (the 0.8.4 update restarted it, `ISO.md`, "Release test (0.8.4 Beta)"), but the
  measurements above were taken with the unit's properties through `systemd-run`.
- Real PC motherboards (`nct67xx`, `it87`) and laptops: fake sysfs only.
- The `rpi-case` action on the real board (it writes the boot partition; not run on the user's
  card) and the case's front USB ports and DAC with `dwc2` in host mode.
- Several board fans with different curves (all fans follow one profile).
- Temperature from disks (the vendor daemon also had an HDD curve).
