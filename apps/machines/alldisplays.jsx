// alldisplays.jsx — All Displays (docs/VIRTUALIZATION.md 5.4): the machine's window keeps output 0 full screen on its own
// display, and one window per further output takes each other display full screen. The plan (which displays, whether
// the machine must start again with more outputs) is alldisplays.js; the engine side is lord.vm.allDisplays (one
// lord-vmdisplay serving every output, each on its own channel) and lord.vm.attach(dir, { head }).
//
//   const ad = useAllDisplays({ sid, dir, name, winId, st, prot, appsOn, begin });
//   ad.item        the View menu's entry      ad.on   whether the machine is across the displays now
//   <HeadWindow dir k name />                one further output's window (index.jsx, args.head)
import { View, Text, MediaView, addListener, wm, windows, screen, createStore, useStore, useAppWindow, displayFor, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { allDisplaysPlan, headInstance, headWindowId, HEADS_MAX } from './alldisplays.js';
import { toPicture, guestSize, wheelSteps, familyOf } from './model.js';
import { readMeta } from './meta.js';

// a further output's window asks the machine's window to go back to one display (dir → a counter)
export const backToOne = createStore({});
// per machine window (its id): across the displays now, and when a start was asked for to go across them. A store, not
// the component's state: full screen remounts the app, and the window's own state (useWindowState) would lose a value
// set by the instance that goes away
const across = createStore({});
const setAcross = (winId, k, v) => across.set((x) => ({ ...x, [winId]: { ...(x[winId] || {}), [k]: v } }));
windows.subscribe(() => {
  const ids = new Set(windows.get().map((w) => w.id));
  if (Object.keys(across.get()).some((k) => !ids.has(k))) across.set((x) => Object.fromEntries(Object.entries(x).filter(([k]) => ids.has(k))));
});

export function useAllDisplays({ sid, dir, name, winId, st, prot, appsOn, begin }) {
  // the window's state, not the component's: full screen remounts the app
  const on = useStore(across, (x) => !!x[winId]?.on);
  const setOn = (v) => setAcross(winId, 'on', !!v);
  const wantAt = useStore(across, (x) => x[winId]?.want || 0);
  const want = { get current() { return across.get()[winId]?.want || 0; }, set current(v) { setAcross(winId, 'want', v); } };
  // the machine as lord.vm.list says it (its outputs, graphics card, display), read again as it starts and stops
  const [cfg, setCfg] = useState(null);
  useEffect(() => { try { setCfg(lord.vm.list().find((x) => x.dir === dir) || null); } catch (e) { setCfg(null); } }, [dir, sid, st?.state]);
  const id = cfg?.id || '';
  const family = cfg ? familyOf({ ...cfg, meta: readMeta(dir) }) : '';
  const myDisplay = () => { const w = windows.get().find((x) => x.id === winId); return w ? displayFor(wm.frame(w)) : null; };
  const running = !!sid && (st?.state === 'running' || st?.state === 'paused');
  const plan = allDisplaysPlan(cfg, screen.displays, { first: myDisplay()?.id, running, prot, appsOn, family });

  const enter = () => {
    const p = allDisplaysPlan(cfg, screen.displays, { first: myDisplay()?.id, running: true, prot, appsOn, family });
    if (!p.ok || p.restart || !sid || !lord.vm.allDisplays(sid, p.sizes)) return false;
    wm.moveToDisplay(winId, p.order[0].id);
    wm.setFullscreen(winId, true);
    for (let k = 1; k < p.n; k++) {
      const hid = headWindowId(id, k);
      wm.open('machines', { instance: headInstance(id, k), protected: prot, args: { head: { dir, k }, name } });
      wm.moveToDisplay(hid, p.order[k].id);
      wm.setFullscreen(hid, true);
    }
    wm.focus(winId);
    setOn(true);
    return true;
  };
  const leave = () => {
    for (let k = 1; k < HEADS_MAX; k++) wm.close(headWindowId(id, k), true);
    if (sid) lord.vm.allDisplays(sid, null);
    wm.setFullscreen(winId, false);
    setOn(false);
  };
  // off: the machine gets its outputs (a hardware change, only while off) and starts across the displays
  const startAcross = () => {
    const p = allDisplaysPlan(cfg, screen.displays, { first: myDisplay()?.id, running: false, prot, appsOn, family });
    if (!p.n || p.n < 2 || (p.why && p.why !== 'notRunning' && p.why !== '')) return;
    want.current = Date.now();
    if (cfg?.heads >= p.n) { begin(); return; }
    // lord-vm --set is a job: the machine starts once machine.json has its outputs
    const job = lord.vm.set(dir, { heads: p.n });
    if (!job) { want.current = 0; return; }
    const t = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      clearInterval(t);
      if (r.ok) begin(); else want.current = 0;
    }, 300);
  };
  useEffect(() => {
    if (want.current && sid && st?.display === 'up' && st.state === 'running') {
      const fresh = Date.now() - want.current < 120000;
      want.current = 0;
      if (fresh) setTimeout(enter, 300);
    }
  }, [sid, st?.display, st?.state, wantAt]);
  // the machine stopped, its window lost its picture, or a further output's window asked: back to one display
  useEffect(() => { if (on && (!sid || st?.ended || (st && st.state !== 'running' && st.state !== 'paused'))) leave(); }, [on, sid, st?.state, st?.ended]);
  const back = useStore(backToOne, (x) => x[dir] || 0);
  const seenBack = useRef(back);
  useEffect(() => { if (back !== seenBack.current) { seenBack.current = back; if (on) leave(); } }, [back]);
  // displays added or taken away: back to one (the plan was made for the displays there were)
  useEffect(() => {
    if (!on) return undefined;
    const n0 = screen.displays.length;
    const t = setInterval(() => { if (screen.displays.length !== n0) leave(); }, 1000);
    return () => clearInterval(t);
  }, [on]);

  const hint = on ? '' : !plan.ok ? (plan.why ? tr(`machines.alld.why.${plan.why}`) : '')
    : plan.restart && running ? tr('machines.alld.why.restart', { n: plan.n }) : '';
  const item = {
    label: tr('machines.alld.item'),
    checked: on,
    disabled: !on && (!plan.ok ? plan.why !== 'notRunning' : plan.restart && running),
    hint,
    onClick: () => (on ? leave() : running ? enter() : startAcross()),
  };
  return { on, item, leave };
}

