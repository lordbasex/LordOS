// hands_deny_test.mjs — the surfaces an AI agent never reaches (sdk/handsdeny.js, docs/HANDS.md "Never"): every key names
// a real Settings page or app (a dead key fails), the pages and apps that must be there are, and the places that use
// the lists really use them (no local copy that could drift).
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { PERSON_ONLY_SETTINGS, PERSON_ONLY_APPS, settingsPageDenied, appDenied } from '../../sdk/handsdeny.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
let pass = 0, fail = 0;
const yes = (name, v) => { if (v) pass++; else { fail++; console.log(`FAIL ${name}`); } };

const settingsSrc = readFileSync(`${root}apps/settings/index.jsx`, 'utf8');
const groups = settingsSrc.slice(settingsSrc.indexOf('const GROUPS'), settingsSrc.indexOf('const SECTIONS'));
const sections = new Set([...groups.matchAll(/\[\s*'([a-z]+)'\s*,/g)].map((m) => m[1]));
yes('Settings has its pages', sections.size > 20);
for (const id of PERSON_ONLY_SETTINGS) yes(`Settings page "${id}" exists`, sections.has(id));
for (const id of PERSON_ONLY_APPS) yes(`app "${id}" exists`, existsSync(`${root}apps/${id}/manifest.json`));
for (const id of ['network', 'sharing', 'cloud', 'privacy', 'users', 'webapps', 'flatpak', 'updates', 'session'])
  yes(`"${id}" is the person's own`, settingsPageDenied(id));
for (const id of ['store', 'passwords', 'backup', 'disks', 'imager']) yes(`the ${id} app is the person's own`, appDenied(id) && appDenied(`${id}#2`));
yes('an ordinary page and app are not', !settingsPageDenied('appearance') && !appDenied('notes'));
// the firewall and its rules live in Settings → Privacy (Monitor registers them there)
const monitor = readFileSync(`${root}apps/monitor/index.jsx`, 'utf8');
yes('the firewall views are Privacy sections', /registerPrivacySection\(\{ id: 'firewall'/.test(monitor) && /registerPrivacySection\(\{ id: 'fwrules'/.test(monitor));
// the places that apply the lists use them, never a copy
yes('Settings uses settingsPageDenied', /handsDeny=\{settingsPageDenied\(section\)\}/.test(settingsSrc) && !/PERSON_ONLY\s*=/.test(settingsSrc));
yes('every app window uses appDenied', /handsDeny=\{appDenied\(app\.id\)\}/.test(readFileSync(`${root}sdk/components.jsx`, 'utf8')));
yes('every destructive confirmation is handsDeny', /handsDeny=\{handsDeny \?\? !!destructive\}/.test(readFileSync(`${root}sdk/controls.jsx`, 'utf8')));
yes('the trust dialog is handsDeny', /ConfirmDialog cautious handsDeny/.test(readFileSync(`${root}apps/settings/ThemeGallery.jsx`, 'utf8')));
yes('the power dialog is handsDeny', /handsDeny: true/.test(readFileSync(`${root}sdk/power.jsx`, 'utf8')));
yes('Settings → AI → Hands is handsDeny', /<View handsDeny style/.test(readFileSync(`${root}apps/settings/AI.jsx`, 'utf8')));
console.log(`hands-deny-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
