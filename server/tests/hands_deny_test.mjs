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
// the person-only controls outside the denied pages and apps (security scan F1, F5)
const has = (f, re) => re.test(readFileSync(`${root}${f}`, 'utf8'));
yes('the Terminal\'s Passwords bar is handsDeny', has('apps/terminal/SecretBar.jsx', /<View handsDeny style/));
yes('the Terminal\'s save-to-Passwords bar is handsDeny', has('apps/terminal/SaveBar.jsx', /<View handsDeny style/));
yes('Monitor\'s per-app firewall switch is handsDeny', has('apps/monitor/Network.jsx', /<View handsDeny onClick=\{\(e\) => \{ e\.stopPropagation\(\); flip\(\); \}\}/));
yes('the firewall rule sheet is handsDeny', (readFileSync(`${root}apps/monitor/RuleSheet.jsx`, 'utf8').match(/<View handsDeny/g) || []).length === 2);
yes('the blocklists are handsDeny', has('apps/monitor/Blocklists.jsx', /<View handsDeny style/));
yes('Sentinela\'s blocklist Remove is handsDeny', has('apps/settings/Sentinela.jsx', /<Button small handsDeny label=\{tr\('sentinela\.block\.remove'\)\}/));
yes('Tab skips person-only controls for an agent', has('sdk/focus.js', /agentKey\(\) && to && deniedUp\(to\)/));
yes('a person-only control swallows an agent\'s key', has('sdk/focus.js', /if \(agentKey\(\) && deniedUp\(n\)\) return true;/));
yes('a person-only app is never opened by an agent', has('sdk/hands.jsx', /if \(appDenied\(app\.id\)\) return \{ error: 'hidden-from-agent' \}/));
yes('a person-only app in front takes no agent key', has('sdk/hands.jsx', /frontDenied = !!\(f && appDenied\(appOfId\(f\)\)\)/));
console.log(`hands-deny-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
