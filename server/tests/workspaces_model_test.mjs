// Lord Workspaces as plain data (themes/workspaces/model/*.js, tokens.js) and the theme.json fields Settings reads
// (sdk/thememeta.js): where things go on each display, the tiles, the activities' settings key, the three effects
// styles and the palette's contrast. node server/tests/workspaces_model_test.mjs
import { displayPlan, shownDisplays, spineMode, onChrome, SPINE_BLUR, SPINE_GAP, FRAME_PAD, SPINE_W, MARGIN, SMALL_W, SPINE_SHADOW_REACH, WINDOW_SHADOW } from '../../themes/workspaces/model/geometry.js';
import { tileRects, fitTile, splitterAt, ratioFrom, orderFor, homeFor, displayKey, displayKeys, judgeFrame, underBudget, ANSWER_MS, MOVES_PER_SEC, savedDisplay, foldMissing, cleanLayout, cleanRatio, LAYOUTS, TILE_GAP, RATIO_MIN, RATIO_MAX } from '../../themes/workspaces/model/layouts.js';
import { recentOrder, spineRecentRoom, activitySummary, barStep, newGroup, groupOfWindow, groupPlan, groupApps, everyActivity, MAX_GROUPS } from '../../themes/workspaces/model/activities.js';
import { wsPrefs, effectiveSurface, metaFromSaved, savedFromMeta, cleanActivity, defaultActivity, freshActivity, togglePin, needsOnboarding, wsPatch, optPatch,
  ACTIVITY_ICONS, ACTIVITY_COLORS, MAX_PINS, WS_VERSION } from '../../themes/workspaces/model/activities.js';
import { makeTheme, accents, deep, material } from '../../themes/workspaces/tokens.js';
import { cleanThemeMeta, themeOptionValue, themeOptionPatch } from '../../sdk/thememeta.js';
import { slotStacks, shownIn, dropInto, slotAt, dropSlot, slotLimits } from '../../themes/workspaces/model/layouts.js';
import { assistFor, follow, other } from '../../themes/workspaces/model/magnet.js';
import { readFileSync } from 'node:fs';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const yes = (name, v) => eq(name, !!v, true);

// ---- geometry ----
const D = { id: 1, x: 0, y: 0, w: 1920, h: 1080, main: true };
const p = displayPlan(D, { spine: 'auto' });
eq('wide display: expanded spine', p.mode, 'expanded');
eq('spine rect', p.spine, { x: MARGIN, y: MARGIN, w: SPINE_W.expanded, h: 1080 - 2 * MARGIN });
yes('top band on the main display', p.top && p.top.y === MARGIN);
yes('the top band clear of the spine blur margin', p.top.x - (p.spine.x + p.spine.w) > 2 * SPINE_BLUR + 1 + SPINE_SHADOW_REACH);
yes('no spine shadow (its reach is part of the repaint margin)', makeTheme({ mode: 'dark' }).spineShadow === 'none');
yes('frame starts after the spine', p.frame.x === MARGIN + SPINE_W.expanded + SPINE_GAP);
yes('work inside the frame', p.work.x > p.frame.x && p.work.y > p.frame.y && p.work.x + p.work.w < p.frame.x + p.frame.w && p.work.y + p.work.h < p.frame.y + p.frame.h);
// the blur of the spine reads 2 × its radius around it: no window may sit in that margin
// a window's frame node reaches 6 px past its edge (sdk/windowshell.jsx WINDOW_EDGE, the resize grips)
yes('windows and their shadows clear of the spine blur margin', p.work.x - Math.max(6, WINDOW_SHADOW) - (p.spine.x + p.spine.w) > 2 * SPINE_BLUR + 1 + SPINE_SHADOW_REACH);
yes('window shadows within their reach', ['dark', 'light'].every((mode) => / 16px rgba/.test(makeTheme({ mode }).shadow) && / 12px rgba/.test(makeTheme({ mode }).shadowSoft)));
eq('blur gap constant', SPINE_GAP + FRAME_PAD - 6 > 2 * SPINE_BLUR + 1 + SPINE_SHADOW_REACH, true);
eq('narrow display collapses', spineMode('auto', SMALL_W - 1), 'collapsed');
eq('explicit expanded wins', spineMode('expanded', 1024), 'expanded');
eq('explicit collapsed wins', spineMode('collapsed', 3840), 'collapsed');
const rtl = displayPlan(D, { rtl: true });
eq('rtl spine on the right', rtl.spine.x + rtl.spine.w, 1920 - MARGIN);
eq('rtl frame on the left', rtl.frame.x, MARGIN);
const second = { id: 2, x: 1920, y: 0, w: 1920, h: 1080, main: false };
const p2 = displayPlan(second, {});
eq('second display: no spine', p2.spine, null);
eq('second display: no top band', p2.top, null);
eq('second display frame', p2.frame.x, 1920 + MARGIN);
yes('second display: spine with every', displayPlan(second, { every: true }).spine);
eq('mirror: only the main display', shownDisplays([second, D], true).map((d) => d.id), [1]);
eq('not mirrored: all', shownDisplays([D, second], false).map((d) => d.id), [1, 2]);
yes('on chrome: the spine', onChrome([p], 20, 500));
yes('on chrome: the top band', onChrome([p], 500, 20));
eq('off chrome: the work area', onChrome([p], 900, 600), false);

