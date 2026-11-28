// navbuttons_test.mjs — a mouse's back and forward buttons in native windows (GitHub #234). runtime/events.js keeps
// buttons 3 and 4 away from onMouseDown, onClick and the 'pointerdown' listeners (#159) and turns the press into
// onNavigate along the chain under the pointer, then the 'navigate' listeners, with the window under the pointer;
// sdk/navmodel.js decides which window takes it and walks a window's history of places. Plain node (the repository's
// node_modules for react-reconciler). Run by tests/run_suites.sh.
import { navFor, visitNote, visitStep } from '../../sdk/navmodel.js';
import { createRequire } from 'node:module';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

let fails = 0, checks = 0;
const check = (name, ok, detail = '') => { checks++; if (!ok) { fails++; console.log(`FAIL ${name} ${detail}`); } };

globalThis.lord = { getLayout: () => [0, 0, 100, 100], now: () => Date.now(), input: {}, keyboard: {} };
// runtime/events.js as the theme gets it: bundled with esbuild (react-reconciler/constants is CommonJS)
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
let esbuild = null;
for (const p of [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)]) {
  try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ }
}
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }
const dir = mkdtempSync(path.join(tmpdir(), 'navbuttons-'));
const out = path.join(dir, 'events.mjs');
esbuild.buildSync({ entryPoints: [path.join(root, 'runtime', 'events.js')], bundle: true, format: 'esm', outfile: out, logLevel: 'error' });
const ev = await import(pathToFileURL(out).href);
rmSync(dir, { recursive: true, force: true });

// a window (lordWin 'w7') holding a row inside a list; a node outside every window
const win = { id: 1, parent: null, props: { lordWin: 'w7' } };
const list = { id: 2, parent: win, props: {} };
const row = { id: 3, parent: list, props: {} };
const bare = { id: 4, parent: null, props: {} };
const got = [];
row.props.onMouseDown = () => got.push('row.mousedown');
row.props.onClick = () => got.push('row.click');
list.props.onNavigate = (e) => got.push(`list.navigate:${e.direction}:${e.win}`);
for (const n of [win, list, row, bare]) ev.register(n);
const offDown = ev.addListener('pointerdown', (e) => got.push(`pointerdown:${e.button}`));
const offNav = ev.addListener('navigate', (e) => got.push(`navigate:${e.direction}:${e.win}`));

globalThis.__lordEvent('down', 3, 10, 10, 3);
globalThis.__lordEvent('up', 3, 10, 10, 3);
check('back over a row: onNavigate bubbles to the list, then the listeners', JSON.stringify(got) === '["list.navigate:back:w7","navigate:back:w7"]', JSON.stringify(got));
got.length = 0;
list.props.onNavigate = (e) => { got.push(`list.navigate:${e.direction}`); e.stopPropagation(); };
globalThis.__lordEvent('down', 3, 10, 10, 4);
globalThis.__lordEvent('up', 3, 10, 10, 4);
check('forward, taken by the list: the listeners don\'t hear it', JSON.stringify(got) === '["list.navigate:forward"]', JSON.stringify(got));
got.length = 0;
globalThis.__lordEvent('down', 4, 10, 10, 3);
check('outside every window: no window named', JSON.stringify(got) === '["navigate:back:null"]', JSON.stringify(got));
got.length = 0;
globalThis.__lordEvent('down', 3, 10, 10, 5);
globalThis.__lordEvent('up', 3, 10, 10, 5);
check('another extra button does nothing (#159)', got.length === 0, JSON.stringify(got));
globalThis.__lordEvent('down', 3, 10, 10, 0);
globalThis.__lordEvent('up', 3, 10, 10, 0);
check('the left button still presses and clicks', got.includes('row.mousedown') && got.includes('row.click') && got.includes('pointerdown:0') && !got.some((g) => g.startsWith('navigate')), JSON.stringify(got));
offDown(); offNav();

// which window takes it
const e = (direction, w) => ({ direction, win: w });
check('the desktop: the window under the pointer', navFor(e('back', 'w7'), 'w7', true) === 'back');
check('the desktop: not another window', navFor(e('back', 'w7'), 'w8', true) === null);
check('the desktop: none outside every window', navFor(e('forward', null), 'w7', true) === null);
check('an isolated app\'s process: its one window', navFor(e('forward', null), 'mail-1', false) === 'forward');
check('nothing but back or forward', navFor({ direction: 'up', win: 'w7' }, 'w7', true) === null && navFor(null, 'w7', true) === null);

// a window's history of places (Settings' sections, Help's articles)
let h = { back: [], fwd: [], cur: undefined };
h = visitNote(h, 'general');
check('the first place has nothing behind it', visitStep(h, 'back') === null && visitStep(h, 'forward') === null);
h = visitNote(visitNote(visitNote(h, 'network'), 'network'), 'sound');
check('places pile up, the same one once', JSON.stringify(h.back) === '["general","network"]' && h.cur === 'sound', JSON.stringify(h));
h = visitStep(h, 'back');
check('back goes to the place before', h.cur === 'network' && JSON.stringify(h.fwd) === '["sound"]', JSON.stringify(h));
h = visitNote(h, 'network');
check('showing the place it went to keeps forward', JSON.stringify(h.fwd) === '["sound"]');
h = visitStep(h, 'forward');
check('forward comes back', h.cur === 'sound' && !h.fwd.length && JSON.stringify(h.back) === '["general","network"]', JSON.stringify(h));
h = visitNote(visitStep(h, 'back'), 'display');
check('a new place drops what was ahead', h.cur === 'display' && !h.fwd.length, JSON.stringify(h));
let big = { back: [], fwd: [], cur: undefined };
for (let i = 0; i < 80; i++) big = visitNote(big, `p${i}`);
check('the history keeps the last 50', big.back.length === 50 && big.back[0] === 'p29', `${big.back.length} ${big.back[0]}`);

// each native app with a history of its own listens (Files' folders, the Store's app page, Mail's one-pane reading,
// Settings' sections, Help's articles)
const { readFileSync } = await import('node:fs');
const src = (app) => readFileSync(path.join(root, 'apps', app, 'index.jsx'), 'utf8');
check('Files walks its folder history', /useNavigateButtons\(\(d\) => goHistory\(d === 'back' \? -1 : 1\)\)/.test(src('files')));
check('the Store leaves and reopens an app\'s page', /useNavigateButtons\(/.test(src('store')) && /setApp\(null\)/.test(src('store')));
check('Mail goes back to the list in one pane', /useNavigateButtons\(\(d\) => \{ if \(single\) setOpened/.test(src('mail')));
check('Settings walks its sections and their sub-pages', /useVisitHistory\(placeOf\(section, sub\),/.test(src('settings')));
check('Help walks its articles', /useVisitHistory\(article, setArticle\)/.test(src('help')));

console.log(`navbuttons-test: ${checks} checks, ${fails} failures`);
process.exit(fails ? 1 : 0);
