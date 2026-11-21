// Apps mode (docs/VIRTUALIZATION.md 5.4, 7.2): each window of the guest as its own LordOS window, with the guest's
// title and its app's icon in the Dock and the taskbar, while the guest's desktop isn't shown. The guest's helper
// (lordos-guest on a LordOS guest, lordos-guest-win on Windows) lists its windows through the machine's agent;
// lord-vm checks every list and passes it on (VM_WINDATA); lord.vm.windows() hands it here. Each LordOS window shows its
// part of the machine's one picture (<MediaView crop>, media/mview.c: nothing is copied per window). The pointer over
// it goes to the guest at the window's place, the keyboard while it's in front. Moving, resizing, minimizing,
// focusing or closing it asks the guest to do the same (lord.vm.winAct); a window the guest moves is followed.
//
// The machine's own window (vmwindow.jsx) holds the session and polls it: useAppsMode runs there. Each guest window is
// the Machines app opened with args.guestWin (index.jsx → GuestWindow), reading what useAppsMode publishes in `guests`.
import {
  View, Text, MediaView, addListener, useAppWindow, useStore, createStore, wm, windows, mainDisplay, windowPixelIcons, windowChrome, t as tr,
} from '@lordos/sdk';
import { useEffect, useRef } from 'react';
import { appsGuestSize, guestScale, toHostRect, toGuestRect, cropOf, cropPoint, nearRect, guestKept, isPopup, guestInstance, planWindows, guestAppItems, wheelSteps } from './model.js';

// dir → { sid, name, mid, gw, gh, media, list: [guest window], apps: [{ name, token }], leave() }
export const guests = createStore({});
// dir → Map gid → { winId, rect (the guest's last), minimized, asked (the rect this side asked for), inset, measured }
const tracks = new Map();
const trackOf = (dir) => { let m = tracks.get(dir); if (!m) tracks.set(dir, m = new Map()); return m; };
const publish = (dir, patch) => guests.set((g) => ({ ...g, [dir]: { ...(g[dir] || {}), ...patch } }));
const display = () => { const d = mainDisplay(); return { x: d?.x || 0, y: d?.y || 0, w: d?.w || 1280, h: d?.h || 800 }; };

// the frame of a LordOS window whose content (the guest's part) should be at r: the theme's chrome around it, learned
// from the window itself once it's measured (inset), or the theme's declared size until then
function frameAround(r, inset) {
  const c = windowChrome() || {};
  const i = inset || { l: (c.w || 0) / 2, t: (c.h || 0) - (c.w || 0) / 2, r: (c.w || 0) / 2, b: (c.w || 0) / 2 };
  return { x: Math.round(r.x - i.l), y: Math.round(r.y - i.t), w: Math.round(r.w + i.l + i.r), h: Math.round(r.h + i.t + i.b) };
}

// The machine's window side: turns apps mode on and off in the guest, and keeps the LordOS windows in step with the
// guest's list. on: the person wants it; ready: the agent says the guest's helper lists its windows.
export function useAppsMode({ dir, sid, st, name, mid, on, ready, onLeave }) {
  const sent = useRef(false);
  const seen = useRef({ w: -1, i: -1, a: -1 });
  // a pause keeps the windows (their pictures stay as they were); stopping, or the agent going, ends it
  const active = !!(on && ready && sid && st?.display === 'up' && (st.state === 'running' || st.state === 'paused'));
  useEffect(() => {
    if (!active) return undefined;
    sent.current = lord.vm.apps(sid, true);
    // the guest's screen as large as this display: its windows are never cut by a smaller one
    const [gw, gh] = appsGuestSize(display());
    lord.vm.resize(sid, gw, gh);
    seen.current = { w: -1, i: -1, a: -1 };
    publish(dir, { sid, name, mid, leave: onLeave, list: [], apps: [] });
    return () => {
      if (sent.current) lord.vm.apps(sid, false);
      sent.current = false;
      const ids = [...trackOf(dir).values()].map((x) => x.winId);
      tracks.delete(dir);
      for (const id of ids) wm.close(id, true);
      windowPixelIcons.set((m) => { const n = { ...m }; for (const id of ids) delete n[id]; return n; });
      guests.set((g) => { const n = { ...g }; delete n[dir]; return n; });
    };
  }, [active, sid]);
  // the picture's size and source can change under it (the guest took the new size)
  useEffect(() => { if (active) publish(dir, { gw: st.w, gh: st.h, media: st.media, name }); }, [active, st?.w, st?.h, st?.media, name]);
  useEffect(() => {
    if (!active) return;
    if (st.guestAppsSeq !== seen.current.a) { seen.current.a = st.guestAppsSeq; publish(dir, { apps: guestAppItems(lord.vm.guestApps(sid)) }); }
    if (st.windowsSeq !== seen.current.w) { seen.current.w = st.windowsSeq; sync(dir, sid, mid, name, lord.vm.windows(sid), st.w, st.h); }
    if (st.iconsSeq !== seen.current.i) {
      seen.current.i = st.iconsSeq;
      const icons = {};
      for (const tr0 of trackOf(dir).values()) { const px = lord.vm.winIcon(sid, tr0.gid); if (px) icons[tr0.winId] = px; }
      windowPixelIcons.set((m) => ({ ...m, ...icons }));
    }
  }, [active, st?.windowsSeq, st?.iconsSeq, st?.guestAppsSeq]);
  return active;
}

