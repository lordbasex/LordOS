# Settings: Displays, Bluetooth, Energy, Cooling, Notifications, Date & Time, Default Apps

Eight Settings pages laid out like the system settings of popular desktop systems, with the hardware side in
`server/src/hw/` (C, libc only) and the session side in `sdk/hardware.jsx`, `sdk/defaults.js` and
`sdk/notify.js`. Tested on VM 118 (`lordos-notes`) with `lord-sessiond`, virtio-gpu and a virtual
Bluetooth controller (`btvirt`). Screenshots: `docs/settings-*.png`.

Screenshots, dark and light: Displays (`settings-displays-dark.png`, `-light.png`), Bluetooth
(`settings-bluetooth-*`), Energy (`settings-power-*`), Notifications (`settings-notifications-*`,
plus `settings-notifications-app-light.png`), Date & Time (`settings-date-time-*`), Default Apps
(`settings-default-apps-*`); the revert countdown in both modes (`settings-displays-revert.png`,
`settings-displays-revert-light.png`), 200 % before sharp HiDPI (`settings-displays-200-percent.png`; now see `DISPLAYS.md`), rotation, Night Light,
pairing, Sound with a Bluetooth headset, a Linux app's notification and Files' "Open With".

| Page | Sidebar group | Code |
|---|---|---|
| Displays | with Appearance, Wallpaper, Desktop & Dock | `apps/settings/Displays.jsx`, `hw/display.c`, `hw/edid.c`, `hw/modes.c`, `hw/night.c`, `kms.c` |
| Bluetooth | with Network and Sound | `apps/settings/Bluetooth.jsx`, `hw/bt.c`, `hw/sysbus.c` |
| Notifications | with Sound | `apps/settings/Notifications.jsx`, `sdk/notify.js`, `hw/notifyd.c` |
| Date & Time | with Language & Region | `apps/settings/DateTime.jsx`, `hw/hw.c` (timedated) |
| Default Apps | with Keyboard and Trackpad | `apps/settings/DefaultApps.jsx`, `sdk/defaults.js`, `hw/defaults.c`, `hw/mimeapps.c` |
| Energy (Battery) | with Privacy | `apps/settings/Power.jsx`, `hw/power.c`, `hw/ppd.c` |
| Cooling | after Energy | `apps/settings/Cooling.jsx`, `sdk/cooling.jsx`, `ui/js_fan.c` → `lord-fand` (`server/src/fan/`); see `FANS.md` |
| Board firmware | after Cooling, only on a Raspberry Pi 5 (BCM2712: Pi 5, Pi 500, CM5) | `apps/settings/Firmware.jsx`, `hw/hw_admin.c` (`rpi-eeprom-status`, `rpi-eeprom-update`) |

Shared pieces: `apps/settings/ui.jsx` (a row with a hint, a pop-up button, the page header) on top of
the SDK's `GroupBox` (`docs/DESIGN.md`). All text goes through `t()` in the `prefs` namespace
(`assets/locales/{en,es,pt}/prefs.json`); `node tools/i18n-check.mjs` passes. Pages use
`textAlign: 'start'` and leading/trailing layout, so they mirror in Arabic and Hebrew; sliders and
the time fields keep `direction: 'ltr'`.

## How it fits together

```
 apps/settings/*.jsx ── sdk/hardware.jsx (stores, Night Light timer, idle, system dialogs)
        │                    ▲ __lordHw(kind, data): "display" "idle" "power" "bt" "bt-prompt"
        ▼                    │  "bt-result" "ppd" "ppd-result" "timedate" "notify" "notify-close"
 lord.display / power / bt / mime / timedate / notifyd   (server/src/ui/js_hw.c, validated)
        │
        ▼
 server/src/hw/   display.c ─ kms.c (modeset, gamma LUT, DPMS)        uevent.c (netlink: drm,
                  bt.c, ppd.c ─ sysbus.c ─ vault/dbus.c (system bus)     power_supply, backlight)
                  notifyd.c ─ vault/dbus.c (session bus)              power.c (sysfs)
                  defaults.c, mimeapps.c (~/.config/mimeapps.list)
        │
        ▼  root work: closed admin actions (hw/hw_admin.c)
 admin.c ─ display-brightness, power-profile, power-suspend, power-buttons, time-ntp, time-set,
           bt-unblock, rpi-case, video-optimize, hw-firmware-missing (the Store's suggestions,
           read-only), hw-memory-modules (System Information, read-only),
           rpi-eeprom-status (read-only), rpi-eeprom-update, boot-test, boot-test-cancel (LordBench's
           boot time test, docs/BENCHMARK.md)

 Cooling: lord.fan (ui/js_fan.c) ── /run/lordos/fand.sock ── lord-fand (root service, src/fan)
```

