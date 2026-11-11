// mail.js — Mail, and the shared channel for Calendar, Contacts and Reminders: everything goes
// through lord.mail to the sandboxed helper lord-maild (docs/MAIL.md). All calls are
// asynchronous: requests return promises; a timer reads results and events (lord.mail.poll),
// so the desktop never waits for the network.
//
//   mailAvailable()                          is lord-maild installed?
//   mailRequest(op, args, { files })         → promise with the reply, or an Error with .code; files are paths, or
//                                            { fd } in an isolated app (lord.mail.appMode)
//   onMailEvent(fn)                          events: new, changed, folders, status, need-auth, auth-failed,
//                                            cert, sent, send-failed, pim-changed, crash
//   mailConfig.get() / mailConfig.save(cfg)  the accounts file (no passwords: those live in the keyring)
//   mailAccounts.add(account, password)      saves the account, the password in the keyring, and starts it
//   mailAccounts.remove(id) / setPassword(id, pw) / trustCertificate(id, which, sha256)
//                                            (setPassword → null once the keyring kept it, else 'locked',
//                                            'missing' or 'error': the password then lasts this session only)
//   autoconfig(email)                        our provider table (Gmail, Yahoo and others) or a guess
//   OAUTH_PROVIDERS                          prepared, disabled until you register a client ID
//   DAV_PRESETS / davPreset(id)              calendar and contacts services for Add Account (generic, or a provider)
//   openDavSetup(preset)                     opens Mail's Add Account sheet for calendars and contacts (Calendar, Contacts)
//   openMailAccounts(acct, { remove })       opens Mail's accounts panel on that account (remove: asks to remove it)
//   mailStatus (store)                       { accounts: { id: { status, error } }, unread, rev, certs: [] }
//   startMail()                              called by startShell: new-mail notifications, passwords on demand
import { createStore } from './store.js';
import { keyring, askKeyringUnlock, onVaultEvent } from './vault.js';
import { keyringTrouble, askUnlock, authAfterStatus } from './mailauthmodel.js';
import { notify } from './notify.js';
import { t } from './i18n.js';
import { wm, getApp } from './windows.js';
import { isAppOff } from './appsoff.js';

const has = () => typeof lord !== 'undefined' && typeof lord.mail !== 'undefined';
export const mailAvailable = () => has() && lord.mail.available();

// Mail, Calendar, Contacts and Reminders in their own process (lord.mail.appMode): files come from
// the system file dialog as fds, never as paths.
export const isolatedPim = () => has() && !!lord.mail.appMode;
// → { fd, name, size } | null; the system draws the dialog, this process only gets the fd
export function pickFileFd() {
  if (typeof lord === 'undefined' || !lord.broker || typeof lord.broker.pick !== 'function') return null;
  const r = lord.broker.pick(false, '');
  if (!r) return null;
  const size = typeof lord.broker.fdSize === 'function' ? lord.broker.fdSize(r.fd) : -1;
  return { fd: r.fd, name: r.name, size: size > 0 ? size : 0 };
}
export const closeFileFd = (f) => { if (f && typeof f === 'object' && f.fd >= 0) { try { lord.broker.fdClose(f.fd); } catch (e) { /* closed */ } } };

const waiting = new Map();
const listeners = new Set();
let fast = null, slow = null;

function dispatch(m) {
  if (m.type === 'evt') {
    for (const fn of listeners) { try { fn(m.data || {}); } catch (e) { console.log(`mail: ${e.message}`); } }
    return;
  }
  if (m.type === 'crash') {
    for (const [id, w] of waiting) { waiting.delete(id); w.reject(Object.assign(new Error('crash'), { code: 'crash' })); }
    for (const fn of listeners) { try { fn({ ev: 'crash' }); } catch (e) { /* listener */ } }
    return;
  }
  const w = waiting.get(m.id);
  if (!w) return;
  waiting.delete(m.id);
  const d = m.data || {};
  if (m.type === 'err' || d.error) w.reject(Object.assign(new Error(d.detail || d.error || 'error'), { code: d.error || 'error', data: d }));
  else w.resolve(d);
}

