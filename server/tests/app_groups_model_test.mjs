// app_groups_model_test.mjs — Lord Classic's start menu by category (#210, sdk/appgroupsmodel.js): the group of a
// LordOS app, a web app, a Linux and a Flatpak app (.desktop Categories, main ones first, then additional ones, else
// Other), the groups with their counts sorted by their translated names with Other last, the remembered view, and
// the keyboard on the categories list (↑/↓, Home/End, Enter, → opens and ← closes, mirrored right to left).
//
//   node tests/app_groups_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/appgroupsmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- one app's group ----
eq('a LordOS app takes its manifest category', M.launcherGroup({ id: 'files', category: 'System' }), 'System');
eq('a web app is Internet (launcherItem gives Network)', M.launcherGroup({ id: 'web:x', kind: 'web', category: 'Network' }), 'Network');
eq('a manifest category wins over .desktop Categories', M.launcherGroup({ category: 'Office', categories: 'Game;' }), 'Office');
eq('an unknown manifest category falls to the .desktop list', M.launcherGroup({ category: 'Bogus', categories: 'Graphics;' }), 'Graphics');
eq('Linux: the first main category', M.launcherGroup({ categories: 'GTK;Development;IDE;' }), 'Development');
eq('Linux: main categories win over an earlier additional one', M.launcherGroup({ categories: 'TextEditor;Development;' }), 'Development');
eq('Linux: Audio is Sound & video', M.launcherGroup({ categories: 'Audio;Player;' }), 'AudioVideo');
eq('Linux: Video is Sound & video', M.launcherGroup({ categories: 'Video;' }), 'AudioVideo');
eq('Linux: Settings', M.launcherGroup({ categories: 'GNOME;GTK;Settings;DesktopSettings;' }), 'Settings');
eq('Linux: Settings listed before System stays Settings', M.launcherGroup({ categories: 'Settings;System;' }), 'Settings');
eq('Linux: an additional category alone (WebBrowser)', M.launcherGroup({ categories: 'WebBrowser;' }), 'Network');
eq('Linux: an additional category alone (TerminalEmulator)', M.launcherGroup({ categories: 'TerminalEmulator;' }), 'System');
eq('Linux: an additional category alone (Spreadsheet)', M.launcherGroup({ categories: 'Qt;Spreadsheet;' }), 'Office');
eq('Linux: an additional category alone (Emulator)', M.launcherGroup({ categories: 'Emulator;' }), 'Game');
eq('Linux: an additional category alone (Math)', M.launcherGroup({ categories: 'Math;' }), 'Science');
eq('Linux: spaces around the entries', M.launcherGroup({ categories: ' Game ; ArcadeGame ;' }), 'Game');
eq('Linux: no categories → Other', M.launcherGroup({ categories: '' }), 'Other');
eq('Linux: only desktop-specific ones → Other', M.launcherGroup({ categories: 'GTK;X-GNOME-Utilities;' }), 'Other');
eq('Flatpak: the same .desktop rules', M.launcherGroup({ id: 'flatpak:org.gimp.GIMP', kind: 'flatpak', categories: 'Graphics;2DGraphics;RasterGraphics;' }), 'Graphics');
eq('no inherited keys (toString is not a category)', M.launcherGroup({ categories: 'toString;constructor;' }), 'Other');
eq('nothing at all → Other', M.launcherGroup(null), 'Other');
eq('every group is known', M.APP_GROUPS.length, 12);

// ---- the groups ----
const apps = [
  { id: 'a', name: 'Alpha', category: 'Utility' },
  { id: 'b', name: 'Beta', categories: 'Game;' },
  { id: 'c', name: 'Gamma', categories: 'X-Unknown;' },
  { id: 'd', name: 'Delta', categories: 'Game;BoardGame;' },
  { id: 'e', name: 'Eps', category: 'System' },
];
const LABEL = { Utility: 'Utilities', Game: 'Games', System: 'System', Other: 'Other' };
const g = M.groupApps(apps, { label: (id) => LABEL[id] || id });
eq('only the groups that hold apps, by label, Other last', g.map((x) => x.id), ['Game', 'System', 'Utility', 'Other']);
eq('counts', g.map((x) => x.count), [2, 1, 1, 1]);
eq('apps keep the order given', g[0].apps.map((a) => a.id), ['b', 'd']);
// another language: the labels sort differently, Other still last even if its name sorts first
const ES = { Utility: 'Utilidades', Game: 'Juegos', System: 'Sistema', Other: 'Aaa otras' };
eq('sorted by the translated names, Other last', M.groupApps(apps, { label: (id) => ES[id] }).map((x) => x.id), ['Game', 'System', 'Utility', 'Other']);
const REV = { Utility: 'A', Game: 'C', System: 'B', Other: 'Z' };
eq('the compare function decides', M.groupApps(apps, { label: (id) => REV[id], compare: (a, b) => (a < b ? 1 : a > b ? -1 : 0) }).map((x) => x.id), ['Game', 'System', 'Utility', 'Other']);
eq('no apps → no groups', M.groupApps([]), []);

