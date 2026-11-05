// A machine's window (alpha, docs/VIRTUALIZATION.md 5.3): the guest's picture scaled to fit (kept in shape, black
// bands), keyboard and pointer while the window is in front (let go of on blur), a bar with Pause or Resume,
// Restart, Ctrl+Alt+Delete, Full screen and Power (Shut Down, Force Off). Resizing the window asks the guest for that
// size after a short pause. Closing the window shuts the machine down; when lord-vm or the guest fails, the reason
// and the log are here, with Start again.
import {
  View, Text, Button, MediaView, ScrollView, addListener, useTheme, useAppWindow, useWindowState, useStore, wm, windows,
  ToolbarButton, MenuButton, spacing, typeScale,
  t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, actionsFor, toPicture, guestSize, refreshRate, wheelSteps, budgetCheck, CTRL_ALT_DEL, windowTitle } from './model.js';
import { AlphaBadge, StateDot, has } from './ui.jsx';

const BAR = 36;
const now = () => (typeof lord !== 'undefined' && lord.now ? lord.now() : Date.now());
// what changes the screen (frames don't: the media node repaints by itself)
const same = (a, b) => !!a && !!b && ['state', 'reason', 'code', 'detail', 'display', 'w', 'h', 'media', 'ended', 'lastError', 'stderr'].every((k) => a[k] === b[k]);

