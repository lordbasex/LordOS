// themegallery.js — Settings → Appearance's gallery of desktop styles (themes plan, stage 1), as plain data: the entry
// shown for each installed theme, the search, the filters (All / Official / Community / Installed), the order, and the
// address of the issue form that Report opens for a theme that couldn't start. Tested by
// server/tests/theme_gallery_test.mjs. The theme.json fields are checked first by sdk/thememeta.js.
import { compareNames } from './collate.js';
import { themeNeedsNewer } from './thememeta.js';
import { encodeQuery, ISSUE_BASE, ISSUE_URL_MAX } from './cloudcheck.js';

export const GALLERY_FILTERS = ['all', 'official', 'community', 'installed'];

// One card. `t` is a theme from sdk/index.js themes.list() (id, name, description, current, failed, failedWhy and the
// checked theme.json fields). The official themes (no `community: true`) are LordOS's: their author is LordOS, their
// license the system's (MIT) and their version the release's. Every listed theme is installed (a store, stage 3, will
// list some that aren't).
export function galleryEntry(t, { lordosVersion = '' } = {}) {
  const community = t.community === true;
  return {
    id: t.id, name: t.name || t.id, description: t.description || '', current: !!t.current,
    community, official: !community, installed: t.installed !== false,
    author: t.author || (community ? '' : 'LordOS'),         // '' → "Unknown author"
    email: t.email || '', organization: t.organization || '', // (0.8.9.6.2) shown only when the theme gives them
    license: t.license || (community ? '' : 'MIT'),
    version: t.version || (community ? '' : String(lordosVersion || '')),
    failed: !!t.failed, failedWhy: typeof t.failedWhy === 'string' ? t.failedWhy : '',
    options: Array.isArray(t.options) ? t.options : [], sketch: t.sketch || null,
    // (0.8.9.6.2) theme.json's minLordOS, and the release it needs when this one is older ('' when it runs here)
    minLordOS: typeof t.minLordOS === 'string' ? t.minLordOS : '', needsLordOS: themeNeedsNewer(t, lordosVersion),
    // (0.8.9.6.2) installed from a file: confirmed by the person or not yet, and what it may read
    user: t.user === true, trusted: t.user === true ? t.trusted === true : true, caps: Array.isArray(t.caps) ? t.caps : [],
  };
}

// Lowercase without accents, so "estilo clasico" finds "Clásico".
export function foldText(s) {
  return String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
}

// Every word of the query is in the name, the description, the author or the id.
export function matchesQuery(e, query) {
  const words = foldText(query).split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const hay = foldText([e.name, e.description, e.author, e.id].join(' '));
  return words.every((w) => hay.includes(w));
}

export function matchesFilter(e, filter) {
  if (filter === 'official') return e.official;
  if (filter === 'community') return e.community;
  if (filter === 'installed') return e.installed;
  return true;
}

// The order of the cards: the official themes first (Lord Flow, then Lord Classic, then by name), then the community
// ones by name; the same order with any query or filter, so a card never jumps while typing.
const rank = (e) => (e.community ? 3 : e.id === 'default' ? 0 : e.id === 'classic' ? 1 : 2);
export function sortGallery(list) {
  return [...list].sort((a, b) => rank(a) - rank(b) || compareNames(a.name, b.name) || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
}

export function filterGallery(list, { query = '', filter = 'all' } = {}) {
  return sortGallery(list.filter((e) => matchesFilter(e, filter) && matchesQuery(e, query)));
}

// How many themes each filter shows (the counts beside the filter names), with the query applied.
export function filterCounts(list, query = '') {
  const shown = list.filter((e) => matchesQuery(e, query));
  return Object.fromEntries(GALLERY_FILTERS.map((f) => [f, shown.filter((e) => matchesFilter(e, f)).length]));
}

// Arrow keys over the grid: the index of the card to focus from `i` (`cols` cards per row), clamped to the list;
// left and right swap in a right-to-left interface.
export function gridStep(i, count, cols, key, rtl = false) {
  if (count <= 0) return -1;
  const c = Math.max(1, cols | 0);
  const cur = i < 0 ? 0 : Math.min(i, count - 1);
  const k = rtl && key === 'ArrowLeft' ? 'ArrowRight' : rtl && key === 'ArrowRight' ? 'ArrowLeft' : key;
  const next = k === 'ArrowRight' ? cur + 1 : k === 'ArrowLeft' ? cur - 1 : k === 'ArrowDown' ? cur + c : k === 'ArrowUp' ? cur - c
    : k === 'Home' ? 0 : k === 'End' ? count - 1 : cur;
  return i < 0 ? 0 : Math.max(0, Math.min(count - 1, next));
}

// How many cards fit in a row of `width` pixels (cards `card` wide, `gap` apart), at least one.
export function gridColumns(width, card, gap) {
  return Math.max(1, Math.floor((Math.max(0, width) + gap) / (card + gap)));
}

// The GitHub bug form (.github/ISSUE_TEMPLATE/bug_report.yml) filled in for a theme that couldn't start, under the
// length lord.apps.launchUri takes. The browser shows it; nothing is sent until the person submits it there. Written in
// English (issues are), with the theme's id and the error, never a home path.
const TRUNC = '\n[truncated]';
export function themeIssueUrl({ theme, fallback, version = '' } = {}) {
  const id = String((theme && theme.id) || '?').slice(0, 64);
  const back = String((fallback && fallback.id) || '?').slice(0, 64);
  const tag = theme && theme.community ? `community theme${theme.author ? ` by ${String(theme.author).slice(0, 80)}` : ''}` : 'official theme';
  const q = (k, v) => `&${k}=${encodeQuery(v)}`;
  const head = `${ISSUE_BASE}?template=bug_report.yml${q('labels', 'bug')}${q('title', `Desktop style "${id}" couldn't start`)}`
    + (version ? q('version', String(version).slice(0, 40)) : '')
    + q('steps', `1. Settings → Appearance → Desktop style\n2. Choose "${id}" (${tag}${theme && theme.version ? `, version ${String(theme.version).slice(0, 30)}` : ''})`)
    + q('expected', `The desktop style "${id}" starts.`)
    + q('actual', `It couldn't start and LordOS went back to "${back}".`);
  const why = String((theme && theme.failedWhy) || '').replace(/\/home\/[^/\s:"]+/g, '/home/<user>');
  let log = `theme: ${id}\nback on: ${back}\nerror:\n${why || '(none recorded)'}`;
  let url = head + q('logs', log);
  if (url.length <= ISSUE_URL_MAX) return url;
  const lines = log.split('\n');
  while (lines.length > 1) {
    lines.pop();
    log = lines.join('\n') + TRUNC;
    url = head + q('logs', log);
    if (url.length <= ISSUE_URL_MAX) return url;
  }
  return head;        // the head alone is always short (an id of 64 characters, an author of 80)
}
