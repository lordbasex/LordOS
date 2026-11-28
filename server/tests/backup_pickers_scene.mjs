// backup_pickers_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Backups → Settings (#200).
// Choose… next to the destination opens the system file dialog in folder mode without the Cloud accounts, its button is
// off in a Cloud account's folder (with the note), and choosing a disk's folder makes it the destination. Choose… in
// Exclusions adds a folder of the home folder, relative to it; a typed folder outside the home folder, or the home folder
// itself, shows the error and is not stored. New Folder in the dialog makes a folder in the one in view, opens it, and it can
// be chosen (GitHub #262). Prints "ok <name>" or "FAIL <name> <detail>".
import './battery_settings_scene_stubs.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { cloudState } from '../../sdk/cloud.js';
import Backup from '../../apps/backup/index.jsx';
import backupEn from '../../assets/locales/en/backup.json';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'backup' ? JSON.stringify(backupEn) : shellCatalog(lang, ns));

// a home folder with Videos and a Cloud account's folder; one USB disk; the Cloud account mounted under ~/Cloud
const HOME = '/home/u', DISK = '/media/u/USB', DRIVE = `${HOME}/Cloud/Drive`;
const TREE = {
  [HOME]: ['Cloud', 'Videos'], [`${HOME}/Cloud`]: ['Drive'], [DRIVE]: ['Photos'], [`${DRIVE}/Photos`]: [],
  [`${HOME}/Videos`]: ['Large'], [`${HOME}/Videos/Large`]: [], [DISK]: ['Saved'], [`${DISK}/Saved`]: [],
};
const made = [];
const dirEntry = (name) => ({ name, dir: true, size: 0, mtime: 0 });
lord.fs = { ...(lord.fs || {}),
  home: () => HOME,
  volumes: () => [{ path: DISK, name: 'USB', fs: 'ext4', total: 1e11, free: 5e10 },
    { path: DRIVE, name: 'Drive', fs: 'fuse.rclone', total: 0, free: 0 }],
  list: (p) => (TREE[p] || []).map(dirEntry),
  stat: (p) => (TREE[p] ? { dir: true, writable: true, size: 0, mtime: 0 } : null),
  mkdir: (p) => { made.push(p); TREE[p] = []; const at = p.slice(0, p.lastIndexOf('/')); TREE[at] = [...(TREE[at] || []), p.slice(at.length + 1)]; return true; },
};
let cfg = { dest: '', interval: 'hourly', pauseOnBattery: true, encrypt: false, exclude: [] };
const configured = [];
lord.backup = {
  status: () => ({ state: 'nodest', dest: cfg.dest, destAvailable: false, last: {}, progress: {}, restore: {}, unlock: {} }),
  config: () => ({ ...cfg, exclude: [...cfg.exclude] }),
  configure: (c) => { configured.push(c); cfg = { ...cfg, ...c }; },
  list: () => [],
};
cloudState.set(() => ({ accounts: [{ id: 'd1', name: 'Drive', mount: DRIVE, state: 'ok' }] }));

enableShellLayers();
rt.render(h(rt.View, { style: { width: 980, height: 640 } }, h(Backup)));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const nodes = (s) => [...texts].filter(([id, x]) => String(x) === s && inTree(id)).map(([id]) => id);
const shown = (s) => nodes(s).length > 0;
const click = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };
const dbl = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };
// a press on a text (the k-th of that text), else on what holds it, until `done`
const clickText = async (s, done, k = 0) => { const all = nodes(s); let n = all[k < 0 ? all.length + k : k]; for (let i = 0; n && i < 5 && !done(); i++, n = parents.get(n)) await click(n); };
// a press on a text only (a row's press stops there; its list's would clear the selection)
const press = async (s, k = 0) => { const all = nodes(s); const n = all[k < 0 ? all.length + k : k]; await click(chainOf(n)); };
// the first node of a text's chain the runtime knows (a text's own node has none; its Text has)
const chainOf = (n) => { while (n && !globalThis.__lordTest.styles.get(n)?.color) n = parents.get(n); return n; };
const openRow = async (s, done) => { let n = nodes(s)[0]; for (let i = 0; n && i < 5 && !done(); i++, n = parents.get(n)) await dbl(n); };

check('Settings shown (no destination yet)', shown(backupEn.useFolder));
check('two Choose… buttons', nodes(backupEn.choose).length === 2, String(nodes(backupEn.choose).length));
check('the Cloud mount is not a destination row', !shown('Drive'));

