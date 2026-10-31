// suggestions.js — "Suggested for your computer" (Store → Discover; docs/PACKAGES.md).
//
//   suggestions          store { list, fixture, ready, scanning } (useStore(suggestions))
//   refreshSuggestions({ firmware })   scans again (lord.hwinfo.scan; the kernel log through the
//                        read-only hw-firmware-missing action when the session can't read it)
//   reevaluate()         same facts, new package state (after an install)
//   dismissSuggestion(id)   "Don't suggest again"
//   markSuggestionsSeen()   the user has looked at Discover: the sidebar badge goes away
//   unseenSuggestions()     installable ones not looked at yet (the badge)
//
// Detection only reads the kernel's hardware files; the rules are data
// (assets/store/suggestions.json, sdk/suggestengine.js). Nothing is sent anywhere and nothing is
// installed without the user: Install goes through the Store's plan and pkg-install.
import { createStore } from './store.js';
import { settings } from './settings.js';
import { admin } from './admin.js';
import { notify } from './notify.js';
import { t } from './i18n.js';
import { wm } from './windows.js';
import { evaluate, firmwareFromLog, fresh } from './suggestengine.js';
import DATA from '../assets/store/suggestions.json';

const has = () => typeof lord !== 'undefined' && !!lord.hwinfo && !!lord.pkg;
const pkgReady = () => has() && lord.pkg.available();

export const SUGGEST_DEFAULTS = { dismissed: [], notified: [], seen: [] };
const prefs = (s = settings.get()) => ({ ...SUGGEST_DEFAULTS, ...(s.suggestions || {}) });
function setPrefs(patch) {
  settings.set((s) => ({ ...s, suggestions: { ...prefs(s), ...patch(prefs(s)) } }));
}

export const suggestions = createStore({ list: [], fixture: false, ready: false, scanning: false });
let facts = null;

const info = (name) => { try { return lord.pkg.info(name); } catch (e) { return null; } };
// an installable card only when its plan can be made (a dependency may live in a component that
// isn't enabled: nvidia-driver needs contrib as well as non-free)
const plannable = (names) => { try { return !!lord.pkg.plan({ install: names }).ok; } catch (e) { return true; } };

export function reevaluate() {
  if (!facts || !pkgReady()) { suggestions.set({ list: [], ready: !!facts }); return []; }
  const list = evaluate(DATA, facts, { info, check: plannable, dismissed: prefs().dismissed });
  suggestions.set({ list, fixture: !!facts.fixture, ready: true });
  return list;
}

// The kernel log through the root helper (only when the session can't read it, kernel.dmesg_restrict).
function firmwareViaHelper() {
  const lines = [];
  return admin.run('hw-firmware-missing', '', (l) => lines.push(l), { quiet: true })
    .then(() => firmwareFromLog(lines), () => null);
}

// The Argon ONE V5 case can only be seen once the USB-C controller is a host: looked for once on a Raspberry Pi 5
// with no case chosen, and again only while one was found and not kept. Its ports and sound stay on if it is there.
let caseAsked = false;
async function caseFound(f) {
  if (!(f.compatible || []).includes('raspberrypi,5-model-b') || !admin.available()) return null;
  let chosen = null;
  try { chosen = lord.fan.caseConfig(); } catch (e) { return null; }
  if (chosen !== 'none' || (prefs().caseChecked && !caseAsked)) return null;
  if (caseAsked === 'found') return 'argon-one-v5';
  if (caseAsked) return null;
  caseAsked = true;
  const said = await admin.run('rpi-case-detect', '', null, { quiet: true }).catch(() => '');
  if (String(said).includes('argon-one-v5')) { caseAsked = 'found'; return 'argon-one-v5'; }
  setPrefs(() => ({ caseChecked: true }));
  return null;
}

let running = null;
export function refreshSuggestions({ firmware = true } = {}) {
  if (!has()) return Promise.resolve([]);
  if (running) return running;
  suggestions.set({ scanning: true });
  running = (async () => {
    let f = null;
    try { f = lord.hwinfo.scan(); } catch (e) { f = null; }
    if (f && !f.fixture && f.firmware && !f.firmware.readable && firmware && admin.available()) {
      const missing = await firmwareViaHelper();
      if (missing) f.firmware = { readable: true, missing };
    }
    if (f) f.caseFound = await caseFound(f);
    facts = f;
    return reevaluate();
  })().finally(() => { running = null; suggestions.set({ scanning: false }); });
  return running;
}

export function dismissSuggestion(id) {
  setPrefs((p) => ({ dismissed: [...new Set([...p.dismissed, String(id)])].slice(-200) }));
  reevaluate();
}

export function unseenSuggestions(list = suggestions.get().list) {
  return fresh(list, prefs().seen);
}

export function markSuggestionsSeen(list = suggestions.get().list) {
  const ids = list.filter((s) => s.state === 'install' || s.state === 'setup').map((s) => s.id);
  const p = prefs();
  if (ids.every((id) => p.seen.includes(id))) return;
  setPrefs((q) => ({ seen: [...new Set([...q.seen, ...ids])].slice(-200), notified: [...new Set([...q.notified, ...ids])].slice(-200) }));
}

// ---- once per new suggestion: a notification (only in a user's desktop session) ----

function maybeNotify(list) {
  const p = prefs();
  const news = fresh(list, p.notified);
  if (!news.length) return;
  setPrefs((q) => ({ notified: [...new Set([...q.notified, ...news.map((s) => s.id)])].slice(-200) }));
  const first = t(`store.suggest.${news[0].text}.title`, news[0].params);
  notify({
    app: 'store',
    title: t('store.suggest.notify.title', { count: news.length }),
    body: news.length === 1 ? first : t('store.suggest.notify.body', { name: first, count: news.length - 1 }),
    actions: [{ id: 'open', label: t('store.suggest.notify.open'), run: () => wm.open('store', { args: { page: 'discover' } }) }],
  });
}

function check() {
  if (!pkgReady()) return;
  refreshSuggestions().then(maybeNotify, () => {});
}

if (has() && typeof lord.admin !== 'undefined' && typeof lord.greeter === 'undefined') {
  // a while after login (the desktop first), then every 6 hours (a new device, firmware installed…)
  setTimeout(() => { check(); setInterval(check, 6 * 3600 * 1000); }, 3 * 60 * 1000);
  admin.onEvent((text) => { if (text === 'pkg changed' && facts) reevaluate(); });
}
