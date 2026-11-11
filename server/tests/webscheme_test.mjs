// webscheme_test.mjs — the script lord-web adds to every frame so a web app's matchMedia answers prefers-color-scheme
// with the desktop's scheme (server/src/web/webscheme.js, docs/WEBAPPS.md "Light and dark"), run against a small
// stand-in for WebKit's matchMedia: an engine that always answers light (WPE 2.48), media queries with not, and,
// only, commas, (prefers-color-scheme[: light|dark]) and (min-width: Npx) on a width the test changes.
//
//   node tests/webscheme_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const SRC = fs.readFileSync(path.join(here, '..', 'src/web/webscheme.js'), 'utf8');
const KEY = '__lordosScheme' + '0123456789abcdef0123456789abcdef';

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- the engine: a tiny media query evaluator, the real MediaQueryList with its brand check ----
function makeEngine({ engineDark = false } = {}) {
  const env = { width: 800, dark: engineDark };
  const brand = new WeakMap();
  const lists = new Set();
  function feature(f) {
    const m = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+?)\s*)?\)$/i.exec(f.trim());
    if (!m) return null;
    const name = m[1].toLowerCase(), v = m[2] && m[2].toLowerCase();
    if (name === 'prefers-color-scheme') return v === undefined ? true : v === 'dark' ? env.dark : v === 'light' ? !env.dark : null;
    if (name === 'min-width') return env.width >= parseInt(v, 10);
    if (name === 'max-width') return env.width <= parseInt(v, 10);
    return null;
  }
  function one(q) {
    let words = q.trim().toLowerCase().replace(/\s+/g, ' ');
    let neg = false;
    if (words.startsWith('not ')) { neg = true; words = words.slice(4); }
    if (words.startsWith('only ')) words = words.slice(5);
    const parts = words.split(/ and /);
    let r = true;
    for (const p of parts) {
      const t = p.trim();
      let v;
      if (t === 'all' || t === 'screen') v = true;
      else if (t === 'print') v = false;
      else v = feature(t);
      if (v === null) return false;            // not valid: "not all"
      r = r && v;
    }
    return neg ? !r : r;
  }
  const evaluate = (q) => q.split(',').some((x) => one(x));
  class MediaQueryList extends EventTarget {
    constructor() { super(); throw new TypeError('Illegal constructor'); }
    get matches() { if (!brand.has(this)) throw new TypeError('Illegal invocation'); return evaluate(brand.get(this).q); }
    get media() { if (!brand.has(this)) throw new TypeError('Illegal invocation'); return brand.get(this).q.trim().replace(/\s+/g, ' ').toLowerCase(); }
    addListener(l) { if (!brand.has(this)) throw new TypeError('Illegal invocation'); this.addEventListener('change', l); }
    removeListener(l) { if (!brand.has(this)) throw new TypeError('Illegal invocation'); this.removeEventListener('change', l); }
  }
  class MediaQueryListEvent extends Event {
    constructor(type, init = {}) { super(type); this.media = init.media || ''; this.matches = !!init.matches; }
  }
  function matchMedia(q) {
    if (arguments.length < 1) throw new TypeError('Not enough arguments');
    const l = new EventTarget();
    Object.setPrototypeOf(l, MediaQueryList.prototype);
    brand.set(l, { q: String(q), last: evaluate(String(q)) });
    lists.add(l);
    return l;
  }
  function setWidth(w) {
    env.width = w;
    for (const l of lists) {
      const b = brand.get(l), m = evaluate(b.q);
      if (m !== b.last) { b.last = m; l.dispatchEvent(new MediaQueryListEvent('change', { media: l.media, matches: m })); }
    }
  }
  const win = { matchMedia, MediaQueryList, MediaQueryListEvent, EventTarget, Event, frames: [] };
  return { win, setWidth, isReal: (x) => brand.has(x) };
}

function inject(win, dark) {
  // the same call webscheme_script makes, with window being this frame's
  new Function('window', `${SRC}(${JSON.stringify(KEY)}, ${dark ? 'true' : 'false'});`)(win);
}

// ---- answers ----
{
  const { win, isReal } = makeEngine();
  inject(win, false);
  const mm = win.matchMedia;
  eq('light: dark query', mm('(prefers-color-scheme: dark)').matches, false);
  eq('light: light query', mm('(prefers-color-scheme: light)').matches, true);
  eq('function name and length', [mm.name, mm.length], ['matchMedia', 1]);
  const width = mm('(min-width: 600px)');
  eq('other queries get the real list', isReal(width), true);
  eq('real list answers', width.matches, true);
  win[KEY](true);
  eq('dark: dark query', mm('(prefers-color-scheme: dark)').matches, true);
  eq('dark: light query', mm('(prefers-color-scheme: light)').matches, false);
  eq('dark: spacing and case', mm('( PREFERS-COLOR-SCHEME :  Dark )').matches, true);
  eq('dark: not', mm('not all and (prefers-color-scheme: dark)').matches, false);
  eq('dark: not light', mm('not all and (prefers-color-scheme: light)').matches, true);
  eq('dark: screen and', mm('screen and (prefers-color-scheme: dark)').matches, true);
  eq('dark: only screen and', mm('only screen and (prefers-color-scheme: dark)').matches, true);
  eq('dark: print and', mm('print and (prefers-color-scheme: dark)').matches, false);
  eq('dark: and width (true)', mm('(prefers-color-scheme: dark) and (min-width: 600px)').matches, true);
  eq('dark: and width (false)', mm('(prefers-color-scheme: dark) and (min-width: 1600px)').matches, false);
  eq('dark: list, one matches', mm('(prefers-color-scheme: light), (min-width: 600px)').matches, true);
  eq('dark: list, none matches', mm('(prefers-color-scheme: light), (min-width: 1600px)').matches, false);
  eq('dark: boolean form', mm('(prefers-color-scheme)').matches, true);
  eq('dark: media is the original', mm('(Prefers-Color-Scheme:  dark)').media, '(prefers-color-scheme: dark)');
  const l = mm('(prefers-color-scheme: dark)');
  eq('instanceof MediaQueryList', l instanceof win.MediaQueryList, true);
  eq('instanceof EventTarget', l instanceof EventTarget, true);
  eq('own props are not enumerable', Object.keys(l), []);
  eq('no arguments throws as the real one', (() => { try { mm(); return 'no'; } catch (e) { return e.constructor.name; } })(), 'TypeError');
  win[KEY](false);
  eq('back to light: a list made in dark follows', l.matches, false);
  eq('hook is not enumerable', Object.keys(win).includes(KEY), false);
  eq('hook cannot be replaced', (() => { try { win[KEY] = 1; } catch (e) { /* strict */ } return typeof win[KEY]; })(), 'function');
}