// One further output's window: the picture of output k of the machine in dir, full screen on its display; the pointer,
// the wheel and the keys while it has them go to that output. Moving over its top edge shows the machine's name and
// Back to One Display.
export function HeadWindow({ dir, k, name }) {
  const { id: winId, focused } = useAppWindow();
  const [sid, setSid] = useState(0);
  const [st, setSt] = useState(null);
  const [bar, setBar] = useState(false);
  const box = useRef(null);
  const sized = useRef({ want: null, at: 0, last: null });
  const wheel = useRef(0);
  const buttons = useRef(new Set());
  useEffect(() => { wm.setTitle(winId, tr('machines.alld.title', { name: name || '', n: k + 1 })); }, [name, k]);
  useEffect(() => {
    let s = 0;
    try { s = lord.vm.attach(dir, { head: k }) || 0; } catch (e) { s = 0; }
    if (!s) { wm.close(winId, true); return undefined; }
    setSid(s);
    lord.vm.rate(s, 60);
    lord.vm.shown(s, true);
    let gone = false;
    const tick = () => {
      if (gone) return;
      const q = lord.vm.poll(s);
      // the machine stopped, or its own window went (its helper with it): this window goes too
      if (!q || q.ended || q.display === 'lost' || (q.state && q.state !== 'running' && q.state !== 'paused' && q.state !== 'starting')) { wm.close(winId, true); return; }
      setSt(q);
      const z = sized.current;
      if (q.display === 'up' && box.current) {
        const want = guestSize(lord.getLayout(box.current.id), z.last);
        if (!want) z.want = null;
        else if (!z.want || z.want[0] !== want[0] || z.want[1] !== want[1]) { z.want = want; z.at = Date.now(); }
        else if (Date.now() - z.at > 400) { lord.vm.resize(s, want[0], want[1]); z.last = want; z.want = null; }
      }
      setTimeout(tick, 100);
    };
    setTimeout(tick, 0);
    return () => { gone = true; lord.vm.close(s); };
  }, [dir, k]);
  const live = !!sid && st?.display === 'up';
  useEffect(() => {
    if (!live || !focused) return undefined;
    const ka = addListener('keydown', (e) => {
      if (!(e.code > 0) || !wm.hasKeyboard(winId) || (e.meta && !e.ctrl && !e.alt)) return;    // ❖ stays LordOS's here
      lord.vm.key(sid, e.code, 1);
      e.preventDefault?.();
    });
    const kb = addListener('keyup', (e) => { if (e.code > 0) lord.vm.key(sid, e.code, 0); });
    const kc = addListener('pointerup', (e) => { if (buttons.current.delete(e.button)) lord.vm.button(sid, e.button, 0); });
    return () => { ka(); kb(); kc(); lord.vm.release(sid); buttons.current.clear(); };
  }, [live, focused, sid]);
  const at = (e) => (box.current && st?.w ? toPicture(e.x, e.y, lord.getLayout(box.current.id), st.w, st.h) : null);
  const move = (e) => {
    const b = box.current && lord.getLayout(box.current.id);
    setBar(!!b && e.y - b[1] < 6 ? true : (bar && b && e.y - b[1] < 48));
    const q = live && at(e);
    if (q) lord.vm.motion(sid, q[0], q[1]);
  };
  const down = (e) => { if (!live) return; move(e); buttons.current.add(e.button); lord.vm.button(sid, e.button, 1); };
  const onWheel = (e) => {
    if (!live) return;
    move(e);
    const [n, rest] = wheelSteps(wheel.current, e.deltaY || 0, !!e.precise);
    wheel.current = rest;
    if (n) lord.vm.wheel(sid, n, 0);
  };
  const back = () => backToOne.set((x) => ({ ...x, [dir]: (x[dir] || 0) + 1 }));
  return (
    <View style={{ flex: 1, backgroundColor: '#000' }}>
      <View ref={box} onMouseDown={down} onMouseMove={move} onWheel={onWheel} style={{ flex: 1 }}>
        {live && st.media ? <MediaView media={st.media} style={{ flex: 1 }} /> : (
          <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ color: '#aaa', fontSize: 14 }}>{tr('machines.alld.waiting')}</Text>
          </View>
        )}
      </View>
      {bar ? (
        <View onMouseLeave={() => setBar(false)} style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 40, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(20,20,24,0.85)' }}>
          <Text style={{ color: '#fff', fontSize: 13, marginEnd: 16 }}>{tr('machines.alld.title', { name: name || '', n: k + 1 })}</Text>
          <View onMouseDown={(e) => { e.stopPropagation?.(); back(); }} style={{ paddingStart: 12, paddingEnd: 12, height: 26, borderRadius: 6, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.16)' }}>
            <Text style={{ color: '#fff', fontSize: 13 }}>{tr('machines.alld.back')}</Text>
          </View>
        </View>
      ) : null}
    </View>
  );
}