function pump() {
  let list = null;
  try { list = lord.mail.poll(); } catch (e) { list = null; }
  if (Array.isArray(list)) for (const m of list) dispatch(m);
  if (!waiting.size && fast) { clearInterval(fast); fast = null; }
}

function startSlow() { if (!slow && has()) slow = setInterval(pump, 500); }

export function mailRequest(op, args = {}, { files } = {}) {
  if (!has()) return Promise.reject(Object.assign(new Error('unavailable'), { code: 'unavailable' }));
  let id;
  // a file is a path (in the session) or { fd } (the isolated apps: an fd from the system file dialog)
  const list = files && files.length ? files.map((f) => (f && typeof f === 'object' ? f.fd : f)) : undefined;
  try { id = lord.mail.request(JSON.stringify({ op, ...args }), list); }
  catch (e) { return Promise.reject(Object.assign(new Error(e.message), { code: 'file' })); }
  if (id < 0) return Promise.reject(Object.assign(new Error('unavailable'), { code: 'unavailable' }));
  startSlow();
  return new Promise((resolve, reject) => {
    waiting.set(id, { resolve, reject });
    if (!fast) fast = setInterval(pump, 12);
  });
}

export function onMailEvent(fn) {
  startSlow();
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// ---- accounts ----

export const mailConfig = {
  get() {
    if (!has()) return { accounts: [] };
    try { const c = JSON.parse(lord.mail.config() || '{}'); return { accounts: [], ...c }; } catch (e) { return { accounts: [] }; }
  },
  save(cfg) { if (has()) lord.mail.saveConfig(JSON.stringify(cfg, null, 1)); bumpStatus(); },
};

export const mailStatus = createStore({ accounts: {}, unread: 0, rev: 0, certs: [], authFailed: {} });
const bumpStatus = () => mailStatus.set((s) => ({ ...s, rev: s.rev + 1 }));

const service = (id) => `mail:${id}`;

// What the store remembers about an account that is gone (or added again under the same id, as the
// setup sheet does after Edit Details): no old "wrong password" or certificate shown for the new one.
function forgetStatus(id) {
  mailStatus.set((s) => {
    const accounts = { ...s.accounts }, authFailed = { ...s.authFailed };
    delete accounts[id];
    delete authFailed[id];
    return { ...s, accounts, authFailed, certs: s.certs.filter((c) => c.acct !== id) };
  });
}

// → { password, trouble }: the saved password (or null), and why the keyring could not be read
// (keyringTrouble: 'locked', 'missing', 'error'; null when it answered)
async function passwordFor(acct) {
  try { const it = await keyring.get('mail', { service: service(acct.id), account: acct.user }); return { password: it ? it.secret : null, trouble: null }; }
  catch (e) { return { password: null, trouble: keyringTrouble(e.code || 'error') }; }
}

// Keeps the password in the keyring: → null, or the keyringTrouble that kept it out (the account still
// works for this session; Mail asks again at the next login).
async function savePassword(acct, password) {
  try {
    await keyring.set('mail', { service: service(acct.id), account: acct.user, secret: password, label: `${t('mail.keyring.label')} — ${acct.email || acct.user}` });
    mailStatus.set((s) => (s.keyringError ? { ...s, keyringError: null } : s));
    return null;
  } catch (e) {
    const trouble = keyringTrouble(e.code || 'error');
    mailStatus.set((s) => ({ ...s, keyringError: trouble }));
    return trouble;
  }
}

const slug = (s) => String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 24) || 'account';