Nothing polls. Hotplugged displays, batteries and chargers arrive as kernel uevents (one
`NETLINK_KOBJECT_UEVENT` socket in the main loop), Bluetooth and power profiles as D-Bus signals,
idle deadlines go into the main loop's poll timeout, and the Night Light schedule is a JavaScript
timer that sleeps until the next change (30 s steps only during the 30-minute fade).

## Displays

![Displays](settings-displays-dark.png)

- **Outputs and modes.** `kms_outputs()` reads every connector with its modes and EDID blob.
  `hw/edid.c` parses the base block (vendor, product, serial, name descriptor, size, native
  timing) and `hw/modes.c` computes exact refresh rates (`clock·1000/(htotal·vtotal)`, interlace
  and double-scan aware), the list of resolutions (largest first, highest rate per size) and the
  best mode for a size and rate (progressive before interlaced; ties go to the preferred mode).
- **Applying.** `lord.display.apply(conn, w, h, mHz, rotation, scale)` changes that display's live
  configuration and `main.c` applies the whole display plan between frames (never with a page flip
  pending): the GPU path stops, heads are modeset, opened or closed (`outputs_apply`, see
  `DISPLAYS.md`), the session takes the new logical space (work areas, windows and Wayland outputs
  follow) and the GPU starts again. If the driver refuses a mode, the previous configuration is
  restored.
- **"Keep these display settings?"** The system draws the dialog (`DisplayConfirm`, mounted by
  `startShell`, so it works even if Settings is closed) and goes back to the previous mode after
  15 seconds without an answer, as popular desktop systems do.

  ![Keep these display settings](settings-displays-revert.png)
  ![The same in light mode](settings-displays-revert-light.png)
- **Saved per display.** "Keep" writes `~/.config/lordos/displays.json`:

  ```json
  { "main": "RHT-1234-QEMU_Monitor", "mirror": false, "captureAll": false,
    "displays": {
      "RHT-1234-QEMU_Monitor": { "w": 1280, "h": 800, "refresh": 74994, "rotation": 0, "scale": 100, "x": 0, "y": 0 },
      "RHT-1234-QEMU_Monitor@Virtual-2": { "w": 2560, "h": 1600, "refresh": 74999, "rotation": 0, "scale": 150, "x": 1280, "y": 0 } } }
  ```

  The key is the EDID identity (`vendor-product-serial`, or the name when there is no serial); two
  connected monitors with the same identity get the connector appended. `scale` is in percent (0 =
  follow the recommended one; files with 1 or 2 are read as 100 and 200 %); `x`, `y` place the display
  relative to the main one. `kms.c` asks `display_pick()` (registered with `kms_set_picker`) every
  time `lord-sessiond` lends the monitor to the session, so the main display's saved mode is used
  from the first frame; the other displays open right after. The greeter always uses the preferred
  mode.
- **Main display.** "Use as Main Display" moves the menu bar and the Dock to another display (no
  modeset; the arrangement stays). A plugged display is added and an unplugged one removed (DRM
  uevent → `display_hotplug()` → resync); its windows go to the main display.
- **Rotation (90°, 180°, 270°)**, per display. Done by software: the display's frame is composed
  upright and copied rotated into the card's buffer, only the damaged rectangles. The cursor is
  composited apart so the frame stays clean. Tablets and touch screens report physical positions,
  which are mapped back (`display_unrotate_point`) when there is one display. A rotated display
  turns the GPU path off.

  ![Rotated 90°](settings-displays-rotated.png)
- **Scale** (100, 125, 150, 175, 200 %), per display, sharp: the UI is laid out in logical px and
  rasterized at the display's resolution (text, vectors, images, the Terminal, the cursor). The
  menu marks the **recommended** one, from the EDID size (about 110 logical px per inch, never
  less than 1024×600 logical). Linux apps get the exact scale through fractional-scale-v1 (GTK 4,
  Qt 6, Chromium render at it) and legacy ones `wl_output.scale`. See `DISPLAYS.md`.

  ![100 % and 150 % side by side](displays-150.png)