// ---- layouts ----
const A = { x: 0, y: 0, w: 1000, h: 600 };
eq('layouts', LAYOUTS, ['free', 'single', 'halves', 'split', 'three', 'grid']);
eq('free has no tiles', tileRects('free', A, 3), null);
eq('unknown layout is free', cleanLayout('spiral'), 'free');
eq('single: monocle', tileRects('single', A, 2), [A, A]);
const sp = tileRects('split', A, 3, 2 / 3);
eq('split: main', sp[0], { x: 0, y: 0, w: 660, h: 600 });
eq('split: side rows', [sp[1], sp[2]], [{ x: 670, y: 0, w: 330, h: 295 }, { x: 670, y: 305, w: 330, h: 295 }]);
const sp6 = tileRects('split', A, 6);
eq('split: extra windows share the last row', sp6[5], sp6[3]);
const th = tileRects('three', A, 3);
eq('three: thirds', th.map((r) => r.w), [327, 327, 327]);
eq('three: no overlap', th[1].x >= th[0].x + th[0].w + TILE_GAP - 1, true);
const gr = tileRects('grid', A, 5);
eq('grid: 5 in 3 + 2', gr.length, 5);
eq('grid: last row stretches', gr[3].w > gr[0].w, true);
eq('tiles stay inside the area', [...sp, ...th, ...gr].every((r) => r.x >= A.x && r.y >= A.y && r.x + r.w <= A.x + A.w + 1 && r.y + r.h <= A.y + A.h + 1), true);
eq('ratio clamped', [cleanRatio('split', 0.05), cleanRatio('split', 2), cleanRatio('split', 'x')], [RATIO_MIN, RATIO_MAX, 2 / 3]);
eq('fit: fits', fitTile({ x: 10, y: 10, w: 400, h: 300 }, [200, 100], A).tight, false);
const tight = fitTile({ x: 700, y: 10, w: 300, h: 300 }, [520, 200], A);
eq('fit: a too-small tile keeps the minimum, inside the area', [tight.w, tight.x + tight.w <= A.w, tight.tight], [520, true, true]);
const sx = splitterAt('split', A, 2 / 3, 3);
eq('splitter in the gap', sx, 665);
eq('splitter: none for grid or one window', [splitterAt('grid', A, 0.5, 4), splitterAt('split', A, 0.5, 1)], [null, null]);
eq('ratio from the splitter round-trips', Math.round(ratioFrom('split', A, sx, 3) * 1000) / 1000, 0.667);
eq('ratio from a drop far left is clamped', ratioFrom('split', A, -50, 3), RATIO_MIN);
const keyOf = (id) => ({ a: 'files', b: 'terminal', c: 'editor', d: 'files' })[id];
eq('order: saved apps first', orderFor(['a', 'b', 'c'], keyOf, ['editor', 'files']), ['c', 'a', 'b']);
eq('order: one window per saved app', orderFor(['a', 'd', 'b'], keyOf, ['files', 'files']), ['a', 'd', 'b']);
eq('order: nothing saved', orderFor(['b', 'a'], keyOf, null), ['b', 'a']);
const saved = { 'DP-1': { apps: ['editor'] }, 'DP-2': { apps: ['terminal', 'files'] }, 'HDMI-A-1': { apps: ['web:docs'] } };
eq('home: saved display connected', homeFor('terminal', saved, ['DP-1', 'DP-2']), 'DP-2');
eq('home: saved display gone', homeFor('web:docs', saved, ['DP-1', 'DP-2']), null);
const folded = foldMissing(saved, ['DP-1', 'DP-2'], 'DP-1');
eq('fewer monitors: a gone display joins the main one', folded['DP-1'].apps, ['editor', 'web:docs']);
eq('fewer monitors: the others stay', folded['DP-2'].apps, ['terminal', 'files']);
eq('home after folding', homeFor('web:docs', folded, ['DP-1', 'DP-2']), 'DP-1');
eq('display key: connector', displayKey({ name: 'S27F350', connector: 'DP-3' }, 2), 'DP-3');
eq('display key: never the model name', displayKey({ name: 'S27F350' }, 2), '#2');
eq('display key: by place', displayKey({}, 2), '#2');
// three monitors of one model (the maintainer's desk, 0.8.9): three keys
const same = [{ name: 'S27F350', connector: 'DP-2' }, { name: 'S27F350', connector: 'HDMI-A-1' }, { name: 'S27F350', connector: 'DP-3' }];
eq('display keys: three of one model', displayKeys(same), ['DP-2', 'HDMI-A-1', 'DP-3']);
eq('display keys: never two alike', displayKeys([{ connector: 'X' }, { connector: 'X' }, {}]), ['X', '#1', '#2']);

