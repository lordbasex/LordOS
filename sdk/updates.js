// updates.js — updating LordOS in place (docs/UPDATES.md).
//
//   lordosUpdate(lang)   { installed, version, available, size, notes } of the lordos package, or null
//   restartNeeded()      null, or { packages: [...] } when an installed update needs a restart
//   updatePrefs()        { autoCheck } (settings.json "updates"); setUpdatePrefs({ autoCheck })
//
// The packages come from the signed LordOS repository on lordos.org (and Debian's), through our own
// package engine (lord.pkg, admin.run('pkg-update' | 'pkg-install' | 'pkg-upgrade')). The optional
// daily check only downloads the signed lists and notifies; it never installs anything.
import { standaloneDesktop } from './platform.js';
import { settings } from './settings.js';
import { admin } from './admin.js';
import { notify } from './notify.js';
import { t } from './i18n.js';
import { wm } from './windows.js';
import { lordosVersion } from './system.js';

const has = () => !standaloneDesktop && typeof lord !== 'undefined' && !!lord.pkg && typeof lord.pkg.rebootRequired === 'function';

export const UPDATE_DEFAULTS = { autoCheck: true, notified: '' };
export const updatePrefs = (s = settings.get()) => ({ ...UPDATE_DEFAULTS, ...(s.updates || {}) });
export function setUpdatePrefs(patch) {
  settings.set((s) => ({ ...s, updates: { ...updatePrefs(s), ...patch } }));
}

// The lordos package: what is installed, the newest version offered and its release notes.
export function lordosUpdate(lang) {
  if (!has() || !lord.pkg.available()) return null;
  const info = lord.pkg.info('lordos', { lang });
  if (!info || !info.installed) return null;
  const offered = (info.versions || []).find((v) => v.version === info.version);
  return {
    installed: info.installed,
    version: info.version,
    available: !!info.upgradable,
    size: offered ? offered.size : info.size,
    notes: info.upgradable ? lord.pkg.releaseNotes('lordos', info.version) : null,
  };
}

// A restart is needed when a package asked for one (/run/reboot-required: a new kernel, or the
// lordos postinst) or when the installed LordOS is not the one running in this session.
export function restartNeeded() {
  if (!has()) return null;
  const asked = lord.pkg.rebootRequired() || null;
  let changed = false;
  if (lord.pkg.available()) {
    const installed = lord.pkg.info('lordos')?.installed;
    const running = lordosVersion().version;
    changed = !!installed && !!running && installed !== running;
  }
  if (!asked && !changed) return null;
  const packages = [...new Set([...(asked || []), ...(changed ? ['lordos'] : [])])];
  return { packages };
}

// ---- the daily check (on by default, Store → Updates turns it off) ----

const DAY = 24 * 3600;
// A short fingerprint of what can be upgraded, to notify once per new set of updates.
function fingerprint(ups) {
  let h = 5381;
  for (const s of ups.map((u) => `${u.name}=${u.to}`).sort().join(',')) h = ((h * 33) ^ s.charCodeAt(0)) >>> 0;
  return ups.length ? `${ups.length}:${h.toString(36)}` : '';
}

function maybeNotify() {
  const ups = lord.pkg.upgrades();
  const key = fingerprint(ups);
  if (!key || key === updatePrefs().notified) return;
  setUpdatePrefs({ notified: key });
  const own = ups.find((u) => u.name === 'lordos');
  const open = [{ id: 'open', label: t('store.notify.open'), run: () => wm.open('store', { args: { page: 'updates' } }) }];
  notify({
    app: 'store',
    title: own ? t('store.notify.lordosTitle', { version: own.to }) : t('store.notify.title', { count: ups.length }),
    body: own ? t('store.notify.lordosBody') : t('store.notify.body'),
    actions: open,
  });
}

let checking = false;
function check() {
  if (checking || !updatePrefs().autoCheck || !admin.available()) return;
  const last = lord.pkg.available() ? lord.pkg.lastUpdate() : 0;
  if (Date.now() / 1000 - last < DAY - 600) return;
  checking = true;
  admin.run('pkg-update', '', null, { quiet: true })
    .then(maybeNotify, () => { /* offline or busy: the next hour tries again */ })
    .finally(() => { checking = false; });
}

// Only in a user's desktop session (not the login screen or an isolated app's process).
if (has() && typeof lord.admin !== 'undefined' && typeof lord.greeter === 'undefined') {
  setTimeout(() => { check(); setInterval(check, 3600 * 1000); }, 5 * 60 * 1000);
}