function Bar({ sid, st, name, winId, onPower }) {
  const t = useTheme();
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const [near, setNear] = useState(false);
  const v = stateView(st?.state, st?.reason);
  const a = actionsFor(st?.state);
  const cmd = (c) => sid && lord.vm.cmd(sid, c);
  const cad = () => {
    if (!sid) return;
    CTRL_ALT_DEL.forEach((k) => lord.vm.key(sid, k, 1));
    [...CTRL_ALT_DEL].reverse().forEach((k) => lord.vm.key(sid, k, 0));
  };
  const bar = (
    <View onMouseLeave={() => setNear(false)} style={{ height: BAR, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 10, backgroundColor: t.windowBg }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start', maxWidth: 260 }}>{name}</Text>
      <StateDot tone={v.tone} />
      <Text style={{ fontSize: 12, color: t.textDim }}>{tr(`machines.state.${v.key}`)}</Text>
      <AlphaBadge />
      <View style={{ flex: 1 }} />
      <ToolbarButton icon={a.resume ? '▶' : '⏸'} title={a.resume ? tr('machines.cmd.resume') : tr('machines.cmd.pause')} disabled={!a.pause && !a.resume} onClick={() => cmd(a.resume ? 'resume' : 'pause')} />
      <ToolbarButton icon="↻" title={tr('machines.cmd.reset')} disabled={!a.reset} onClick={() => cmd('reset')} />
      <ToolbarButton label="⌃⌥⌦" title={tr('machines.cmd.cad')} disabled={!a.keys} onClick={cad} />
      <ToolbarButton icon="⛶" title={full ? tr('machines.cmd.exitFullscreen') : tr('machines.cmd.fullscreen')} active={full} onClick={() => wm.setFullscreen(winId, !full)} />
      <MenuButton icon={null} label={tr('machines.cmd.power')} title={tr('machines.cmd.power')} items={[
        { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => onPower('shutdown') },
        { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => onPower('poweroff') }]} />
    </View>
  );
  if (!full) return <View style={{ position: 'absolute', left: 0, right: 0, top: 0 }}>{bar}</View>;
  // full screen: the bar comes back at the top edge, as in Remote Desk
  return <View onMouseEnter={() => setNear(true)} style={{ position: 'absolute', left: 0, right: 0, top: 0, height: near ? BAR : 3 }}>{near ? bar : null}</View>;
}

// the card over the black area when there's no picture: starting, stopped, failed, lost display
function Card({ title, text, detail, actions }) {
  const t = useTheme();
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 460, maxHeight: '90%', padding: spacing.xl, gap: spacing.s, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}` }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {text ? <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{text}</Text> : null}
        {detail ? (
          <ScrollView style={{ maxHeight: 220 }} contentStyle={{ padding: spacing.xs }}>
            <Text style={{ fontFamily: 'monospace', fontSize: 11, color: t.text, textAlign: 'start', direction: 'ltr', whiteSpace: 'pre-wrap' }}>{detail}</Text>
          </ScrollView>
        ) : null}
        {actions?.length ? <View style={{ flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end', marginTop: spacing.s }}>{actions}</View> : null}
      </View>
    </View>
  );
}

export function MachineWindow({ dir, name: name0 }) {
  const { id: winId, focused } = useAppWindow();
  // full screen moves the app in the tree and remounts it: the session belongs to the window
  const [sid, setSid] = useWindowState('vm.sid', 0);
  const [st, setSt] = useWindowState('vm.state', null);
  const [err, setErr] = useWindowState('vm.err', '');
  const [closing, setClosing] = useWindowState('vm.closing', false);
  const [log, setLog] = useState('');
  const [name, setName] = useState(name0);
  const box = useRef(null);
  const sized = useRef({ last: null, want: null, at: 0, rate: false });
  const buttons = useRef(new Set());
  const wheel = useRef(0);
  const sidRef = useRef(sid);
  sidRef.current = sid;
  const closingRef = useRef(closing);
  closingRef.current = closing;

  const begin = () => {
    if (!has()) { setErr(tr('machines.noBackend')); return; }
    const m = lord.vm.list().find((x) => x.dir === dir);
    if (!m) { setErr(tr('machines.win.gone')); return; }
    setName(m.name);
    setLog('');
    const active = stateView(m.state, m.reason).active;
    if (!active) {
      const b = budgetCheck(lord.vm.list(), m, lord.vm.host());
      if (!b.ok) { setErr(tr('machines.win.noMemory', { need: b.need, free: b.free })); return; }
    }
    const id = active ? lord.vm.attach(dir) : lord.vm.start(dir);
    if (!id) { setErr(active ? tr('machines.win.cantAttach') : tr('machines.win.cantStart')); return; }
    sized.current = { last: null, want: null, at: 0, rate: false };
    setErr(''); setSt(null); setSid(id);
  };
  useEffect(() => { if (!sid && !st && !err) begin(); }, []);
  // the window went away (not a remount): let go of the machine; one this window started is shut down by lord-vm
  useEffect(() => () => { if (sid && !windows.get().some((w) => w.id === winId)) lord.vm.close(sid); }, [sid]);

  useEffect(() => {
    if (!sid) return undefined;
    const timer = setInterval(() => {
      const p = lord.vm.poll(sid);
      if (!p) return;
      setSt((prev) => (same(prev, p) ? prev : p));
      const z = sized.current;
      if (p.display === 'up' && !z.rate) { z.rate = true; try { lord.vm.rate(sid, refreshRate(lord.display.list())); } catch (e) { lord.vm.rate(sid, 60); } }
      // the window's size, to the guest after it stayed the same for 400 ms
      if (p.display === 'up' && box.current) {
        const want = guestSize(lord.getLayout(box.current.id), z.last);
        if (!want) z.want = null;
        else if (!z.want || z.want[0] !== want[0] || z.want[1] !== want[1]) { z.want = want; z.at = now(); }
        else if (now() - z.at > 400) { lord.vm.resize(sid, want[0], want[1]); z.last = want; z.want = null; }
      }
      if (p.ended) {
        lord.vm.close(sid);
        setSid(0);
        if (closingRef.current) { setClosing(false); wm.close(winId, true); return; }
        if (p.state !== 'stopped' || !['guest-shutdown', 'host-shutdown', 'poweroff'].includes(p.reason)) setLog(lord.vm.log(dir).split('\n').slice(-24).join('\n'));
      }
    }, 16);
    return () => clearInterval(timer);
  }, [sid]);

  // closing the window shuts the machine down first (a paused one is resumed so it can hear the power button)
  useEffect(() => wm.setCloseGuard(winId, () => {
    const s = sidRef.current;
    if (!s) return true;
    if (!closingRef.current) {
      setClosing(true);
      if (st?.state === 'paused') lord.vm.cmd(s, 'resume');
      lord.vm.cmd(s, 'shutdown');
    }
    return false;
  }), [winId, st?.state]);

  const v = stateView(st?.state, st?.reason);
  useEffect(() => { wm.setTitle(winId, windowTitle(name || dir, err ? tr('machines.state.crashed') : tr(`machines.state.${v.key}`))); }, [name, v.key, err]);

  // keyboard while in front; everything held is let go of on blur
  const live = !!sid && st?.display === 'up' && (st.state === 'running' || st.state === 'paused') && !closing;
  useEffect(() => {
    if (!live || !focused) { if (sid) { lord.vm.release(sid); buttons.current.clear(); } return undefined; }
    const a = addListener('keydown', (e) => { if (e.code > 0) lord.vm.key(sid, e.code, 1); });
    const b = addListener('keyup', (e) => { if (e.code > 0) lord.vm.key(sid, e.code, 0); });
    const c = addListener('pointerup', (e) => { if (buttons.current.has(e.button)) { buttons.current.delete(e.button); lord.vm.button(sid, e.button, 0); } });
    return () => { a(); b(); c(); lord.vm.release(sid); buttons.current.clear(); };
  }, [live, focused, sid]);

  const at = (e) => (box.current && st?.w ? toPicture(e.x, e.y, lord.getLayout(box.current.id), st.w, st.h) : null);
  const move = (e) => { const p = live && at(e); if (p) lord.vm.motion(sid, p[0], p[1]); };
  const down = (e) => { if (!live) return; move(e); buttons.current.add(e.button); lord.vm.button(sid, e.button, 1); };
  const onWheel = (e) => {
    if (!live) return;
    move(e);
    const [n, rest] = wheelSteps(wheel.current, e.deltaY || 0, !!e.precise);
    wheel.current = rest;
    if (n) lord.vm.wheel(sid, n, 0);
  };
  const power = (how) => { if (sid) lord.vm.cmd(sid, how); };

  let card = null;
  if (err) card = <Card title={tr('machines.win.problem')} text={err} actions={[<Button key="r" primary label={tr('machines.win.tryAgain')} onClick={() => { setErr(''); begin(); }} />]} />;
  else if (closing) card = <Card title={tr('machines.win.shuttingDown', { name })} text={tr('machines.win.shuttingDownHint')} actions={[<Button key="f" destructive label={tr('machines.cmd.poweroff')} onClick={() => power('poweroff')} />]} />;
  else if (!sid && st) {
    const why = v.why ? tr(`machines.why.${v.why}`) : '';
    const detail = [st.detail, st.lastError, st.stderr].filter(Boolean).join('\n');
    card = <Card title={tr(`machines.state.${v.key}`)} text={[why, v.why ? tr(`machines.fix.${v.why}`, { code: st.code }) : tr('machines.win.stoppedHint')].filter(Boolean).join(' ')}
      detail={v.why ? (log || detail || '') : ''} actions={[<Button key="s" primary label={tr('machines.start')} onClick={begin} />]} />;
  } else if (!st || st.state === 'starting' || (st.display !== 'up' && st.display !== 'none' && st.display !== 'lost' && (st.state === 'running' || st.state === 'paused'))) {
    card = <Card title={tr('machines.win.starting', { name })} text={tr('machines.win.startingHint')} />;
  } else if (st.display === 'lost') {
    card = <Card title={tr('machines.win.lost')} text={tr('machines.win.lostHint')} actions={[<Button key="d" primary label={tr('machines.win.reconnect')} onClick={() => lord.vm.cmd(sid, 'display')} />]} />;
  } else if (st.display === 'none') card = <Card title={tr('machines.win.noDisplay')} text={tr('machines.win.noDisplayHint')} />;
  else if (st.state === 'stopping') card = <Card title={tr('machines.state.stopping')} text={tr('machines.win.shuttingDownHint')} />;

  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  return (
    <View style={{ flex: 1, backgroundColor: '#000' }}>
      <View ref={box} onMouseDown={down} onMouseMove={move} onWheel={onWheel} style={{ flex: 1, marginTop: full ? 0 : BAR }}>
        {st?.media && st.display === 'up' ? <MediaView media={st.media} style={{ flex: 1 }} /> : null}
      </View>
      {st?.state === 'paused' && !card ? (
        <View style={{ position: 'absolute', top: BAR + 12, left: 0, right: 0, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 12, height: 26, borderRadius: 13, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.6)' }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{tr('machines.state.paused')}</Text>
          </View>
        </View>
      ) : null}
      {card}
      <Bar sid={sid} st={st} name={name} winId={winId} onPower={power} />
    </View>
  );
}