- **Brightness.** Read from `/sys/class/backlight` (anyone can), written through the closed admin
  action `display-brightness <name> <value>`: the name must be a real backlight under `/sys/devices`
  and the value within `0..max_brightness`. External monitors (DDC/CI) and virtual machines have
  none; the row says so. Untested on hardware (the VM has no backlight).
- **Night Light.** A color temperature from 6500 K (off) down to 1900 K, from a blackbody fit
  (`night_temp_rgb`) normalized so that 6500 K is neutral. It goes into the CRTC's gamma LUT
  (`DRM_IOCTL_MODE_SETGAMMA`) when the card has one: free, and the cursor and direct scanout are
  tinted too. Without a LUT (virtio-gpu, some ARM boards) a 3×256 table is applied by software to
  each damaged rectangle while copying the frame, and the page says so; the cursor is not tinted
  then, and the GPU path is not tinted (pending: a multiply pass in `gpu.c`).

  ![Night Light applied](settings-night-light.png)

  Schedule: off, **sunset to sunrise** or **custom hours**, with a 30-minute fade, plus "Turn on
  now" (until the end of tonight's schedule, or 07:00). Sunrise and sunset come from the Almanac
  for Computers algorithm (`night_sun_times`, within ±4 min of NOAA in the tests) at the
  coordinates of the **chosen time zone** in `/usr/share/zoneinfo/zone1970.tab`. The location is
  never looked up on the network.
- **Video in the browser.** "Optimize video for this computer" (Automatic, Always, Never) and
  "Lower quality automatically when video stutters": when the hardware can't decode VP9 or AV1,
  the browsers are asked for H.264 (`Video.jsx`, `sdk/video.js`, the closed admin action
  `video-optimize`). See [VIDEO.md](VIDEO.md).
- **Multiple displays** (`DISPLAYS.md`): extend or mirror, the arrangement edited by dragging the
  displays (they snap next to each other), "Show menu bar on all displays", and what screenshots,
  recording and the remote desktop see (main display or all). Each display has its own CRTC,
  frame, page flips and scale. Since 0.8.5 each display has a number (the main one is 1); a click on
  a display in the arrangement, or **Identify**, shows that number big on the monitor for a moment;
  a dragged display snaps edge to edge and aligns with the others, and a drop never leaves two
  displays overlapping or apart (`DISPLAYS.md`, "Numbers, identify and snapping").

  ![Arrangement](displays-arrangement.png)

## Bluetooth

![Bluetooth](settings-bluetooth-dark.png)

`hw/bt.c` talks to BlueZ (`bluetoothd`, Debian package `bluez`) on the system bus with our own D-Bus
client (`vault/dbus.c`, plus `dbus_connect_system()`). One connection for Settings
(`hw/sysbus.c`) carries Bluetooth, power profiles and timedated, with replies matched by serial
and signals dispatched to handlers.

