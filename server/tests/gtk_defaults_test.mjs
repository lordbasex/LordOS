// gtk_defaults_test.mjs — the window-button layout GTK apps get when the session can't give its own
// (#52; assets/gtk/90_lordos.gschema.override and settings.ini, installed by tools/iso/mkdeb.sh). The
// three places that name the default layout must agree: the schema override, settings.ini, the SDK's
// default (sdk/theme.js) and lord-server's portal (ui/portal.c). With glib-compile-schemas, gsettings
// and Debian's org.gnome.desktop.wm.preferences schema, the override is also compiled in a temporary
// folder and read back the way GLib reads it.
//
//   (cd server && node tests/gtk_defaults_test.mjs)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = (p) => fs.readFileSync(path.join(REPO, p), 'utf8');
let okN = 0, bad = 0;
function eq(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) { okN++; return; }
  bad++;
  console.log(`✗ ${name}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`);
}

const WANT = 'close,minimize,maximize:';
// a GTK layout: names left of the colon go on the left; only GTK's button names
const LAYOUT_RE = /^(?:(?:appmenu|menu|icon|minimize|maximize|close|spacer)(?:,(?=[a-z]))?)*:(?:(?:appmenu|menu|icon|minimize|maximize|close|spacer)(?:,(?=[a-z]))?)*$/;

const ov = read('assets/gtk/90_lordos.gschema.override');
const groups = {};
let group = null;
for (const raw of ov.split('\n')) {
  const line = raw.trim();
  if (!line || line.startsWith('#')) continue;
  const g = /^\[([a-z0-9.-]+)\]$/i.exec(line);
  if (g) { group = groups[g[1]] = {}; continue; }
  const kv = /^([a-z0-9-]+)\s*=\s*(.+)$/.exec(line);
  eq(`override line in a group and key=value: ${line}`, !!(group && kv), true);
  if (group && kv) group[kv[1]] = kv[2];
}
eq('override: only the window preferences group', Object.keys(groups), ['org.gnome.desktop.wm.preferences']);
const wm = groups['org.gnome.desktop.wm.preferences'] || {};
eq('override: button-layout only', Object.keys(wm), ['button-layout']);
eq('override: Lord Flow\'s layout', wm['button-layout'], `'${WANT}'`);

const ini = read('assets/gtk/settings.ini').split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'));
eq('settings.ini: one group and one key', ini, ['[Settings]', `gtk-decoration-layout=${WANT}`]);

const theme = /value\.gtkButtons \|\| '([^']*)'/.exec(read('sdk/theme.js'));
eq('sdk/theme.js default', theme && theme[1], WANT);
const portal = /buttons\[64\] = "([^"]*)"/.exec(read('server/src/ui/portal.c'));
eq('portal.c default', portal && portal[1], WANT);
const classic = /gtkButtons:\s*'([^']*)'/.exec(read('themes/classic/tokens.js'));
eq('Lord Classic asks for a valid layout', !!(classic && LAYOUT_RE.test(classic[1])), true);
eq('the default is a valid layout', LAYOUT_RE.test(WANT), true);

const mk = read('tools/iso/mkdeb.sh');
for (const dst of ['/usr/share/glib-2.0/schemas/90_lordos.gschema.override', '/etc/gtk-3.0/settings.ini', '/etc/gtk-4.0/settings.ini'])
  eq(`mkdeb.sh installs ${dst}`, mk.includes(`"$PKG${dst}"`), true);

// compiled the way GLib's dpkg trigger compiles it, next to Debian's schema
const SYS = '/usr/share/glib-2.0/schemas/org.gnome.desktop.wm.preferences.gschema.xml';
const ENUMS = '/usr/share/glib-2.0/schemas/org.gnome.desktop.enums.xml';   // the schema's enums
const has = (cmd) => { try { execFileSync('sh', ['-c', `command -v ${cmd}`], { stdio: 'ignore' }); return true; } catch { return false; } };
if (!fs.existsSync(SYS) || !fs.existsSync(ENUMS) || !has('glib-compile-schemas') || !has('gsettings')) {
  console.log('  skipped the compiled check: needs gsettings-desktop-schemas and libglib2.0-bin');
} else {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'gtk-defaults-'));
  try {
    for (const f of [SYS, ENUMS]) fs.copyFileSync(f, path.join(tmp, path.basename(f)));
    const env = { ...process.env, GSETTINGS_SCHEMA_DIR: tmp, GSETTINGS_BACKEND: 'memory' };
    const get = () => execFileSync('gsettings', ['get', 'org.gnome.desktop.wm.preferences', 'button-layout'], { env, encoding: 'utf8' }).trim();
    execFileSync('glib-compile-schemas', ['--strict', tmp], { stdio: 'pipe' });
    eq('without the override: Debian\'s lone close button', get(), "'appmenu:close'");
    fs.copyFileSync(path.join(REPO, 'assets/gtk/90_lordos.gschema.override'), path.join(tmp, '90_lordos.gschema.override'));
    const out = execFileSync('glib-compile-schemas', ['--strict', tmp], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    eq('the override compiles without warnings', out.trim(), '');
    eq('with the override: Lord Flow\'s layout', get(), `'${WANT}'`);
    // a value the user stored still wins (keyfile backend, as in a LordOS session)
    const kdir = path.join(tmp, 'cfg', 'glib-2.0', 'settings');
    fs.mkdirSync(kdir, { recursive: true });
    fs.writeFileSync(path.join(kdir, 'keyfile'), "[org/gnome/desktop/wm/preferences]\nbutton-layout=':minimize,maximize,close'\n");
    const user = execFileSync('gsettings', ['get', 'org.gnome.desktop.wm.preferences', 'button-layout'],
      { env: { ...env, GSETTINGS_BACKEND: 'keyfile', XDG_CONFIG_HOME: path.join(tmp, 'cfg') }, encoding: 'utf8' }).trim();
    eq('a stored value wins over the default', user, "':minimize,maximize,close'");
  } finally { fs.rmSync(tmp, { recursive: true, force: true }); }
}

console.log(`gtk-defaults-test: ${okN} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