// ---- the tiler's decisions: bounded whatever the app does ----
const T = { x: 100, y: 50, w: 800, h: 600 };
const entry = { tile: T, got: T, t: 1000 };
eq('judge: where the tiler left it', judgeFrame(entry, { ...T }, 1100), 'same');
eq('judge: the app answered another size at the corner', judgeFrame(entry, { ...T, w: 812, h: 590 }, 1200), 'answer');
eq('judge: the answer is kept afterwards', judgeFrame({ ...entry, got: { ...T, w: 812, h: 590 } }, { ...T, w: 812, h: 590 }, 9000), 'same');
eq('judge: a late change is a hand', judgeFrame(entry, { ...T, w: 700 }, 1000 + ANSWER_MS + 1), 'hand');
eq('judge: moved is a hand', judgeFrame(entry, { ...T, x: 140 }, 1100), 'hand');
eq('judge: nothing known', judgeFrame(null, T, 0), 'hand');
let times = [];
for (let i = 0; i < MOVES_PER_SEC; i++) times = underBudget(times, 5000 + i);
eq('budget: ten moves in a second', times.length, MOVES_PER_SEC);
eq('budget: one more waits', underBudget(times, 5100), null);
eq('budget: a second later it fits', underBudget(times, 6010)?.length, 1);
// An app that never takes its size: the tiler (as tiler.js uses these) asks it once per tile, whatever it answers.
{
  const asked = [];
  let e = null, frame = { x: 0, y: 0, w: 10, h: 10 }, t = 0;
  for (let pass = 0; pass < 200; pass++, t += 7) {
    const how = e ? judgeFrame(e, frame, t) : 'same';
    if (how === 'hand') break;
    if (how === 'answer') e = { ...e, got: { ...frame } };
    if (!(e && e.tile.x === T.x && e.tile.y === T.y && e.tile.w === T.w && e.tile.h === T.h)) { e = { tile: T, got: T, t }; asked.push(t); frame = { ...T }; }
    frame = { ...frame, w: T.w - 3 - (pass % 5), h: T.h - 7 };     // the app answers something else every time
  }
  eq('a stubborn app: asked once for its tile', asked.length, 1);
}
eq('saved display', savedDisplay('three', 0.5, ['files', null, 'terminal']), { layout: 'three', ratio: 0.5, apps: ['files', 'terminal'] });

// ---- activities ----
eq('empty settings', wsPrefs({}), { v: WS_VERSION, surface: 'crystal', spine: 'auto', spineEvery: false, contrast: false, activities: [] });
eq('junk key', wsPrefs({ workspaces: 'x', themeOptions: { workspaces: { surface: 'glass', spine: 4 } } }).surface, 'crystal');
eq('options from themeOptions', wsPrefs({ themeOptions: { workspaces: { surface: 'opaque', spine: 'collapsed', spineEvery: true } } }),
  { v: 1, surface: 'opaque', spine: 'collapsed', spineEvery: true, contrast: false, activities: [] });
