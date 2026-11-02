// calendar.js — Calendar and Reminders (docs/MAIL.md): calendars, events and to-dos kept by
// axia-maild (local, or synced with CalDAV), and the scheduler that rings their alarms with the
// apps closed, like the Clock alarms (sdk/alarms.js).
//
//   calendars.list() → [{ id, name, color, acct, acctName, kind: 'local'|'dav', todos, events, hidden, count }]
//   calendars.create({ name, color, todos, events }) / update(id, { name, color, hidden }) / remove(id)
//   events.range(from, to, cals?) → occurrences [{ uid, title, loc, notes, url, start, end (ms), allDay,
//        sdate, edate ('YYYY-MM-DD' for all-day), tz, rrule, occ, alarms: [minutes], org, att: [{ email, name, ps }],
//        status, cal, color }]
//   events.save(cal, event, { from }) → uid    (event: { uid?, title, start, end, allDay, loc, notes, url, rrule,
//        alarms, att, tz }; from = previous calendar when moving)
//   events.remove(cal, uid, { occ, all })      one occurrence of a series (occ) or the whole event
//   events.importFile(path, cal)
//   reminders.lists() → calendars with todos
//   reminders.items(lists?) → [{ uid, title, notes, due (ms|null), dueAllDay, dueDate, prio (0, 1 high, 5, 9 low),
//        flag, done, doneAt, parent, list, color, alarms }]
//   reminders.save(list, todo, { from }) / remove(list, uid) / toggle(item)
//   usePim() → a revision number that changes whenever calendars, events, to-dos or contacts change
//   startReminders()                           called by startShell
import { useEffect, useState } from 'react';
import { createStore, useStore } from './store.js';
import { mailRequest, onMailEvent } from './mail.js';
import { notify, dismissNotification } from './notify.js';
import { settings } from './settings.js';
import { t, formatTime } from './i18n.js';
import { wm, getApp } from './windows.js';

export const pimRev = createStore(0);
let hooked = false;
function hook() {
  if (hooked) return;
  hooked = true;
  onMailEvent((ev) => { if (ev.ev === 'pim-changed' || ev.ev === 'ready' || ev.ev === 'crash') pimRev.set((v) => v + 1); });
}
export function usePim() { hook(); return useStore(pimRev); }

export const calendars = {
  list: async () => { hook(); return (await mailRequest('cal-list')).cals || []; },
  create: (c) => mailRequest('cal-create', c),
  update: (id, patch) => mailRequest('cal-update', { id, ...patch }),
  remove: (id) => mailRequest('cal-delete', { id }),
};

export const events = {
  range: async (from, to, cals) => (await mailRequest('ev-list', { from: +from, to: +to, ...(cals ? { cals } : {}) })).events || [],
  save: async (cal, event, { from } = {}) => (await mailRequest('ev-save', { cal, event, ...(from ? { from } : {}) })).uid,
  remove: (cal, uid, { occ, all } = {}) => mailRequest('ev-delete', { cal, uid, ...(occ ? { occ } : {}), ...(all ? { all: true } : {}) }),
  importFile: (path, cal) => mailRequest('ev-import', { cal }, { files: [path] }),
};

export const reminders = {
  lists: async () => (await calendars.list()).filter((c) => c.todos),
  items: async (lists) => (await mailRequest('todo-list', lists ? { lists } : {})).todos || [],
  save: async (list, todo, { from } = {}) => (await mailRequest('todo-save', { list, todo, ...(from ? { from } : {}) })).uid,
  remove: (list, uid) => mailRequest('todo-delete', { list, uid }),
  toggle: (item) => reminders.save(item.list, { ...item, done: !item.done }),
};

// ---- alarms with the apps closed ----
//
// Upcoming alarms come from axia-maild ('alarms' for the next 36 hours). One setTimeout to the
// next one (cut in ≤ 1 minute pieces when close, like the Clock, because the monotonic clock
// stops during suspend). An alarm missed by less than 10 minutes still rings; what was shown is
// remembered in settings.pimAlarms.fired so it never rings twice.

