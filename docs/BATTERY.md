# Battery: Settings → Battery and Monitor → Battery

For any laptop and every theme, through the kernel's standard power_supply attributes (no vendor tool). Not tested on
laptop hardware yet: the tests use a fake sysfs (`tests/settings_hw_test.c`) and a fake battery
(`tests/battery_settings_scene.mjs`).

## What shows where

- **Settings → Battery** (shown when the computer has a battery or something connected has one): the state with three
  marks (plug in, resume, limit), the last charge, the daily use of the last 8 days and the level by hour today, screen
  active and idle time on battery, the estimated use per app today, the battery's condition (normal or needs service,
  maximum capacity, cycles, manufacture date, first use with LordOS, maker and model), the optimized charging and the
  devices with their notices. History and condition live here.
- **Monitor → Battery**: only what happens now (charge, power, time, temperature, voltage, current, energy, state; the
  power of the last 60 s), read every 2 s while the tab is shown (`lord.power.live()`). See docs/DEVICES.md for the
  Devices tab.

## Optimized charging (maintainer's approval 2026-11-17, AGENTS.md)

Only an administrator's session changes it: the closed admin actions `battery-thresholds <start> <end>` (integers
50..100, start below end) and `battery-behaviour auto|inhibit-charge|force-discharge` (only one the battery lists),
written to the system battery chosen root-side (`src/hw/battery_admin.c`). What the battery lacks shows disabled with
why.

| Choice | How |
|---|---|
| Charge limit | `charge_control_end_threshold` |
| Sailing mode (resume below) | `charge_control_start_threshold`, where the battery has it |
| Notify to plug in | a notice at the mark (5..50 %, default 20 %); replaces the old 20 % low-battery notice (10 % and 5 % stay) |
| Full charge this time | thresholds up to 100, then back to the person's once full or unplugged after charging |
| Pause charging | `charge_behaviour` inhibit-charge |
| Heat protection | inhibit-charge while charging at 40 °C or more, back to auto at 36 °C or less (only what it paused itself) |
| Discharge on power | force-discharge down to the limit, then auto |
| Devices' notices | the same marks as notices for mice, keyboards and headphones (they decide their own charging) |

The decisions are `sdk/batterypolicy.js` (pure, `tests/battery_policy_test.mjs`), run on every power or Bluetooth
change by `sdk/hardware.jsx`. At the start of a session the person's thresholds are set again: some laptops forget
them at boot. Nothing runs while nobody is logged in except what the firmware keeps.

## History (`src/hw/batthist.c`)

Kept by lord-server in `~/.local/state/lordos/battery-history`, never sent anywhere, only on a computer with a battery:
the level every 10 minutes for 30 days; the discharge per day and in total; screen seen / not seen time on battery; each
session cgroup scope's CPU time on battery per day (the estimate per app: today's discharge split by CPU share); the last
time charging ended; the first day seen. When the battery reports no `cycle_count` (many don't), LordOS counts cycles: each
100 % discharged is one. `lord.power.history()`; `tests/batthist_test.c`.
