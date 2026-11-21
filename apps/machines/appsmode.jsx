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
  View, Text, MediaView, addListener, useAppWindow, useStore, createStore, wm, windows, mainDisplay, workArea, windowPixelIcons, windowChrome, t as tr,
  useFilesDrop, FilesDropHint, notify,
} from '@lordos/sdk';
import { useEffect, useRef } from 'react';
import { ClipAskBar } from './clipask.jsx';
import { appsGuestSize, guestScale, toHostRect, toGuestRect, cropOf, cropPoint, nearRect, guestKept, insideWork, isPopup, guestInstance, planWindows, guestAppItems, wheelSteps, guestPlace, offsetRect, minimizedByUs, AGENT_CAPS, copyLine, placeBatch } from './model.js';

// how long a click that brought a new clipboard into the machine waits for it (the guest's helper sets it); the machine's
// window (vmwindow.jsx) and its apps mode windows alike
export const CLIP_HOLD_MS = 150;
// dir → { sid, name, mid, gw, gh, media, list: [guest window], apps: [{ name, token }], leave() }
export const guests = createStore({});
// dir → the LordOS window id of that machine's guest window in front, if one is: the machine counts as in use then (its
// clipboard's gate, 6.8)
export const guestFocus = createStore({});
// dir → Map gid → { winId, rect (the guest's last), minimized, asked (the rect this side asked for), inset, measured, off (where
// the LordOS window is, from the guest's window mapped here: another monitor, model.js guestPlace) }
const tracks = new Map();
const trackOf = (dir) => { let m = tracks.get(dir); if (!m) tracks.set(dir, m = new Map()); return m; };
const publish = (dir, patch) => guests.set((g) => ({ ...g, [dir]: { ...(g[dir] || {}), ...patch } }));
const display = () => { const d = mainDisplay(); return { x: d?.x || 0, y: d?.y || 0, w: d?.w || 1280, h: d?.h || 800 }; };

// the frame of a LordOS window whose content (the guest's part) should be at r: the theme's chrome around it, learned
// from the window itself once it's measured (inset), or the theme's declared size until then
function insetOf(inset) {
  const c = windowChrome() || {};
  return inset || { l: (c.w || 0) / 2, t: (c.h || 0) - (c.w || 0) / 2, r: (c.w || 0) / 2, b: (c.w || 0) / 2 };
}
function frameAround(r, inset) {
  const i = insetOf(inset);
  return { x: Math.round(r.x - i.l), y: Math.round(r.y - i.t), w: Math.round(r.w + i.l + i.r), h: Math.round(r.h + i.t + i.b) };
}

// Where a guest window's picture goes here (x: its track): the guest's own place, or, when its LordOS frame would be
// under the menu bar (a window near the guest's top edge), just below it; the guest is asked to move its window there
// as well (once per place), so the crop stays the guest's window 1:1. A guest that keeps it where it was still shows
// right: the crop is always its rectangle.
function placeOf(x, w, sid, k, d, have) {
  // a window the person put elsewhere (another monitor) keeps its distance from the guest's; a menu goes with its owner
  const off = x.popup ? have?.get(w.owner)?.off : x.off;
  const r = offsetRect(toHostRect(w, k, d), off);
  if (x.popup || off) return r;
  const fix = insideWork(r, insetOf(x.inset), workArea());
  if (!fix) return r;
  const g = toGuestRect(fix, k, d);
  if (sid && !(x.workAsk && nearRect(x.workAsk, g))) {
    x.workAsk = g;
    x.asked = g;
    x.askedAt = Date.now();
    lord.vm.winAct(sid, w.id, 'move', g.x, g.y, g.w, g.h);
  }
  return fix;
}

