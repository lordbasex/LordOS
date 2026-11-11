// mem_figure_test.mjs — memory in use reads the same everywhere (sdk/memfigure.js, GitHub #118): the
// Control Center used total − MemAvailable while Monitor, About and Settings used the server's memUsedKB, and the
// Cleaner panel showed what was available with no total.
import { readFileSync } from 'node:fs';
import { memUsedKB } from '../../sdk/memfigure.js';

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${what}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } };

// the reporter's machine: 15 GB, 1.7 GB in use, 13 GB available (the kernel's reserve is the difference)
const info = { memTotalKB: 15.4e6, memAvailableKB: 13.4e6, memUsedKB: 1.75e6 };
eq('the server figure wins over total − available', memUsedKB(info), 1.75e6);
eq('an older server without memUsedKB: total − available', memUsedKB({ memTotalKB: 8e6, memAvailableKB: 5e6 }), 3e6);
eq('memUsedKB of 0 is a reading, not a missing one', memUsedKB({ memTotalKB: 8e6, memAvailableKB: 5e6, memUsedKB: 0 }), 0);
eq('no reading: 0, never negative', memUsedKB({ memTotalKB: 0, memAvailableKB: 1 }), 0);
eq('no info: 0', memUsedKB(null), 0);

// every place that shows memory in use goes through memUsedKB, none works out its own figure
const root = new URL('../../', import.meta.url);
for (const f of ['themes/default/components/ControlCenter.jsx', 'sdk/cleanerstatus.jsx', 'apps/about/index.jsx', 'apps/settings/General.jsx']) {
  const src = readFileSync(new URL(f, root), 'utf8');
  eq(`${f} uses memUsedKB`, /memUsedKB\(/.test(src), true);
  eq(`${f} does not subtract MemAvailable itself`, /memAvailableKB/.test(src), false);
}
// the Cleaner panel says "used of total" for memory and disk, like its bars
const en = JSON.parse(readFileSync(new URL('assets/locales/en/cleaner.json', root), 'utf8'));
eq('the Cleaner panel names the total', /\{used\}.*\{total\}/.test(en['panel.usedOf']), true);

console.log(`mem_figure_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
