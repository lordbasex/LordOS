// photos_places_test.mjs — Photos' cloud folders (apps/photos/places.js, GitHub #89): only folders kept on this computer,
// a kept folder inside another listed once, accounts in alphabetical order.
import { keptPlaces, inPlaces } from '../../apps/photos/places.js';

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${what}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } };
const M = '/home/u/Cloud';
const accounts = [
  { id: 'g', name: 'Google', mount: `${M}/Google`, kept: [`${M}/Google`, `${M}/Google/Photos`] },
  { id: 'p', name: 'pCloud perso', mount: `${M}/pCloud perso`, kept: [`${M}/pCloud perso/Pictures/2026`, `${M}/pCloud perso/Docs`] },
  { id: 'd', name: 'drive', mount: `${M}/drive`, kept: [] },
  { id: 'x', name: 'odd', mount: `${M}/odd`, kept: ['/etc', `${M}/odd-other/x`] },
];
const pl = keptPlaces(accounts);
eq('one row per kept folder, nested ones once', pl.map((p) => p.label), ['Google', 'pCloud perso · Docs', 'pCloud perso · Pictures/2026']);
eq('nothing for an account with nothing kept, nor paths outside the account', pl.some((p) => p.account === 'drive' || p.account === 'odd'), false);
eq('inside a place', [inPlaces(pl, `${M}/Google/a.jpg`), inPlaces(pl, `${M}/pCloud perso/Pictures/2025/b.jpg`)], [true, false]);
eq('no state', keptPlaces(undefined), []);
console.log(`photos_places_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
