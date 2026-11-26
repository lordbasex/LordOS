// theme_gallery_test.mjs — Settings → Appearance's gallery (sdk/themegallery.js) and the theme.json fields it shows
// (sdk/thememeta.js: community, author, license, version). Run: node tests/theme_gallery_test.mjs
import { cleanThemeMeta, themeAuthor } from '../../sdk/thememeta.js';
import { galleryEntry, foldText, matchesQuery, filterGallery, filterCounts, sortGallery, gridStep, gridColumns, themeIssueUrl, GALLERY_FILTERS } from '../../sdk/themegallery.js';
import { ISSUE_URL_MAX } from '../../sdk/cloudcheck.js';

let ok = 0, bad = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) ok++;
  else { bad++; console.log(`FAIL ${name}\n  got  ${g}\n  want ${w}`); }
}

// ---- theme.json fields ----
const m = cleanThemeMeta({ community: true, author: '  Ana   Pérez ', license: 'MIT OR Apache-2.0', version: '1.2.0-beta.1' });
eq('community fields', [m.community, m.author, m.license, m.version], [true, 'Ana Pérez', 'MIT OR Apache-2.0', '1.2.0-beta.1']);
eq('author pending is unknown', [themeAuthor('author pending'), themeAuthor('Author Pending'), themeAuthor(''), themeAuthor(5)], ['', '', '', '']);
eq('author with controls or bidi overrides', [themeAuthor('a‮b'), themeAuthor('a\nb'), themeAuthor('x'.repeat(81))], ['', '', '']);
// (0.8.9.6.2) the author's email and organization: one plain address, a one-line name; anything else is dropped
const mo = cleanThemeMeta({ author: 'Federico Pereira', email: ' fpereira@iperfex.com ', organization: 'IPERFEX' });
eq('email and organization', [mo.author, mo.email, mo.organization], ['Federico Pereira', 'fpereira@iperfex.com', 'IPERFEX']);
eq('bad emails', ['a@b', 'a b@c.com', 'a@@c.com', 'mailto:a@c.com', `${'a'.repeat(110)}@c.com`, 'a@c.com\n', 'a@-c.com', 5].map((v) => cleanThemeMeta({ email: v }).email), ['', '', '', '', '', '', '', '']);
eq('bad organizations', ['x'.repeat(61), 'a\nb', 'a\u202eb', 7].map((v) => cleanThemeMeta({ organization: v }).organization), ['', '', '', '']);
eq('junk license and version', [cleanThemeMeta({ license: 'MIT; rm -rf' }).license, cleanThemeMeta({ version: 'v1' }).version, cleanThemeMeta({ version: '1.2.3.4.5' }).version], ['', '', '']);
eq('community must be true', [cleanThemeMeta({ community: 'yes' }).community, cleanThemeMeta({}).community], [false, false]);

// ---- entries ----
const raw = [
  { id: 'classic', name: 'Lord Classic', description: 'A centered taskbar with a start menu.' },
  { id: 'default', name: 'Lord Flow', description: 'Menu bar, magnifying dock.', current: true },
  { id: 'workspaces', name: 'Lord Workspaces', description: 'Activities with their own apps.' },
  { id: 'caelestia', name: 'Caelestia', description: 'A shell inspired by Caelestia.', community: true, author: '', version: '0.1.0', failed: true, failedWhy: 'TypeError: x is undefined\n    at /home/ana/x.js' },
  { id: 'aurora', name: 'Aurora', description: 'Soft colors.', community: true, author: 'Ana Pérez', license: 'GPL-3.0-or-later' },
];
const list = raw.map((t) => galleryEntry(t, { lordosVersion: '0.9.0' }));
const by = (id) => list.find((e) => e.id === id);
eq('official entry', [by('default').official, by('default').author, by('default').license, by('default').version], [true, 'LordOS', 'MIT', '0.9.0']);
eq('no email or organization unless given', [by('default').email, by('default').organization], ['', '']);
eq('community entry', [by('caelestia').community, by('caelestia').author, by('caelestia').license, by('caelestia').version], [true, '', '', '0.1.0']);
eq('failed entry', [by('caelestia').failed, by('aurora').failed], [true, false]);
eq('installed', list.every((e) => e.installed), true);

