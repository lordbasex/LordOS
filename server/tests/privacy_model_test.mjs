// privacy_model_test.mjs — which apps Settings → Privacy lists under a permission and the choices each row offers
// (apps/settings/privacymodel.js; GitHub #219: Screenshots was not listed, so a denied capture could not be allowed).
//
//   node tests/privacy_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { privacyApps, permChoices } = await import(path.join(here, '../../apps/settings/privacymodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const shots = { id: 'screenshots', name: 'Screenshots', system: true, permissions: ['screen', 'microphone', 'fs:home:write'] };
const photos = { id: 'photos', name: 'Photos', system: true, permissions: ['files'] };
const pkgApp = { id: 'com.example.rec', name: 'Rec', system: false, package: true, permissions: ['screen'] };
const names = { screenshots: 'Screenshots', photos: 'Photos', 'com.example.rec': 'Rec', 'com.example.old': 'Old' };
const nameOf = (id) => names[id] || id;

eq('a built-in app that declares it is listed before it ever asked', privacyApps('screen', { systemApps: [shots, photos], nameOf }), [['screenshots', 'Screenshots']]);
eq('and under its other permissions too', privacyApps('microphone', { systemApps: [shots], nameOf }), [['screenshots', 'Screenshots']]);
eq('a manifest permission that is not a Privacy one is not a row', privacyApps('files', { systemApps: [shots], nameOf }), []);
eq('a package from the registry is not taken as built-in', privacyApps('screen', { systemApps: [pkgApp], nameOf }), []);
eq('a package declaring it is listed (from packages)', privacyApps('screen', { packages: [{ id: 'com.example.rec', name: 'Rec', permissions: ['screen'] }], nameOf }), [['com.example.rec', 'Rec']]);
eq('a saved decision is listed', privacyApps('screen', { decisions: [{ app: 'com.example.old', perm: 'screen', state: 'deny' }], nameOf }), [['com.example.old', 'Old']]);
eq('one row per app: declared and decided', privacyApps('screen', { systemApps: [shots], decisions: [{ app: 'screenshots', perm: 'screen', state: 'deny' }], nameOf }), [['screenshots', 'Screenshots']]);
eq('a decision the page does not list stays out', privacyApps('screen', { decisions: [{ app: 'x', perm: 'screen' }], listed: () => false, nameOf }), []);
eq('another permission\'s decision stays out', privacyApps('screen', { decisions: [{ app: 'photos', perm: 'camera' }], nameOf }), []);
eq('nothing: nothing', privacyApps('screen'), []);

eq('a permission that starts allowed: Allow or Don\'t Allow', permChoices('allow'), ['allow', 'deny']);
eq('any other: Allow, Ask, Don\'t Allow', permChoices('ask'), ['allow', 'ask', 'deny']);
eq('no answer: the three', permChoices(undefined), ['allow', 'ask', 'deny']);

console.log(bad ? `privacy-model: ${bad} of ${n} failed` : `privacy-model: ${n} checks passed`);
process.exit(bad ? 1 : 0);
