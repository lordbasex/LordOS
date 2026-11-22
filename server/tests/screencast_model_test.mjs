// screencast_model_test.mjs — the ScreenCast chooser's choices (sdk/screencastmodel.js): what is offered for what the
// app asked, picking one or several, and what lord.portal.cast gets.
import { castChoices, castToggle, castSources, castAskerName, castWhoName, castWindowOf, CAST_MAX, shotSource, shotDisplayAt, shotDragRect, shotArea, shotStamp,
  shotFreeName, shotPills, SHOT_PILL_MS } from '../../sdk/screencastmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const displays = [{ id: 51, name: 'Built-in', main: true }, { id: 77, name: '' }];
const windows = [
  { id: 'files', app: 'files', title: 'Home' },
  { id: 'linux#3', app: 'linux', title: 'A page', minimized: true },
  { id: 'screenshots', app: 'screenshots', instance: 'overlay1', title: '' },
  { id: 'linux#4', app: 'linux', title: 'Editor' },
];

// what is offered
const both = castChoices({ monitor: true, window: true }, displays, windows);
eq('screens: every display, in order', both.screens.map((s) => [s.key, s.display, s.index]), [['d:51', 51, 0], ['d:77', 77, 1]]);
eq('windows: front first, no minimized or overlay', both.windows.map((w) => w.key), ['w:linux#4', 'w:files']);
eq('only screens asked', castChoices({ monitor: true }, displays, windows).windows, []);
eq('only windows asked', castChoices({ window: true }, displays, windows).screens, []);
eq('nothing asked', castChoices(null, displays, windows), { screens: [], windows: [] });
eq('no lists', castChoices({ monitor: true, window: true }, null, null), { screens: [], windows: [] });

// picking
eq('one: a new pick replaces', castToggle(['d:51'], 'w:files', false), ['w:files']);
eq('one: a second click unpicks', castToggle(['d:51'], 'd:51', false), []);
eq('several: added in order', castToggle(['d:51'], 'w:files', true), ['d:51', 'w:files']);
eq('several: unpick keeps the rest', castToggle(['d:51', 'w:files'], 'd:51', true), ['w:files']);
const full = ['a', 'b', 'c', 'd'];
eq('several: at most CAST_MAX', castToggle(full, 'e', true), full);
eq('CAST_MAX is the helper\'s', CAST_MAX, 4);
eq('from nothing', castToggle(undefined, 'd:77', false), ['d:77']);

// what lord-server gets
const frames = { 'linux#4': { node: 812, also: [900, 901] }, files: { node: 0, also: [] } };
const frameOf = (id) => frames[id] || null;
eq('a screen and a window, in the order picked', castSources(['w:linux#4', 'd:77'], both, frameOf), [{ node: 812, also: [900, 901] }, { display: 77 }]);
eq('a window with no frame now is left out', castSources(['w:files'], both, frameOf), []);
eq('a key not offered is left out', castSources(['d:99', 'w:nope'], both, frameOf), []);
eq('nothing picked', castSources([], both, frameOf), []);
const many = { node: 5, also: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] };
eq('at most 8 sheets', castSources(['w:linux#4'], both, () => many)[0].also.length, 8);

// the asking app's name: from lord-server's verified identity only (a Flatpak app by its Flathub name, not "flatpak:<id>")
const look = { linux: (id) => (id === 'gimp' ? 'GIMP' : ''), flatpak: (id) => (id === 'org.chromium.Chromium' ? 'Chromium' : '') };
eq('asker: flatpak named, with its verified id', castAskerName('flatpak:org.chromium.Chromium', look), 'Chromium \u00b7 org.chromium.Chromium');
// an app's name is its own metadata: one calling itself "Firefox" still shows the id lord-server verified
eq('asker: a spoofed name keeps the real id', castAskerName('flatpak:org.evil.App', { flatpak: () => 'Firefox' }), 'Firefox \u00b7 org.evil.App');
eq('asker: flatpak unknown keeps its id', castAskerName('flatpak:org.example.App', look), 'org.example.App');
eq('asker: desktop named', castAskerName('desktop:gimp', look), 'GIMP');
eq('asker: desktop unknown', castAskerName('desktop:foo', look), 'foo');
eq('asker: lpk', castAskerName('lpk:com.x', look), 'com.x');
eq('asker: none', castAskerName(undefined, look), '');
// what lord-server tells from the kernel without a verified scope (2026-11-22): a machine, LordOS's parts
const more = { ...look, machine: (id) => (id === '1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2' ? 'Machines — Windows 11 Pro (prueba)' : ''),
  lordos: (id) => ({ lordos: 'LordOS', 'lordos:terminal': 'Terminal' })[id] || '' };
