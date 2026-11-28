// locale_dates_test.mjs — no date or number is formatted by the JavaScript engine's own locale (BUG-25 of the ws170 QA:
// System Information showed "11/27/2026, 07:08:26 AM" in a Spanish interface). Dates go through the SDK's formatDate /
// formatTime and numbers through formatNumber (sdk/i18n.js), which follow the interface's language and region. Fails on
// any toLocaleString(), toLocaleDateString() or toLocaleTimeString() called without a locale in apps/, sdk/, themes/.
//   node tests/locale_dates_test.mjs   (from server/)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const found = [];
const walk = (d) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) { if (e.name !== 'node_modules' && !e.name.startsWith('.')) walk(p); continue; }
    if (!/\.(m?js|jsx)$/.test(e.name) || /bundle/.test(e.name)) continue;
    fs.readFileSync(p, 'utf8').split('\n').forEach((line, i) => {
      if (/\.toLocale(Date|Time)?String\(\s*(\)|undefined\b)/.test(line)) found.push(`${path.relative(root, p)}:${i + 1}`);
    });
  }
};
for (const d of ['apps', 'sdk', 'themes']) walk(path.join(root, d));
for (const f of found) console.log(`✗ toLocale…String() without a locale: ${f}`);
console.log(`locale-dates-test: ${found.length} found`);
process.exit(found.length ? 1 : 0);