eq('transparency off forces opaque', effectiveSurface({ surface: 'crystal' }, { effects: false }), 'opaque');
eq('transparency on keeps the style', effectiveSurface({ surface: 'translucent' }, { effects: true }), 'translucent');
const act = cleanActivity({ icon: 'code', color: '#14a89a', pins: ['files', 'files', 3, 'terminal'], layouts: { 'DP-1': { layout: 'split', ratio: 9, apps: ['files', 7] }, bad: 3 }, setup: true });
eq('clean activity', act, { icon: 'code', color: '#14A89A', pins: ['files', 'terminal'], layouts: { 'DP-1': { layout: 'split', ratio: RATIO_MAX, apps: ['files'] } }, setup: true });
eq('bad icon and colour fall back', [cleanActivity({ icon: 'nope', color: 'red' }, 1).icon, cleanActivity({ color: 'red' }, 1).color], [ACTIVITY_ICONS[1], ACTIVITY_COLORS[1]]);
const meta = metaFromSaved([1, 2, 3], [null, { icon: 'music', color: '#E5487F', pins: [], layouts: {}, setup: true }]);
eq('meta by position', [meta[1].icon, meta[2].icon, meta[3].icon], [defaultActivity(0).icon, 'music', defaultActivity(2).icon]);
eq('saved by position, defaults left out', savedFromMeta([1, 2, 3], meta), [null, { icon: 'music', color: '#E5487F', pins: [], layouts: {}, setup: true }]);
eq('a reordered list keeps each activity', savedFromMeta([2, 1], meta)[0].icon, 'music');
const fresh = freshActivity({ 1: { icon: 'spark', color: ACTIVITY_COLORS[0] } }, 1);
yes('fresh activity: unused icon and colour', fresh.icon !== 'spark' && fresh.color !== ACTIVITY_COLORS[0]);
eq('pin toggles', [togglePin(['a'], 'b'), togglePin(['a', 'b'], 'a')], [['a', 'b'], ['b']]);
eq('pins capped', togglePin(Array.from({ length: MAX_PINS }, (_, i) => `x${i}`), 'y').length, MAX_PINS);
eq('onboarding only for one fresh empty activity', [needsOnboarding(defaultActivity(0), 1, 0), needsOnboarding(defaultActivity(0), 2, 0),
  needsOnboarding({ ...defaultActivity(0), setup: true }, 1, 0), needsOnboarding(defaultActivity(0), 1, 1)], [true, false, false, false]);
eq('activities patch keeps the rest', wsPatch({ workspaces: { activities: [], x: 1 } }, { activities: [null] }), { workspaces: { activities: [null], x: 1, v: WS_VERSION } });
eq('option patch', optPatch({ themeOptions: { other: { a: 1 } } }, 'surface', 'opaque'), { themeOptions: { other: { a: 1 }, workspaces: { surface: 'opaque' } } });

// ---- theme.json for Settings (sdk/thememeta.js) ----
const tj = JSON.parse(readFileSync(new URL('../../themes/workspaces/theme.json', import.meta.url)));
const cm = cleanThemeMeta(tj);
eq('theme.json sketch', cm.sketch, { rail: true, pills: true, frame: true });
eq('theme.json options', cm.options.map((o) => o.key), ['surface', 'spine', 'spineEvery', 'contrast']);
eq('theme.json arrows', cm.desktopArrows, true);
eq('theme.json under 4 KB', readFileSync(new URL('../../themes/workspaces/theme.json', import.meta.url)).length < 4096, true);
eq('bundled themes declare nothing', ['default', 'classic'].map((id) => cleanThemeMeta(JSON.parse(readFileSync(new URL(`../../themes/${id}/theme.json`, import.meta.url))))),
  [{ sketch: null, options: [], desktopArrows: false }, { sketch: null, options: [], desktopArrows: false }]);
eq('junk meta', cleanThemeMeta({ sketch: 'x', options: [{ key: '__proto__', label: 'shell.x' }, { key: 'a', label: 'evil', toggle: true }, { key: 'b', label: 'shell.b', choices: [['x', 'shell.x']] }], desktopArrows: 1 }),
  { sketch: null, options: [], desktopArrows: false });
const surfaceOpt = cm.options[0];
eq('option default', themeOptionValue({}, 'workspaces', surfaceOpt), 'crystal');
eq('option stored', themeOptionValue({ themeOptions: { workspaces: { surface: 'opaque' } } }, 'workspaces', surfaceOpt), 'opaque');
eq('option junk value', themeOptionValue({ themeOptions: { workspaces: { surface: 'x' } } }, 'workspaces', surfaceOpt), 'crystal');
eq('option patch', themeOptionPatch({}, 'workspaces', 'spine', 'collapsed'), { themeOptions: { workspaces: { spine: 'collapsed' } } });
eq('a theme id never reaches another setting', Object.keys(themeOptionPatch({}, 'mode', 'x', 1)), ['themeOptions']);