eq('asker: a machine', castAskerName('desktop:lordos-vm-1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2', more), 'Machines — Windows 11 Pro (prueba)');
eq('asker: a machine not known', castAskerName('desktop:lordos-vm-00000000-0000-0000-0000-000000000000', more), 'lordos-vm-00000000-0000-0000-0000-000000000000');
eq('asker: not a machine id', castAskerName('desktop:lordos-vm-XYZ', more), 'lordos-vm-XYZ');
eq('asker: the Terminal', castAskerName('lordos:terminal', more), 'Terminal');
eq('asker: LordOS', castAskerName('lordos', more), 'LordOS');
eq('asker: a component not known', castAskerName('lordos:nothing', more), 'lordos:nothing');
// the indicator's name of a share: the verified app, else what lord-server saw, else the executable
const wn = { app: (id) => castAskerName(id, { linux: (x) => (x === 'google-chrome' ? 'Google Chrome' : '') }), unknown: (exe) => (exe ? `An unknown program (${exe})` : 'An unknown program') };
eq('share: verified', castWhoName({ who: 'desktop:google-chrome', seen: 'desktop:x' }, wn), 'Google Chrome');
eq('share: Chrome\'s browser process out of its scope', castWhoName({ who: '', seen: 'desktop:google-chrome', exe: '' }, wn), 'Google Chrome');
eq('share: only its executable', castWhoName({ who: '', seen: '', exe: 'obs' }, wn), 'An unknown program (obs)');
eq('share: nothing known', castWhoName({}, wn), 'An unknown program');
eq('share: junk', castWhoName({ who: 3, seen: {}, exe: 5 }, wn), 'An unknown program');
// a window choice is named from the live window (its args), not the choice (which has none: "Linux" for all)
const live = [{ id: 'linux#8', app: 'linux', args: { appId: 'org.chromium.Chromium' } }];
eq('window of a choice', castWindowOf({ id: 'linux#8', title: 'x' }, live), live[0]);
eq('window gone: the choice', castWindowOf({ id: 'linux#9', title: 'x' }, live), { id: 'linux#9', title: 'x' });

// the Screenshot portal (approved 2026-11-22): one source, an area dragged on one display, the file's name, the pills
eq('shot: one source, the first picked', shotSource(['d:77', 'w:linux#4'], both, frameOf), { display: 77 });
eq('shot: a window', shotSource(['w:linux#4'], both, frameOf), { node: 812, also: [900, 901] });
eq('shot: nothing picked', shotSource([], both, frameOf), null);
eq('shot: a window gone meanwhile', shotSource(['w:files'], both, frameOf), null);
const ds = [{ id: 51, x: 0, y: 0, w: 1440, h: 900 }, { id: 77, x: 1440, y: 0, w: 1920, h: 1080 }];
eq('shot: the display under a point', shotDisplayAt(ds, 1500, 10)?.id, 77);
eq('shot: a gap between displays is none', shotDisplayAt(ds, 100, 1000), null);
eq('shot: a drag, any direction', shotDragRect({ x: 300, y: 200 }, 100, 50, ds[0]), { x: 100, y: 50, w: 200, h: 150 });
eq('shot: a drag stays on its display', shotDragRect({ x: 1400, y: 100 }, 1700, 120, ds[0]), { x: 1400, y: 100, w: 40, h: 20 });
eq('shot: the area lord.portal.shot gets, whole pixels outwards', shotArea({ x: 10.5, y: 20.2, w: 100.2, h: 50 }, ds[1]),
  { display: 77, area: { x: 10, y: 20, width: 101, height: 51 } });
eq('shot: a click is no area', shotArea({ x: 10, y: 10, w: 0, h: 0 }, ds[0]), null);
eq('shot: no display, no area', shotArea({ x: 10, y: 10, w: 5, h: 5 }, null), null);
eq('shot: the stamp', shotStamp(new Date(2026, 10, 22, 9, 5, 7)), '2026-11-22 09.05.07');
const taken = new Set(['/h/Pictures/Screenshots/Shot.png', '/h/Pictures/Screenshots/Shot (2).png']);
eq('shot: a free name, never over a file', shotFreeName('/h/Pictures/Screenshots', 'Shot', (p) => taken.has(p)), '/h/Pictures/Screenshots/Shot (3).png');
eq('shot: the first name when free', shotFreeName('/d', 'A', () => false), '/d/A.png');
eq('shot: pills last SHOT_PILL_MS', shotPills([{ key: 'a', at: 1000 }, { key: 'b', at: 1000 + SHOT_PILL_MS }], 1000 + SHOT_PILL_MS + 1).map((p) => p.key), ['b']);
eq('shot: no list', shotPills(null, 5), []);

console.log(`screencast-model-test: ${ok}/${ok + bad}`);
process.exit(bad ? 1 : 0);
