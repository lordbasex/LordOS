// pending_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself), docs/MAIN-THREAD.md: the controls
// that answer a press before a slow action (<Toggle pending>, <Button pending>, usePending), useTrackedSettings (a
// component renders again only for the settings it read), ThemeProvider keeping its value while the tokens are equal,
// and sortByName giving compareNames' order. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { Toggle, Button, usePending } from '../../sdk/components.jsx';
import { ThemeProvider, useTheme } from '../../sdk/theme.js';
import { settings, useTrackedSettings } from '../../sdk/settings.js';
import { compareNames, sortByName } from '../../sdk/collate.js';

const h = React.createElement;
const T = globalThis.__lordTest;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
// frames come when the scene says: afterPaint waits for one (the press drawn), then a timer
let frames = [];
globalThis.requestAnimationFrame = (fn) => { frames.push(fn); return frames.length; };
const frame = async () => { const f = frames; frames = []; for (const fn of f) fn(lord.now()); await wait(5); await flush(); };
const click = async (id) => { globalThis.__lordEvent('down', id, 1, 1, 0); globalThis.__lordEvent('up', id, 1, 1, 0); await flush(); };

// ---- <Toggle pending> ----
const calls = [];
let release = null, failNext = false, real = false, errors = [];
function Scene() {
  const [on, setOn] = useState(false);
  const act = (v) => {
    calls.push(v);
    return new Promise((res, rej) => { release = () => { if (failNext) rej(new Error('refused')); else { real = v; setOn(v); res(); } }; });
  };
  return h(ThemeProvider, { tokens: { accent: '#0a84ff', control: '#333333' } },
    h(rt.View, null,
      h(Toggle, { small: true, pending: true, on, onChange: act, onError: (e) => errors.push(e.message) }),
      h(Button, { label: 'Go', pending: true, onClick: () => slowButton() })));
}
let pressed = 0;
const slowButton = () => { pressed++; return new Promise((res) => { buttonDone = res; }); };
let buttonDone = null;

rt.render(h(Scene));
await flush();
const toggleId = [...T.styles.entries()].find(([, s]) => s.width === 36 && s.height === 20)?.[0];
const knobLeft = () => T.styles.get([...T.parents.entries()].find(([c, p]) => p === toggleId)?.[0])?.left;
check('the toggle is drawn', !!toggleId);
const offLeft = knobLeft();
await click(toggleId);
check('pending: the switch flips at once', knobLeft() !== offLeft, `${knobLeft()} vs ${offLeft}`);
check('pending: dimmed while it runs', T.styles.get(toggleId).opacity < 1, String(T.styles.get(toggleId).opacity));
await wait(5);
await flush();
check('pending: nothing runs before the press is drawn', calls.length === 0, JSON.stringify(calls));
await frame();
check('pending: the action runs after the press was drawn', calls.length === 1 && calls[0] === true, JSON.stringify(calls));
await click(toggleId);
await wait(5);
check('pending: presses while it runs are ignored', calls.length === 1, JSON.stringify(calls));
release();
await flush();
await wait(5);
await flush();
check('pending: done, it follows the real state', real === true && knobLeft() !== offLeft && T.styles.get(toggleId).opacity === 1);
// a failure flips it back at once, with the error
failNext = true;
await click(toggleId);
await frame();
check('pending: the second flip runs', calls.length === 2 && calls[1] === false, JSON.stringify(calls));
check('pending: shown off meanwhile', knobLeft() === offLeft);
release();
await flush();
await wait(5);
await flush();
check('pending: a failure flips it back', knobLeft() !== offLeft, String(knobLeft()));
check('pending: the error goes to onError', errors.length === 1 && errors[0] === 'refused', JSON.stringify(errors));

// ---- <Button pending> ----
const goText = [...T.texts.entries()].find(([, s]) => s === 'Go')?.[0];
const buttonId = T.parents.get(T.parents.get(goText));
await click(buttonId);
await wait(5);
await flush();
check('button pending: not run before the press is drawn', pressed === 0);
await frame();
check('button pending: run after it', pressed === 1);
await click(buttonId);
await wait(5);
check('button pending: ignored while busy', pressed === 1);
buttonDone();
await flush();
await click(buttonId);
await frame();
check('button pending: pressed again once done', pressed === 2);
buttonDone();
await flush();

// ---- usePending: an error is kept, run resolves ----
globalThis.requestAnimationFrame = undefined;       // afterPaint without frames: a timer only
let hook = null;
function HookUser() { hook = usePending(async (x) => { if (x === 'bad') throw new Error('nope'); return x * 2; }); return null; }
rt.render(h(HookUser));
await flush();
const r1 = await hook[0](21);
await flush();
check('usePending: the result', r1 === 42, String(r1));
const r2 = await hook[0]('bad');
await flush();
check('usePending: a failure resolves undefined and keeps the error', r2 === undefined && hook[2]?.message === 'nope', String(hook[2]));

// ---- useTrackedSettings ----
let renders = 0, seenMode = null;
function Reader() { const [s] = useTrackedSettings(); renders++; seenMode = s.mode; return null; }
rt.render(h(Reader));
await flush();
const r0 = renders;
settings.set({ storeSideWidth: 300 });
await flush();
check('tracked: a setting it never read changes: no render', renders === r0, `${renders} vs ${r0}`);
settings.set({ mode: settings.get().mode === 'dark' ? 'light' : 'dark' });
await flush();
check('tracked: one it read changes: it renders, with the new value', renders === r0 + 1 && seenMode === settings.get().mode, `${renders} ${seenMode}`);

// ---- ThemeProvider: the same value while the tokens are equal ----
const values = new Set();
let bump = null;
function Probe() { values.add(useTheme()); return null; }
function Root() { const [n, setN] = useState(0); bump = () => setN(n + 1); return h(ThemeProvider, { tokens: { accent: '#0a84ff', list: [1, 2] } }, h(Probe)); }
rt.render(h(Root));
await flush();
for (let i = 0; i < 3; i++) { bump(); await flush(); }
check('theme: equal tokens made again keep one value', values.size === 1, `${values.size} values`);

// ---- sortByName = compareNames' order ----
const names = ['Drive 10', 'drive 2', 'Éte', 'ete', 'go-link Desk', 'Gimp', 'a', 'B', '', 'zeta 01', 'zeta 1', 'Ärger', 'arg'];
const want = [...names].sort(compareNames);
check('sortByName: the same order as compareNames', JSON.stringify(sortByName(names)) === JSON.stringify(want), JSON.stringify(sortByName(names)));
const objs = names.map((n, i) => ({ n, i }));
check('sortByName: by a key, stable', JSON.stringify(sortByName(objs, (o) => o.n).map((o) => o.n)) === JSON.stringify(want));
process.exit(0);
