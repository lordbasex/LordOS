// app_skin_helpers.js — what every app's stub-regions scene shares (tests/<app>_skin_scene.mjs, run by
// tests/menubar_test.mjs): mounting an app with every region it names replaced by a stub that records its model, and the
// checks each of them makes (the stubs drawn, the model frozen, the commands a closed list that refuses anything else,
// layouts off bringing the app's own regions back, no React errors). The stub lord has no catalogs: texts are keys.
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';

const { parents, texts, removed } = globalThis.__lordTest;
// a home folder with nothing in it, unless the scene brought its own lord.fs
if (!globalThis.lord.fs || typeof globalThis.lord.fs.home !== 'function' || !globalThis.lord.fs.home()) {
  globalThis.lord.fs = new Proxy({ home: () => '/home/test', list: () => [], read: () => { throw new Error('ENOENT'); }, stat: () => { throw new Error('ENOENT'); }, volumes: () => [], mounts: () => [] },
    { get: (o, k) => (k in o ? o[k] : () => undefined) });
}
const h = React.createElement;
export const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
export const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
export const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));
export const errs = [];
const ce = console.error;
console.error = (...a) => { errs.push(a.map(String).join(' ').slice(0, 300)); ce(...a); };

// Mounts App as window `id` with every region of `app` stubbed; returns the models the stubs last got, by region.
export async function mountStubbed(app, App, { id = `${app}-1`, args = {}, width = 1000, height = 650 } = {}) {
  const seen = {};
  const regions = APP_REGIONS[app] || [];
  const stub = (region) => function Stub({ model }) { seen[region] = model; return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`)); };
  registerAppSkins({ [app]: Object.fromEntries(regions.map((r) => [r, stub(r)])) });
  globalThis.lord.getLayout = () => [0, 0, width, height];
  rt.render(h(rt.View, { style: { width, height } }, h(AppWindowContext.Provider, { value: { id, args } }, h(App))));
  await flush();
  return { seen, regions };
}

// The checks every app makes. own: a text the app's own region draws (gone while skinned, back with layouts off).
export async function commonChecks(app, { seen, regions }, { own, bar = 'toolbar' } = {}) {
  for (const r of regions.filter((x) => x !== 'contextMenu')) check(`${app}: the ${r} stub is drawn`, shown(`stub-${r}`));
  const m = seen[bar];
  check(`${app}: the model is frozen`, !!m && Object.isFrozen(m) && Object.isFrozen(m.actions), JSON.stringify(Object.keys(seen)));
  if (m && m.commands) {
    check(`${app}: the commands are plain data`, m.commands.every((c) => Object.isFrozen(c) && Object.keys(c).every((k) => ['id', 'label', 'disabled', 'active', 'group'].includes(k))));
    const before = JSON.stringify(seen[bar]);
    m.actions.command('rm -rf /');
    m.actions.command('__proto__');
    await flush();
    check(`${app}: a command not on the list does nothing`, JSON.stringify(seen[bar]) === before);
  }
  if (own) check(`${app}: its own region is not drawn while skinned`, !shown(own));
  settings.set({ themeParts: { layouts: false } });
  await flush();
  check(`${app}: layouts off brings its own regions back`, regions.every((r) => !shown(`stub-${r}`)) && (!own || shown(own)));
  settings.set({ themeParts: { layouts: true } });
  await flush();
  check(`${app}: layouts on again`, regions.filter((x) => x !== 'contextMenu').every((r) => shown(`stub-${r}`)));
  check(`${app}: no React errors`, !errs.length, JSON.stringify(errs.slice(0, 2)));
}
export { settings };
