// store_installed_test.mjs — the Store's installed apps by package system (apps/store/installedmodel.js): each
// launcher entry's system, the general Installed page's filters (All, each system with apps, Deactivated) and what
// each one keeps, and the second column's "Installed" value.
//
//   node tests/store_installed_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../apps/store/installedmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const apps = [
  { id: 'files', kind: 'lordos' }, { id: 'notes', kind: 'lordos', off: true }, { id: 'game', kind: 'package' },
  { id: 'desktop:gimp', kind: 'linux' }, { id: 'desktop:vlc', kind: 'linux' },
  { id: 'flatpak:org.libreoffice.LibreOffice', kind: 'flatpak' },
  { id: 'web:telegram', kind: 'web' }, { id: 'web:whatsapp', kind: 'web' }, { id: 'odd', kind: 'other' },
];

eq('lordos app', M.systemOf(apps[0]), 'lordos');
eq('installed .lpk', M.systemOf(apps[2]), 'lordos');
eq('linux app', M.systemOf(apps[3]), 'debian');
eq('flatpak app', M.systemOf(apps[5]), 'flathub');
eq('web app', M.systemOf(apps[6]), 'web');
eq('unknown kind', M.systemOf(apps[8]), null);
eq('no app', M.systemOf(null), null);

eq('filters: all, the systems with apps in order, deactivated', M.installedFilters(apps),
  [['all', 9], ['lordos', 3], ['debian', 2], ['flathub', 1], ['web', 2], ['off', 1]]);
const noWeb = apps.filter((a) => a.kind !== 'web');
eq('a system without apps has no filter', M.installedFilters(noWeb).map(([k]) => k), ['all', 'lordos', 'debian', 'flathub', 'off']);
eq('the chosen one stays, empty', M.installedFilters(noWeb, 'web').map(([k, c]) => `${k}=${c}`), ['all=7', 'lordos=3', 'debian=2', 'flathub=1', 'web=0', 'off=1']);
eq('deactivated is always there', M.installedFilters([]), [['all', 0], ['off', 0]]);

const ids = (l) => l.map((a) => a.id);
eq('all keeps every app', M.filterInstalled(apps, 'all').length, apps.length);
eq('lordos', ids(M.filterInstalled(apps, 'lordos')), ['files', 'notes', 'game']);
eq('debian', ids(M.filterInstalled(apps, 'debian')), ['desktop:gimp', 'desktop:vlc']);
eq('flathub', ids(M.filterInstalled(apps, 'flathub')), ['flatpak:org.libreoffice.LibreOffice']);
eq('web', ids(M.filterInstalled(apps, 'web')), ['web:telegram', 'web:whatsapp']);
eq('off', ids(M.filterInstalled(apps, 'off')), ['notes']);
eq('a wrong filter keeps all', M.filterInstalled(apps, 'nope').length, apps.length);

eq('filterOk keeps the known ones', ['all', 'off', 'lordos', 'debian', 'flathub', 'web'].map(M.filterOk), ['all', 'off', 'lordos', 'debian', 'flathub', 'web']);
eq('filterOk: anything else is all', [undefined, '', 'x', 'installed'].map(M.filterOk), ['all', 'all', 'all', 'all']);

eq('the second column value', [M.isMine(M.MINE), M.isMine(null), M.isMine('Graphics')], [true, false, false]);
eq('no category is called like it', ['AudioVideo', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office', 'Science', 'Settings', 'System', 'Utility']
  .includes(M.MINE), false);
eq('origins', ['debian', 'flathub', 'web', 'lordos'].map(M.originOf), ['src:debian', 'src:flathub', 'src:web', null]);

console.log(`store-installed-test: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