// ---- change events ----
{
  const { win, setWidth } = makeEngine();
  inject(win, false);
  const l = win.matchMedia('(prefers-color-scheme: dark)');
  const seen = [];
  const a = (e) => seen.push(['a', e.type, e.matches, e.media, e.target === l]);
  l.addEventListener('change', a);
  l.addListener((e) => seen.push(['b', e.matches]));
  l.onchange = (e) => seen.push(['on', e.matches]);
  win[KEY](true);
  eq('flip to dark: every listener once', seen, [['a', 'change', true, '(prefers-color-scheme: dark)', true], ['b', true], ['on', true]]);
  seen.length = 0;
  win[KEY](true);
  eq('same scheme again: nothing', seen, []);
  l.removeEventListener('change', a);
  l.onchange = null;
  win[KEY](false);
  eq('after removing: only the rest', seen, [['b', false]]);
  eq('onchange read back', l.onchange, null);

  const both = win.matchMedia('(prefers-color-scheme)');
  const s2 = [];
  both.addEventListener('change', () => s2.push(1));
  win[KEY](true);
  eq('a query true in both schemes: no change', s2, []);

  const w = win.matchMedia('(prefers-color-scheme: dark) and (min-width: 600px)');
  const s3 = [];
  w.addEventListener('change', (e) => s3.push(e.matches));
  setWidth(500);
  eq('the real list changing passes on', s3, [false]);
  setWidth(900);
  win[KEY](false);
  eq('then width and scheme', s3, [false, true, false]);

  const once = win.matchMedia('(prefers-color-scheme: light)');
  const s4 = [];
  const f = () => s4.push(1);
  once.addEventListener('change', f);
  once.addEventListener('change', f);
  win[KEY](true);
  eq('the same listener twice runs once', s4, [1]);
  const throwing = win.matchMedia('(prefers-color-scheme: light)');
  const s5 = [];
  throwing.addEventListener('change', () => { throw new Error('page bug'); });
  throwing.addEventListener('change', () => s5.push(1));
  const quiet = process.listeners('uncaughtException');
  process.removeAllListeners('uncaughtException');
  process.on('uncaughtException', () => {});
  try { win[KEY](false); } catch (e) { s5.push('threw'); }
  process.removeAllListeners('uncaughtException');
  for (const q of quiet) process.on('uncaughtException', q);
  eq('a throwing page listener stops nothing', s5, [1]);
}

// ---- an engine that already answers dark: nothing is swapped ----
{
  const { win } = makeEngine({ engineDark: true });
  inject(win, true);
  eq('engine dark, desktop dark', win.matchMedia('(prefers-color-scheme: dark)').matches, true);
  win[KEY](false);
  eq('engine dark, desktop light', win.matchMedia('(prefers-color-scheme: dark)').matches, false);
}

// ---- frames ----
{
  const top = makeEngine(), child = makeEngine();
  inject(top.win, false);
  inject(child.win, false);
  const other = new Proxy({}, { get() { throw new Error('SecurityError'); } });
  top.win.frames = [other, child.win];
  top.win[KEY](true);
  eq('a same-origin frame follows, another origin is skipped', child.win.matchMedia('(prefers-color-scheme: dark)').matches, true);
}

// ---- twice in one frame, no matchMedia ----
{
  const { win } = makeEngine();
  inject(win, false);
  const first = win.matchMedia;
  inject(win, true);
  eq('a second injection does nothing', [win.matchMedia === first, win.matchMedia('(prefers-color-scheme: dark)').matches], [true, false]);
  const bare = { EventTarget, Event, frames: [] };
  inject(bare, true);
  eq('no matchMedia: nothing added', Object.getOwnPropertyNames(bare).includes(KEY), false);
}

// ---- what lord-web ships: mk/web.mk drops the whole-line comments ----
{
  const shipped = SRC.split('\n').filter((l) => !/^\s*\/\//.test(l)).join('\n');
  const { win } = makeEngine();
  new Function('window', `${shipped}(${JSON.stringify(KEY)}, true);`)(win);
  eq('the shipped script works', win.matchMedia('(prefers-color-scheme: dark)').matches, true);
}

console.log(`webscheme: ${n - bad}/${n} passed`);
process.exit(bad ? 1 : 0);