// ---- the destination ----
await clickText(backupEn.choose, () => shown(backupEn.pickDest), 0);
check('Choose… opens the destination dialog', shown(backupEn.pickDest));
check('the dialog lists the disk', shown('USB'));
check('the dialog has no Cloud section or account', !shown('Drive'));
// into ~/Cloud, then select the account's folder: the button is off, with the note
await openRow('Cloud', () => shown('Drive'));
check('~/Cloud is browsable', shown('Drive'));
await clickText('Drive', () => shown('A Cloud folder can’t be chosen here.'));
check('a Cloud account\'s folder selected: the note', shown('A Cloud folder can’t be chosen here.'));
await clickText('Choose this folder', () => configured.length > 0);
check('…and Choose this folder does nothing', configured.length === 0, JSON.stringify(configured));
// to the disk, and choose its Saved folder
await clickText('USB', () => shown('Saved'), -1);
check('the disk in the dialog\'s sidebar opens it', shown('Saved'));
await press('Saved');
await clickText('Choose this folder', () => configured.length > 0);
check('a disk\'s folder becomes the destination', cfg.dest === `${DISK}/Saved`, JSON.stringify(cfg));
check('the dialog closed', !shown(backupEn.pickDest));

// ---- the exclusions ----
await clickText(backupEn.choose, () => shown(backupEn.pickExclude), 1);
check('Choose… opens the exclusion dialog', shown(backupEn.pickExclude));
await openRow('Videos', () => shown('Large'));
await press('Large');
await clickText('Choose this folder', () => cfg.exclude.length > 0);
check('a chosen folder is excluded, relative to home', JSON.stringify(cfg.exclude) === '["Videos/Large"]', JSON.stringify(cfg.exclude));
check('…and listed', shown('Videos/Large'));
check('the exclusion dialog closed', !shown(backupEn.pickExclude));

// typed: outside the home folder, the home folder itself, then one inside it
const key = async (name, chr = '') => { globalThis.__lordKey(0, 1, name, chr, (chr || name).toLowerCase(), 0); globalThis.__lordKey(0, 0, name, '', '', 0); await flush(); };
// a press on the field (its placeholder, or the text left in it) focuses it; that text is erased, then the keys
const typeAndAdd = async (s, left = '') => {
  await press(left || backupEn.excludePlaceholder);
  await key('End');
  for (let i = 0; i < left.length; i++) await key('Backspace');
  for (const ch of s) await key(ch, ch);
  await press(backupEn.add);
};
const before = configured.length;
check('the exclusion field', shown(backupEn.excludePlaceholder));
await typeAndAdd('/etc');
check('a typed folder outside home: the error', shown(backupEn.excludeOutside));
check('…and nothing stored', configured.length === before && cfg.exclude.length === 1, JSON.stringify(cfg.exclude));
await typeAndAdd('~', '/etc');
check('the home folder itself: its error', shown(backupEn.excludeHome) && !shown(backupEn.excludeOutside));
check('…and nothing stored either', configured.length === before && cfg.exclude.length === 1, JSON.stringify(cfg.exclude));
await typeAndAdd('~/Music', '~');
check('a typed folder in home is stored relative to it', JSON.stringify(cfg.exclude) === '["Videos/Large","Music"]', JSON.stringify(cfg.exclude));
check('…and the error is gone', !shown(backupEn.excludeHome) && !shown(backupEn.excludeOutside));

// ---- New Folder in the dialog (GitHub #262) ----
// the dialog opens on the destination (the disk's Saved); this harness has only the shell catalog, so the proposed name
// is its key, files.newFolder
const NEW = `${DISK}/Saved/files.newFolder`;
await clickText(backupEn.choose, () => shown(backupEn.pickDest), 0);
check('New Folder\'s button in a folder dialog', nodes('+').length > 0);
await clickText('+', () => shown('Create'), -1);
check('New Folder asks for a name', shown('Create') && shown('files.newFolder'));
await press('Create');
check('…and makes it in the folder in view', JSON.stringify(made) === JSON.stringify([NEW]), JSON.stringify(made));
check('…and opens it', !shown('Create') && shown('files.newFolder'));
await clickText('Choose this folder', () => cfg.dest === NEW);
check('the new folder can be chosen', cfg.dest === NEW, JSON.stringify(cfg));
process.exit(0);
