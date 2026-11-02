// brand-check.mjs — keeps third-party brand and product names out of LordOS's public text.
//
//   node tools/brand-check.mjs           (also run by tools/i18n-check.mjs)
//
// Scans the user-facing text: assets/locales/*/*.json, the Help articles (assets/help), the app
// manifests and theme.json files, the language packs in docs/packs, README.md and CHANGELOG.md.
// LordOS uses its own names for its features (Search, Apps, Night Light, Quick Preview, Backups,
// Files…) and generic wording for comparisons ("other operating systems"). Exits with code 1 if
// a name shows up outside the ALLOW list below.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));

// Names that must not appear (word boundaries, case-sensitive: "windows" as in app windows is fine).
const BRANDS = [
  'Apple', 'macOS', 'Mac OS', 'OS X', 'Mac', 'MacBook', 'iMac', 'iPhone', 'iPad', 'iOS', 'iCloud', 'Safari',
  'Xcode', 'Finder', 'Spotlight', 'Launchpad', 'Night Shift', 'Quick Look', 'Time Machine', 'AirPrint',
  'AirDrop', 'Mission Control', 'Disk Utility', 'TCC', 'Windows', 'Microsoft', 'Outlook', 'Hotmail',
  'Little Snitch', 'iTerm', 'iTerm2', 'Parallels', 'KeePass', 'KeePassXC', '1Password', 'Bitwarden', 'LastPass', 'Have I Been Pwned',
];
const BRAND_RE = new RegExp(`(?<![\\w.-])(?:${BRANDS.map((b) => b.replace(/ /g, '\\s+')).join('|')})(?![\\w])`, 'g');

// Allowed occurrences: [file pattern, text that may contain the name, why].
const ALLOW = [
  [/(^|\/)shell\.json$/, 'Show All Windows', 'app windows, not the OS'],
  [/^assets\/help\/[a-z]+\/windows\.md$/, /^(title: Windows|# Windows)$/m, 'the Help article about app windows'],
  [/^assets\/help\//, '[Windows](help:windows)', 'link to the Help article about app windows'],
  [/^assets\/locales\/fr\/(countries|emojikw)\.json$/, 'Macédoine', 'the country in French: the check reads "Mac" before an é as a whole word'],
];

function files() {
  const out = [];
  const add = (p) => { if (fs.existsSync(path.join(root, p))) out.push(p); };
  const walk = (d, test) => {
    const abs = path.join(root, d);
    if (!fs.existsSync(abs)) return;
    for (const e of fs.readdirSync(abs, { withFileTypes: true })) {
      const rel = path.join(d, e.name);
      if (e.isDirectory()) { if (e.name !== 'node_modules') walk(rel, test); } else if (test(rel)) out.push(rel);
    }
  };
  walk('assets/locales', (p) => p.endsWith('.json'));
  walk('assets/help', (p) => p.endsWith('.md'));
  walk('apps', (p) => p.endsWith('/manifest.json'));
  walk('themes', (p) => p.endsWith('/theme.json'));
  walk('docs/packs', (p) => p.endsWith('.json'));
  add('README.md');
  add('CHANGELOG.md');
  return out;
}

function allowed(file, text, index, match) {
  for (const [fp, what] of ALLOW) {
    if (!fp.test(file)) continue;
    if (typeof what === 'string') {
      // the allowed phrase must cover this occurrence
      for (let i = text.indexOf(what); i >= 0; i = text.indexOf(what, i + 1)) {
        if (index >= i && index + match.length <= i + what.length) return true;
      }
    } else {
      const re = new RegExp(what.source, what.flags.includes('g') ? what.flags : what.flags + 'g');
      for (const m of text.matchAll(re)) if (index >= m.index && index + match.length <= m.index + m[0].length) return true;
    }
  }
  return false;
}

export function checkBrands({ quiet = false } = {}) {
  const problems = [];
  for (const file of files()) {
    const text = fs.readFileSync(path.join(root, file), 'utf8');
    for (const m of text.matchAll(BRAND_RE)) {
      if (allowed(file, text, m.index, m[0])) continue;
      const line = text.slice(0, m.index).split('\n').length;
      problems.push({ file, line, name: m[0] });
    }
  }
  if (!quiet) {
    for (const p of problems) console.log(`✗ brand name "${p.name}" in ${p.file}:${p.line}`);
    console.log(`brands: ${problems.length ? `${problems.length} problem(s)` : 'ok'}`);
  }
  return { problems };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exit(checkBrands().problems.length ? 1 : 0);
}