// ---- tokens: three styles, two modes, the shared accent names, contrast ----
const hex = (c) => { const m = /^#([0-9a-f]{6})$/i.exec(c); const n = parseInt(m[1], 16); return [n >> 16, (n >> 8) & 255, n & 255]; };
const rgba = (c) => { const m = /rgba?\(([^)]+)\)/.exec(c); const a = m[1].split(',').map(Number); return [a[0], a[1], a[2], a[3] ?? 1]; };
const over = (fg, bg) => { const [r, g, b, a] = fg.startsWith('#') ? [...hex(fg), 1] : rgba(fg); const B = hex(bg); return [r * a + B[0] * (1 - a), g * a + B[1] * (1 - a), b * a + B[2] * (1 - a)]; };
const lum = ([r, g, b]) => { const f = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; }; return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b); };
const contrast = (fg, bg) => { const a = lum(over(fg, bg)), b = lum(hex(bg)); return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05); };
for (const mode of ['dark', 'light']) {
  for (const surface of ['opaque', 'translucent', 'crystal']) {
    const t = makeTheme({ mode, accent: 'violeta', effects: true, surface });
    eq(`${mode} ${surface}: surface`, t.surface, surface);
    eq(`${mode} ${surface}: blur only when not opaque`, !!t.mat.blur, surface !== 'opaque');
    eq(`${mode} ${surface}: crystal's light only on crystal`, !!t.mat.sheen && !!t.mat.edge && !!t.mat.rim, surface === 'crystal');
    yes(`${mode} ${surface}: sheen has two stops`, !t.mat.sheen || (t.mat.sheen.match(/rgba/g) || []).length === 2);
  }
  const t = makeTheme({ mode, accent: 'violeta', effects: true, surface: 'crystal' });
  yes(`${mode}: text on windows ≥ 7`, contrast(t.text, t.windowBg) >= 7);
  yes(`${mode}: dim text on windows ≥ 4.5`, contrast(t.textDim, t.windowBg) >= 4.5);
  yes(`${mode}: text on panels ≥ 7`, contrast(t.text, t.solidBg) >= 7);
  yes(`${mode}: dim text on panels ≥ 4.5`, contrast(t.textDim, t.solidBg) >= 4.5);
  yes(`${mode}: text on the title bar ≥ 7`, contrast(t.text, t.titleBg) >= 7);
  for (const name of Object.keys(accents)) {
    const a = makeTheme({ mode, accent: name, effects: true, surface: 'opaque' }).accent;
    yes(`${mode} ${name}: white on the accent ≥ 3`, contrast('#ffffff', a) >= 3);
  }
  eq(`${mode}: transparency off is opaque`, makeTheme({ mode, accent: 'azul', effects: false, surface: 'crystal' }).surface, 'opaque');
}
eq('the shared accent names, and the proposal\'s amber', Object.keys(accents), ['azul', 'violeta', 'rosa', 'naranja', 'verde', 'grafito', 'ambar']);
eq('amber: the focus ring stays violet', [makeTheme({ mode: 'dark', accent: 'ambar' }).focusRing, makeTheme({ mode: 'light', accent: 'ambar' }).focusRing], ['#7C5CFF', '#5B3FE0']);
eq('deep accents for every name', Object.keys(deep), Object.keys(accents));
eq('brand violets', [accents.violeta, deep.violeta], ['#7C5CFF', '#5B3FE0']);
eq('an unknown accent is the brand violet', makeTheme({ mode: 'dark', accent: 'x' }).accent, '#7C5CFF');
eq('material is plain data', JSON.parse(JSON.stringify(material('crystal', true))).blur, SPINE_BLUR);

// ---- two halves ----
{
  const A = { x: 0, y: 0, w: 1010, h: 600 };
  const h2 = tileRects('halves', A, 2, null);
  eq('halves: two equal columns', h2, [{ x: 0, y: 0, w: 500, h: 600 }, { x: 510, y: 0, w: 500, h: 600 }]);
  eq('halves: one window takes it all', tileRects('halves', A, 1, null), [{ x: 0, y: 0, w: 1010, h: 600 }]);
  const h3 = tileRects('halves', A, 3, null);
  eq('halves: three windows, the left column holds two rows', [h3[0].x, h3[2].x, h3[1].x, h3[0].h, h3[1].h], [0, 0, 510, 295, 600]);
  const h9 = tileRects('halves', A, 9, null);
  yes('halves: every window inside the area', h9.every((r) => r.x >= 0 && r.y >= 0 && r.x + r.w <= 1010 && r.y + r.h <= 600));
  eq('halves: the divider in the middle', splitterAt('halves', A, null, 2), 505);
  eq('halves: dragging the divider', ratioFrom('halves', A, 705, 2), cleanRatio('halves', 700 / 1000));
  yes('halves is a layout', LAYOUTS.includes('halves') && cleanLayout('halves') === 'halves');
}

