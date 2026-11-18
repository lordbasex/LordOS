// batterypolicy.js — what Settings → Battery's choices do over time (docs/BATTERY.md), pure (tested by
// server/tests/battery_policy_test.mjs). Each power change, batteryStep() looks at the battery, the person's choices
// and what it did before, and says what to do: a charge mode or thresholds for the admin actions (only an
// administrator's session can apply them), notices to show, choices that are done. Nothing here touches the system.
//
// prefs: { limit, resume, plugAt, heatGuard, fullOnce, dischargeTo, devicesNotify } (settings.power.battery)
// bat: the system battery's supply from lord.power.state() (percent, status, tempC, behaviour, behaviours,
//      startThreshold, endThreshold), or null
// devices: devicesmodel.js's list (the computer's own entry is skipped)
// mem: this function's own memory between calls (start with {})

export const BATTERY_DEFAULTS = { limit: 80, resume: 70, plugAt: 20, heatGuard: true, fullOnce: false, dischargeTo: false, devicesNotify: true };
export const HEAT_PAUSE_C = 40, HEAT_RESUME_C = 36;
const BEH = { auto: 1, 'inhibit-charge': 2, 'force-discharge': 4 };

export function batteryPrefs(p) {
  const o = { ...BATTERY_DEFAULTS, ...(p || {}) };
  const clamp = (v, lo, hi, d) => (Number.isFinite(v) ? Math.max(lo, Math.min(hi, Math.round(v))) : d);
  o.limit = clamp(o.limit, 50, 100, 80);
  o.resume = clamp(o.resume, 50, o.limit - 5, Math.min(70, o.limit - 5));
  o.plugAt = clamp(o.plugAt, 5, 50, 20);
  return o;
}

const can = (bat, mode) => !!bat && ((bat.behaviours || 0) & BEH[mode]) !== 0;

export function batteryStep(bat, rawPrefs, devices, mem = {}) {
  const prefs = batteryPrefs(rawPrefs);
  const out = { behaviour: null, thresholds: null, notify: [], done: [], mem: { ...mem, notified: { ...(mem.notified || {}) } } };
  const m = out.mem;
  if (bat && bat.percent >= 0) {
    const p = bat.percent, charging = bat.status === 'Charging', discharging = bat.status === 'Discharging';
    // plug in: once when it drops to the mark on battery; again only after it charged or rose 5 points above it
    if (discharging && p <= prefs.plugAt) {
      if (!m.notified.plug) { out.notify.push({ id: 'plug', key: 'prefs.battery.plugNotice', vars: { percent: p } }); m.notified.plug = true; }
    } else if (charging || p > prefs.plugAt + 5) m.notified.plug = false;

    // charge once to 100 %: thresholds up, then back to the person's once it is full or unplugged after charging
    if (prefs.fullOnce && bat.endThreshold >= 0) {
      if (!m.fullApplied) { out.thresholds = [Math.min(prefs.resume, 95), 100]; m.fullApplied = true; m.fullCharged = false; }
      else {
        if (charging) m.fullCharged = true;
        if (p >= 99 || (m.fullCharged && discharging)) { out.thresholds = [prefs.resume, prefs.limit]; out.done.push('fullOnce'); m.fullApplied = false; }
      }
    } else if (m.fullApplied) { out.thresholds = [prefs.resume, prefs.limit]; m.fullApplied = false; }

    // discharge on power down to the limit, then normal charging
    if (prefs.dischargeTo && can(bat, 'force-discharge')) {
      if (p > prefs.limit && bat.behaviour !== BEH['force-discharge']) { out.behaviour = 'force-discharge'; m.discharging = true; }
      else if (p <= prefs.limit) { out.behaviour = 'auto'; out.done.push('dischargeTo'); m.discharging = false; }
    } else if (m.discharging) { out.behaviour = 'auto'; m.discharging = false; }

    // heat: pause charging above HEAT_PAUSE_C, resume under HEAT_RESUME_C (only what this paused itself)
    if (!out.behaviour) {
      const hot = bat.tempC > -1000 && bat.tempC >= HEAT_PAUSE_C, cool = bat.tempC > -1000 && bat.tempC <= HEAT_RESUME_C;
      if (prefs.heatGuard && can(bat, 'inhibit-charge') && charging && hot && !m.heatPaused) {
        out.behaviour = 'inhibit-charge'; m.heatPaused = true;
        out.notify.push({ id: 'heat', key: 'prefs.battery.heatNotice', vars: { temp: Math.round(bat.tempC) } });
      } else if (m.heatPaused && (cool || !prefs.heatGuard)) { out.behaviour = 'auto'; m.heatPaused = false; }
    }
  }
  // devices: the same marks as notices (a mouse or keyboard decides its own charging)
  for (const d of devices || []) {
    if (!prefs.devicesNotify || d.kind === 'laptop' || !(d.percent >= 0)) continue;
    const lowId = `low:${d.id}`, fullId = `full:${d.id}`;
    if (!d.charging && d.percent <= prefs.plugAt) {
      if (!m.notified[lowId]) { out.notify.push({ id: lowId, key: 'prefs.battery.deviceLow', vars: { name: d.name, percent: d.percent } }); m.notified[lowId] = true; }
    } else if (d.charging || d.percent > prefs.plugAt + 5) m.notified[lowId] = false;
    if (d.charging && d.percent >= prefs.limit) {
      if (!m.notified[fullId]) { out.notify.push({ id: fullId, key: 'prefs.battery.deviceFull', vars: { name: d.name, percent: d.percent } }); m.notified[fullId] = true; }
    } else if (!d.charging || d.percent < prefs.limit - 5) m.notified[fullId] = false;
  }
  return out;
}
