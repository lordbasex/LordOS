// smart.js — disk health through smartmontools (Disks app, Monitor; docs/SYSTEM.md, "SMART with
// smartmontools", and docs/MONITOR.md, "Disk temperature and health").
//
//   smart.installed()                 is /usr/sbin/smartctl there (lord.disks.tools().smartctl)
//   await smart.install(onLog)        installs Debian's smartmontools with the package engine
//                                     (pkg-install, an administrator; as Settings → Sharing does for SSH)
//   await smart.info(disk)            the full model (smart-info: smartctl -j -x), sdk/smartparse.js
//   await smart.health(disk)          the summary (smart-health: does not wake a resting disk)
//   await smart.test(disk, 'short' | 'long'), await smart.abort(disk)
//   await smart.refresh(disk, { awake, force })   updates smartSummaries[disk.name], at most every 5 min
//   useSmartSummaries()               { [disk]: { health, reasons, temp, wear, hours, standby, t } }
//
// All of it runs as the root helper's closed smart-* actions: fixed /usr/sbin/smartctl, a validated
// whole disk, JSON back in LOG lines. Without smartmontools the summary comes from LordOS's own
// reader (disk-smart), only for disks that will not be woken up by it.
import { standaloneDesktop } from './platform.js';
import { admin } from './admin.js';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { codeError } from './errors.js';
import { parseSmartctl, summarize, summaryFromOwn } from './smartparse.js';

export const SMART_PACKAGE = 'smartmontools';
export const SUMMARY_MS = 5 * 60 * 1000;      // the Monitor asks a disk at most every 5 minutes
export const smartSummaries = createStore({});
export const useSmartSummaries = () => useStore(smartSummaries);

const hasLord = () => typeof lord !== 'undefined' && !!lord.disks;
const smartable = (name) => /^(sd[a-z]{1,3}|hd[a-z]{1,3}|nvme\d{1,3}n\d{1,3})$/.test(name || '');

// The helper sends the JSON in pieces; OK says "<lines> <smartctl exit status>".
function runJson(action, arg) {
  let text = '';
  return admin.run(action, arg, (line) => { text += line; }, { quiet: true }).then((ok) => {
    const rc = Number(String(ok).split(' ')[1]) || 0;
    let j;
    try { j = JSON.parse(text); } catch (e) { throw codeError('disk.smartFailed'); }
    return parseSmartctl(j, rc);
  });
}

export const smart = {
  smartable,
  installed: () => { try { return hasLord() && !!lord.disks.tools().smartctl; } catch (e) { return false; } },
  install(onLog) {
    if (standaloneDesktop) return Promise.reject(codeError('admin.unavailable'));
    const go = () => admin.run('pkg-install', SMART_PACKAGE, onLog);
    return go().catch((e) => {
      if (e.code !== 'pkg.noIndex') throw e;    // no package lists yet (fresh install): fetch them once
      return admin.run('pkg-update', '', onLog).then(go);
    });
  },
  info: (name) => runJson('smart-info', name),
  health: (name) => runJson('smart-health', name),
  test: (name, kind) => runJson('smart-test', `${name}\n${kind === 'long' ? 'long' : 'short'}`).then((m) => {
    if (m.exit & 6) throw codeError('disk.smartTestFailed');   // device open or a command failed
    return m;
  }),
  abort: (name) => runJson('smart-abort', name),
  // disk: an entry of lord.disks.list() or of a Monitor sample ({ name, rota|rotational }).
  // awake: it had I/O lately (a spinning disk that rests is never asked by our own reader).
  async refresh(disk, { awake = false, force = false } = {}) {
    const name = disk?.name;
    if (!smartable(name)) return null;
    const old = smartSummaries.get()[name];
    if (!force && old && old.t && Date.now() - old.t < SUMMARY_MS) return old;
    let s = null;
    try {
      if (smart.installed()) {
        const m = await smart.health(name);
        s = m.standby && old ? { ...old, standby: true, t: Date.now() } : summarize(m);
      } else if (!(disk.rota ?? disk.rotational) || awake) {
        let own = null;
        await admin.run('disk-smart', name, (line) => {
          if (line.startsWith('smart ')) { try { own = JSON.parse(line.slice(6)); } catch (e) { /* malformed */ } }
        }, { quiet: true });
        s = summaryFromOwn(own);
      } else {
        // a spinning disk with no I/O lately, and no smartmontools to ask without waking it:
        // read it the next time it is in use (t 0: retried at the next refresh)
        s = old && old.health !== 'unknown' ? old : { health: 'unknown', reasons: [], temp: null, wear: null, hours: null, standby: false, supported: true, idle: true, t: 0 };
        if (s === old) return old;
      }
    } catch (e) {
      s = { health: 'unknown', reasons: [], temp: null, wear: null, hours: null, standby: false, supported: false, t: Date.now(), error: e.code || 'disk.smartFailed' };
    }
    if (s) smartSummaries.set((all) => ({ ...all, [name]: s }));
    return s;
  },
};

// When a disk counts as too hot: its own limit when it reports one, otherwise a conservative
// default for its kind (spinning disks run cooler than flash). crit: the drive's critical limit.
export function diskTempLimit(d) {
  const max = d.tempMax ?? (d.sensor === 'nvme' ? 75 : d.rota ? 55 : 70);
  return { max, crit: d.tempCrit ?? null, own: d.tempMax != null };
}

// settings.monitor.diskAlerts (default on): notices when a disk gets too hot or less healthy.
export const diskAlertPrefs = (s = settings.get()) => (s.monitor || {}).diskAlerts !== false;
export function setDiskAlerts(on) {
  settings.set((s) => ({ ...s, monitor: { ...(s.monitor || {}), diskAlerts: !!on } }));
}