// ---- the free layout's magnet ----
{
  const area = { x: 0, y: 0, w: 1000, h: 600 };
  const far = { x: 1000, y: 0, w: 1000, h: 600 };
  const win = (id, x, w, snapped) => ({ id, x, y: 0, w, h: 600, snapped });
  eq('other side', [other('left'), other('right'), other('max')], ['right', 'left', null]);
  eq('magnet: the only other window fills the half', assistFor([win('a', 0, 500, 'left'), win('b', 200, 400)], 'a', area), { fill: 'b' });
  eq('magnet: a window already in the other half pairs', assistFor([win('a', 0, 500, 'left'), win('b', 500, 500, 'right'), win('c', 100, 300)], 'a', area), { pair: 'b' });
  eq('magnet: several windows: a choice, this display first',
    assistFor([win('x', 1200, 300), win('a', 500, 500, 'right'), win('b', 100, 300), win('c', 50, 200)], 'a', area), { choose: ['b', 'c', 'x'] });
  eq('magnet: a window on another display alone is offered, never moved', assistFor([win('a', 0, 500, 'left'), win('x', 1200, 300)], 'a', area), { choose: ['x'] });
  eq('magnet: alone', assistFor([win('a', 0, 500, 'left')], 'a', area), null);
  eq('magnet: not a half', assistFor([win('a', 0, 500, 'max'), win('b', 100, 300)], 'a', area), null);
  void far;
  const pair = { left: 'a', right: 'b' };
  const f = (x, w, more) => ({ x, y: 0, w, h: 600, minimized: false, maximized: false, ...more });
  const before = { a: f(0, 500), b: f(500, 500) };
  eq('magnet: the left one wider, the right one follows', follow(pair, before, { a: f(0, 620), b: f(500, 500) }, area), { patch: { id: 'b', x: 620, w: 380 } });
  eq('magnet: the right one wider, the left one follows', follow(pair, before, { a: f(0, 500), b: f(300, 700) }, area), { patch: { id: 'a', x: 0, w: 300 } });
  eq('magnet: never under the minimum width', follow(pair, before, { a: f(0, 950), b: f(500, 500) }, area, () => 200), { patch: { id: 'b', x: 800, w: 200 } });
  eq('magnet: nothing changed', follow(pair, before, before, area), null);
  eq('magnet: the follower already there (its own write coming back)', follow(pair, { a: f(0, 500), b: f(500, 500) }, { a: f(0, 620), b: f(620, 380) }, area), { break: true });
  eq('magnet: moved by hand breaks', follow(pair, before, { a: f(40, 500), b: f(500, 500) }, area), { break: true });
  eq('magnet: minimized breaks', follow(pair, before, { a: f(0, 500, { minimized: true }), b: f(500, 500) }, area), { break: true });
  eq('magnet: closed breaks', follow(pair, before, { a: f(0, 500) }, area), { break: true });
  eq('magnet: the outer edge of the right one breaks', follow(pair, before, { a: f(0, 500), b: f(500, 400) }, area), { break: true });
}

// the padlock is kept in the settings key, and only as true
{
  const c = cleanActivity({ icon: 'code', color: '#7C5CFF', pins: [], setup: true, layouts: { 'DP-1': { layout: 'free', locked: true }, 'DP-2': { layout: 'grid', locked: 'yes' } } }, 0);
  eq('padlock kept', [c.layouts['DP-1'].locked, 'locked' in c.layouts['DP-2']], [true, false]);
}

// ---- slots: a minimized window keeps its place ----
{
  const min = (ids) => (id) => ids.includes(id);
  // Main and side keeps three slots (the side split in two) and Grid four, free ones shown even with fewer windows
  eq('slots: main and side keeps three', slotStacks([], ['files'], min([]), new Set(), { min: slotLimits('split')[0] }).stacks, [['files'], [], []]);
  eq('slots: grid keeps four', slotStacks([], ['files', 'chrome'], min([]), new Set(), { min: slotLimits('grid')[0] }).stacks, [['files'], ['chrome'], [], []]);
  eq('slots: grid with nothing open still shows its four', slotStacks([], [], min([]), new Set(), { min: slotLimits('grid')[0] }).stacks.length, 4);
  let r = slotStacks([], ['files', 'chrome'], min([]));
  eq('slots: two windows, two slots', [r.stacks, r.swapOut], [[['files'], ['chrome']], []]);
  r = slotStacks(r.stacks, ['files', 'chrome'], min(['files']));
  eq('slots: files minimized keeps its slot', r.stacks, [['files'], ['chrome']]);
  eq('slots: its slot is free, chrome still shown', [shownIn(r.stacks[0], min(['files'])), shownIn(r.stacks[1], min(['files']))], [null, 'chrome']);
  r = slotStacks(r.stacks, ['files', 'chrome', 'term'], min(['files']));
  eq('slots: a new window takes the free slot', [r.stacks, r.swapOut], [[['files', 'term'], ['chrome']], []]);
  r = slotStacks(r.stacks, ['files', 'chrome', 'term'], min([]), new Set(['files']));
  eq('slots: files brought back swaps with term', [r.stacks, r.swapOut], [[['term', 'files'], ['chrome']], ['term']]);
  r = slotStacks(r.stacks, ['files', 'chrome', 'term'], min(['term']));
  eq('slots: after the swap, nothing more to do', r.swapOut, []);
  r = slotStacks(r.stacks, ['chrome', 'term'], min(['term']));
  eq('slots: files closed, term keeps the slot (free)', r.stacks, [['term'], ['chrome']]);
  r = slotStacks(r.stacks, ['chrome'], min([]));
  eq('slots: term closed, its slot goes', r.stacks, [['chrome']]);
  eq('slots: a minimized window never placed gets no slot', slotStacks([], ['a', 'b'], min(['a'])).stacks, [['b']]);
  eq('slots: two new windows, one free slot', slotStacks([['x']], ['x', 'p', 'q'], min(['x'])).stacks, [['x', 'p'], ['q']]);
}