- **State.** `ObjectManager.GetManagedObjects` once, then `InterfacesAdded`, `InterfacesRemoved`
  and `PropertiesChanged` (matched with `AddMatch`, only from org.bluez's unique name).
  `Adapter1`, `Device1` and `Battery1` properties are parsed into a small model
  (`bt_apply_props`); changes are sent to the theme once per main-loop turn.
- **Actions.** Power (`Properties.Set Powered`), discovery while the page is open
  (`StartDiscovery`/`StopDiscovery`), pair (then `Trusted = true` and `Connect`, as other desktop systems do),
  connect, disconnect and forget (`Adapter1.RemoveDevice`). A soft rfkill block is lifted with the
  closed action `bt-unblock`.
- **Pairing agent.** lord-server exports `org.bluez.Agent1` at `/org/lordos/BluetoothAgent` with
  the `KeyboardDisplay` capability and becomes the default agent. PIN, passkey, numeric comparison
  and authorization requests are drawn by the system (`BluetoothPrompt`, outside any app) and
  answered with `lord.bt.answer`; PINs are 1–16 alphanumerics and passkeys 0–999999, validated in C.
  Calls to the agent from anyone but bluetoothd are refused. A device that went through pairing may
  use its services without asking again.

  ![Pairing request](settings-bluetooth-pairing.png)
- **Audio.** Headphones and speakers become PipeWire devices through WirePlumber's BlueZ monitor
  (Debian `libspa-0.2-bluetooth`) and appear in Settings → Sound. WirePlumber 0.5 only enables
  Bluetooth for a user whose session is active **on a logind seat**; sessions started by
  `lord-sessiond` are not logind sessions, so LordOS needs this drop-in
  (`/etc/wireplumber/wireplumber.conf.d/lordos-bluez.conf` on the ISO):

  ```
  wireplumber.profiles = {
    main = {
      monitor.bluez.seat-monitoring = disabled
    }
  }
  ```

  ![A Bluetooth headset in Sound](settings-sound-bluetooth-output.png)

Tested with `btvirt -l2` (bluez-test-tools) on VM 118: two virtual controllers on one virtual
radio. hci1 plays a headset (class 0x240404, discoverable, a `bluetoothctl` agent fed through a
FIFO); from Settings on hci0: discovery, pairing with the numeric-comparison prompt, connection,
the HFP output and input in Sound, disconnect and forget. Unit tests: `build/bt-test` (managed
objects, property dictionaries, battery, truncated input, removal, paths, state JSON). **Not tested
with real hardware** (no controller in the VMs).

## Energy (Battery)

![Energy](settings-power-dark.png)

- **Battery.** `hw/power.c` reads `/sys/class/power_supply` (`energy_*`/`power_now` or
  `charge_*`/`current_now`/`voltage_now`): percentage, time to empty or to full (the kernel's
  `time_to_*_now` when present), health (`full / full_design`), cycles, watts, model and
  technology. Device batteries (mice, keyboards: `scope = Device`) are listed but not counted.
  Refreshed on `power_supply` uevents. Unit-tested over a fake sysfs tree; **untested on a real
  laptop** (the VMs have no battery and the Debian kernel has no `test_power` module).
- **Low battery.** A notification at 20 %, 10 % and 5 % (sticky at 5 %) while discharging.
- **Menu bar.** A battery item with the percentage (optional) through `registerStatusItem`, so
  every theme that draws `<StatusItems />` shows it; hidden without a system battery.
- **Energy mode.** power-profiles-daemon over D-Bus (`org.freedesktop.UPower.PowerProfiles`, or the
  older `net.hadess.PowerProfiles`), following its `PropertiesChanged`. Setting it as the user goes
  to the daemon; if polkit refuses (the session is not a logind session) the page falls back to the
  closed action `power-profile`, which runs `/usr/bin/powerprofilesctl set <p>` or, without the
  daemon, writes the cpufreq governor (`performance`, `schedutil`/`ondemand`, `powersave`) and the
  energy/performance preference of every policy.
- **Display off, lock, sleep.** Seconds of inactivity (keyboard, mouse, touchpad, remote input),
  different on battery and on power. lord-server keeps the deadlines (`display_timeout` in the
  poll timeout): screen off is the connector's DPMS (`kms_dpms`; nothing is drawn while it is off),
  "require password" locks the session through `lord-sessiond` N seconds after the screen goes off,
  and sleep runs the closed action `power-suspend` (`systemctl suspend`). The VM is never put to
  sleep in the tests. Since 0.8.5 a Linux app that holds a Wayland idle inhibitor
  (`zwp_idle_inhibit_manager_v1`, `wl.c`) on a shown surface, such as a video player, counts as
  activity (`wl_idle_inhibited` in `main.c`), so the screen stays on.
- **Power button and lid.** What systemd-logind does (sleep, hibernate, shut down, do nothing). The
  admin action `power-buttons <power> <lid> <lid-on-ac>` writes
  `/etc/systemd/logind.conf.d/lordos.conf` (a short list of values, validated) and runs
  `systemctl reload systemd-logind`. LordOS never grabs the ACPI power button (only keyboards, mice
  and tablets are grabbed), so logind sees it. Lid rows only show with a battery.

## Cooling

![Cooling](fans-settings.png)

Fans by temperature, from the first boot, by the root service `lord-fand` (all the details in
`FANS.md`). The page draws the detected hardware (the fan turns with the real duty and the fins warm
with the temperature), the profile (System, Quiet, Balanced, Performance, Always on, Custom), the
curve with its hysteresis bands and the live point (the custom curve's points are dragged on it),
**Heavy work** (full speed for 30 min, 1 h, 2 h or until turned off) and, on a Raspberry Pi 5, the
**Case** (`rpi-case`). Without controllable fans it says so ("No controllable fans", or "Fans
managed by the firmware" with their RPM). Only administrators change anything: `lord-fand` checks
the peer and reports `canChange`. Monitor has the same view read-only, with a history and a
"Full speed for 30 min" button.

## Board firmware (Raspberry Pi 5)

| | | |
|---|---|---|
| ![Update available](settings-board-firmware-available.png) | ![Update ready](settings-board-firmware-pending.png) | ![Up to date](settings-board-firmware-uptodate.png) |
| An update is available | Updated: restart to finish | After the restart |

LordOS's own page for the bootloader in the board's EEPROM, instead of `raspi-config`. It is in the
sidebar only on a BCM2712 board (`lord.system.info().board === 'pi5'`, from the device tree's
`compatible` list: Raspberry Pi 5, Pi 500, Compute Module 5). It shows:

- the **status**: *Up to date*, *Update available*, or *Update ready: restart to finish*;
- the **installed** bootloader (its build date) and the **latest available**, which is the newest
  image in the installed `rpi-eeprom` package for the configured release;
- the **release** (*Default*, Raspberry Pi's recommended one, or *Latest*);
- **Update** (administrators), and after it **Restart…** (the session's Restart dialog) with a note
  to keep the power connected. Without `rpi-eeprom` it offers to install it with the package engine.

New bootloader images arrive as updates of the `rpi-eeprom` package, from the LordOS repository
([UPDATES.md](UPDATES.md), "Raspberry Pi firmware packages") or Raspberry Pi's archive that the
image also configures. The work is two closed admin actions:

- **`rpi-eeprom-status`** (anyone, no argument, read-only) runs `/usr/bin/rpi-eeprom-update` with no
  options and parses its report strictly (`hw_eeprom_parse`): only the bootloader block, its
  `BOOTLOADER: update available|up to date` verdict, the time stamps in `CURRENT:` and `LATEST:`
  (between 2001 and 2096) and a lowercase release name; the verdict has to agree with the numbers,
  and anything else is `hw.eepromFailed`. The program's own text never reaches the UI (its hint to
  run `raspi-config` included: on LordOS this page is the way). "Pending" means
  `/run/rpi-eeprom-update-pending` exists (the program's root-owned marker on tmpfs: an update was
  written or staged during this boot). The reply is `OK <uptodate|available|pending> <current>
  <latest> <release>`, checked again by the page.
- **`rpi-eeprom-update`** (administrators, no argument) runs `/usr/bin/rpi-eeprom-update -A
  bootloader` (fixed argument list, no shell, no file, the bootloader only) and answers with the
  same report. Exit status 3 is `hw.eepromFrozen` (`FREEZE_VERSION=1` in the bootloader's
  configuration), any other failure `hw.eepromUpdateFailed`.

Both check the board first (`hw.notRaspberryPi5`) and that the program is root-owned and not
writable by others (`hw.noEeprom`). How the update is applied is `rpi-eeprom-update`'s decision: on
the LordOS image (no `flashrom`, no `rpieepromab`) it stages the image and `recovery.bin` on the boot
partition and the bootloader flashes itself at the next boot, keeping its recovery path; with
`rpieepromab` or `flashrom` installed it writes the EEPROM at once. Either way the new version runs
after a restart.

**Not exposed:** editing the bootloader's configuration (`rpi-eeprom-config`) and the release
choice. The release lives in `/etc/default/rpi-eeprom-update`, a conffile of `rpi-eeprom`: writing
it from LordOS would make dpkg ask about it on every package update (or drop the choice), so it stays
`default` unless an administrator edits the file. Tested on the Raspberry Pi 5 (ISO.md, "Release test
(0.8.4.1 Beta)").

## Notifications

![Notifications](settings-notifications-dark.png)

`sdk/notify.js` gained a policy, applied to every `notify()`:

- **Per app:** allow, style (none / banners / alerts: alerts stay until dismissed), sound, and
  "show on lock screen" (stored, shown disabled: the lock screen is the greeter and does not show
  notifications yet). Apps appear after their first notification, plus LordOS apps that notify
  (Backup, Clock, Settings…) and packages that declare the `notifications` permission. For `.lpk`
  packages "allow" **is** the sandbox's `notifications` permission (`perms.set`), the same switch as
  Privacy & Security.
- **Do Not Disturb:** manual, and every day on a schedule. Notifications are kept in the history
  without a banner or sound.
- **Grouping:** several banners from the same app collapse into one with "+N".
- **Linux apps:** `hw/notifyd.c` serves `org.freedesktop.Notifications` on the session bus
  (spec 1.2: `Notify`, `CloseNotification`, `GetCapabilities`, `GetServerInformation`,
  `NotificationClosed`, `ActionInvoked`), so GTK, Qt, Electron and `notify-send` notifications
  become LordOS banners with the app's icon and actions, and follow the same per-app settings (the
  key is `linux:<desktop id>`, from the `desktop-entry` hint). Body markup is reduced to text.

  ![A notification banner (the test button in Settings; a Linux app's notifications look the same)](settings-notifications-linux-app.png)

## Date & Time

![Date & Time](settings-date-time-dark.png)

- **Automatic time:** systemd-timesyncd, switched with the closed action `time-ntp on|off`
  (`timedatectl set-ntp`); its state (NTP on, synchronized, available) is read from timedated over
  D-Bus.
- **Manual:** `time-set YYYY-MM-DD HH:MM:SS` (`timedatectl set-time`; a real date from 2000 to 2099,
  only with automatic time off).
- **Time zone:** a searchable list with the UTC offset of each zone (our TZif reader), applied with
  the existing `timezone` action. **Never guessed from the network**: no IP leaves the computer.
- **12/24 hours:** "like the region" (Language & Region), or forced; `formatTime` honors it
  everywhere.
- **Menu bar clock:** weekday and date, weekday only or time only, with or without seconds
  (`menuClock()`; the default theme uses it, the classic one honors the seconds).

## Default Apps

![Default Apps](settings-default-apps-dark.png)

Browser, mail, music, video, images, PDF, text editor and terminal. Candidates are LordOS's own apps
and the Linux apps whose `.desktop` `MimeType` covers the category (terminals: `Categories` with
`TerminalEmulator`).

- Saved in `~/.config/mimeapps.list` (freedesktop spec: the `[Default Applications]` line is
  replaced and the app goes first in `[Added Associations]`, as GLib does; every other line is
  kept), so `xdg-open` and GNOME/KDE apps agree. LordOS apps are written as `lordos-<app>.desktop`.
  The terminal goes in `~/.config/xdg-terminals.list`.
- Lookup follows the spec's order (`$XDG_CONFIG_HOME`, `/etc/xdg`, `~/.local/share/applications`,
  `/usr/local/share/applications`, `/usr/share/applications`, `defaults.list`), with the user's
  `[Removed Associations]` applied. File types come from shared-mime-info's `globs2` (highest
  weight, longest pattern, case-sensitive patterns first).
- **Automatic default (0.8.6, in development).** When the user never chose an app for a type,
  `apps_default_for()` (`server/src/apps.c`) still finds one, using only apps the launcher itself
  would run (a `.desktop` in `/usr/share/applications` or `~/.local/share/applications` that
  `apps_list` lists: not `Hidden`/`NoDisplay`/`Terminal`, `TryExec` found, `OnlyShowIn`/`NotShowIn`
  honoured, a `MimeType`, and an `Exec` with `%f`/`%F`/`%u`/`%U` naming a program that exists).
  The order:
  1. `[Default Applications]` of the files above, file by file and entry by entry, skipping entries
     that are not installed: the user's own files first (an *explicit* choice; there a
     `lordos-<app>.desktop` counts too, and the theme opens it), then what the system marks as
     default (`/etc/xdg/mimeapps.list`, `/usr/local/share/applications/mimeapps.list`,
     `/usr/share/applications/mimeapps.list`, `defaults.list`);
  2. their `[Added Associations]`, in the same file order;
  3. the installed apps whose `MimeType` declares the type, in `mimeinfo.cache` order
     (`~/.local/share/applications`, then `/usr/share/applications`), then the rest sorted by
     desktop id (strcmp), so the choice is the same on every run.

  `[Removed Associations]` of a file apply to it and to every file after it (and to steps 2–3).
  No MIME subclasses or aliases are followed (`text/html` does not match an app that only lists
  `application/xhtml+xml`). No app → nothing opens, as before. No path or argument was added:
  the folders are the launcher's, the file still goes through `launchWith`'s checks.
- **The JS side.** `lord.mime.resolve(mime)` returns `{ id, explicit, source }` (`source`:
  `user`, `system` for a system default or added association, `declared` for the first app that
  declares the type) or `null`; `lord.mime.unset(mime)` removes the user's line for that type
  from `[Default Applications]` and `[Added Associations]` of `~/.config/mimeapps.list` (as
  GLib's reset does; `[Removed Associations]` and every other line are kept). `lord.mime.get`
  still returns the first `[Default Applications]` entry without checking it is installed.
- **What Settings shows.** A category the user never chose shows **Automatic (<app>)**: the
  system's default if there is one, else LordOS's own app for the category (Photos, Player, PDF
  Viewer, Text Editor, Mail), else the first installed app that declares the type (the web
  browser on a fresh account). Choosing an app writes it as before; an explicit choice can go
  back with **Automatic**, which calls `lord.mime.unset` for every type of the category. The
  terminal is unchanged (`~/.config/xdg-terminals.list`).
- **Files** lists the default app first in "Open With", marked "(default)", and double-click opens
  it (`defaultOpenerId`: the user's choice or the system's default if installed, else the
  category's LordOS app, else the first declaring app; on a fresh account an `.html` file now
  opens in the installed browser instead of the Text Editor). **`lord.apps.launchWith('', path)`**
  opens a file with the default Linux app for its type, the automatic one included (the same
  rules as `launchWith`: a real path in the home or `/media/$USER`, one argument, no shell); it
  returns false when the default is a LordOS app, which the theme opens itself, or when no
  installed app opens the type.
- Tests: `build/defaults-test` (`tests/defaults_test.c`, in `make -C server test`) builds a fake
  XDG tree (`defaults_set_root`, tests only) and checks the user's choice, the system lists and
  their order, the first declaring app (by id and by `mimeinfo.cache`), the skipped apps
  (Hidden, NoDisplay, Terminal, OnlyShowIn, a missing TryExec, an Exec without a file code or with
  a missing program), ids outside the launcher's folders, `[Removed]`/`[Added Associations]`,
  going back to automatic, and that `launchWith('')` really opens the chosen app.

  ![Open With](files-open-with-menu.png)

## Appearance: «Browsers: tabs in the title bar»

Not a hardware page, but its native side lives here too: `hw/brframe.c`, exposed as
`lord.browserFrame.apply(on, 'all'|'chromium'|'firefox')`, `.state()`, `.tidy()` and
`.chromiumRunning()` in `ui/js_hw.c`; the page is `apps/settings/BrowserFrame.jsx` (strings in
`settings.json`, `browserTabs.*`) and the logic `sdk/browserframe.js`. It writes one preference in the
user's own Chromium and Firefox profiles; see `WAYLAND.md`, "The switch in Settings".

## Other pages that changed in 0.8.5

Not hardware pages, listed here so this document points to them:

- **Appearance → Desktop style** lists every installed theme and is the only place to choose one; the
  separate Themes page is gone (an old link to it opens Appearance). The two LordOS themes are now
  called **Lord Flow** (menu bar and Dock) and **Lord Classic** (taskbar and start menu); the choice
  is kept (`THEMES-GUIDE.md`).
- **Sharing → Advanced: root over SSH**, off by default, administrators only, every change confirmed
  with the administrator's own password (`REMOTE.md`, "Root over SSH").
- **Privacy → Blocklists** and the **Deny silently** firewall mode, plus the optional network item in
  the menu bar (`FIREWALL.md`, "Blocklists" and "The modes"; `MONITOR.md`).

## Admin actions

All in `server/src/hw/hw_admin.c`, in the `actions` table through `HW_ADMIN_ACTIONS`:

| Action | Argument | Who | Does |
|---|---|---|---|
| `display-brightness` | `<backlight> <value>` | anyone | writes `/sys/class/backlight/<name>/brightness` (real device under `/sys/devices`, `0..max`) |
| `power-profile` | `performance` \| `balanced` \| `power-saver` | anyone | `/usr/bin/powerprofilesctl set`, or the cpufreq governor and EPP |
| `power-suspend` | — | anyone | `/usr/bin/systemctl suspend` |
| `power-buttons` | `<power> <lid> <lid-on-ac>` (`suspend`, `hibernate`, `poweroff`, `ignore`) | admin | generates `/etc/systemd/logind.conf.d/lordos.conf`, `systemctl reload systemd-logind` |
| `time-ntp` | `on` \| `off` | admin | `/usr/bin/timedatectl set-ntp` |
| `time-set` | `YYYY-MM-DD HH:MM:SS` | admin | `/usr/bin/timedatectl set-time` |
| `bt-unblock` | — | anyone | rfkill "unblock all Bluetooth" on `/dev/rfkill` |
| `rpi-case` | `none` \| `argon-one-v5` | admin | Raspberry Pi 5 only: writes `/boot/firmware/lordos-case.txt` (fixed text, included from `config.txt`; see `FANS.md`) |
| `rpi-eeprom-status` | — | anyone | BCM2712 boards only, read-only: `/usr/bin/rpi-eeprom-update` (no options), report parsed strictly; `OK <uptodate\|available\|pending> <current> <latest> <release>` (see "Board firmware") |
| `rpi-eeprom-update` | — | admin | BCM2712 boards only: `/usr/bin/rpi-eeprom-update -A bootloader`, then the same report; the update is finished by the next restart |
| `hw-firmware-missing` | — | anyone | read-only: reads `/dev/kmsg` and returns only the validated names of firmware files a driver asked for and that are still missing (`LOG fw <file> <driver>`), for Store → Discover's suggestions (`hw/hwscan.c`, see `PACKAGES.md`) |
| `hw-memory-modules` | — | anyone | read-only: parses the firmware's SMBIOS memory devices (`/sys/firmware/dmi/entries/17-<n>/raw`, root only) strictly in C (`sysreport/smbios.c`) and returns one `LOG mem <size_mb>\|<type>\|<speed>\|<configured>\|<slot>\|<bank>\|<maker>` line per slot; serials, asset tags and part numbers are never read (System Information → Hardware → Memory, see `BENCHMARK.md`) |

The validators are unit-tested in `build/settings-hw-test`.

## Tests

- `build/settings-hw-test` (`make test`): EDID (QEMU's real block, serial/name identities, broken
  checksums), mode selection and refresh rates, Night Light math (temperature, ramp, sunrise and
  sunset in London, Buenos Aires, Tokyo and Svalbard, schedule across midnight, zone1970.tab),
  mimeapps.list editing and globs2, the power_supply reader, the admin validators and the
  `rpi-eeprom-update` report parser (the real report from a Pi 5, verdicts that disagree with the
  numbers, repeated or missing lines, bad time stamps and release names, the Pi 4's VL805 block, the
  BCM2712 check).
- `build/bt-test` (`make test`): BlueZ parsing. 32 checks.
- `build/brframe-test` (`make test`): the browsers' title bar switch (Chromium `Preferences`,
  Firefox `user.js`, running browsers, hostile profile folders). 90 checks.
- `build/fan-test` (`make test`): curves, hysteresis, `fan.json`, the socket requests and the fan
  driver on fake sysfs trees (see `FANS.md`).
- On VM 118: live mode changes with revert and keep, persistence across a session restart,
  rotation, software Night Light, Bluetooth with `btvirt`, power profiles with
  power-profiles-daemon (placeholder driver), the logind drop-in, NTP and manual time, Linux
  notifications with `busctl`, default apps with `xdg-mime` and Files.

## Not done or untested

- Several displays and HiDPI on real monitors (tested with two virtio-gpu heads; see `DISPLAYS.md`).
- Night Light on the GPU path when the card has no gamma LUT.
- Brightness, battery, lid and power profiles on real laptops; Bluetooth with real controllers.
- Notifications on the lock screen; a notification center listing the history.

## ISO packages

`bluez`, `libspa-0.2-bluetooth`, `power-profiles-daemon`, `systemd-timesyncd` and
`shared-mime-info`, plus the WirePlumber drop-in above (see the final report for the exact lines).
