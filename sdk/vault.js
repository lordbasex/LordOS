// vault.js — Passwords and the system keyring: talks to lord.vault (the confined helper
// lord-vault, see docs/PASSWORDS.md). Everything is asynchronous: requests return promises and a
// timer reads the replies (lord.vault.poll); the desktop never waits.
//
//   vaultAvailable()                                is lord-vault there?
//   vaultRequest(op, args, { files })               → promise with the reply ({ ok, … }) or an error with .code
//   onVaultEvent(fn)                                events: locked, unlocked, changed, touch, ss-prompt, keyring-mismatch, crash
//   keyring.set(app, { service, account, secret, label }) / get(app, { service, account }) / remove / list
//       the session keyring, per app: an app only sees its own items (unless the user shares them);
//       inside an isolated .lpk app it goes through the broker and the owner is the app's own id
//   autofill(app, { url | service })                → [{ id, title, username }] from Passwords (if it is unlocked)
//   <KeyringPrompt />                               system prompt to unlock the keyring (mounted by startShell)
//   <AutofillButton app service|url|ssid onFill />   "fill from Passwords" next to a login field
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { TextInput, Button } from './components.jsx';
import { Overlay } from './layers.js';

const has = () => typeof lord !== 'undefined' && typeof lord.vault !== 'undefined';
export const vaultAvailable = () => has() && lord.vault.available();

const waiting = new Map();          // id → { resolve, reject }
const listeners = new Set();
let fast = null, slow = null;

function dispatch(m) {
  if (m.kind === 'evt') {
    for (const fn of listeners) { try { fn(m.data || {}); } catch (e) { console.log(`vault: ${e.message}`); } }
    if (m.data?.ev === 'crash') for (const [id, w] of waiting) { waiting.delete(id); w.reject(Object.assign(new Error('crash'), { code: 'crash' })); }
    return;
  }
  const w = waiting.get(m.id);
  if (!w) return;
  waiting.delete(m.id);
  const d = m.data || {};
  if (d.error) w.reject(Object.assign(new Error(d.error), { code: d.error, data: d }));
  else w.resolve(d);
}

function pump() {
  let s = null;
  try { s = lord.vault.poll(); } catch (e) { s = null; }
  if (s) {
    let list = [];
    try { list = JSON.parse(s); } catch (e) { list = []; }
    for (const m of list) dispatch(m);
  }
  if (!waiting.size && fast) { clearInterval(fast); fast = null; }
}

// Events (auto-lock, Secret Service notices) arrive even when nobody is waiting for anything.
function startSlow() { if (!slow && has()) slow = setInterval(pump, 600); }

export function vaultRequest(op, args = {}, { files, fds } = {}) {
  if (!has()) return Promise.reject(Object.assign(new Error('unavailable'), { code: 'unavailable' }));
  let id;
  // files: paths opened by lord-server (shell); fds: files picked in the system dialog (app process)
  try { id = lord.vault.request(JSON.stringify({ op, ...args }), files || fds ? { files, fds } : undefined); }
  // lord.vault.request throws translated errors with a code (server/src/ui/js_err.h)
  catch (e) { return Promise.reject(Object.assign(new Error(e.message), { code: e.code === 'vault.unavailable' ? 'unavailable' : 'file' })); }
  startSlow();
  return new Promise((resolve, reject) => {
    waiting.set(id, { resolve, reject });
    if (!fast) fast = setInterval(pump, 8);
  });
}