// ---- the remembered view ----
eq('categories is remembered', M.startView('categories'), 'categories');
eq('az is remembered', M.startView('az'), 'az');
eq('nothing saved → A–Z', M.startView(undefined), 'az');
eq('a bad value → A–Z', M.startView('grid'), 'az');

// ---- the rows ----
const closed = M.groupRows(g, new Set());
eq('closed: one row per group', closed.map((r) => `${r.kind}:${r.id}`), ['group:Game', 'group:System', 'group:Utility', 'group:Other']);
const opened = M.groupRows(g, new Set(['Game']));
eq('an open group lists its apps under it', opened.map((r) => (r.kind === 'group' ? r.id : r.app.id)), ['Game', 'b', 'd', 'System', 'Utility', 'Other']);
eq('the open flag', opened.map((r) => r.open ?? null), [true, null, null, false, false, false]);
eq('no set → all closed', M.groupRows(g).length, 4);

// ---- the keyboard ----
eq('↓ moves down', M.groupKey('ArrowDown', closed, 0), { sel: 1 });
eq('↓ stops at the end', M.groupKey('ArrowDown', closed, 3), { sel: 3 });
eq('↑ stops at the top', M.groupKey('ArrowUp', closed, 0), { sel: 0 });
eq('Home', M.groupKey('Home', opened, 4), { sel: 0 });
eq('End', M.groupKey('End', opened, 0), { sel: 5 });
eq('Enter on a group toggles it', M.groupKey('Enter', closed, 1), { sel: 1, toggle: 'System' });
eq('Enter on an app launches it', M.groupKey('Enter', opened, 2).launch.id, 'd');
eq('→ on a closed group opens it', M.groupKey('ArrowRight', closed, 0), { sel: 0, toggle: 'Game' });
eq('→ on an open group goes to its first app', M.groupKey('ArrowRight', opened, 0), { sel: 1 });
eq('← on an open group closes it', M.groupKey('ArrowLeft', opened, 0), { sel: 0, toggle: 'Game' });
eq('← on an app goes to its group', M.groupKey('ArrowLeft', opened, 2), { sel: 0 });
eq('← on a closed group stays', M.groupKey('ArrowLeft', closed, 2), { sel: 2 });
eq('right to left: ← opens', M.groupKey('ArrowLeft', closed, 0, true), { sel: 0, toggle: 'Game' });
eq('right to left: → closes', M.groupKey('ArrowRight', opened, 0, true), { sel: 0, toggle: 'Game' });
eq('right to left: → on an app goes to its group', M.groupKey('ArrowRight', opened, 1, true), { sel: 0 });
eq('a key it does not use', M.groupKey('a', closed, 0), null);
eq('no rows', M.groupKey('ArrowDown', [], 0), null);
eq('an index past the end is clamped', M.groupKey('ArrowUp', closed, 99), { sel: 2 });
// the A–Z list uses the same keys over app rows without a group
const az = apps.map((app) => ({ kind: 'app', app }));
eq('A–Z: ↓', M.groupKey('ArrowDown', az, 1), { sel: 2 });
eq('A–Z: ← stays', M.groupKey('ArrowLeft', az, 1), { sel: 1 });
eq('A–Z: Enter launches', M.groupKey('Enter', az, 4).launch.id, 'e');

// ---- the selection when the rows change ----
eq('a group closing keeps it selected', M.keepSelection(opened, closed, 0), 0);
eq('a group below the opened one moves down with it', M.keepSelection(closed, opened, 1), 3);
eq('an app whose group closed selects the group', M.keepSelection(opened, M.groupRows(g, new Set()), 2), 0);
eq('an app found again by id (the list was made again)', M.keepSelection(opened, M.groupRows(M.groupApps(apps.map((a) => ({ ...a })), { label: (id) => LABEL[id] }), new Set(['Game'])), 2), 2);
eq('nothing to keep → clamped', M.keepSelection([], closed, 9), 3);

console.log(`app-groups-model: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