// The machine's window side: turns apps mode on and off in the guest, and keeps the LordOS windows in step with the
// guest's list. on: the person wants it; ready: the agent says the guest's helper lists its windows.
export function useAppsMode({ dir, sid, st, name, mid, on, ready, onLeave, onTouch }) {
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
    publish(dir, { sid, name, mid, leave: onLeave, touch: onTouch, list: [], apps: [] });
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
  // the picture's size and source can change under it (the guest took the new size); caps: what the guest's tools take
  useEffect(() => { if (active) publish(dir, { gw: st.w, gh: st.h, media: st.media, name, caps: st.agent?.caps || 0 }); }, [active, st?.w, st?.h, st?.media, name, st?.agent?.caps]);
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
    const popup = isPopup(w);
    const x = { gid: w.id, winId: 0, rect: w, minimized: false, asked: null, inset: popup ? { l: 0, t: 0, r: 0, b: 0 } : null, popup };
    const r = placeOf(x, w, sid, k, d, have);
    const args = { guestWin: { dir, gid: w.id }, guest: { machine: name, mid, app: w.app || w.title, key: (w.app || '').toLowerCase() || String(w.id) } };
    const winId = popup ? wm.open('machines', { instance, frame: r, pip: r, args: { ...args, notice: true } })
      : wm.open('machines', { instance, frame: frameAround(r), args });
    if (!winId) continue;
    x.winId = winId;
    have.set(w.id, x);
  }
  for (const w of plan.minimize) {
    const x = have.get(w.id);
    if (!x) continue;
    // the guest hid it right after a move this side asked for: brought back there, not minimized here
    if (minimizedByUs(x.askedAt, Date.now(), !!wm.get(x.winId)?.minimized)) { lord.vm.winAct(sid, w.id, 'restore'); continue; }
    x.minimized = true;
    wm.minimize(x.winId);
  }
  for (const w of plan.restore) { const x = have.get(w.id); if (x) { x.minimized = false; wm.focus(x.winId); } }
  for (const w of plan.follow) {
    const x = have.get(w.id);
    if (!x) continue;
    const r = placeOf(x, w, sid, k, d, have);
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
  const held = useRef(0);          // a press held back for a new clipboard offer: its release waits until after it
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
  // in front: the machine is in use (the clipboard's gate)
  useEffect(() => {
    if (!focused) return undefined;
    guestFocus.set((f) => ({ ...f, [dir]: winId }));
    return () => guestFocus.set((f) => { if (f[dir] !== winId) return f; const n = { ...f }; delete n[dir]; return n; });
  }, [focused]);
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
      if (!x.inset && !x.popup) {
        // first look: the theme's chrome around the content, then the content put where the guest's window is
        const f = wm.get(winId);
        if (!f) return;
        x.inset = { l: lx - f.x, t: ly - f.y, r: f.x + f.w - (lx + lw), b: f.y + f.h - (ly + lh) };
        const at = placeOf(x, w, s.sid, k, d, trackOf(dir));
        if (!nearRect(here, at)) wm.setFrame(winId, frameAround(at, x.inset));
        return;
      }
      const want = placeOf(x, w, s.sid, k, d, trackOf(dir));
      if (!lastBox || !nearRect(here, lastBox, 0)) { lastBox = here; stableAt = Date.now(); return; }
      if (Date.now() - stableAt < 120) return;
      if (nearRect(here, want) || (x.followed && nearRect(here, x.followed))) return;
      // never a place outside the guest's one screen (another monitor, HiDPI or not): the guest's window stays inside it,
      // and this window keeps where it was put (model.js guestPlace)
      const { ask: r, off } = guestPlace(here, k, d, s.gw, s.gh);
      x.off = off;
      if (x.asked && nearRect(r, x.asked)) {
        // asked already; the guest kept its window where it was (its minimum size, maximized): back to where it is
        if (guestKept(x.asked, x.askedAt, w, Date.now())) { x.asked = null; x.off = null; x.followed = toHostRect(w, k, d); wm.setFrame(winId, frameAround(x.followed, x.inset)); }
        return;
      }
      if (nearRect(r, w)) { x.followed = here; return; }      // the guest's window is there already (clamped): nothing to ask
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
    // while the clipboard's bar asks, the keys are the bar's
    const ka = addListener('keydown', (e) => { if (!(e.code > 0) || gref.current?.ask) return; e.preventDefault?.(); lord.vm.key(sid, e.code, 1); });
    const kb = addListener('keyup', (e) => { if (e.code > 0) lord.vm.key(sid, e.code, 0); });
    const kc = addListener('pointerup', (e) => {
      if (!buttons.current.has(e.button)) return;
      buttons.current.delete(e.button);
      const wait = held.current - Date.now(), b = e.button;
      if (wait > 0) setTimeout(() => lord.vm.button(sid, b, 0), wait + 10); else lord.vm.button(sid, b, 0);
    });
    return () => { ka(); kb(); kc(); lord.vm.release(sid); buttons.current.clear(); };
  }, [focused, g?.sid]);
  const at = (e) => (box.current && me && g?.gw ? cropPoint(e.x, e.y, lord.getLayout(box.current.id), cropOf(me), g.gw, g.gh) : null);
  // files dragged from Files onto this window (7.2.1): copied into the machine as any drop (transfer folder or agent's
  // way, SHA-256 checked), then the guest's tools hand them to this window: into the folder an Explorer window shows,
  // dropped on a window that takes files, or on the clipboard to paste. Only files, at most AG_PLACE_MAX at once
  const sent = useRef(new Set());
  const drop = useFilesDrop({
    onDrop: ({ files }, how) => {
      const s = gref.current, w = meRef.current;
      if (!s?.sid || !w || !files.length) return;
      if (!(s.caps & AGENT_CAPS.place)) { notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.apps.dropCant'), body: '', sound: false }); return; }
      const box0 = box.current ? lord.getLayout(box.current.id) : null;
      const pt = (Number.isFinite(how?.x) && box0 && cropPoint(how.x, how.y, box0, cropOf(w), s.gw, s.gh)) || [w.x + Math.floor(w.w / 2), w.y + Math.floor(w.h / 2)];
      const plan = placeBatch(files, w.id, pt);
      if (plan.left) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.apps.dropMany', { count: plan.items.length }), body: '', sound: false });
      const refused = [];
      for (const it of plan.items) {
        const id = lord.vm.sendFile(s.sid, it.path, { place: it.place });
        if (id) sent.current.add(id); else refused.push(it.path.split('/').pop());
      }
      if (refused.length) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.drop.refused', { name: refused.join(', ') }), body: '', sound: false });
    },
  });
  // this window's drops still copying (the machine's window publishes them), as a line at its bottom
  const line = copyLine((g?.copies || []).filter((f) => sent.current.has(f.id)));
  const move = (e) => { const q = g?.sid && at(e); if (q) lord.vm.motion(g.sid, q[0], q[1]); };
  const down = (e) => {
    if (trackOf(dir).get(gid)?.popup) e.stopPropagation?.();     // a popup's own frame (picture-in-picture) would start a drag
    if (!g?.sid || g.ask) return;     // the clipboard's bar is asking: its clicks are its own
    // a click on the machine: its clipboard's gate opens and the guest gets the offer; a click that just carried a new
    // clipboard in waits a moment, as on the machine's window, so a right-click's Paste finds it there
    const fresh = !!g.touch?.();
    move(e);
    buttons.current.add(e.button);
    const sid = g.sid, b = e.button;
    if (fresh) { held.current = Date.now() + CLIP_HOLD_MS; setTimeout(() => lord.vm.button(sid, b, 1), CLIP_HOLD_MS); }
    else lord.vm.button(sid, b, 1);
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
    <View ref={box} {...drop.props} onMouseDown={down} onMouseMove={move} onWheel={onWheel} style={{ flex: 1, backgroundColor: '#000' }}>
      {g.media ? <MediaView media={g.media} crop={cropOf(me)} style={{ flex: 1 }} /> : null}
      {focused && g.ask ? <ClipAskBar ask={g.ask} onAnswer={(yes) => g.askAnswer?.(yes)} /> : null}
      {line ? (
        <View style={{ position: 'absolute', left: 8, right: 8, bottom: 8, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 12, paddingVertical: 6, borderRadius: 10, backgroundColor: 'rgba(28,28,33,0.94)' }}>
            <Text style={{ fontSize: 12, color: '#f2f2f5', textAlign: 'start' }}>{tr(line.fast ? 'machines.drop.copyingFast' : 'machines.drop.copying', { name: line.name, pct: line.pct })}</Text>
          </View>
        </View>
      ) : null}
      <FilesDropHint over={drop.over} title={drop.over && (drop.over.files.length ? tr('machines.apps.dropTitle') : tr('machines.drop.none'))}
        note={drop.over && drop.over.files.length ? tr('machines.apps.dropNote') : ''} />
    </View>
  );
}
