// browser_profiles_test.mjs — a Chromium-family browser's profiles read from its own Local State as untrusted data, and
// the profile an activity or a display picks (sdk/browserprofilemodel.js). node server/tests/browser_profiles_test.mjs
import { parseLocalState, skColor, windowProfile, profileChoice, cleanProfileChoices, cleanDisplayProfiles, LOCAL_STATE_MAX, MAX_PROFILES, PROFILE_NAME_MAX, isProfileBrowser } from '../../sdk/browserprofilemodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
const C = 'desktop:google-chrome';

const ls = (cache) => JSON.stringify({ profile: { info_cache: cache, last_used: 'Profile 1' } });
eq('profiles in the browser\'s order', parseLocalState(ls({ 'Profile 2': { name: 'Dev', profile_highlight_color: -16744320 }, Default: { name: 'Personal', default_avatar_fill_color: -14776091 } })),
  [{ dir: 'Default', name: 'Personal', color: '#1e88e5' }, { dir: 'Profile 2', name: 'Dev', color: '#008080' }]);
eq('skColor: ARGB ints only', [skColor(-1), skColor(0xff102030), skColor('#fff'), skColor(1.5), skColor(2 ** 40)], ['#ffffff', '#102030', null, null, null]);
// hostile Local State
eq('not JSON', parseLocalState('{"profile":'), []);
eq('wrong shape', [parseLocalState('[]'), parseLocalState('{"profile":{"info_cache":[1,2]}}'), parseLocalState('{"profile":{"info_cache":"x"}}')], [[], [], []]);
eq('too big', parseLocalState(' '.repeat(LOCAL_STATE_MAX + 1)), []);
eq('folder names: only the browser\'s own form, never a path', parseLocalState(ls({ '../../.ssh': { name: 'x' }, '/etc': { name: 'y' }, 'Profile 0': {}, 'Profile 1;rm': {}, 'Profile 12': { name: 'ok' } })).map((p) => p.dir), ['Profile 12']);
eq('names cleaned and cut', parseLocalState(ls({ Default: { name: `A‮B\n${'z'.repeat(200)}` } }))[0].name.length <= PROFILE_NAME_MAX, true);
eq('names: bidi override dropped', parseLocalState(ls({ Default: { name: 'Work‮exe.' } }))[0].name, 'Work exe.');
eq('a name that is not a string: the folder', parseLocalState(ls({ Default: { name: { toString: 1 } } }))[0].name, 'Default');
const many = {};
for (let i = 1; i <= 100; i++) many[`Profile ${i}`] = { name: `P${i}` };
eq('at most MAX_PROFILES', parseLocalState(ls(many)).length, MAX_PROFILES);
eq('__proto__ in the cache is ignored', parseLocalState('{"profile":{"info_cache":{"__proto__":{"name":"x"},"Default":{"name":"D"}}}}').map((p) => p.dir), ['Default']);
// which window is which: only what LordOS launched, never the title
const profs = { [C]: parseLocalState(ls({ Default: { name: 'Personal' }, 'Profile 1': { name: 'Work' } })) };
const w = { id: 'linux#5', title: 'Inbox - Google Chrome - Work' };
eq('title never decides', windowProfile(w, C, profs, new Map()), null);
eq('a launched window knows its profile', windowProfile(w, C, profs, new Map([['linux#5', 'Profile 1']]))?.name, 'Work');
eq('not a browser', windowProfile(w, 'desktop:gedit', profs, new Map([['linux#5', 'Profile 1']])), null);
eq('a bad folder in the map', windowProfile(w, C, profs, new Map([['linux#5', '../x']])), null);
// the choice: the activity first, then the display by connector
const prefs = { displays: { 'DP-3': { [C]: 'Profile 2' }, 'bad connector!': { [C]: 'Profile 3' } } };
eq('activity first', profileChoice(C, { [C]: 'Profile 1' }, prefs, 'DP-3'), 'Profile 1');
eq('display as fallback', profileChoice(C, {}, prefs, 'DP-3'), 'Profile 2');
eq('neither', profileChoice(C, {}, prefs, 'HDMI-A-1'), null);
eq('a bad connector is dropped', cleanDisplayProfiles(prefs), { displays: { 'DP-3': { [C]: 'Profile 2' } } });
eq('choices: browsers and folder names only', cleanProfileChoices({ [C]: '../../x', 'desktop:gedit': 'Default', 'desktop:chromium': 'Default' }), { 'desktop:chromium': 'Default' });
eq('known browsers', [isProfileBrowser(C), isProfileBrowser('desktop:firefox-esr'), isProfileBrowser('__proto__')], [true, false, false]);

console.log(`browser-profiles: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
