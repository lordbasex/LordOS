// appsoffactions.js — turning a built-in LordOS app off and on (Store → Installed: Deactivate / Activate;
// docs/PACKAGES.md, "Deactivating LordOS apps"). The rules are in appsoffmodel.js, the list in
// settings.json → appsOff (appsoff.js).
//
//   deactivateApp(id) → { ok: true } | { ok: false, reason: 'core' | 'vm' | 'open' | 'backup' }
//   activateApp(id)
//   offerActivate(id, then?)   the notice "<App> is deactivated" with Activate (then: what to open after)
//   startAppsOff()             called once by startShell
//
// Deactivating closes the app's windows (one with unsaved work asks, and nothing changes), unpins it
// from the Dock and stops what it does with its window closed:
//   - Mail: lord-maild stops syncing the mail accounts (mail config "mailPaused"); with Calendar,
//     Contacts and Reminders all off too, the calendar-and-contacts accounts ("davPaused");
//   - Calendar, Reminders: their alarms don't ring (calendar.js); Clock: its alarms and timer (alarms.js);
//   - Backups: no scheduled backups (its schedule becomes "manual", put back on Activate; a backup
//     running now is cancelled, a restore or deletion must finish first);
//   - Cleaner: its item by the clock goes;
//   - Machines: refused while a machine runs (a running VM is never stopped by this);
//   - the others (Voice Recorder, Camera, Remote Desktop, Scanner…) only work in their window.
import { settings } from './settings.js';
import { t, appName } from './i18n.js';
import { notify, dismissNotification } from './notify.js';
import { wm, windows, getApp, appIdOf, setOpenGate } from './windows.js';
import { launchApp } from './apps.js';
import { dock } from './dock.js';
import { appsOff, isAppOff } from './appsoff.js';
import { canDeactivate, withOff, vmActive, mailPause } from './appsoffmodel.js';
import { mailConfig } from './mail.js';
import { backup, backupStatus } from './backup.js';
import { arm as armClock, timerReset } from './alarms.js';

const hasLord = () => typeof lord !== 'undefined';
const label = (id) => { const a = getApp(id); return a ? appName(a) : id; };
const kept = () => settings.get().appsOffKept || {};
const setKept = (patch) => {
  const next = { ...kept(), ...patch };
  for (const k of Object.keys(next)) if (next[k] == null) delete next[k];
  settings.set({ appsOffKept: Object.keys(next).length ? next : null });
};

function machines() {
  try { return hasLord() && lord.vm && typeof lord.vm.list === 'function' ? lord.vm.list() || [] : []; } catch (e) { return []; }
}

// The app's windows: its own (in this process) and an isolated app's (a Wayland window with args.appId).
const windowsOf = (id) => windows.get().filter((w) => appIdOf(w.id) === id || (w.args?.pkg && w.args?.appId === id));

// ---- what runs with the window closed ----

// lord-maild's config: which accounts it leaves alone (server/src/mail/maild.c accounts_apply_config).
function applyMail(list) {
  if (!hasLord() || !lord.mail || typeof lord.mail.available !== 'function' || !lord.mail.available()) return;
  const want = mailPause(list);
  const cfg = mailConfig.get();
  if (!!cfg.mailPaused === want.mailPaused && !!cfg.davPaused === want.davPaused) return;
  const next = { ...cfg };
  for (const k of ['mailPaused', 'davPaused']) { if (want[k]) next[k] = true; else delete next[k]; }
  try { mailConfig.save(next); } catch (e) { console.warn(`apps off: mail config: ${e.message || e}`); }
}

