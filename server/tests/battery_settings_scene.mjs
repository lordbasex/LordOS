// battery_settings_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Settings → Battery
// (apps/settings/Battery.jsx) on a laptop with charge control, in a session that isn't an administrator's: the
// sections render, the controls say only an administrator changes them, the battery's own cycles show, and the history
// (lord.power.history) fills the daily use. Prints "ok <name>" or "FAIL <name> <detail>".
import './battery_settings_scene_stubs.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { battery } from '../../sdk/hardware.jsx';
import { BatterySettings } from '../../apps/settings/Battery.jsx';
import prefsEn from '../../assets/locales/en/prefs.json';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'prefs' ? JSON.stringify(prefsEn) : shellCatalog(lang, ns));
const now = Math.floor(Date.now() / 1000);
const d = new Date();
const ymd = d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate();
lord.power = { ...(lord.power || {}), history: () => ({ firstSeen: now - 86400 * 30, cycles: 3.4, lastCharge: { t: now - 3600, p: 80 },
  samples: [[now - 600, 79, 0]], days: [{ ymd, drain: 160, on: 7200, off: 900, apps: { 'app-lordos-google-chrome.scope': 600000000, server: 200000000 } }] }) };
lord.admin = { ...(lord.admin || {}), available: () => false };
battery.set({ onAC: true, hasBattery: true, percent: 78, minutesLeft: -1, charging: false, supplies: [
  { name: 'BAT0', battery: true, device: false, present: true, percent: 78, status: 'Not charging', level: '', minutesLeft: -1, health: 91, cycles: 312,
    watts: 0, model: '5B10W13930', vendor: 'LGC', technology: 'Li-poly', volts: 12.4, amps: 0, tempC: 33, whNow: 40.6, whFull: 52, whDesign: 57,
    mfgYear: 2024, mfgMonth: 3, startThreshold: 70, endThreshold: 80, behaviours: 3, behaviour: 1 },
] });

enableShellLayers();
rt.render(h(rt.View, null, h(BatterySettings)));
await flush();
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (v) => [...texts].some(([id, x]) => String(x) === v && inTree(id));
const has = (re) => [...texts].some(([id, x]) => re.test(String(x)) && inTree(id));
for (const k of ['usage', 'apps', 'condition', 'charge', 'limit', 'resume', 'plug', 'fullOnce', 'pause', 'heat', 'discharge']) {
  check(`section or row: ${k}`, shown(prefsEn[`battery.${k}`]), prefsEn[`battery.${k}`]);
}
check('the percentage', shown('78 %'));
check('not an administrator: says so', shown(prefsEn['battery.adminOnly']));
check('the battery\'s own cycles', shown('312'));
check('the capacity', shown('91 %'));
check('today\'s use from the history', shown('16 %'));
check('an app named from its scope', has(/Google Chrome|Chrome/), JSON.stringify([...texts.values()].filter((x) => /chrome/i.test(String(x)))));
check('discharge on power not offered by this battery: says why', shown(prefsEn['battery.notSupported']));
process.exit(0);
