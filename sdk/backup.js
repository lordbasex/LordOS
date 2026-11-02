// backup.js — Backups (lord.backup, see server/src/backup and docs/SYSTEM.md).
//
//   const st = useBackup()            live status: { state, progress, last, next, dest, … }
//   backup.config() / backup.configure({ dest, interval, pauseOnBattery, exclude })
//   backup.runNow() / pause(on) / cancel()
//   backup.list() → [{ id, time }]    backup.ls(id, rel) → [{ name, dir, link, size, mtime }]
//   backup.restore(id, rel, { replace, suffix })   ('' = everything; result in status.restore)
//   backup.unlock(pass, create) / backup.lock()     encrypted backups (result in status.unlock)
//
// A axia-server thread at the lowest priority does the copying; the status is pushed
// (__lordBackup), never polled. When a run fails (or succeeds, if the user asked for it) the
// user is notified with notify(), even with the app closed.
import { createStore, useStore } from './store.js';
import { notify } from './notify.js';
import { t } from './i18n.js';
import { errorText } from './errors.js';
import { formatBytes } from './system.js';
import { wm } from './windows.js';

const has = () => typeof lord !== 'undefined' && lord.backup;
const read = () => { try { return has() ? lord.backup.status() : null; } catch (e) { return null; } };

export const backupStatus = createStore(read());
let asked = false;            // "Back Up Now": also notify on success
let restoring = false;
let lockedNoticed = false;    // "unlock your backups": once per session

function changed(prev, st) {
  if (!prev || !st) return;
  const open = [{ id: 'open', label: t('backup.notify.open'), run: () => wm.open('backup') }];
  if (st.last.when && st.last.when !== prev.last.when) {
    if (st.last.ok === 1 && asked) {
      notify({ app: 'backup', title: t('backup.notify.doneTitle'),
        body: t('backup.notify.doneBody', { count: st.last.files, size: formatBytes(st.last.bytes) }), actions: open, sound: false });
    } else if (st.last.ok === 0) {
      notify({ app: 'backup', title: t('backup.notify.failTitle'), body: errorText(st.last.error) || '', actions: open });
    }
    asked = false;
  }
  if (st.state === 'locked' && prev.state !== 'locked' && !lockedNoticed) {
    lockedNoticed = true;
    notify({ app: 'backup', title: t('backup.notify.lockedTitle'), body: t('backup.notify.lockedBody'), actions: open });
  }
  if (restoring && st.restore.done && !prev.restore.done) {
    restoring = false;
    notify({ app: 'backup', title: t(st.restore.ok ? 'backup.notify.restoredTitle' : 'backup.notify.restoreFailTitle'),
      body: t('backup.notify.restoredBody', { count: st.restore.files, both: st.restore.keptBoth }), actions: open, sound: false });
  }
}

globalThis.__lordBackup = (st) => {
  const prev = backupStatus.get();
  backupStatus.set(st);
  try { changed(prev, st); } catch (e) { console.log(`backup: ${e}`); }
};

export const backup = {
  available: () => !!has(),
  status: read,
  config: () => (has() ? lord.backup.config() : null),
  configure: (c) => lord.backup.configure(c),
  runNow: () => { asked = true; return lord.backup.runNow(); },
  pause: (on) => lord.backup.pause(!!on),
  cancel: () => lord.backup.cancel(),
  list: () => (has() ? lord.backup.list() || [] : []),
  ls: (id, rel = '') => lord.backup.ls(id, rel),
  path: (id) => lord.backup.path(id),
  restore: (id, rel, opts = {}) => {
    restoring = true;
    return lord.backup.restore(id, rel, { suffix: t('backup.restoredSuffix'), ...opts });
  },
  remove: (id) => lord.backup.remove(id),
  // encryption (Monocypher): the passphrase becomes the key in the backup thread; kept only in memory
  unlock: (pass, create = false) => lord.backup.unlock(String(pass), !!create),
  lock: () => lord.backup.lock(),
};

export const useBackup = () => useStore(backupStatus);