export const mailAccounts = {
  list: () => mailConfig.get().accounts,
  get: (id) => mailConfig.get().accounts.find((a) => a.id === id),
  newId(base) {
    const ids = new Set(mailConfig.get().accounts.map((a) => a.id));
    let id = slug(base), n = 2;
    while (ids.has(id)) id = `${slug(base).slice(0, 20)}-${n++}`;
    return id;
  },
  // account: { id?, kind: 'mail'|'dav', name, email, fullName, user, imap, smtp, url, carddav, cal, card, sentSaved, signature, color }
  // (dav: url is where calendars are found, and contacts too unless carddav names their own address)
  async add(account, password) {
    const acct = { kind: 'mail', auth: 'password', ...account, id: account.id || mailAccounts.newId(account.email || account.name || account.url) };
    forgetStatus(acct.id);
    // the keyring may be locked or missing (no login password, e.g. autologin): the account still
    // works for this session and Mail asks again next time
    if (password) await savePassword(acct, password);
    const cfg = mailConfig.get();
    cfg.accounts = [...cfg.accounts.filter((a) => a.id !== acct.id), acct];
    mailConfig.save(cfg);
    lord.mail.start();
    if (password) await mailRequest('auth', { acct: acct.id, password }).catch(() => {});
    return acct.id;
  },
  async update(id, patch) {
    const cfg = mailConfig.get();
    cfg.accounts = cfg.accounts.map((a) => (a.id === id ? { ...a, ...patch } : a));
    mailConfig.save(cfg);
  },
  async remove(id) {
    const cfg = mailConfig.get();
    const a = cfg.accounts.find((x) => x.id === id);
    cfg.accounts = cfg.accounts.filter((x) => x.id !== id);
    mailConfig.save(cfg);
    forgetStatus(id);
    if (a) await keyring.remove('mail', { service: service(id), account: a.user }).catch(() => {});
  },
  async setPassword(id, password) {
    const a = mailAccounts.get(id);
    if (!a) return null;
    const trouble = await savePassword(a, password);
    mailStatus.set((s) => ({ ...s, authFailed: { ...s.authFailed, [id]: false } }));
    await mailRequest('auth', { acct: id, password });
    return trouble;
  },
  // the user looked at the certificate and chose to trust exactly this one
  trustCertificate(id, which, sha256) {
    const a = mailAccounts.get(id);
    if (!a) return;
    mailAccounts.update(id, { trust: { ...(a.trust || {}), [which]: sha256 } });
    mailStatus.set((s) => ({ ...s, certs: s.certs.filter((c) => c.acct !== id) }));
  },
  testSmtp: (id) => mailRequest('send', { acct: id, test: true }),
};

// ---- autoconfig: our own provider table (Mozilla ISPDB style, built in, no lookups) ----

const PROVIDERS = [
  { domains: ['gmail.com', 'googlemail.com'], name: 'Gmail', imap: { host: 'imap.gmail.com', port: 993, security: 'ssl' },
    smtp: { host: 'smtp.gmail.com', port: 465, security: 'ssl' }, sentSaved: true, appPassword: 'https://myaccount.google.com/apppasswords', oauth: 'google',
    caldav: 'https://apidata.googleusercontent.com/caldav/v2/', davNote: 'oauth' },
  { domains: ['outlook.com', 'hotmail.com', 'live.com', 'msn.com', 'outlook.es', 'hotmail.es'], name: 'Outlook.com',
    imap: { host: 'outlook.office365.com', port: 993, security: 'ssl' }, smtp: { host: 'smtp-mail.outlook.com', port: 587, security: 'starttls' },
    sentSaved: true, appPassword: 'https://account.live.com/proofs/AppPassword', oauth: 'microsoft' },
  { domains: ['yahoo.com', 'ymail.com', 'yahoo.es', 'yahoo.com.ar', 'yahoo.com.br', 'rocketmail.com'], name: 'Yahoo',
    imap: { host: 'imap.mail.yahoo.com', port: 993, security: 'ssl' }, smtp: { host: 'smtp.mail.yahoo.com', port: 465, security: 'ssl' },
    sentSaved: false, appPassword: 'https://login.yahoo.com/account/security' },
  { domains: ['icloud.com', 'me.com', 'mac.com'], name: 'iCloud', imap: { host: 'imap.mail.me.com', port: 993, security: 'ssl' },
    smtp: { host: 'smtp.mail.me.com', port: 587, security: 'starttls' }, sentSaved: false, appPassword: 'https://appleid.apple.com/account/manage',
    caldav: 'https://caldav.icloud.com/', carddav: 'https://contacts.icloud.com/' },
];

