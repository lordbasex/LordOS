// viewstate.js — what each connection's tab remembers while another tab is in front (the database, table and pane,
// the history of where it went, a pane's filter, the query text and its results) and the table metadata the panes share
// (fields and indexes, read once per table until something changes it). Kept in memory with the connection.
import { useEffect, useReducer } from 'react';
import { run1 } from './conn.js';
import { columnsSql, indexesSql, parseColumns, parseIndexes } from './model.js';

const views = new Map();        // session key → { …state }
const subs = new Map();         // session key → Set(fn)

export function viewOf(key) {
  if (!views.has(key)) views.set(key, { pane: 'structure', table: '', kind: 'table', nav: [], navAt: -1, meta: new Map() });
  return views.get(key);
}
export function patchView(key, patch) {
  Object.assign(viewOf(key), patch);
  for (const fn of subs.get(key) || []) fn();
}
export function dropView(key) { views.delete(key); subs.delete(key); }

// the tab's state, re-rendering on every patch
export function useView(key) {
  const [, force] = useReducer((n) => n + 1, 0);
  useEffect(() => {
    if (!subs.has(key)) subs.set(key, new Set());
    subs.get(key).add(force);
    return () => subs.get(key)?.delete(force);
  }, [key]);
  return viewOf(key);
}

// fields and indexes of a table (cached; `fresh` reads them again) → { fields, indexes }
export function tableMeta(key, db, table, { fresh = false, view = false } = {}) {
  const v = viewOf(key);
  const id = `${db}\u0000${table}`;
  if (!fresh && v.meta.has(id)) return v.meta.get(id);
  const p = (async () => {
    const fields = parseColumns(await run1(key, columnsSql(db, table)));
    const indexes = view ? [] : parseIndexes(await run1(key, indexesSql(db, table)));
    return { fields, indexes };
  })();
  v.meta.set(id, p);
  p.catch(() => v.meta.delete(id));
  return p;
}
export function forgetMeta(key, db, table) { viewOf(key).meta.delete(`${db}\u0000${table}`); }
