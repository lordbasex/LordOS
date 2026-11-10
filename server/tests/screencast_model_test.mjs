// screencast_model_test.mjs — the ScreenCast chooser's choices (sdk/screencastmodel.js): what is offered for what the
// app asked, picking one or several, and what lord.portal.cast gets.
import { castChoices, castToggle, castSources, CAST_MAX } from '../../sdk/screencastmodel.js';

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

console.log(`screencast-model-test: ${ok}/${ok + bad}`);
process.exit(bad ? 1 : 0);
