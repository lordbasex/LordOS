// companion.js — the app's side of a packaged app's companion and of its item by the clock (docs/GO-LINK.md).
//
//   companion.start(manifest)     the app host does this; asks for `network` first if the manifest declares it
//   companion.on((msg) => …)      one JSON value per message. { companion: 'started' | 'refused' | 'ended', code } are the system's
//   companion.send(value)         false if it isn't running or can't take it now
//
//   statusItem.set({ icon, title, alert, menu: [{ id, label, off }, { line: true }] })     false if refused
//   statusItem.clear()
//   statusItem.onSelect((id) => …)
//
// A home .lpk gets no companion: lord-server refuses, whatever its manifest says. The status item needs the
// `status-item` permission; the shell draws it from this description, and lord-server checks every field.
const fns = new Set(), picks = new Set();
const early = [];                         // said before the app's first listener: it starts before the app mounts

export function companionEvent(data) {
  if (!fns.size) { if (early.length < 64) early.push(data); return; }
  for (const fn of [...fns]) fn(data);
}
export function statusItemEvent(data) { if (data && data.item) for (const fn of [...picks]) fn(data.item); }

function listen(set, fn) { set.add(fn); return () => set.delete(fn); }

export const companion = {
  on(fn) {
    const off = listen(fns, fn);
    for (const d of early.splice(0)) fn(d);
    return off;
  },
  send: (value) => lord.broker.companionSend(JSON.stringify(value)),
  start(manifest) {
    try {
      if ((manifest?.permissions || []).includes('network')) lord.broker.request('network');
      if (!lord.broker.companion()) companionEvent({ companion: 'refused', code: 'companion.none' });
    } catch (e) { companionEvent({ companion: 'refused', code: e?.code || 'companion.none' }); }
  },
};

export const statusItem = {
  set(spec) {
    try {
      if (!lord.broker.request('status-item')) return false;
      return lord.broker.statusItem(JSON.stringify(spec));
    } catch (e) { return false; }
  },
  clear: () => lord.broker.statusItem(''),
  onSelect: (fn) => listen(picks, fn),
};
