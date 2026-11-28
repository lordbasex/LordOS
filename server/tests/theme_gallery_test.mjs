// theme_gallery_test.mjs — Settings → Appearance's gallery (sdk/themegallery.js) and the theme.json fields it shows
// (sdk/thememeta.js: community, author, license, version, and (0.8.9.6.2) minLordOS and a community theme's own
// translated labels, namespace theme-<id>). Run: node tests/theme_gallery_test.mjs
import { cleanThemeMeta, themeAuthor, lordosAtLeast, themeNeedsNewer, themeNamespace, themeText, themeLabel, THEME_TEXT_MAX } from '../../sdk/thememeta.js';
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

// ---- minLordOS (0.8.9.6.2): the oldest LordOS release a theme runs on ----
eq('minLordOS kept', [cleanThemeMeta({ minLordOS: '0.8.9.6.2' }).minLordOS, cleanThemeMeta({ minLordOS: '1.0' }).minLordOS, cleanThemeMeta({}).minLordOS], ['0.8.9.6.2', '1.0', '']);
eq('minLordOS junk dropped', ['latest', '0.8.9.6.2-beta', '1', 'v1.0', '0.8.9.6.2 ', ' 0.9', '1.2.3.4.5.6.7', '123456.1', '1..2', 5, null, ['0.9']]
  .map((v) => cleanThemeMeta({ minLordOS: v }).minLordOS), ['', '', '', '', '', '', '', '', '', '', '', '']);
eq('lordosAtLeast', [lordosAtLeast('0.8.9.6.2', '0.8.9.6.2'), lordosAtLeast('0.8.9.6.1+ws155', '0.8.9.6.2'), lordosAtLeast('0.8.10', '0.8.9.6.2'),
  lordosAtLeast('0.8.9', '0.8.9.0'), lordosAtLeast('0.8.9', '0.8.9.1'), lordosAtLeast('0.9.0-beta', '0.9'), lordosAtLeast('1.0', '0.99.99')],
  [true, false, true, true, false, true, true]);
eq('lordosAtLeast: no or invalid minimum, unknown release', [lordosAtLeast('0.8.9', ''), lordosAtLeast('0.8.9', 'latest'), lordosAtLeast('', '9.9'),
  lordosAtLeast('dev', '9.9'), lordosAtLeast(undefined, '9.9')], [true, true, true, true, true]);
eq('themeNeedsNewer', [themeNeedsNewer({ minLordOS: '0.8.9.6.2' }, '0.8.9.6.1'), themeNeedsNewer({ minLordOS: '0.8.9.6.2' }, '0.8.9.6.2'),
  themeNeedsNewer({}, '0.1'), themeNeedsNewer(null, '0.1')], ['0.8.9.6.2', '', '', '']);
const needs = galleryEntry({ id: 'aurora', community: true, minLordOS: '0.9.1' }, { lordosVersion: '0.9.0' });
eq('gallery entry needs a newer LordOS', [needs.minLordOS, needs.needsLordOS], ['0.9.1', '0.9.1']);
eq('gallery entry runs here', galleryEntry({ id: 'aurora', minLordOS: '0.9.0' }, { lordosVersion: '0.9.0' }).needsLordOS, '');

// ---- a community theme's own texts (0.8.9.6.2): namespace theme-<id>, never another theme's ----
eq('namespace', [themeNamespace('aurora'), themeNamespace('a'.repeat(26)), themeNamespace('a'.repeat(27)), themeNamespace('x.y'), themeNamespace('../x'), themeNamespace('')],
  ['theme-aurora', `theme-${'a'.repeat(26)}`, '', '', '', '']);
const own = { options: [
  { key: 'glass', label: 'theme-aurora.opt.glass', hint: 'theme-aurora.opt.glassHint', toggle: true },
  { key: 'bar', label: 'theme-aurora.opt.bar', choices: [['top', 'theme-aurora.bar.top'], ['bottom', 'shell.x.bottom'], ['side', 'theme-other.bar.side']] },
  { key: 'stolen', label: 'theme-other.opt.x', toggle: true },
  { key: 'prefix', label: 'theme-aurora2.opt.x', toggle: true },
  { key: 'long', label: `theme-aurora.${'x'.repeat(81)}`, toggle: true },
  { key: 'dots', label: 'theme-aurora.', toggle: true },
  { key: 'hintOther', label: 'shell.ok', hint: 'theme-other.h', toggle: true },
] };
const kept = cleanThemeMeta(own, 'aurora').options;
eq('own labels kept', kept.map((o) => o.key), ['glass', 'bar', 'hintOther']);
eq('own hint and choices kept, another theme\'s dropped', [kept[0].hint, kept[1].choices, kept[2].hint], ['theme-aurora.opt.glassHint', [['top', 'theme-aurora.bar.top'], ['bottom', 'shell.x.bottom']], undefined]);
eq('own labels dropped for another id', cleanThemeMeta(own, 'other').options.map((o) => o.key), ['stolen', 'hintOther']);
eq('own labels dropped without an id', cleanThemeMeta(own).options.map((o) => o.key), ['hintOther']);
eq('shell labels still work', cleanThemeMeta({ options: [{ key: 'a', label: 'shell.a', toggle: true }] }, 'aurora').options.length, 1);
eq('themeText', [themeText('  Glass\n  bar ', 20), themeText('a‮b', 20), themeText('x'.repeat(30), 10), themeText(5, 10), themeText('😀'.repeat(5), 3)],
  ['Glass bar', 'a b', `${'x'.repeat(9)}…`, '', '😀😀…']);
const fake = (k) => ({ 'theme-aurora.opt.glass': `Glass${'!'.repeat(100)}`, 'shell.long': 'L'.repeat(100), 'theme-aurora.h': 'a\nb' })[k] ?? k;
eq('themeLabel cuts a theme\'s own text, never LordOS\'s', [[...themeLabel(fake, 'theme-aurora.opt.glass')].length, themeLabel(fake, 'shell.long').length, themeLabel(fake, 'theme-aurora.h', 'hint')],
  [THEME_TEXT_MAX.label, 100, 'a b']);

// ---- entries ----
const raw = [
  { id: 'classic', name: 'Classic', description: 'A centered taskbar with a start menu.' },
  { id: 'default', name: 'Flow', description: 'Menu bar, magnifying dock.', current: true },
  { id: 'workspaces', name: 'Workspaces', description: 'Activities with their own apps.' },
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