// the guest's new list → LordOS windows opened, closed, minimized, restored or moved after the guest's
function sync(dir, sid, mid, name, list, gw, gh) {
  const have = trackOf(dir);
  const d = display(), k = guestScale(gw, gh, d);
  const plan = planWindows(list, have);
  publish(dir, { list });
  for (const gid of plan.close) {
    const x = have.get(gid);
    have.delete(gid);
    if (!x) continue;
    wm.close(x.winId, true);
    windowPixelIcons.set((m) => { if (!(x.winId in m)) return m; const n = { ...m }; delete n[x.winId]; return n; });
  }
  // bottom first, so the guest's front window ends up in front here too
  for (const w of [...plan.open].reverse()) {
    const instance = guestInstance(mid, w.id);
    const r = toHostRect(w, k, d);
    const popup = isPopup(w);
    const args = { guestWin: { dir, gid: w.id }, guest: { machine: name, mid, app: w.app || w.title, key: (w.app || '').toLowerCase() || String(w.id) } };
    const winId = popup ? wm.open('machines', { instance, frame: r, pip: r, args: { ...args, notice: true } })
      : wm.open('machines', { instance, frame: frameAround(r), args });
    if (!winId) continue;
    have.set(w.id, { gid: w.id, winId, rect: w, minimized: false, asked: null, inset: popup ? { l: 0, t: 0, r: 0, b: 0 } : null, popup });
  }
  for (const w of plan.minimize) { const x = have.get(w.id); if (x) { x.minimized = true; wm.minimize(x.winId); } }
  for (const w of plan.restore) { const x = have.get(w.id); if (x) { x.minimized = false; wm.focus(x.winId); } }
  for (const w of plan.follow) {
    const x = have.get(w.id);
    if (!x) continue;
    const r = toHostRect(w, k, d);
    if (x.popup) wm.setPip(x.winId, r); else wm.setFrame(x.winId, frameAround(r, x.inset));
    x.followed = r;
  }
  for (const w of list) { const x = have.get(w.id); if (x) { x.rect = w; if (!w.minimized) x.minimized = false; } }
}

// The Applications menu's items: the guest's apps, each launched in the guest (lord.vm.launch sends only a token the
// guest listed itself)
export function appsMenuItems(g) {
  if (!g?.apps?.length) return [{ label: tr('machines.apps.noApps'), disabled: true }];
  return g.apps.map((a) => ({ label: a.name, onClick: () => lord.vm.launch(g.sid, a.token) }));
}

