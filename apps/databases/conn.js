// conn.js — the open connections (one lord-sql per tab, docs/DATABASES.md "How it connects"), the queries the app and
// the person send through them (one at a time per connection: lord-sql answers `busy` otherwise), and the console's
// log of every one of them (in memory, gone at disconnect, passwords redacted). The Console window, another window
// of the same app, reads the same store.
import { createStore, sqlRequest, onSqlEvent, sqlFavorites, settings } from '@lordos/sdk';
import { redactSql, normalizeFavorite, cleanFavorite, pushRecent, isReadOnlySql, groupColor } from './model.js';

// sessions: [{ key, fav, conn, server, tls, threadId, db, openedAt, locked, closed, log, busy }]; active: key or null
// (null = the Connect tab); cert: the last certificate problem (an event), for the trust sheet
export const store = createStore({ sessions: [], active: null, cert: null, rev: 0 });
let seq = 1;
const chains = new Map();         // key → the promise of the last query (queries wait their turn)
const LOG_MAX = 5000;

const bump = () => store.set((s) => ({ ...s, rev: s.rev + 1 }));
export const sessionOf = (key) => store.get().sessions.find((x) => x.key === key) || null;

let listening = false;
export function listen() {
  if (listening) return;
  listening = true;
  onSqlEvent((ev) => {
    if (ev.ev === 'closed') {
      const s = store.get().sessions.find((x) => x.conn === ev.conn);
      if (s && !s.closed) { s.closed = ev.reason || 'closed'; s.busy = false; bump(); }
    } else if (ev.ev === 'cert') {
      store.set({ cert: { ...ev, at: Date.now() } });
    } else if (ev.ev === 'hostkey') {
      store.set({ hostkey: { ...ev, at: Date.now() } });           // an SSH server seen for the first time
    } else if (ev.ev === 'crash') {
      for (const s of store.get().sessions) if (!s.closed && (ev.conn === undefined || ev.conn === s.conn)) s.closed = 'crash';
      bump();
    }
  });
}

// ---- favorites.json ----
export function readFavorites() {
  const f = sqlFavorites.get();
  return { version: 1, groups: Array.isArray(f.groups) ? f.groups : [], favorites: (Array.isArray(f.favorites) ? f.favorites : []).map(normalizeFavorite) };
}
// throws when lord-server refuses the file
export function writeFavorites(file) {
  sqlFavorites.save({ version: 1, groups: file.groups.map((g) => ({ id: g.id, name: String(g.name || ''), parent: g.parent || null, expanded: g.expanded !== false, color: groupColor(g) })),
    favorites: file.favorites.map(cleanFavorite) });
  favRev.set((n) => n + 1);
}
export const favRev = createStore(0);

// ---- the app's own settings (settings.json "databases") ----
export const prefs = () => ({ history: {}, recent: [], safeMode: true, ...(settings.get().databases || {}) });
export const setPrefs = (patch) => settings.set({ databases: { ...prefs(), ...patch } });

// ---- connections ----
// → the new session; rejects with Error { code, data } (docs/DATABASES.md errors)
export async function openSession(fav, password) {
  listen();
  const args = { fav: fav.id };
  if (password) args.password = password;
  if (fav.database) args.database = fav.database;
  const t0 = Date.now();
  const r = await sqlRequest('open', args);
  const s = {
    key: `s${seq++}`, fav, conn: r.conn, server: r.server || {}, tls: r.tls || null, threadId: r.threadId, db: r.db || fav.database || '',
    openedAt: Date.now(), locked: !!(fav.production || fav.readOnly), closed: null, busy: false,
    log: [{ at: t0, src: 'app', sql: `-- connect ${fav.user || ''}@${fav.type === 'socket' ? fav.socket : `${fav.host}:${fav.port}`}`, ms: Date.now() - t0 }],
  };
  store.set((st) => ({ ...st, sessions: [...st.sessions, s], active: s.key, rev: st.rev + 1 }));
  setPrefs({ recent: pushRecent(prefs().recent, fav.id) });
  return s;
}

// Test Connection: open and close at once → what the server said
export async function testFavorite(fav, password) {
  const args = { fav: fav.id };
  if (password) args.password = password;
  const t0 = Date.now();
  const r = await sqlRequest('open', args);
  const ms = Date.now() - t0;
  sqlRequest('close', { conn: r.conn }).catch(() => {});
  return { ...r, ms };
}

export function closeSession(key) {
  const s = sessionOf(key);
  if (!s) return;
  if (!s.closed) sqlRequest('close', { conn: s.conn }).catch(() => {});
  chains.delete(key);
  store.set((st) => {
    const sessions = st.sessions.filter((x) => x.key !== key);
    const i = st.sessions.findIndex((x) => x.key === key);
    const active = st.active === key ? (sessions[Math.min(i, sessions.length - 1)]?.key ?? null) : st.active;
    return { ...st, sessions, active, rev: st.rev + 1 };
  });
}
export const setActive = (key) => store.set({ active: key });
export function patchSession(key, patch) { const s = sessionOf(key); if (s) { Object.assign(s, patch); bump(); } }

function log(s, entry) {
  s.log.push(entry);
  if (s.log.length > LOG_MAX) s.log.splice(0, s.log.length - LOG_MAX);
}

// One query (one statement, or several the server runs in turn). src: 'app' (the app's own) or 'you' (typed in Query,
// or an edit the person made). Rejects with Error { code, data }; a locked connection refuses anything that writes.
export function run(key, sql, { src = 'app', maxRows } = {}) {
  const s = sessionOf(key);
  if (!s) return Promise.reject(Object.assign(new Error('closed'), { code: 'closed', data: {} }));
  if (s.locked && !isReadOnlySql(sql)) return Promise.reject(Object.assign(new Error('readonly'), { code: 'readonly', data: {} }));
  const prev = chains.get(key) || Promise.resolve();
  const next = prev.catch(() => {}).then(async () => {
    if (s.closed) throw Object.assign(new Error('closed'), { code: 'closed', data: {} });
    const entry = { at: Date.now(), src, sql: redactSql(sql), ms: null, error: null };
    log(s, entry);
    s.busy = true; bump();
    const t0 = Date.now();
    try {
      const r = await sqlRequest('query', { conn: s.conn, sql, ...(maxRows ? { maxRows } : {}) });
      entry.ms = r.ms ?? Date.now() - t0;
      return r;
    } catch (e) {
      entry.ms = Date.now() - t0;
      entry.error = e.data?.message || e.code || 'error';
      if (e.code === 'closed') s.closed = s.closed || 'closed';
      throw e;
    } finally { s.busy = false; bump(); }
  });
  chains.set(key, next);
  return next;
}
// the first result of a query
export const run1 = (key, sql, opts) => run(key, sql, opts).then((r) => (r.results || [])[0] || { columns: [], rows: [] });

export async function useDatabase(key, db) {
  const s = sessionOf(key);
  if (!s) return;
  const prev = chains.get(key) || Promise.resolve();
  const next = prev.catch(() => {}).then(async () => {
    const entry = { at: Date.now(), src: 'app', sql: `USE \`${String(db).replace(/`/g, '``')}\``, ms: null, error: null };
    log(s, entry);
    const t0 = Date.now();
    try { await sqlRequest('use', { conn: s.conn, db }); entry.ms = Date.now() - t0; s.db = db; bump(); } catch (e) { entry.ms = Date.now() - t0; entry.error = e.data?.message || e.code; bump(); throw e; }
  });
  chains.set(key, next);
  return next;
}

export function clearLog(key) { const s = sessionOf(key); if (s) { s.log = []; bump(); } }
