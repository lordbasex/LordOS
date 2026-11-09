// sql.js — Databases: MySQL and MariaDB through lord.sql and the sandboxed helper lord-sql
// (docs/DATABASES.md). Every call is asynchronous; a timer reads replies (lord.sql.poll), so the
// desktop never waits for a server.
//
//   sqlAvailable()                       is lord-sql installed?
//   sqlRequest(op, args)                 → promise with the reply, or an Error with .code (and .data)
//   onSqlEvent(fn)                       events: closed, cert, crash
//   sqlFavorites.get() / save(file)      favorites.json (no passwords: those live in the keyring)
//   sqlPasswords.get(fav) / set(fav, pw) / remove(fav)
import { keyring } from './vault.js';
import { t } from './i18n.js';

const has = () => typeof lord !== 'undefined' && typeof lord.sql !== 'undefined';
export const sqlAvailable = () => has() && lord.sql.available();

const waiting = new Map();
const listeners = new Set();
let fast = null, slow = null;

function dispatch(m) {
  if (m.type === 'evt') {
    for (const fn of listeners) { try { fn(m.data || {}); } catch (e) { console.log(`sql: ${e.message}`); } }
    return;
  }
  const w = waiting.get(m.id);
  if (!w) return;
  waiting.delete(m.id);
  const d = m.data || {};
  if (m.type === 'err' || d.error) w.reject(Object.assign(new Error(d.message || d.error || 'error'), { code: d.error || 'error', data: d }));
  else w.resolve(d);
}

function pump() {
  let list = null;
  try { list = lord.sql.poll(); } catch (e) { list = null; }
  if (Array.isArray(list)) for (const m of list) dispatch(m);
  if (!waiting.size && fast) { clearInterval(fast); fast = null; }
}

function startSlow() { if (!slow && has()) slow = setInterval(pump, 500); }

export function sqlRequest(op, args = {}) {
  if (!has()) return Promise.reject(Object.assign(new Error('unavailable'), { code: 'unavailable' }));
  const id = lord.sql.request(JSON.stringify({ op, ...args }));
  if (id < 0) return Promise.reject(Object.assign(new Error('unavailable'), { code: 'unavailable' }));
  startSlow();
  return new Promise((resolve, reject) => {
    waiting.set(id, { resolve, reject });
    if (!fast) fast = setInterval(pump, 12);
  });
}

export function onSqlEvent(fn) {
  startSlow();
  listeners.add(fn);
  return () => listeners.delete(fn);
}

const EMPTY = { version: 1, groups: [], favorites: [] };

export const sqlFavorites = {
  get() {
    if (!has()) return { ...EMPTY };
    try { return { ...EMPTY, ...JSON.parse(lord.sql.favorites() || '{}') }; } catch (e) { return { ...EMPTY }; }
  },
  // throws when lord-server refuses the file (docs/DATABASES.md, "favorites.json")
  save(file) { if (has()) lord.sql.saveFavorites(JSON.stringify(file, null, 1)); },
};

const service = (fav) => `databases:${fav.id}`;

export const sqlPasswords = {
  async get(fav) {
    try { const it = await keyring.get('databases', { service: service(fav), account: fav.user || '' }); return it ? it.secret : null; }
    catch (e) { return null; }
  },
  // → true once the keyring kept it
  async set(fav, password) {
    try {
      await keyring.set('databases', { service: service(fav), account: fav.user || '', secret: password, label: `${t('databases.keyring.label')} — ${fav.name}` });
      return true;
    } catch (e) { return false; }
  },
  async remove(fav) { await keyring.remove('databases', { service: service(fav), account: fav.user || '' }).catch(() => {}); },
};