export function autoconfig(email) {
  const domain = String(email || '').split('@')[1]?.toLowerCase().trim();
  if (!domain) return null;
  const p = PROVIDERS.find((x) => x.domains.includes(domain));
  if (p) return { ...p, user: email, known: true };
  // a guess for custom domains: the most common names; the setup screen lets you change them
  return { name: domain, imap: { host: `imap.${domain}`, port: 993, security: 'ssl' }, smtp: { host: `smtp.${domain}`, port: 465, security: 'ssl' },
    sentSaved: false, user: email, known: false };
}

// Calendar and contacts services offered by Add Account → Calendars & Contacts. Provider names are
// data shown as {name} (like PROVIDERS), never words in the catalogs. `oauth`: the provider allows
// no app passwords for these services; it waits for a registered client ID (OAUTH_PROVIDERS,
// docs/MAIL.md, "Google calendars and contacts"), so the sheet shows "not available yet".
export const DAV_PRESETS = [
  { id: 'other' },
  { id: 'icloud', name: 'iCloud', caldav: 'https://caldav.icloud.com/', carddav: 'https://contacts.icloud.com/',
    appPassword: 'https://appleid.apple.com/account/manage', userIsEmail: true },
  { id: 'google', name: 'Google', oauth: 'google', caldav: 'https://apidata.googleusercontent.com/caldav/v2/',
    carddav: 'https://www.googleapis.com/carddav/v1/principals/' },
];
export const davPreset = (id) => DAV_PRESETS.find((p) => p.id === id) || DAV_PRESETS[0];

// Calendar and Contacts add accounts through Mail's sheet (the accounts file and the keyring item
// belong to Mail): this opens it in "Calendars & Contacts" mode.
export function openDavSetup(preset = 'other') {
  wm.open('mail', { args: { setup: 'dav', preset, nonce: Date.now() } });
}

// Calendar and Contacts cannot write the accounts file (sec/brokermail.c: Mail only), so their
// Account Settings… and Remove Account… open Mail's accounts panel on that account.
export function openMailAccounts(acct, { remove = false } = {}) {
  wm.open('mail', { args: { accounts: String(acct || ''), remove: !!remove, nonce: Date.now() } });
}

// OAuth2 (XOAUTH2) is prepared but OFF: large mail providers only hand out tokens to apps whose
// client ID is registered in their developer consoles. Put yours here and enable it
// in docs/MAIL.md, "OAuth"; until then these accounts use app passwords.
export const OAUTH_PROVIDERS = {
  google: { enabled: false, clientId: '', authUrl: 'https://accounts.google.com/o/oauth2/v2/auth', tokenUrl: 'https://oauth2.googleapis.com/token',
    scopes: ['https://mail.google.com/', 'https://www.googleapis.com/auth/calendar', 'https://www.googleapis.com/auth/carddav'], needs: 'client-id' },
  microsoft: { enabled: false, clientId: '', authUrl: 'https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize',
    tokenUrl: 'https://login.microsoftonline.com/consumers/oauth2/v2.0/token',
    scopes: ['https://outlook.office.com/IMAP.AccessAsUser.All', 'https://outlook.office.com/SMTP.Send', 'offline_access'], needs: 'client-id' },
};

// ---- the session side: passwords on demand, notifications, unread count ----

let started = false;

async function refreshCounts() {
  try { const c = await mailRequest('counts'); mailStatus.set((s) => ({ ...s, unread: c.unread || 0 })); } catch (e) { /* not running */ }
}

function openMessage(acct, folder, uid) {
  if (getApp('mail')) wm.open('mail', { args: { acct, folder, uid, nonce: Date.now() } });
}

