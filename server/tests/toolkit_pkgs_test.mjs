// toolkit_pkgs_test.mjs — the toolkit audit's bindings and Qt's platform theme (docs/TOOLKIT-AUDIT.md, "Shipped"): the
// same packages in the ISO (tools/iso/build.sh PKGS_TOOLKITS), the Raspberry Pi image (tools/rpi/build-img.sh) and
// the lordos package's Recommends (tools/iso/mkdeb.sh TOOLKITRECS, which the Store's update installs on existing
// systems), each list really installed, and the platform theme lord-server names (server/src/wl/toolkitenv.h) among
// them. Run: node tests/toolkit_pkgs_test.mjs
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = (f) => readFileSync(path.join(repo, f), 'utf8');
let ok = 0, bad = 0;
const check = (name, c) => { if (c) ok++; else { bad++; console.log(`FAIL ${name}`); } };
const words = (s) => new Set((s || '').split(/[\s,]+/).filter(Boolean));
const same = (a, b) => a.size === b.size && [...a].every((x) => b.has(x));

const WANT = ['python3-gi', 'gir1.2-gtk-3.0', 'gir1.2-gtk-4.0', 'python3-pyqt5', 'python3-pyqt6', 'qtwayland5', 'qt6-wayland',
  'qgnomeplatform-qt5', 'qgnomeplatform-qt6'];
const iso = read('tools/iso/build.sh'), rpi = read('tools/rpi/build-img.sh'), mkdeb = read('tools/iso/mkdeb.sh');
const isoL = words((iso.match(/^PKGS_TOOLKITS="([^"]*)"/m) || [])[1]);
const rpiL = words((rpi.match(/^PKGS_TOOLKITS="([^"]*)"/m) || [])[1]);
const recL = words((mkdeb.match(/^TOOLKITRECS=", ([^"]*)"/m) || [])[1]);
for (const p of WANT) check(`${p} in the ISO's PKGS_TOOLKITS`, isoL.has(p));
check('the Raspberry Pi image has the ISO\'s list', same(isoL, rpiL));
check('the lordos package recommends the ISO\'s list', same(isoL, recL));
check('the ISO installs PKGS_TOOLKITS', /apt-get install [^\n]*\$PKGS_TOOLKITS/.test(iso));
check('the Raspberry Pi image installs PKGS_TOOLKITS', /apt-get install [^\n]*\$PKGS_TOOLKITS/.test(rpi));
check('the Recommends line carries TOOLKITRECS', /^Recommends: .*\$TOOLKITRECS/m.test(mkdeb));
// QT_QPA_PLATFORMTHEME=gnome is QGnomePlatform's plugin: both Qt versions' packages must be there
const theme = (read('server/src/wl/toolkitenv.h').match(/#define TOOLKITENV_QT_THEME "([^"]*)"/) || [])[1];
check('toolkitenv.h names the gnome platform theme', theme === 'gnome');
check('QGnomePlatform for Qt 5 and Qt 6 shipped with it', isoL.has('qgnomeplatform-qt5') && isoL.has('qgnomeplatform-qt6'));
// recorded for the dev VM (CLAUDE.md: every package installed there is listed)
const vm = read('docs/VM-SETUP.md');
for (const p of WANT) check(`${p} in docs/VM-SETUP.md`, vm.includes(p));
console.log(`toolkit-pkgs-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
