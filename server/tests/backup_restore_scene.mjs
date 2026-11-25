// backup_restore_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Backups → Backups, the
// restore browser (GitHub #231). A file's icon is a page with lines, not a bare outline that reads as an empty check
// box; a selected file with a long name gives the Restore button a label that ends with "…" inside the button's width
// (the label used to overflow the 260 px button and lose both ends), and the button restores that file. Prints
// "ok <name>" or "FAIL <name> <detail>". Several rows at once (#231): Ctrl adds a row, Shift picks a range, Restore sends
// the list, the progress names the item going, and each item that failed is named with its error.
import './battery_settings_scene_stubs.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import Backup from '../../apps/backup/index.jsx';
import { backupStatus } from '../../sdk/backup.js';
import backupEn from '../../assets/locales/en/backup.json';

const { texts, parents, removed, styles } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'backup' ? JSON.stringify(backupEn) : shellCatalog(lang, ns));

const LONG = 'lordos-rclone_1.75.2-1_amd64-with-a-much-longer-name.deb';
const restored = [];
let status = { state: 'idle', dest: '/media/u/USB', destAvailable: true, last: { when: 1 }, progress: {}, restore: {}, unlock: {} };
lord.backup = {
  config: () => ({ dest: '/media/u/USB', interval: 'hourly', pauseOnBattery: true, encrypt: false, exclude: [] }),
  configure: () => {},
  list: () => [{ id: 's1', time: 1763900000 }],
  ls: (id, rel) => (rel ? [] : [{ name: 'Soft', dir: true, size: 0, mtime: 1 }, { name: LONG, dir: false, size: 20e6, mtime: 1 },
    { name: 'notes.txt', dir: false, size: 10, mtime: 1 }, { name: 'Photos', dir: true, size: 0, mtime: 1 }]),
  restore: (id, rel, opts) => { restored.push([id, rel, !!opts.replace]); },
  status: () => status,
  path: () => '/media/u/USB/s1',
};

backupStatus.set(lord.backup.status());      // the store read lord.backup when the SDK loaded, before this stub
enableShellLayers();
rt.render(h(rt.View, { style: { width: 980, height: 640 } }, h(Backup)));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const nodes = (s) => [...texts].filter(([id, x]) => String(x) === s && inTree(id)).map(([id]) => id);
const shown = (s) => nodes(s).length > 0;
const click = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };
const clickText = async (s, done) => { let n = nodes(s)[0]; for (let i = 0; n && i < 5 && !done(); i++, n = parents.get(n)) await click(n); };
const kids = (p) => [...parents].filter(([c, q]) => q === p && inTree(c)).map(([c]) => c);
// the first node of a text's chain the runtime styles (a text's own node has none; its Text has)
const chainOf = (n) => { while (n && !styles.get(n)?.color) n = parents.get(n); return n; };

check('the backup lists its file', shown(LONG) && shown('Soft'));
// the row of the file: its name's Text → the row; the icon box is the row's first child
const row = parents.get(chainOf(nodes(LONG)[0]));
const iconBox = kids(row).sort((a, b) => a - b)[0];
const glyph = kids(iconBox)[0];
check('a file\'s icon has the lines of a page (#231)', kids(glyph).length === 3, `${kids(glyph).length}`);

await clickText(LONG, () => shown(backupEn.restoreItem.replace('{name}', LONG)));
const label = backupEn.restoreItem.replace('{name}', LONG);
check('selecting the file names it on Restore', shown(label));
const ls = styles.get(chainOf(nodes(label)[0])) || {};
check('a long name ends with … inside the button (#231)', ls.textOverflow === 'ellipsis' && ls.overflow === 'hidden' && ls.flexShrink === 1 && ls.minWidth === 0,
  JSON.stringify(ls));
await clickText(label, () => restored.length > 0);
check('Restore restores that file', JSON.stringify(restored) === `[["s1","${LONG}",false]]`, JSON.stringify(restored));

// ---- several rows (#231) ----
const keyMod = async (flags) => { globalThis.__lordKey(flags ? 29 : 29, flags ? 1 : 0, flags & 1 ? 'Shift' : 'Control', '', '', flags); await flush(); };
const rowOf = (name) => parents.get(chainOf(nodes(name)[0]));
const clickRow = async (name, flags = 0) => {
  if (flags) await keyMod(flags);
  const r = rowOf(name);
  __lordEvent('down', r, 2, 2, 0); __lordEvent('up', r, 2, 2, 0); await flush();
  if (flags) await keyMod(0);
};
const selLabel = (n) => backupEn.restoreSelected.replace('{count}', n);
await clickRow('Soft');
check('a plain click picks one row', shown(backupEn.restoreItem.replace('{name}', 'Soft')));
await clickRow('notes.txt', 2);
check('Ctrl adds a row: Restore names how many', shown(selLabel(2)));
await clickRow('Photos', 1);
check('Shift picks the range from the last row', shown(selLabel(2)));
// (never the same row twice in a row: that is a double click, which opens a folder)
await clickRow(LONG, 2);
check('Ctrl puts a row in', shown(selLabel(3)));
await clickRow('notes.txt', 2);
check('Ctrl takes a picked row out', shown(selLabel(2)));
await clickRow('Soft', 2);
await clickRow('notes.txt', 2);
await clickRow('Soft', 2);
check('Ctrl in and out again', shown(selLabel(3)));
restored.length = 0;
await clickText(selLabel(3), () => restored.length > 0);
check('Restore sends the list of the picked rows, in their order', JSON.stringify(restored) === `[["s1",["${LONG}","notes.txt","Photos"],false]]`, JSON.stringify(restored));
// progress, then a result with one item that failed
status = { ...status, state: 'restoring', restore: { done: false, id: 's1', count: 3, index: 2, current: 'notes.txt' } };
globalThis.__lordBackup(status);
await flush();
const going = backupEn.restoringItem.replace('{index}', 2).replace('{count}', 3).replace('{name}', 'notes.txt');
check('the progress names the item going', shown(going));
status = { ...status, state: 'idle', restore: { done: true, ok: false, id: 's1', files: 4, keptBoth: 0, replaced: 0, errors: 1, error: 'backup.badPath',
  count: 3, index: 3, current: '', failedCount: 1, failed: [{ rel: 'Photos', error: 'backup.badPath' }] } };
globalThis.__lordBackup(status);
await flush();
// (this stub has no errors catalog: the error shows as its code)
const failedLine = (e) => backupEn.restoreItemFailed.replace('{name}', 'Photos').replace('{error}', e);
check('the item that failed is named with its error', shown(failedLine('Invalid path')) || shown(failedLine('backup.badPath')),
  [...texts.values()].filter((x) => String(x).includes('Photos')).join(' | '));
check('the others are counted as restored', shown(backupEn.restoredInfo.other.replace('{count}', 4).replace('{both}', 0).replace('{replaced}', 0)));
process.exit(0);
