// settings_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Settings with both regions a theme
// may draw (sidebar and home; docs/THEMES-GUIDE.md "App skins → Settings") replaced by a minimal stub. Settings opens
// on the home stub, the stubs get the sections, open() shows Settings' own page for that section (never the skin's),
// home() comes back, a section Settings doesn't show here or doesn't know is refused, the search filters the groups,
// and layouts off brings Settings' own sidebar back. The stub lord has no catalogs: texts are their keys.
// Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { AppWindowContext } from '../../sdk/components.jsx';
import { registerAppSkins, APP_REGIONS, settings } from '../../sdk/index.js';
import Settings from '../../apps/settings/index.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 20)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const shown = (label) => [...texts].some(([id, s]) => s === label && inTree(id));

const errs = [];
const ce = console.error;
console.error = (...a) => { errs.push(a.map(String).join(' ').slice(0, 400)); ce(...a); };
const seen = {};
const stub = (region) => function Stub({ model }) {
  seen[region] = model;
  return h(rt.View, { style: { height: 10 } }, h(rt.Text, null, `stub-${region}`));
};
check('Settings names two regions', JSON.stringify(APP_REGIONS.settings) === '["sidebar","home"]');
registerAppSkins({ settings: { sidebar: stub('sidebar'), home: stub('home') } });

rt.render(h(rt.View, { style: { width: 1100, height: 700 } },
  h(AppWindowContext.Provider, { value: { id: 'settings-1', args: {} } }, h(Settings))));
await flush();

const m = () => seen.sidebar;
check('the sidebar stub is drawn', shown('stub-sidebar'));
check('Settings opens on the home stub', shown('stub-home') && m().section === null, String(m() && m().section));
check("Settings' own sidebar is not drawn", !shown('settings.search'));
check('the model has the four groups', m().groups.length === 4 && m().groups.map((g) => g.id).join() === 'look,connect,input,system', JSON.stringify(m().groups.map((g) => g.id)));
check('each group has its sections', m().groups[0].sections.some((x) => x.id === 'appearance') && m().groups[3].sections.some((x) => x.id === 'privacy'));
check('the model is frozen', Object.isFrozen(m()) && Object.isFrozen(m().groups[0].sections[0]) && Object.isFrozen(m().actions));

m().actions.open('clipboard');
await flush();
check("open: Settings' own page for the section", m().section === 'clipboard' && !shown('stub-home') && shown('settings.section.clipboard'), String(m().section));
check('open: the section is marked selected', m().groups[1].sections.find((x) => x.id === 'clipboard').selected === true);
m().actions.open('nosuchsection');
await flush();
check('open: an unknown section is refused', m().section === 'clipboard');
m().actions.open('firmware');
await flush();
check("open: a section Settings doesn't show here is refused", m().section === 'clipboard');
m().actions.home();
await flush();
check('home: the categories again', m().section === null && shown('stub-home'));
m().actions.search('settings.section.sound');
await flush();
check('search: only the matching group', m().groups.length === 1 && m().groups[0].sections.map((x) => x.id).join() === 'sound', JSON.stringify(m().groups.map((g) => g.sections.map((x) => x.id))));
m().actions.search('');
await flush();

settings.set({ themeParts: { layouts: false } });
await flush();
check("layouts off: Settings' own sidebar", shown('settings.search') && !shown('stub-sidebar'));
check('layouts off: no home page, a section is open', !shown('stub-home'));
settings.set({ themeParts: { layouts: true } });
await flush();
check('layouts on again: the stubs come back', shown('stub-sidebar'));
check('no React errors', !errs.length, JSON.stringify(errs.slice(0, 2)));
process.exit(0);