// ---- drag to rearrange ----
{
  eq('drop: on another slot swaps', dropInto([['a'], ['b'], ['c']], 'a', 2), [['c'], ['b'], ['a']]);
  eq('drop: on its own slot, nothing', dropInto([['a'], ['b']], 'a', 0), [['a'], ['b']]);
  eq('drop: a slot travels with its minimized windows', dropInto([['m', 'a'], ['b']], 'a', 1), [['b'], ['m', 'a']]);
  eq('drop: from another display, a slot of its own there', dropInto([['a'], ['b']], 'x', 1), [['a'], ['x'], ['b']]);
  eq('drop: past the end, last', dropInto([['a']], 'x', 5), [['a'], ['x']]);
  const R2 = [{ x: 0, y: 0, w: 100, h: 100 }, { x: 110, y: 0, w: 100, h: 100 }];
  eq('slotAt: under the point', [slotAt(R2, 150, 50), slotAt(R2, 10, 10)], [1, 0]);
  eq('slotAt: in the gap, the nearest', slotAt(R2, 107, 50), 1);
  eq('slotAt: none', slotAt([], 1, 1), -1);
}

// ---- a drop takes a free slot ----
{
  const isMin = (id) => id === 'm';
  eq('free slot: a window of the display takes it, its own slot left empty (trimmed by the layout)', dropInto([['m'], ['mon'], ['ch']], 'ch', 0, isMin), [['m', 'ch'], ['mon'], []]);
  eq('free slot: one from elsewhere takes it too (no new slot)', dropInto([['m'], ['mon']], 'x', 0, isMin), [['m', 'x'], ['mon']]);
  eq('free slot: a shown slot still swaps', dropInto([['m'], ['mon'], ['ch']], 'ch', 1, isMin), [['m'], ['ch'], ['mon']]);
  const A = { x: 0, y: 0, w: 1000, h: 600 };
  const rf = (n) => tileRects('halves', A, n, 0.5);
  eq('dropSlot: over the free slot, that one', dropSlot([['m'], ['mon'], ['ch']], 'ch', 100, 100, rf, isMin), { at: 0, n: 3 });
  eq('dropSlot: a newcomer over a shown window, a new slot', dropSlot([['a'], ['b']], 'x', 900, 300, rf, isMin).n, 3);
  eq('dropSlot: a newcomer over the free slot, no new slot', dropSlot([['m'], ['b']], 'x', 100, 300, rf, isMin), { at: 0, n: 2 });
}

// high contrast: pure text, dim text and the accent at 7:1 or more, the text on the accent too, no blur or shadow
for (const mode of ['dark', 'light']) {
  const t = makeTheme({ mode, accent: 'violeta', surface: 'crystal', contrast: true });
  yes(`${mode} contrast: text 21:1`, contrast(t.text, t.windowBg) >= 20.9);
  yes(`${mode} contrast: dim text ≥ 7`, contrast(t.textDim, t.windowBg) >= 7);
  yes(`${mode} contrast: accent ≥ 7`, contrast(t.accent, t.windowBg) >= 7);
  yes(`${mode} contrast: text on the accent ≥ 7`, contrast(t.onAccent, t.accent) >= 7);
  eq(`${mode} contrast: no blur, no shadow, a line round surfaces`, [t.mat.blur, t.panelBlur, t.shadow, t.mat.border === t.text], [0, 0, 'none', true]);
}
eq('contrast option: read from the theme options', wsPrefs({ themeOptions: { workspaces: { contrast: true } } }).contrast, true);