// One guest window as a LordOS window: its part of the picture, the pointer and keyboard to the guest, and what the
// person does to the window asked of the guest
export function GuestWindow({ dir, gid }) {
  const { id: winId, focused } = useAppWindow();
  const g = useStore(guests, (x) => x[dir] || null);
  const me = g?.list?.find((w) => w.id === gid) || null;
  const box = useRef(null);
  const buttons = useRef(new Set());
  const wheel = useRef(0);
  const gref = useRef(g);
  gref.current = g;
  const meRef = useRef(me);
  meRef.current = me;
  const minimized = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.minimized);
  const title = me ? (me.title || me.app || '') : '';
  // the guest's title, with the machine's name: a guest's window is always told apart from this computer's (7.2)
  useEffect(() => { if (g) wm.setTitle(winId, tr('machines.apps.windowTitle', { title, machine: g.name || '' })); }, [title, g?.name]);
  useEffect(() => wm.setCloseGuard(winId, () => {
    const s = gref.current;
    // closing asks the guest's window to close (it may ask about unsaved work); it goes once the guest's list says so
    if (s?.sid && meRef.current) { lord.vm.winAct(s.sid, gid, 'close'); return false; }
    return true;
  }), [winId]);
  useEffect(() => {
    if (!g) return;
    wm.setMenus(winId, [
      { label: tr('machines.apps.menu'), items: appsMenuItems(g) },
      { label: g.name || tr('machines.title'), items: [{ label: tr('machines.apps.leave'), onClick: () => g.leave?.() }] },
    ]);
  }, [g?.apps, g?.name]);
  // focus: the guest's window comes to the front in the guest too
  useEffect(() => { if (focused && g?.sid && me && !me.focused) lord.vm.winAct(g.sid, gid, 'focus'); }, [focused]);
  // minimized here (the Dock, the window's button) or brought back: the guest does the same
  useEffect(() => {
    if (!g?.sid || !me) return;
    if (minimized && !me.minimized) lord.vm.winAct(g.sid, gid, 'minimize');
    else if (!minimized && me.minimized) lord.vm.winAct(g.sid, gid, 'restore');
  }, [minimized]);
  // moved or resized here: the guest's window follows (measured, as the theme's frame decides where the content is)
  useEffect(() => {
    let lastBox = null, stableAt = 0;
    const timer = setInterval(() => {
      const s = gref.current, w = meRef.current, n = box.current;
      const x = s && trackOf(dir).get(gid);
      if (!s?.sid || !w || !n || !x || w.minimized || !s.gw) return;
      const [lx, ly, lw, lh] = lord.getLayout(n.id);
      if (!(lw > 0) || !(lh > 0)) return;
      const d = display(), k = guestScale(s.gw, s.gh, d);
      const here = { x: lx, y: ly, w: lw, h: lh };
      const want = toHostRect(w, k, d);
      if (!x.inset && !x.popup) {
        // first look: the theme's chrome around the content, then the content put where the guest's window is
        const f = wm.get(winId);
        if (!f) return;
        x.inset = { l: lx - f.x, t: ly - f.y, r: f.x + f.w - (lx + lw), b: f.y + f.h - (ly + lh) };
        if (!nearRect(here, want)) wm.setFrame(winId, frameAround(want, x.inset));
        return;
      }
      if (!lastBox || !nearRect(here, lastBox, 0)) { lastBox = here; stableAt = Date.now(); return; }
      if (Date.now() - stableAt < 120) return;
      if (nearRect(here, want) || (x.followed && nearRect(here, x.followed))) return;
      const r = toGuestRect(here, k, d);
      if (x.asked && nearRect(r, x.asked)) {
        // asked already; the guest kept its window where it was (its minimum size, maximized): back to where it is
        if (guestKept(x.asked, x.askedAt, w, Date.now())) { x.asked = null; x.followed = want; wm.setFrame(winId, frameAround(want, x.inset)); }
        return;
      }
      x.asked = r;
      x.askedAt = Date.now();
      lord.vm.winAct(s.sid, gid, 'move', r.x, r.y, r.w, r.h);
    }, 50);
    return () => clearInterval(timer);
  }, [winId]);
  // the keyboard while this window is in front; everything held is let go of when it isn't
  useEffect(() => {
    if (!focused || !g?.sid) return undefined;
    const sid = g.sid;
    const ka = addListener('keydown', (e) => { if (!(e.code > 0)) return; e.preventDefault?.(); lord.vm.key(sid, e.code, 1); });
    const kb = addListener('keyup', (e) => { if (e.code > 0) lord.vm.key(sid, e.code, 0); });
    const kc = addListener('pointerup', (e) => { if (buttons.current.has(e.button)) { buttons.current.delete(e.button); lord.vm.button(sid, e.button, 0); } });
    return () => { ka(); kb(); kc(); lord.vm.release(sid); buttons.current.clear(); };
  }, [focused, g?.sid]);
  const at = (e) => (box.current && me && g?.gw ? cropPoint(e.x, e.y, lord.getLayout(box.current.id), cropOf(me), g.gw, g.gh) : null);
  const move = (e) => { const q = g?.sid && at(e); if (q) lord.vm.motion(g.sid, q[0], q[1]); };
  const down = (e) => {
    if (trackOf(dir).get(gid)?.popup) e.stopPropagation?.();     // a popup's own frame (picture-in-picture) would start a drag
    if (!g?.sid) return;
    move(e);
    buttons.current.add(e.button);
    lord.vm.button(g.sid, e.button, 1);
  };
  const onWheel = (e) => {
    if (!g?.sid) return;
    move(e);
    const [n, rest] = wheelSteps(wheel.current, e.deltaY || 0, !!e.precise);
    wheel.current = rest;
    if (n) lord.vm.wheel(g.sid, n, 0);
  };
  if (!g || !me) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: '#111' }}>
        <Text style={{ fontSize: 13, color: '#bbb' }}>{tr('machines.apps.gone')}</Text>
      </View>
    );
  }
  return (
    <View ref={box} onMouseDown={down} onMouseMove={move} onWheel={onWheel} style={{ flex: 1, backgroundColor: '#000' }}>
      {g.media ? <MediaView media={g.media} crop={cropOf(me)} style={{ flex: 1 }} /> : null}
    </View>
  );
}