// In the isolated Mail app (lord-server --app mail, lord.mail.appMode) startMail never runs: the
// session keeps notifications, the unread count and passwords on demand. The app keeps its own copy
// of the account status from the events lord-server forwards to it (sec/brokermail.c), seeded with
// lord-maild's current status.
let tracking = false;
export function trackMailStatus() {
  if (tracking || !has() || !lord.mail.appMode) return;
  tracking = true;
  onMailEvent(async (ev) => {
    if (ev.ev === 'status') {
      mailStatus.set((s) => ({ ...s, accounts: { ...s.accounts, [ev.acct]: { status: ev.status, error: ev.error } },
        authFailed: authAfterStatus(s.authFailed, ev.acct, ev.status) }));
    } else if (ev.ev === 'cert') mailStatus.set((s) => ({ ...s, certs: [...s.certs.filter((c) => !(c.acct === ev.acct && c.host === ev.host)), ev] }));
    else if (ev.ev === 'auth-failed') mailStatus.set((s) => ({ ...s, authFailed: { ...s.authFailed, [ev.acct]: true } }));
    else if (ev.ev === 'need-auth') {
      // the session answers too (and asks to unlock the keyring); here it only matters whether a
      // password can be read at all. A mark set here goes away with the account's next good status.
      const a = mailAccounts.get(ev.acct);
      const pw = a ? (await passwordFor(a)).password : null;
      if (pw) mailRequest('auth', { acct: ev.acct, password: pw }).catch(() => {});
      else mailStatus.set((s) => ({ ...s, authFailed: { ...s.authFailed, [ev.acct]: true } }));
    } else if (ev.ev === 'new' || ev.ev === 'changed' || ev.ev === 'folders' || ev.ev === 'ready') refreshCounts();
  });
  lord.mail.start();
  mailRequest('status').then((st) => {
    const accounts = {};
    for (const a of st.accounts || []) accounts[a.id] = { status: a.status, error: a.error };
    mailStatus.set((s) => ({ ...s, accounts: { ...accounts, ...s.accounts } }));
  }).catch(() => {});
  refreshCounts();
}

// Calendar-and-contacts accounts added before 0.8.8.4 from a provider that keeps contacts on their
// own address had only the calendars' one, so their contacts never synced. Once, in the accounts
// file (settings.json's MIGRATIONS don't reach it); idempotent.
function fixDavAccounts(cfg) {
  let changed = false;
  const accounts = cfg.accounts.map((a) => {
    if (a.kind !== 'dav' || a.carddav) return a;
    const p = DAV_PRESETS.find((x) => x.carddav && !x.oauth && x.caldav && String(a.url || '').replace(/\/+$/, '') === x.caldav.replace(/\/+$/, ''));
    if (!p) return a;
    changed = true;
    return { ...a, carddav: p.carddav };
  });
  if (changed) mailConfig.save({ ...cfg, accounts });
  return changed ? { ...cfg, accounts } : cfg;
}

// lord-maild asks for each account's password once (need-auth). When the session keyring is not open
// yet (automatic login, a login password changed outside LordOS, or lord-maild asking before
// lord-vault finished opening it at login), the accounts wait here: the system keyring prompt is
// shown once, and every waiting account is answered as soon as the keyring opens, from that prompt,
// a screen unlock with the password or the login itself (issue #63: before, the password was taken
// as missing and Mail asked for it again at every boot, while its "Save to Keyring" failed quietly).
const waitingKeyring = new Set();
let unlockAsked = false;
const ASK_AFTER_MS = 3000;    // lord-vault may still be opening it at login: no prompt for that

async function answerNeedAuth(id) {
  const a = mailAccounts.get(id);
  if (!a) { waitingKeyring.delete(id); return; }
  const { password, trouble } = await passwordFor(a);
  if (password) {
    waitingKeyring.delete(id);
    mailRequest('auth', { acct: id, password }).catch(() => {});
    return;
  }
  mailStatus.set((s) => ({ ...s, authFailed: { ...s.authFailed, [id]: true } }));
  if (trouble !== 'locked' && trouble !== 'missing') { waitingKeyring.delete(id); return; }
  waitingKeyring.add(id);
  if (!askUnlock(trouble, unlockAsked)) return;
  unlockAsked = true;
  setTimeout(() => { if (waitingKeyring.size) askKeyringUnlock(); }, ASK_AFTER_MS);
}