const MIN = 60000, LATE = 10 * MIN, AHEAD = 36 * 60 * MIN;
let list = [], timer = null, started = false, refreshing = false;
const keyOf = (a) => `${a.cal}|${a.uid}|${a.at}`;
const fired = () => settings.get().pimAlarms?.fired || {};
function markFired(k) {
  const now = Date.now();
  const f = Object.fromEntries(Object.entries(fired()).filter(([, v]) => now - v < 3 * 86400000));
  f[k] = now;
  settings.set({ pimAlarms: { ...(settings.get().pimAlarms || {}), fired: f } });
}
const snoozed = createStore([]);       // [{ ...alarm, at }] in memory

function ring(a) {
  markFired(keyOf(a));
  const when = a.allDay ? '' : formatTime(new Date(a.start));
  const body = [when, a.loc].filter(Boolean).join(' · ');
  const isTodo = a.kind === 'todo';
  const app = isTodo ? 'reminders' : 'calendar';
  const actions = [
    { id: 'snooze', label: t('calendar.alarm.snooze'), run: () => { snoozed.set((s) => [...s, { ...a, at: Date.now() + 10 * MIN, snooze: true }]); arm(); } },
  ];
  if (isTodo) actions.push({ id: 'done', label: t('reminders.alarm.done'), primary: true, run: () => completeTodo(a) });
  actions.push({ id: 'open', label: t('calendar.alarm.open'), run: () => { if (getApp(app)) wm.open(app, { args: { uid: a.uid, cal: a.cal, at: a.start, nonce: Date.now() } }); } });
  notify({ app, title: a.title || t(isTodo ? 'reminders.untitled' : 'calendar.untitled'), body, sticky: true, sound: true, actions });
}

async function completeTodo(a) {
  try {
    const items = await reminders.items([a.cal]);
    const it = items.find((x) => x.uid === a.uid);
    if (it) await reminders.save(it.list, { ...it, done: true });
  } catch (e) { /* not running */ }
}

function arm() {
  if (timer) { clearTimeout(timer); timer = null; }
  const now = Date.now();
  const f = fired();
  const pending = [...list, ...snoozed.get()].filter((a) => !f[keyOf(a)] || a.snooze);
  // due now (or missed by a little)
  for (const a of pending) {
    if (a.at <= now && now - a.at < LATE) {
      if (a.snooze) snoozed.set((s) => s.filter((x) => x !== a));
      ring(a);
    }
  }
  const next = pending.filter((a) => a.at > now).sort((x, y) => x.at - y.at)[0];
  const wait = next ? Math.min(next.at - now, next.at - now < 60 * MIN ? MIN : 10 * MIN) : 10 * MIN;
  timer = setTimeout(() => { if (Date.now() - lastFetch > 30 * MIN) refresh(); else arm(); }, Math.max(500, wait));
}

let lastFetch = 0;
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const now = Date.now();
    const r = await mailRequest('alarms', { from: now - LATE, to: now + AHEAD });
    list = r.alarms || [];
    lastFetch = Date.now();
  } catch (e) { /* axia-maild not there yet: try later */ }
  refreshing = false;
  arm();
}

export function startReminders() {
  if (started || typeof lord === 'undefined' || !lord.mail || !lord.mail.available()) return;
  started = true;
  hook();
  let last = pimRev.get();
  pimRev.subscribe(() => { if (pimRev.get() !== last) { last = pimRev.get(); refresh(); } });
  setTimeout(refresh, 1500);
}

// small helper for apps: the current date at midnight
export const startOfDay = (d) => { const x = new Date(d); x.setHours(0, 0, 0, 0); return x; };
export { dismissNotification };
// a tiny hook for apps that show a value loaded from axia-maild (rev = usePim())
export function usePimQuery(fn, deps = []) {
  const rev = usePim();
  const [v, setV] = useState(null);
  useEffect(() => { let live = true; fn().then((x) => { if (live) setV(x); }).catch(() => { if (live) setV(null); }); return () => { live = false; }; }, [rev, ...deps]);
  return v;
}