// Backups: the schedule becomes "manual" while off (kept in appsOffKept.backupInterval). If the
// destination can't be validated now (a disk that isn't plugged in), the engine is paused for this
// session instead, and startAppsOff tries again at the next login.
let pausedBackup = false;
function applyBackup(off) {
  if (!backup.available()) return;
  let cfg = null;
  try { cfg = backup.config(); } catch (e) { cfg = null; }
  if (off) {
    if (backupStatus.get()?.state === 'running') { try { backup.cancel(); } catch (e) { /* not running */ } }
    if (!cfg || !cfg.dest || cfg.interval === 'manual') return;
    try {
      backup.configure({ ...cfg, interval: 'manual' });
      setKept({ backupInterval: cfg.interval });
    } catch (e) {
      try { backup.pause(true); pausedBackup = true; } catch (e2) { /* no engine */ }
    }
    return;
  }
  const iv = kept().backupInterval;
  if (iv && cfg && cfg.dest && cfg.interval === 'manual') { try { backup.configure({ ...cfg, interval: iv }); } catch (e) { /* the app can set it */ } }
  setKept({ backupInterval: null });
  if (pausedBackup) { try { backup.pause(false); } catch (e) { /* no engine */ } pausedBackup = false; }
}

function apply(id, off) {
  const list = appsOff();
  if (id === 'mail' || id === 'calendar' || id === 'contacts' || id === 'reminders') applyMail(list);
  if (id === 'backup') applyBackup(off);
  if (id === 'clock') { if (off) timerReset(); else armClock(); }
}

// ---- the actions ----

export function deactivateApp(id) {
  if (!canDeactivate(id)) return { ok: false, reason: 'core' };
  if (isAppOff(id)) return { ok: true };
  if (id === 'machines' && vmActive(machines())) return { ok: false, reason: 'vm' };
  if (id === 'backup' && ['restoring', 'deleting', 'unlocking'].includes(backupStatus.get()?.state)) return { ok: false, reason: 'backup' };
  // its windows close first; one that keeps itself open (unsaved changes) stops it. An isolated app's
  // window is asked to close by its own process (it always answers "not yet" here) and goes by itself.
  for (const w of windowsOf(id)) {
    if (!wm.close(w.id) && !w.args?.surface) return { ok: false, reason: 'open' };
  }
  const pinned = dock.isPinned(id);
  settings.set({ appsOff: withOff(settings.get().appsOff, id, true) });
  if (pinned) { dock.unpin(id); setKept({ pinned: [...new Set([...(kept().pinned || []), id])] }); }
  apply(id, true);
  return { ok: true };
}

export function activateApp(id) {
  if (!isAppOff(id)) return;
  settings.set({ appsOff: withOff(settings.get().appsOff, id, false) });
  const pins = kept().pinned || [];
  if (pins.includes(id)) {
    dock.pin(id);
    const rest = pins.filter((x) => x !== id);
    setKept({ pinned: rest.length ? rest : null });
  }
  apply(id, false);
}

// The notice shown instead of opening a deactivated app: "<App> is deactivated", Activate.
const shown = new Map();
export function offerActivate(id, then) {
  if (shown.has(id)) dismissNotification(shown.get(id));
  const nid = notify({
    app: 'store', title: t('store.off.notice', { name: label(id) }), body: t('store.off.noticeBody'), sound: false, timeout: 8000,
    actions: [{ id: 'activate', label: t('store.off.activate'), primary: true, run: () => { activateApp(id); if (then) then(); } }],
    onClose: () => shown.delete(id),
  });
  if (nid) shown.set(id, nid);
}

let started = false;
export function startAppsOff() {
  if (started) return;
  started = true;
  // wm.open and launchApp ask here first: a deactivated app opens nothing, it offers to activate it
  setOpenGate((appId, opts) => {
    if (!isAppOff(appId)) return true;
    if (!opts?.quiet) offerActivate(appId, () => (opts?.args || opts?.new || opts?.instance ? wm.open(appId, opts) : launchApp(appId)));
    return false;
  });
  // what an earlier session left (a mail config written by another version, a backup paused only
  // for that session): applied again, idempotent
  const list = appsOff();
  if (!list.length) return;
  applyMail(list);
  if (list.includes('backup')) applyBackup(true);
}
