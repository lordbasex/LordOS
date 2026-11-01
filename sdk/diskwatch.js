// diskwatch.js — session notices when a disk gets too hot or its SMART health gets worse
// (docs/MONITOR.md, "Disk temperature and health"). startShell calls startDiskWatch().
//
// Temperatures: lord.monitor.watchDisks(true) lets the Monitor's thread read only the disk sensors
// every 30 s while the Monitor is closed (a spinning disk only while awake); this polls the result
// every 30 s. A notice when a disk reaches its limit (diskTempLimit), at most one per disk every 30
// minutes, and again only after it has cooled 3 °C below the limit.
// Health: every 6 hours (the first 3 minutes after login) the cached summary of each disk is
// refreshed (smart.refresh: smart-health, which never wakes a resting disk). A notice only when a
// disk is worse than the last time it was noticed (settings.monitor.diskHealth, by model and serial).
// Both are off with settings.monitor.diskAlerts = false (the switch in Monitor → Disks).
import { settings } from './settings.js';
import { notify } from './notify.js';
import { t } from './i18n.js';
import { wm } from './windows.js';
import { smart, diskTempLimit, diskAlertPrefs } from './smart.js';
import { disks, needsDiskFormats, openDiskFormats } from './disks.js';
import { HEALTH_RANK } from './smartparse.js';

const TEMP_EVERY = 30 * 1000, HEALTH_EVERY = 6 * 3600 * 1000, HEALTH_FIRST = 3 * 60 * 1000;
const RENOTIFY = 30 * 60 * 1000, HYSTERESIS = 3;
const hot = new Map();               // disk → { at (ms of the last notice), armed }
let started = false;

const label = (d) => d.model || `/dev/${d.name}`;

let watching = false;
function checkTemps() {
  const on = diskAlertPrefs();
  if (on !== watching) { watching = on; try { lord.monitor.watchDisks(on); } catch (e) { /* older server */ } }
  if (!on) return;
  let r = null;
  try { r = lord.monitor.diskTemps(); } catch (e) { return; }
  if (!r || !Array.isArray(r.disks)) return;
  const now = Date.now();
  for (const d of r.disks) {
    if (d.temp == null || !d.sensor) continue;
    const lim = diskTempLimit(d);
    const st = hot.get(d.name) || { at: 0, armed: true };
    if (d.temp < lim.max - HYSTERESIS) st.armed = true;
    if (d.temp >= lim.max && st.armed && now - st.at > RENOTIFY) {
      st.at = now;
      st.armed = false;
      const critical = lim.crit != null && d.temp >= lim.crit;
      notify({
        app: 'monitor',
        title: t(critical ? 'monitor.diskAlert.critTitle' : 'monitor.diskAlert.hotTitle', { name: label(d) }),
        body: t('monitor.diskAlert.hotBody', { dev: `/dev/${d.name}`, temp: Math.round(d.temp), max: lim.max }),
        sticky: critical,
        actions: [{ id: 'open', label: t('monitor.diskAlert.open'), run: () => wm.open('monitor', { args: { tab: 'disks', disk: d.name, nonce: Date.now() } }) }],
      });
    }
    hot.set(d.name, st);
  }
}

async function checkHealth() {
  if (!diskAlertPrefs()) return;
  let list = [];
  try { list = lord.disks.list() || []; } catch (e) { return; }
  let temps = null;
  try { temps = lord.monitor.diskTemps(); } catch (e) { /* none yet */ }
  const awake = new Set((temps?.disks || []).filter((d) => d.awake).map((d) => d.name));
  for (const d of list) {
    if (!smart.smartable(d.name) || d.transport === 'loop') continue;
    const s = await smart.refresh(d, { awake: awake.has(d.name) });
    if (!s || s.standby || s.health === 'unknown') continue;
    const key = `${d.model || d.name}|${d.serial || d.name}`;
    const seen = ((settings.get().monitor || {}).diskHealth || {})[key];
    if (seen === s.health) continue;
    settings.set((all) => {
      const mon = all.monitor || {};
      return { ...all, monitor: { ...mon, diskHealth: { ...(mon.diskHealth || {}), [key]: s.health } } };
    });
    // the first time a disk is seen healthy, or when it gets better: nothing to say
    if (HEALTH_RANK[s.health] <= HEALTH_RANK[seen || 'ok']) continue;
    notify({
      app: 'monitor',
      title: t(s.health === 'failing' ? 'monitor.diskAlert.failTitle' : 'monitor.diskAlert.checkTitle', { name: label(d) }),
      body: t(s.health === 'failing' ? 'monitor.diskAlert.failBody' : 'monitor.diskAlert.checkBody', { dev: `/dev/${d.name}` }),
      sticky: s.health === 'failing',
      actions: [{ id: 'open', label: t('monitor.health.openDisks'), run: () => wm.open('disks', { args: { disk: d.name, nonce: Date.now() } }) }],
    });
  }
}

// A disk plugged in with a format that needs Disk Formats: one offer per volume while this session lasts.
const offered = new Set();
function volumesOf() {
  const out = [];
  try { for (const d of lord.disks.list() || []) if (!d.system) out.push(d, ...(d.partitions || d.children || [])); } catch (e) { /* none */ }
  return out.filter((v) => v.fstype && !v.mountpoint);
}
function offerFormats(first) {
  for (const v of volumesOf()) {
    const key = v.uuid || v.name;
    if (offered.has(key) || !needsDiskFormats(v.fstype)) continue;
    offered.add(key);
    if (first) continue;                  // what was already here at login isn't news
    notify({ app: 'store', title: t('files.disk.formatsTitle', { name: v.label || v.name }), body: t('files.disk.formatsDetail'),
      actions: [{ id: 'store', label: t('files.disk.formatsGet'), run: openDiskFormats }] });
  }
}

export function startDiskWatch() {
  if (started || typeof lord === 'undefined' || !lord.monitor?.watchDisks || !lord.disks || typeof lord.greeter !== 'undefined') return;
  started = true;
  checkTemps();                       // turns the background reading on (or leaves it off)
  setInterval(checkTemps, TEMP_EVERY);
  setTimeout(() => { checkHealth(); setInterval(checkHealth, HEALTH_EVERY); }, HEALTH_FIRST);
  offerFormats(true);
  disks.watch(() => offerFormats(false));
}