export function startMail() {
  if (started || !has() || !lord.mail.available()) return;
  started = true;
  const cfg = fixDavAccounts(mailConfig.get());
  onVaultEvent((ev) => {
    if (ev.ev !== 'unlocked' || ev.v !== 'login') return;
    for (const id of [...waitingKeyring]) {
      // the person typed the password in Mail meanwhile: lord-maild already has it
      const st = mailStatus.get().accounts[id]?.status;
      if (st === 'idle' || st === 'sync') waitingKeyring.delete(id);
      else answerNeedAuth(id);
    }
  });
  onMailEvent(async (ev) => {
    if (ev.ev === 'need-auth') {
      answerNeedAuth(ev.acct);
    } else if (ev.ev === 'auth-failed') {
      mailStatus.set((s) => ({ ...s, authFailed: { ...s.authFailed, [ev.acct]: true } }));
    } else if (ev.ev === 'status') {
      mailStatus.set((s) => ({ ...s, accounts: { ...s.accounts, [ev.acct]: { status: ev.status, error: ev.error } },
        authFailed: authAfterStatus(s.authFailed, ev.acct, ev.status) }));
    } else if (ev.ev === 'cert') {
      mailStatus.set((s) => ({ ...s, certs: [...s.certs.filter((c) => !(c.acct === ev.acct && c.host === ev.host)), ev] }));
    } else if (ev.ev === 'new') {
      if (isAppOff('mail')) return;                // Mail deactivated (Store → Installed): lord-maild pauses its accounts
      refreshCounts();
      const prefs = (mailConfig.get().notify ?? true);
      if (!prefs) return;
      const one = ev.msgs?.[0];
      if (ev.count === 1 && one) {
        notify({ app: 'mail', title: one.from, body: one.subject || t('mail.noSubject'), timeout: 8000,
          actions: [{ id: 'open', label: t('mail.notify.open'), run: () => openMessage(ev.acct, ev.folder, one.uid) }] });
      } else if (ev.count > 1) {
        notify({ app: 'mail', title: t('mail.notify.many', { count: ev.count }), body: ev.msgs.slice(0, 3).map((m) => `${m.from}: ${m.subject}`).join('\n'),
          timeout: 8000, actions: [{ id: 'open', label: t('mail.notify.open'), run: () => openMessage(ev.acct, ev.folder) }] });
      }
    } else if (ev.ev === 'changed' || ev.ev === 'folders') {
      refreshCounts();
    } else if (ev.ev === 'dav-conflict') {
      // the server's version won; ours was kept on this computer (docs/MAIL.md, "Conflicts")
      const app = ev.kind === 'contact' ? 'contacts' : ev.kind === 'todo' ? 'reminders' : 'calendar';
      notify({ app, title: t('mail.notify.davConflict'), body: t(`mail.notify.davConflictBody.${ev.kind === 'contact' ? 'contact' : 'item'}`, { title: ev.title || '', list: ev.collName || '' }),
        timeout: 12000, actions: getApp(app) ? [{ id: 'open', label: t('mail.notify.open'), run: () => wm.open(app, { args: { nonce: Date.now() } }) }] : [] });
    } else if (ev.ev === 'send-failed') {
      notify({ app: 'mail', title: t('mail.notify.sendFailed'), body: `${ev.subject || ''}\n${ev.error || ''}`, sticky: true });
    } else if (ev.ev === 'crash') {
      // lord-maild went away (crash or update): start it again; lord-server waits 3 s between tries
      setTimeout(() => { try { lord.mail.start(); } catch (e) { /* not available */ } }, 3500);
    } else if (ev.ev === 'ready') {
      refreshCounts();
    }
  });
  // lord-maild also keeps calendars and reminders: start it if there is anything to keep
  lord.mail.start();
  if (cfg.accounts.length) refreshCounts();
}