export function onVaultEvent(fn) {
  startSlow();
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// ---- the session keyring, per app ----
// In an isolated .lpk app (lord-server --app <id>) there is no lord.vault: lord.keyring goes through
// the broker, and lord-server sets the owner from the verified app id ("app.<id>"), so the `app`
// argument is ignored there. Reading another app's item makes the system ask the user first.
// Errors carry the same short codes in both cases (locked, no-keyring, not-found, not-shared,
// unavailable); `errorCode` keeps the full code (keyring.locked…) and `message` is translated.

const hasAppKeyring = () => typeof lord !== 'undefined' && !has() && lord.keyring && typeof lord.keyring.request === 'function';
const KR_SHORT = { 'keyring.locked': 'locked', 'keyring.noKeyring': 'no-keyring', 'keyring.notFound': 'not-found',
  'keyring.notShared': 'not-shared', 'keyring.badRequest': 'bad-request', 'keyring.saveFailed': 'save', 'vault.unavailable': 'unavailable' };

function appKeyring(op, service = '', account = '', label = '', secret) {
  return Promise.resolve().then(() => {
    try { return JSON.parse(lord.keyring.request(op, String(service), String(account), String(label), secret == null ? '' : String(secret))); }
    catch (e) { throw Object.assign(e, { errorCode: e.code, code: KR_SHORT[e.code] || 'error' }); }
  });
}

export const keyring = {
  set: (app, { service, account = '', secret, label = '' }) => (hasAppKeyring()
    ? appKeyring('set', service, account, label, String(secret ?? ''))
    : vaultRequest('kr-set', { owner: app, service, account, secret: String(secret ?? ''), label })),
  get: async (app, { service, account = '' }) => {
    try { const r = hasAppKeyring() ? await appKeyring('get', service, account) : await vaultRequest('kr-get', { owner: app, service, account }); return r.item; }
    catch (e) { if (e.code === 'not-found') return null; throw e; }
  },
  remove: (app, { service, account = '' }) => (hasAppKeyring() ? appKeyring('delete', service, account) : vaultRequest('kr-del', { owner: app, service, account })),
  list: async (app) => (hasAppKeyring() ? await appKeyring('list') : await vaultRequest('kr-list', { owner: app })).items || [],
  // the user agreed that `app` may use another app's item (the system asks, not the app); not
  // available inside an isolated app: there lord-server asks the user by itself on get
  share: (id, app, allow = true) => (hasAppKeyring()
    ? Promise.reject(Object.assign(new Error('denied'), { code: 'denied' }))
    : vaultRequest('kr-share', { admin: true, id, app, allow })),
};

// ---- autofill in the login fields of native apps ----

const hostOf = (u) => String(u || '').replace(/^[a-z]+:\/\//i, '').split(/[/:?#]/)[0].toLowerCase().replace(/^www\./, '');

// Passwords entries that match a site, a service or a Wi-Fi network (only while the vault is unlocked).
export async function autofill(app, { url, service, ssid } = {}) {
  const host = hostOf(url || service);
  if (!host && !ssid) return [];
  let list;
  try { list = (await vaultRequest('list')).entries || []; } catch (e) { return []; }
  const hit = (e) => (ssid ? e.cat === 'wifi' && e.sub === ssid
    : hostOf(e.url) === host || hostOf(e.url).endsWith(`.${host}`) || (hostOf(e.url) && host.endsWith(`.${hostOf(e.url)}`)) ||
      String(e.title).toLowerCase().includes(host.split('.')[0]));
  return list.filter((e) => !e.trash && hit(e)).map((e) => ({ id: e.id, title: e.title, username: e.sub || '' }));
}

// The user picked one: username and password (the password leaves lord-vault only now).
export async function autofillReveal(id) {
  const e = (await vaultRequest('get', { id, reveal: true })).entry;
  const f = (k) => e.f.find((x) => x.k === k)?.v || '';
  return { username: f('username') || f('email'), password: f('password') };
}

// ---- system prompt: unlock the keyring (Secret Service asked for it) ----

const prompts = createStore([]);          // [{ prompt }]
if (has()) {
  onVaultEvent((ev) => {
    if (ev.ev === 'ss-prompt') prompts.set((q) => [...q, { prompt: ev.prompt }]);
  });
}

export function KeyringPrompt() {
  const th = useTheme();
  const q = useStore(prompts);
  const [pw, setPw] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const cur = q[0];
  // a new prompt clears the field; more of them queuing up behind it doesn't (they used to wipe what
  // was being typed)
  useEffect(() => { setPw(''); setErr(''); }, [cur?.prompt]);
  if (!q.length) return null;
  // every queued prompt asks for the same keyring: one answer goes to all of them, so Cancel doesn't
  // bring the same box back once per request an app made
  const done = (ok) => {
    const all = prompts.get();
    for (const p of all) { try { lord.vault.promptDone(p.prompt, ok); } catch (e) { /* */ } }
    prompts.set((l) => l.filter((p) => !all.includes(p)));
  };
  const submit = async () => {
    setBusy(true);
    try { await vaultRequest('unlock', { v: 'login', method: 'session', password: pw }); setBusy(false); done(true); }
    catch (e) { setBusy(false); setErr(t('passwords.keyring.wrong')); }
  };
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100001 } },
  h(View, { style: { width: 380, padding: 18, gap: 10, borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
    boxShadow: '0 20px 60px rgba(0,0,0,0.5)' } },
  h(Text, { style: { fontSize: 15, fontWeight: 700, color: th.text } }, t('passwords.keyring.title')),
  h(Text, { style: { fontSize: 12, color: th.textDim } }, t('passwords.keyring.body')),
  h(TextInput, { value: pw, onChange: setPw, secure: true, autoFocus: true, placeholder: t('passwords.keyring.placeholder'), onSubmit: submit, onCancel: () => done(false) }),
  err ? h(Text, { style: { fontSize: 12, color: '#ff453a' } }, err) : null,
  h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8 } },
    h(Button, { label: t('passwords.cancel'), onClick: () => done(false) }),
    h(Button, { label: busy ? t('passwords.unlock.working') : t('passwords.keyring.unlock'), primary: true, onClick: submit }))));
}