// the spine's recent activities: the one in view first, the others by when they were last shown
{
  eq('recent: first time, desktop order', recentOrder([], 2, [1, 2, 3]), [2, 1, 3]);
  eq('recent: the shown one moves to the front', recentOrder([2, 1, 3], 3, [1, 2, 3]), [3, 2, 1]);
  eq('recent: a removed desktop goes away', recentOrder([3, 2, 1], 1, [1, 3]), [1, 3]);
  eq('recent: a new desktop goes last', recentOrder([1, 3], 1, [1, 3, 4]), [1, 3, 4]);
  const ws = [{ id: 'a', desktop: 2 }, { id: 'b', desktop: 2, minimized: true }, { id: 'c', desktop: 2 }, { id: 'd', desktop: 1 }];
  eq('summary: front first, minimized last', activitySummary(ws, 2).shown.map((w) => w.id), ['c', 'a', 'b']);
  eq('summary: total and more', [activitySummary(ws, 2, 1).total, activitySummary(ws, 2, 1).more], [3, 2]);
  eq('bars: next, round, first and last', [barStep(['a', 'b', 'c'], 'c', 1), barStep(['a', 'b', 'c'], 'a', -1), barStep(['a', 'b'], 'b', -Infinity), barStep(['a', 'b'], 'a', Infinity)], ['a', 'c', 'a', 'b']);
  eq('bars: a control gone starts again', barStep(['a', 'b'], 'x', 1), 'a');
  // groups: more activities on one desktop
  const ga = cleanActivity({ icon: 'code', color: '#7C5CFF', groups: [{ id: 'g1', name: 'Docs', icon: 'book', color: '#22A565', apps: ['editor'] }, { id: 'bad' }, { id: 'g2' }, { id: 'g3' }, { id: 'g4' }], group: 'g1' });
  eq('groups: cleaned, at most MAX_GROUPS, the one in view kept', [ga.groups.map((g) => g.id), ga.group], [['g1', 'g2', 'g3'], 'g1']);
  eq('groups: a group in view that is gone is dropped', cleanActivity({ groups: [{ id: 'g1' }], group: 'g7' }).group, undefined);
  eq('updatedAt: an ISO date kept, anything else dropped', [cleanActivity({ updatedAt: '2026-11-17T10:00:00.000Z' }).updatedAt, cleanActivity({ updatedAt: 'yesterday' }).updatedAt], ['2026-11-17T10:00:00.000Z', undefined]);
  eq('groups: none, no key', 'groups' in cleanActivity({}), false);
  const ng = newGroup({ icon: 'code', color: '#7C5CFF', groups: [{ id: 'g1', icon: 'spark', color: '#3D7BFD' }] }, 'Mail');
  eq('groups: a new one, its own id, icon and colour', [ng.id, ng.icon !== 'code' && ng.icon !== 'spark', ng.color !== '#7C5CFF' && ng.color !== '#3D7BFD', ng.name], ['g2', true, true, 'Mail']);
  eq('groups: a window joins its saved group, else the one in view', [groupOfWindow(ga, undefined, 'editor'), groupOfWindow(ga, undefined, 'files'), groupOfWindow({}, undefined, 'files'), groupOfWindow(ga, null, 'editor')], ['g1', 'g1', null, null]);
  const gw = [{ id: 'a' }, { id: 'b', minimized: true }, { id: 'c' }, { id: 'd', minimized: true }];
  const tagsG = { a: null, b: 'g1', c: 'g1', d: 'g1' };
  eq('groups: showing the own hides g1, brings back only what the groups hid', groupPlan(gw, (w) => tagsG[w.id], new Set(['b']), null), { hide: ['c'], show: [] });
  eq('groups: showing g1', groupPlan(gw, (w) => tagsG[w.id], new Set(['b']), 'g1'), { hide: ['a'], show: ['b'] });
  eq('groups: apps remembered', groupApps([{ id: 'g1', apps: [] }], [{ id: 'c', app: 'mail' }], (w) => tagsG[w.id], (w) => w.app)[0].apps, ['mail']);
  eq('groups: every activity counted', everyActivity([1, 2], { 1: ga, 2: cleanActivity({}) }, (d) => `D${d}`).length, 1 + 3 + 1);
  eq('groups: up to 36 activities on 9 desktops', 9 * (1 + MAX_GROUPS), 36);
  eq('recent: room by height', [600, 700, 800, 1080].map(spineRecentRoom), [0, 1, 2, 3]);
}

console.log(process.exitCode ? 'workspaces-model-test: FAILED' : `workspaces-model-test: ${ok} ok`);