// ---- order, search, filters ----
eq('order', sortGallery(list).map((e) => e.id), ['default', 'classic', 'workspaces', 'aurora', 'caelestia']);
eq('all', filterGallery(list).map((e) => e.id), ['default', 'classic', 'workspaces', 'aurora', 'caelestia']);
eq('official', filterGallery(list, { filter: 'official' }).map((e) => e.id), ['default', 'classic', 'workspaces']);
eq('community', filterGallery(list, { filter: 'community' }).map((e) => e.id), ['aurora', 'caelestia']);
eq('installed filter', filterGallery(list, { filter: 'installed' }).length, 5);
eq('search by name, folded', filterGallery(list, { query: 'CLASSIC' }).map((e) => e.id), ['classic']);
eq('search without accents', filterGallery(list, { query: 'perez' }).map((e) => e.id), ['aurora']);
eq('search every word', filterGallery(list, { query: 'lord dock' }).map((e) => e.id), ['default']);
eq('search by author LordOS', filterGallery(list, { query: 'lordos', filter: 'community' }).length, 0);
eq('search and filter', filterGallery(list, { query: 'a', filter: 'community' }).map((e) => e.id), ['aurora', 'caelestia']);
eq('empty query', matchesQuery(by('aurora'), '   '), true);
eq('no match', filterGallery(list, { query: 'zzz' }), []);
eq('fold', foldText('Árbol Ñandú'), 'arbol nandu');
eq('counts', filterCounts(list), { all: 5, official: 3, community: 2, installed: 5 });
eq('counts with query', filterCounts(list, 'lord'), { all: 3, official: 3, community: 0, installed: 3 });
eq('filters', GALLERY_FILTERS, ['all', 'official', 'community', 'installed']);
eq('unknown filter is all', filterGallery(list, { filter: 'x' }).length, 5);

// ---- keyboard ----
eq('right', gridStep(0, 5, 3, 'ArrowRight'), 1);
eq('right in rtl', gridStep(1, 5, 3, 'ArrowRight', true), 0);
eq('down', gridStep(1, 5, 3, 'ArrowDown'), 4);
eq('down clamps', gridStep(2, 5, 3, 'ArrowDown'), 4);
eq('up clamps', gridStep(1, 5, 3, 'ArrowUp'), 0);
eq('home end', [gridStep(3, 5, 3, 'Home'), gridStep(0, 5, 3, 'End')], [0, 4]);
eq('none focused', gridStep(-1, 5, 3, 'ArrowDown'), 0);
eq('empty', gridStep(0, 0, 3, 'ArrowDown'), -1);
eq('columns', [gridColumns(600, 180, 12), gridColumns(100, 180, 12), gridColumns(756, 180, 12)], [3, 1, 4]);

// ---- report ----
const url = themeIssueUrl({ theme: by('caelestia'), fallback: by('default'), version: '0.9.0 · Beta' });
const qs = new URLSearchParams(url.split('?')[1]);
eq('issue template', [qs.get('template'), qs.get('version')], ['bug_report.yml', '0.9.0 · Beta']);
eq('issue title', qs.get('title'), 'Desktop style "caelestia" couldn\'t start');
eq('issue log', qs.get('logs'), 'theme: caelestia\nback on: default\nerror:\nTypeError: x is undefined\n    at /home/<user>/x.js');
eq('issue steps name a community theme', qs.get('steps').includes('community theme, version 0.1.0'), true);
const long = themeIssueUrl({ theme: { id: 'x', failedWhy: 'line of an error message that goes on\n'.repeat(200) }, fallback: { id: 'default' } });
eq('long log fits', [long.length <= ISSUE_URL_MAX, new URLSearchParams(long.split('?')[1]).get('logs').endsWith('[truncated]')], [true, true]);
eq('no characters launchUri refuses', /[!'()*]/.test(url), false);

console.log(`theme-gallery-test: ${ok} ok${bad ? `, ${bad} FAILED` : ''}`);
process.exit(bad ? 1 : 0);