// ---- "fill from Passwords" button for native login fields ----
//
//   <AutofillButton app="settings" service="wifi:Casa" ssid="Casa" onFill={({ username, password }) => …} />
// Offers what the app saved in the session keyring (service) and matching Passwords entries.
// The secret leaves lord-vault only when the user picks one.
export function AutofillButton({ app, service, url, ssid, onFill }) {
  const th = useTheme();
  const [open, setOpen] = useState(false);
  const [items, setItems] = useState(null);
  const load = async () => {
    const out = [];
    if (service) { try { const k = await keyring.get(app, { service }); if (k) out.push({ kind: 'kr', title: k.label || service, username: k.account, secret: k.secret }); } catch (e) { /* locked */ } }
    for (const e of await autofill(app, { url, service: url ? undefined : service, ssid })) out.push({ kind: 'pw', ...e });
    setItems(out);
  };
  const pick = async (it) => {
    setOpen(false);
    if (it.kind === 'kr') { onFill({ username: it.username, password: it.secret }); return; }
    try { onFill(await autofillReveal(it.id)); } catch (e) { /* locked */ }
  };
  const btn = useRef(null);
  // the list goes in the overlay layer (sdk/layers.js), above the rest of the form
  return h(View, null,
    h(View, { ref: btn, onClick: () => { setOpen(!open); if (!open) load(); }, style: { width: 30, height: 30, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
      backgroundColor: th.control } }, h(Text, { style: { fontSize: 14 } }, '🔑')),
    open ? h(Overlay, { anchor: btn, onClose: () => setOpen(false) }, h(View, { style: { position: 'absolute', top: 34, right: 0, width: 260, padding: 4, borderRadius: 9, backgroundColor: th.windowBg,
      border: `1px solid ${th.separator}`, boxShadow: '0 10px 28px rgba(0,0,0,0.4)' } },
      h(Text, { style: { fontSize: 11, fontWeight: 600, color: th.textDim, padding: 6 } }, t('passwords.autofill.title')),
      items == null ? null : !items.length ? h(Text, { style: { fontSize: 12, color: th.textDim, padding: 6 } }, t('passwords.autofill.none'))
        : items.map((it, i) => h(View, { key: i, onClick: () => pick(it), style: { padding: 6, borderRadius: 6 } },
          h(Text, { style: { fontSize: 13, color: th.text } }, it.title),
          it.username ? h(Text, { style: { fontSize: 11, color: th.textDim } }, it.username) : null)))) : null);
}
