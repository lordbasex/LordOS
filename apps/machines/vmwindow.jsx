// A machine's window (the approved mocks "Toolbar", "FullScreen" and "BootFailed"): the guest's picture scaled to fit,
// keyboard and pointer while the window is in front, and a toolbar with the machine's name and power light, the
// resolution, Devices, Logs, Screenshot, Pause, Power, Configure and Full Screen; the same actions in the menu bar
// (Actions, Devices). In full screen the bar floats at the top edge. When the machine fails to start or stops with an
// error, the window shows why, a likely fix, the log and Start Again.
import {
  View, Text, Image, MediaView, ScrollView, fs, FilePicker, Toggle, Terminal, Overlay, term, addListener, media, screen, openFileWithApps, useAppWindow, useWindowState, useStore, wm, windows, clipboard, notify, revealPath,
  t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, actionsFor, toPicture, guestSize, refreshRate, wheelSteps, budgetCheck, CTRL_ALT_DEL, windowTitle, closeChoice, resolutions, agentLines, agentTone, clipTo, clipFrom, thumbSrc, AGENT_CAPS, mountCommand } from './model.js';
import { readMeta, writeMeta } from './meta.js';
import { usePal, has, Btn, IconBtn, IconMenuBtn, BarGroup, BarSep, icons, StateDot, TONE, overlayRoot, menus, vmRefusal } from './ui.jsx';
import { openConfigure, openLogs, openSnapshots } from './library.jsx';

const BAR = 48;
const now = () => (typeof lord !== 'undefined' && lord.now ? lord.now() : Date.now());
const THUMB_MS = 5000;
// what changes the screen (frames don't: the media node repaints by itself)
const jobKey = (j) => (j ? `${j.kind}.${j.status}.${j.pct}` : '');
const agKey = (g) => (g ? `${g.state}.${g.caps}.${g.version}.${g.system}.${g.install}` : '');
const same = (a, b) => !!a && !!b && ['state', 'reason', 'code', 'detail', 'display', 'w', 'h', 'media', 'ended', 'lastError', 'stderr', 'cd', 'cdMessage', 'notice', 'noticeSeq', 'console', 'clipIn'].every((k) => a[k] === b[k])
  && jobKey(a.job) === jobKey(b.job) && agKey(a.agent) === agKey(b.agent);

// the agent's state, first on the bar (mock "Session", 6.17): a light, and a popover with what it said and what works
function AgentButton({ ctl }) {
  const [open, setOpen] = useState(false);
  const ref = useRef(null);
  const g = ctl.st?.agent, cfg = ctl.cfg || {};
  // off while the machine is: nothing to wait for
  const tone = ctl.a.start ? 'off' : agentTone(g, cfg.agent);
  const label = !cfg.agent ? tr('machines.agent.off') : g?.state === 'connected' ? tr('machines.agent.connected') : ctl.a.start ? tr('machines.agent.idle') : tr('machines.agent.waiting');
  const light = () => (
    <View style={{ width: 16, height: 16, borderRadius: 8, alignItems: 'center', justifyContent: 'center', border: `1.5px solid ${TONE[tone]}` }}>
      <StateDot tone={tone} size={8} />
    </View>
  );
  return (
    <View ref={ref}>
      <IconBtn icon={light} label={label} on={open} onClick={() => setOpen(!open)} />
      {open ? (
        <Overlay anchor={ref}>
          <View style={{ position: 'absolute', top: 32, left: 0, width: 300 }}>
            <AgentPop ctl={ctl} onClose={() => setOpen(false)} />
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}
function AgentPop({ ctl, onClose }) {
  const p = usePal();
  const ref = useRef(null);
  const g = ctl.st?.agent, cfg = ctl.cfg || {};
  useEffect(() => {
    menus.open++;
    const a = addListener('pointerdown', (e) => { const n = ref.current; if (!n) return; const [lx, ly, w, h] = lord.getLayout(n.id); if (e.x < lx || e.x > lx + w || e.y < ly || e.y > ly + h) onClose(); });
    const b = addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); });
    return () => { menus.open--; a(); b(); };
  }, []);
  const on = g?.state === 'connected';
  const lines = agentLines(g, cfg);
  return (
    <View ref={ref} style={{ padding: 12, gap: 6, borderRadius: 10, backgroundColor: p.dark ? '#2b2b31' : '#fff', border: `1px solid ${p.line}`, boxShadow: '0 12px 34px rgba(0,0,0,0.5)' }}>
      <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{!cfg.agent ? tr('machines.agent.off') : on ? tr('machines.agent.connected') : tr('machines.agent.waiting')}</Text>
      {on ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{[g.system, tr('machines.agent.version', { v: g.version })].filter(Boolean).join(', ')}</Text>
        : <Text style={{ fontSize: 12, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{!cfg.agent ? tr('machines.agent.offHint') : ctl.st?.state === 'running' ? tr('machines.agent.installHint') : tr('machines.agent.bootHint')}</Text>}
      <View style={{ height: 4 }} />
      {lines.map(([k, st]) => (
        <View key={k} style={{ flexDirection: 'row', gap: 8 }}>
          <Text style={{ width: 86, fontSize: 12, fontWeight: 600, color: st === 'works' ? '#7be3a4' : p.text, textAlign: 'start' }}>{tr(`machines.agent.st.${st}`)}</Text>
          <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(`machines.agent.cap.${k}`)}</Text>
        </View>
      ))}
      {on ? (
        <View style={{ flexDirection: 'row', gap: 6, marginTop: 6 }}>
          <Btn label={g.install === 1 ? tr('machines.agent.installing') : tr('machines.agent.installHelper')} disabled={g.install === 1} onClick={() => lord.vm.agent(ctl.sid, 'install')} style={{ height: 28 }} />
          <Btn label={tr('machines.agent.timesync')} onClick={() => lord.vm.agent(ctl.sid, 'timesync')} style={{ height: 28 }} />
        </View>
      ) : null}
      {g?.install === 2 ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.agent.installed')}</Text> : null}
      {g?.install === 3 ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{g.message}</Text> : null}
    </View>
  );
}

// The bar, in groups (5.3): the agent and what it brings (clipboard, folders, apps) | the guest's display and devices |
// shell, logs, screenshot, record | pause and power | configure and full screen. Icons, each named in its tooltip.
function Tools({ ctl, floating }) {
  const { st, a, follow, cfg } = ctl;
  // the mock "Toolbar": sizes that fit this display, then Follow the window (5.3)
  const res = [
    ...resolutions(screen.width, screen.height, st?.w ? [st.w, st.h] : null).map(([w, h]) => ({ label: `${w} × ${h}`, checked: !follow && st?.w === w && st?.h === h, disabled: !a.keys, onClick: () => ctl.fixed(w, h) })),
    '-',
    { label: tr('machines.tb.follow'), checked: follow, onClick: () => ctl.setFollow(true) },
    { label: tr('machines.tb.sizesNote'), disabled: true },
  ];
  const g = st?.agent;
  const clip = cfg?.clipboard || 'off';
  const clipOn = clip !== 'off';
  // the clipboard's directions are part of the machine's description: they change in Configure, with it off
  const clipItems = [
    ...['both', 'to-guest', 'from-guest', 'off'].map((x) => ({ label: tr(`machines.cfg.clip.${x}`), checked: clip === x, disabled: true })),
    '-',
    g?.state === 'connected' && clipOn && !(g.caps & AGENT_CAPS.clipboard)
      ? { label: g.install === 1 ? tr('machines.agent.installing') : tr('machines.agent.installHelper'), disabled: g.install === 1, onClick: () => lord.vm.agent(ctl.sid, 'install') } : null,
    { label: tr('machines.tb.clipChange'), onClick: () => ctl.configure('sharing') },
  ];
  // shared folders (6.7): each one opens in Files; the mount command for a Linux guest is a click away
  const shares = cfg?.shares || [];
  const shareItems = [
    ...shares.map((x) => ({ label: `${x.path.split('/').pop()}  ·  ${x.tag}`, hint: tr(x.writable ? 'machines.tb.shareRw' : 'machines.tb.shareRo'), onClick: () => revealPath(x.path) })),
    ...shares.map((x) => ({ label: tr('machines.tb.shareCopy', { tag: x.tag }), onClick: () => clipboard.set(mountCommand(x.tag)) })),
    shares.length ? '-' : null,
    !shares.length ? { label: tr('machines.tb.sharesNone'), disabled: true } : null,
    { label: shares.length ? tr('machines.tb.shareChange') : tr('machines.tb.shareAdd'), onClick: () => ctl.configure('sharing') },
  ];
  const resLabel = st?.w ? `${tr('machines.tb.resolution')}: ${st.w} × ${st.h}` : tr('machines.tb.resolution');
  return <>
    {!floating ? <>
      <BarGroup>
        <AgentButton ctl={ctl} />
        <IconMenuBtn icon={icons.clipboard} label={`${tr('machines.cfg.clipboard')}: ${tr(`machines.cfg.clip.${clip}`)}`} items={clipItems} color={clipOn ? '#7be3a4' : null} />
        <IconMenuBtn icon={icons.folder} label={shares.length ? tr('machines.tb.foldersN', { n: shares.length }) : tr('machines.tb.folders')} items={shareItems} color={shares.length ? '#7be3a4' : null} />
        <IconBtn icon={icons.apps} label={tr('machines.tb.appsLater')} disabled />
      </BarGroup>
      <BarSep />
    </> : null}
    <BarGroup>
      <IconMenuBtn glyph="⛶" size={15} label={resLabel} items={res} disabled={!a.keys} on={!follow} />
      <IconMenuBtn glyph="⏏" label={tr('machines.tb.devices')} items={ctl.devices} />
    </BarGroup>
    <BarSep />
    <BarGroup>
      {!floating ? <IconBtn glyph=">_" size={12} label={tr('machines.tb.shell')} on={ctl.shell} disabled={!ctl.shell && !ctl.a.reset} onClick={ctl.toggleShell} /> : null}
      <IconBtn glyph="≡" label={tr('machines.cmd.logs')} onClick={ctl.logs} />
      <IconBtn glyph="◉" label={tr('machines.tb.screenshot')} disabled={!a.keys} onClick={ctl.screenshot} />
      {ctl.rec ? <IconBtn icon={icons.stop} danger label={tr('machines.tb.recording', { t: ctl.recTime })} onClick={ctl.record} />
        : <IconBtn icon={icons.record} color="#ff453a" label={tr('machines.tb.record')} disabled={!a.keys} onClick={ctl.record} />}
    </BarGroup>
    {ctl.rec ? <Text style={{ fontSize: 12, fontWeight: 600, color: '#ff6b6b', marginLeft: 4 }}>{ctl.recTime}</Text> : null}
    <BarSep />
    <BarGroup>
      {a.start ? <IconBtn glyph="▶" color="#30d158" label={st?.suspended || stateView(st?.state, st?.reason).key === 'suspended' ? tr('machines.cmd.resume') : tr('machines.start')} onClick={ctl.begin} />
        : <IconBtn glyph={a.resume ? '▶' : null} icon={a.resume ? null : icons.pause} label={a.resume ? tr('machines.cmd.resume') : tr('machines.cmd.pause')} disabled={!a.pause && !a.resume} onClick={() => ctl.cmd(a.resume ? 'resume' : 'pause')} />}
      <IconMenuBtn icon={icons.power} label={tr('machines.cmd.power')} items={ctl.power} disabled={a.start} />
    </BarGroup>
    <BarSep />
    <BarGroup>
      {!floating ? <IconBtn glyph="⚙" label={tr('machines.cmd.configure')} onClick={() => ctl.configure()} /> : null}
      {floating ? <IconBtn glyph="⤡" label={tr('machines.cmd.exitFullscreen')} onClick={() => ctl.setFull(false)} />
        : <IconBtn glyph="⤢" label={tr('machines.cmd.fullscreen')} onClick={() => ctl.setFull(true)} />}
    </BarGroup>
  </>;
}

function Header({ ctl, name }) {
  const p = usePal();
  const v = stateView(ctl.st?.state, ctl.st?.reason);
  return (
    <View style={{ height: BAR, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 12, backgroundColor: p.raised }}>
      <StateDot tone={v.tone} size={9} />
      <Text style={{ marginLeft: 4, fontSize: 13, fontWeight: 700, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: 260 }}>{name}</Text>
      <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap' }}>{tr(`machines.state.${v.key}`)}</Text>
      <View style={{ flex: 1 }} />
      <Tools ctl={ctl} />
    </View>
  );
}

// full screen: a floating bar at the top edge, shown near it; a hint at the bottom for a few seconds
function FloatingBar({ ctl, name }) {
  const p = usePal();
  const [near, setNear] = useState(true);
  const [hint, setHint] = useState(true);
  useEffect(() => { const a = setTimeout(() => setNear(false), 2500), b = setTimeout(() => setHint(false), 5000); return () => { clearTimeout(a); clearTimeout(b); }; }, []);
  const v = stateView(ctl.st?.state, ctl.st?.reason);
  return <>
    <View onMouseEnter={() => setNear(true)} style={{ position: 'absolute', left: 0, right: 0, top: 0, height: near ? 70 : 4, alignItems: 'center' }}>
      {near ? (
        <View onMouseLeave={() => setNear(false)} style={{ marginTop: 10, height: 46, flexDirection: 'row', alignItems: 'center', gap: 4, paddingLeft: 14, paddingRight: 8,
          borderRadius: 14, backgroundColor: 'rgba(28,28,33,0.94)', boxShadow: '0 10px 28px rgba(0,0,0,0.45)' }}>
          <StateDot tone={v.tone} size={9} />
          <Text style={{ marginLeft: 6, marginRight: 10, fontSize: 13, fontWeight: 700, color: '#f2f2f5' }}>{name}</Text>
          <Tools ctl={ctl} floating />
        </View>
      ) : null}
    </View>
    {hint ? (
      <View style={{ position: 'absolute', left: 0, right: 0, bottom: 20, alignItems: 'center', pointerEvents: 'none' }}>
        <View style={{ paddingHorizontal: 14, paddingVertical: 8, borderRadius: 18, backgroundColor: 'rgba(0,0,0,0.68)' }}>
          <Text style={{ fontSize: 13, color: '#f2f2f5' }}>{tr('machines.fs.hint')}</Text>
        </View>
      </View>
    ) : null}
  </>;
}

// the card over the black area: starting, shutting down, no picture, lost display
function Card({ title, text, actions, width = 460, extra }) {
  const p = usePal();
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width, padding: 20, gap: 8, borderRadius: 14, backgroundColor: p.bg, border: `1px solid ${p.line}` }}>
        <Text style={{ fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {text ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{text}</Text> : null}
        {extra}
        {actions?.length ? <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 6 }}>{actions}</View> : null}
      </View>
    </View>
  );
}

// Off or suspended: the last picture, dimmed, and one round Play in the middle (a player's, 5.3)
function OffView({ dir, name, title, hint, label, onPlay }) {
  const [hover, setHover] = useState(false);
  let src = '';
  try { const st = fs.stat(`${dir}/thumb.png`); src = thumbSrc(dir, st?.mtime || 0); } catch (e) { src = ''; }
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: '#0c0c0f' }}>
      {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover', opacity: 0.35 }} /> : null}
      <View onClick={onPlay} title={label} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
        style={{ width: 96, height: 96, borderRadius: 48, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? 'rgba(10,132,255,0.92)' : 'rgba(0,0,0,0.55)',
          border: '2px solid rgba(255,255,255,0.9)', boxShadow: '0 12px 34px rgba(0,0,0,0.5)' }}>
        <Text style={{ fontSize: 40, color: '#fff', marginLeft: 7 }}>▶</Text>
      </View>
      <Text style={{ marginTop: 18, fontSize: 17, fontWeight: 700, color: '#f2f2f5' }}>{name}</Text>
      <Text style={{ marginTop: 4, fontSize: 13, color: 'rgba(242,242,245,0.75)' }}>{title}</Text>
      {hint ? <Text style={{ marginTop: 2, maxWidth: 420, fontSize: 12, color: 'rgba(242,242,245,0.6)', textAlign: 'center' }}>{hint}</Text> : null}
    </View>
  );
}

// BootFailed: why, a likely fix, Start Again, Settings, and the machine's log
function Failed({ name, v, st, log, onStart, onSettings, onAnyway, onDiscard }) {
  const p = usePal();
  const neverRan = v.why === 'start-failed';
  const lines = (log || '').split('\n').filter(Boolean).slice(-60);
  const bad = (l) => /fail|error|refus|crash|exited|killed|cannot|can't|panic/i.test(l);
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: BAR, bottom: 0, padding: 22, gap: 12, backgroundColor: p.bg }}>
      <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{neverRan ? tr('machines.fail.didntStart', { name }) : tr('machines.fail.stopped', { name })}</Text>
      <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr(`machines.why.${v.why}`)} {neverRan ? tr('machines.fail.diskSafe') : ''}</Text>
      <View style={{ paddingHorizontal: 14, paddingVertical: 12, borderRadius: 12, backgroundColor: p.badBg }}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{`${tr('machines.fail.likely')} ${tr(`machines.fix.${v.why}`, { code: st?.code })}`}</Text>
        {st?.detail ? <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start', direction: 'ltr' }}>{st.detail}</Text> : null}
      </View>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Btn big primary label={tr('machines.fail.startAgain')} onClick={onStart} />
        {v.why === 'resume-failed' ? <>
          <Btn big label={tr('machines.fail.resumeAnyway')} onClick={onAnyway} />
          <Btn big danger label={tr('machines.fail.discardState')} onClick={onDiscard} />
        </> : <Btn big label={tr('machines.cmd.configure')} onClick={onSettings} />}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.fail.log')}</Text>
        <Btn label={tr('machines.logs.copy')} onClick={() => clipboard.set(log || '')} />
      </View>
      <ScrollView style={{ flex: 1, borderRadius: 12, backgroundColor: p.dark ? '#16171b' : 'rgba(0,0,0,0.04)' }} contentStyle={{ paddingHorizontal: 14, paddingVertical: 12 }}>
        {lines.map((l, i) => <Text key={i} style={{ fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: bad(l) ? '#ff9a9a' : p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{l}</Text>)}
      </ScrollView>
    </View>
  );
}

export function MachineWindow({ dir, name: name0, loadvm }) {
  const p = usePal();
  const { id: winId, focused } = useAppWindow();
  // full screen moves the app in the tree and remounts it: the session belongs to the window
  const [sid, setSid] = useWindowState('vm.sid', 0);
  const [st, setSt] = useWindowState('vm.state', null);
  const [err, setErr] = useWindowState('vm.err', '');
  const [closing, setClosing] = useWindowState('vm.closing', false);   // false, 'shutdown' or 'suspend'
  const [askClose, setAskClose] = useWindowState('vm.ask', false);
  const [always, setAlways] = useState(false);
  const [follow, setFollow] = useWindowState('vm.follow', true);
  const [log, setLog] = useState('');
  const [name, setName] = useState(name0);
  const [iso, setIso] = useState('');
  const [pickCd, setPickCd] = useState(false);
  const [cdWant, setCdWant] = useState(null);       // the disc asked for while lord-vm changes it
  const [cdErr, setCdErr] = useState('');
  const snapJobs = useRef([]);
  // the Shell (6.18): the guest's serial console in a terminal pane under the picture; the keyboard goes to whichever
  // of the two was clicked last
  const [shell, setShell] = useWindowState('vm.shell', false);
  const [shellKeys, setShellKeys] = useState(false);
  const shellRef = useRef(null);
  const [rec, setRec] = useWindowState('vm.rec', null);       // { id, at } while the picture is recorded
  const [, tick] = useState(0);
  useEffect(() => { if (!rec) return undefined; const t2 = setInterval(() => tick((x) => x + 1), 1000); return () => clearInterval(t2); }, [rec]);
  const box = useRef(null);
  const root = useRef(null);
  const sized = useRef({ last: null, want: null, at: 0, rate: false, thumb: 0 });
  const buttons = useRef(new Set());
  const wheel = useRef(0);
  const sidRef = useRef(sid);
  sidRef.current = sid;
  const closingRef = useRef(closing);
  closingRef.current = closing;
  const followRef = useRef(follow);
  followRef.current = follow;
  overlayRoot.current = root.current;

  const begin = (opts) => {
    if (!has()) { setErr(tr('machines.noBackend')); return; }
    const m = lord.vm.list().find((x) => x.dir === dir);
    if (!m) { setErr(tr('machines.win.gone')); return; }
    setName(m.name); setIso(m.iso || '');
    setLog('');
    const active = stateView(m.state, m.reason).active;
    if (!active) {
      const b = budgetCheck(lord.vm.list(), m, lord.vm.host());
      if (!b.ok) { setErr(tr('machines.win.noMemory', { need: b.need, free: b.free })); return; }
    }
    const id = active ? lord.vm.attach(dir) : lord.vm.start(dir, opts || {});
    if (!id) { setErr(active ? tr('machines.win.cantAttach') : vmRefusal(tr('machines.win.cantStart'))); return; }
    sized.current = { last: null, want: null, at: 0, rate: false, thumb: 0 };
    setErr(''); setSt(null); setSid(id);
  };
  // a window opened to go back to a snapshot (with memory, the machine off) starts from it
  useEffect(() => { if (!sid && !st && !err) begin(loadvm ? { loadvm } : undefined); }, []);
  const stRef = useRef(st);
  stRef.current = st;
  // the window went away (not a remount): let go of the machine; one this window started is shut down by lord-vm
  useEffect(() => () => { if (sid && !windows.get().some((w) => w.id === winId)) lord.vm.close(sid); }, [sid]);

  useEffect(() => {
    if (!sid) return undefined;
    const timer = setInterval(() => {
      const q = lord.vm.poll(sid);
      if (!q) return;
      setSt((prev) => (same(prev, q) ? prev : q));
      snapJobs.current = snapJobs.current.filter((j) => { const r = lord.vm.job(j); if (r?.done && !r.ok) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.snap.failed'), body: r.out.trim().split('\n').pop() || '', sound: false }); return r && !r.done; });
      if (q.cd === 2 || q.cd === 3) setCdWant((w) => { if (w !== null) { if (q.cd === 2) setIso(w); else setCdErr(q.cdMessage || '?'); } return null; });
      const z = sized.current;
      if (q.display === 'up' && !z.rate) { z.rate = true; try { lord.vm.rate(sid, refreshRate(lord.display.list())); } catch (e) { lord.vm.rate(sid, 60); } }
      // the window's size, to the guest after it stayed the same for 400 ms (unless a fixed resolution was chosen)
      if (q.display === 'up' && box.current && followRef.current) {
        const want = guestSize(lord.getLayout(box.current.id), z.last);
        if (!want) z.want = null;
        else if (!z.want || z.want[0] !== want[0] || z.want[1] !== want[1]) { z.want = want; z.at = now(); }
        else if (now() - z.at > 400) { lord.vm.resize(sid, want[0], want[1]); z.last = want; z.want = null; }
      }
      // the card picture in the library, every few seconds while it runs
      if (q.display === 'up' && q.state === 'running' && now() - z.thumb > THUMB_MS) { z.thumb = now(); lord.vm.snapshot(sid); }
      if (q.ended) {
        lord.vm.close(sid);
        setSid(0);
        if (closingRef.current) { setClosing(false); wm.close(winId, true); return; }
        // an unattended install ended with the installer's restart: lord-vm took the installer and the seed disk away,
        // so starting again is the installed system (3.9, "Unattended install")
        if (q.state === 'stopped' && q.reason === 'installed') { setTimeout(() => begin(), 300); return; }
        if (q.state !== 'stopped' || !['guest-shutdown', 'host-shutdown', 'poweroff', 'installed'].includes(q.reason)) setLog(lord.vm.log(dir, 'vm', 16384));
      }
    }, 16);
    return () => clearInterval(timer);
  }, [sid]);

  // Closing the window (5.3): Suspend, Shut Down or Keep Running, asked unless the machine's Configure says which. A
  // shutdown resumes a paused machine first, so it can hear the power button; Keep Running tells lord-vm to stay when
  // this window lets go of it.
  const finish = (how) => {
    const s = sidRef.current;
    setAskClose(false);
    if (!s) { wm.close(winId, true); return; }
    if (how === 'keep') { lord.vm.cmd(s, 'keep'); lord.vm.close(s); setSid(0); wm.close(winId, true); return; }
    setClosing(how);
    if (how === 'suspend') { lord.vm.cmd(s, 'suspend'); return; }
    if (stRef.current?.state === 'paused') lord.vm.cmd(s, 'resume');
    lord.vm.cmd(s, 'shutdown');
  };
  useEffect(() => wm.setCloseGuard(winId, () => {
    const s = sidRef.current;
    const q = stRef.current;
    if (!s || !q || !['running', 'paused', 'starting', 'stopping'].includes(q.state)) return true;
    if (closingRef.current) return false;
    const how = closeChoice(readMeta(dir));
    if (how === 'ask' || q.state === 'starting') setAskClose(true); else finish(how);
    return false;
  }), [winId]);

  // lord-vm's notices (VM_NOTICE): the network going and coming back (6.5), a Clone's new identity (6.3); the good news
  // goes away by itself
  const [noticeGone, setNoticeGone] = useState(0);
  useEffect(() => {
    if (!st?.noticeSeq || !['network-back', 'identity'].includes(st.notice)) return undefined;
    const seq = st.noticeSeq;
    const timer = setTimeout(() => setNoticeGone(seq), 8000);
    return () => clearTimeout(timer);
  }, [st?.noticeSeq]);
  const noticeOn = !!st?.notice && st.noticeSeq !== noticeGone;
  const noticeText = !st?.notice ? '' : st.notice === 'recovered' ? tr('machines.win.recovered')
    : st.notice === 'network-back' ? tr('machines.win.netBack', { s: (Math.max(0, st.noticeCode || 0) / 1000).toFixed(1) })
    : st.notice === 'identity' ? tr('machines.win.identityDone')
    : /reconnecting/.test(st.noticeText || '') ? tr('machines.win.netReconnecting') : tr('machines.win.netLost');
  const v = stateView(st?.state, st?.reason);
  const a = actionsFor(sid ? st?.state : 'stopped');
  useEffect(() => { wm.setTitle(winId, windowTitle(name || dir, err ? tr('machines.state.crashed') : tr(`machines.state.${v.key}`))); }, [name, v.key, err]);

  // keyboard while in front; everything held is let go of on blur
  const live = !!sid && st?.display === 'up' && (st.state === 'running' || st.state === 'paused') && !closing && !askClose && !(shell && shellKeys) && !pickCd;
  // the console's stream, asked for once the pane is open on a running machine
  useEffect(() => { if (shell && sid && !st?.console && (st?.state === 'running' || st?.state === 'paused')) lord.vm.console(sid); }, [shell, sid, st?.state, !!st?.console]);
  useEffect(() => {
    if (!shell || !shellKeys || !focused || !st?.console) return undefined;
    const id = st.console;
    return addListener('keydown', (e) => {
      if (menus.open > 0) return;
      const key = e.key && e.key.length > 1 ? e.key : '';
      const base = e.ctrl || e.alt ? e.base || '' : '';
      if (!key && !e.char && !base) return;
      e.preventDefault?.();                      // Tab goes to the machine, not to the window's buttons (sdk/focus.js)
      term.key(id, key, e.char || '', base, e.mods & 7);
    });
  }, [shell, shellKeys, focused, st?.console]);
  useEffect(() => {
    if (!live || !focused) { if (sid) { lord.vm.release(sid); buttons.current.clear(); } return undefined; }
    // a key pressed while one of the window's menus is open is the menu's, down and up (Esc closes it)
    const kept = new Set();
    const ka = addListener('keydown', (e) => {
      if (!(e.code > 0)) return;
      if (menus.open > 0 || kept.has(e.code)) { kept.add(e.code); return; }
      e.preventDefault?.();                      // Tab goes to the machine, not to the window's buttons (sdk/focus.js)
      lord.vm.key(sid, e.code, 1);
    });
    const kb = addListener('keyup', (e) => {
      if (!(e.code > 0)) return;
      if (kept.delete(e.code)) return;
      lord.vm.key(sid, e.code, 0);
    });
    const kc = addListener('pointerup', (e) => { if (buttons.current.has(e.button)) { buttons.current.delete(e.button); lord.vm.button(sid, e.button, 0); } });
    return () => { ka(); kb(); kc(); lord.vm.release(sid); buttons.current.clear(); };
  }, [live, focused, sid]);

  const at = (e) => (box.current && st?.w ? toPicture(e.x, e.y, lord.getLayout(box.current.id), st.w, st.h) : null);
  const move = (e) => { const q = live && at(e); if (q) lord.vm.motion(sid, q[0], q[1]); };
  const down = (e) => { if (!live) return; move(e); buttons.current.add(e.button); lord.vm.button(sid, e.button, 1); };
  const onWheel = (e) => {
    if (!live) return;
    move(e);
    const [n, rest] = wheelSteps(wheel.current, e.deltaY || 0, !!e.precise);
    wheel.current = rest;
    if (n) lord.vm.wheel(sid, n, 0);
  };
  const cmd = (c) => { if (sid) { if (c === 'shutdown' && st?.state === 'running') lord.vm.snapshot(sid); lord.vm.cmd(sid, c); } };
  const cad = () => {
    if (!sid) return;
    CTRL_ALT_DEL.forEach((k) => lord.vm.key(sid, k, 1));
    [...CTRL_ALT_DEL].reverse().forEach((k) => lord.vm.key(sid, k, 0));
  };
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const m0 = { dir, name, id: winId.split('#vm-')[1] };
  const shot = () => {
    const path = sid ? lord.vm.screenshot(sid) : '';
    notify({ app: 'machines', label: tr('machines.title'), title: path ? tr('machines.tb.shotSaved') : tr('machines.tb.shotFailed'), body: path ? path.split('/').pop() : '', sound: false,
      actions: path ? [{ id: 'show', label: tr('machines.cmd.showInFiles'), run: () => revealPath(path) }] : [] });
  };
  // a long request of lord-vm's in progress (suspend, resume, a snapshot): its card, and no second one
  const job = st?.job?.status === 'running' ? st.job : null;
  const busy = !!job;
  // the CD/DVD drive while the machine runs (lord-vm changes the disc through QMP; machine.json follows)
  const cdOk = !!sid && (st?.state === 'running' || st?.state === 'paused') && cdWant === null;
  const cdTo = (path) => { setCdErr(''); if (lord.vm.cdrom(sid, path || null)) setCdWant(path); else setCdErr(tr('machines.tb.cdRefused')); };
  // Take Snapshot from the window: named by time, with the live picture; lord-vm reports it as a VM_JOB
  const snapNow = () => {
    const d = new Date();
    const r = lord.vm.snapTake(dir, tr('machines.snap.atTime', { time: `${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')}` }), sid);
    if (!r) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.snap.failed'), body: '', sound: false });
    else snapJobs.current.push(r.job);
  };
  // Record (6.20): the machine's own picture into ~/Pictures/Screenshots, no screen permission; a second click stops it
  const record = () => {
    if (rec) { media.stop(rec.id); return; }
    const id = sid ? lord.vm.record(sid) : 0;
    if (!id) { notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.tb.recordFailed'), body: '', sound: false }); return; }
    setRec({ id, at: now() });
    const off = media.on(id, (type, d) => {
      if (type !== 'done') return;
      off();
      setRec(null);
      const secs = Math.round((d.duration || 0) / 1000);
      notify({ app: 'machines', label: tr('machines.title'), title: d.ok ? tr('machines.tb.recSaved') : tr('machines.tb.recordFailed'),
        body: d.ok ? tr('machines.tb.recSavedBody', { file: (d.path || '').split('/').pop(), m: Math.floor(secs / 60), s: secs % 60 }) : '', sound: false,
        actions: d.ok && d.path ? [{ id: 'open', label: tr('machines.tb.open'), run: () => openFileWithApps(d.path, (d.path || "").split("/").pop()) }, { id: 'show', label: tr('machines.cmd.showInFiles'), run: () => revealPath(d.path) }] : [] });
    });
  };
  const recSecs = rec ? Math.floor((now() - rec.at) / 1000) : 0;
  // the shared clipboard (6.8), through the agent's channel and only the ways the machine allows: the host's clipboard
  // (text, rich text, a picture) goes in when the window comes to the front, the guest's comes out as it changes; a
  // guest's picture is re-encoded on this side before any app sees it
  const cfgRef = useRef({});
  const [cfg, setCfg] = useState({});
  useEffect(() => { if (!has()) return; const m = lord.vm.list().find((x) => x.dir === dir); if (m) { cfgRef.current = m; setCfg({ agent: m.agent, clipboard: m.clipboard, shares: m.shares || [] }); } }, [sid, st?.state]);
  const clipSent = useRef('');
  useEffect(() => {
    if (!focused || !sid || !clipTo(cfg.clipboard) || st?.agent?.state === undefined) return;
    if (!(st?.agent?.caps & 32)) return;
    if (lord.vm.clipboardSendHost) { lord.vm.clipboardSendHost(sid); return; }
    const text = clipboard.get();
    if (text && text !== clipSent.current && lord.vm.clipboardSend(sid, text)) clipSent.current = text;
  }, [focused, sid, cfg.clipboard, st?.agent?.caps]);
  const clipSeen = useRef(0);
  useEffect(() => {
    if (!sid || !st?.clipIn || st.clipIn === clipSeen.current) return;
    clipSeen.current = st.clipIn;
    if (!clipFrom(cfg.clipboard)) return;
    const kind = lord.vm.clipboardApply ? lord.vm.clipboardApply(sid) : 'text';
    if (kind !== 'text') return;
    const text = lord.vm.clipboardTake(sid);
    if (text) { clipSent.current = text; clipboard.set(text, { origin: 'machines' }); }
  }, [st?.clipIn]);
  const ctl = {
    sid, cfg,
    shell, toggleShell: () => { setShell(!shell); setShellKeys(!shell); },
    rec: !!rec, recTime: `${String(Math.floor(recSecs / 60)).padStart(2, '0')}:${String(recSecs % 60).padStart(2, '0')}`, record,
    st, a, full, follow, cmd,
    setFull: (on) => wm.setFullscreen(winId, on),
    setFollow: (on) => { setFollow(on); sized.current.last = null; },
    fixed: (w, h) => { setFollow(false); if (sid) lord.vm.resize(sid, w, h); },
    logs: () => openLogs(m0),
    configure: (section) => openConfigure(m0, section),
    begin: () => begin(),
    screenshot: shot,
    power: [
      { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => cmd('shutdown') },
      { label: tr('machines.cmd.suspend'), disabled: !a.suspend || busy, onClick: () => cmd('suspend') },
      { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => cmd('reset') },
      { label: tr('machines.cmd.cad'), disabled: !a.keys, onClick: cad },
      '-',
      { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => cmd('poweroff') },
    ],
    devices: [
      { label: iso ? tr('machines.tb.cdIn', { name: iso.split('/').pop() }) : tr('machines.tb.cdNone'), disabled: true },
      { label: tr('machines.tb.cdChoose'), disabled: !cdOk, onClick: () => setPickCd(true) },
      { label: tr('machines.tb.cdEject'), disabled: !cdOk || !iso, onClick: () => cdTo('') },
      '-',
      { label: tr('machines.tb.sound'), disabled: true, hint: tr('machines.later') },
    ],
  };
  useEffect(() => {
    wm.setMenus(winId, [
      { label: tr('machines.menu.actions'), items: [
        { label: a.resume ? tr('machines.cmd.resume') : tr('machines.cmd.pause'), disabled: !a.pause && !a.resume, onClick: () => cmd(a.resume ? 'resume' : 'pause') },
        { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => cmd('reset') },
        { label: tr('machines.cmd.cad'), disabled: !a.keys, onClick: cad },
        '-',
        { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => cmd('shutdown') },
        { label: tr('machines.cmd.suspend'), disabled: !a.suspend || busy, onClick: () => cmd('suspend') },
        { label: tr('machines.cmd.poweroff'), disabled: !a.poweroff, onClick: () => cmd('poweroff') },
        '-',
        { label: tr('machines.snap.takeNow'), disabled: !sid || busy || !(st?.state === 'running' || st?.state === 'paused'), onClick: snapNow },
        { label: tr('machines.cmd.snapshots'), onClick: () => openSnapshots(m0) },
        '-',
        { label: tr('machines.tb.screenshot'), disabled: !a.keys, onClick: shot },
        { label: full ? tr('machines.cmd.exitFullscreen') : tr('machines.cmd.fullscreen'), onClick: () => wm.setFullscreen(winId, !full) },
        { label: tr('machines.cmd.logs'), onClick: () => openLogs(m0) },
        { label: tr('machines.cmd.configure'), onClick: () => openConfigure(m0) },
      ] },
      { label: tr('machines.menu.devices'), items: ctl.devices.filter((x) => x !== '-') },
    ]);
  }, [st?.state, full, iso, sid, cdWant]);

  let card = null;
  let failed = null;
  if (err) card = <Card title={tr('machines.win.problem')} text={err} actions={[<Btn key="r" primary label={tr('machines.win.tryAgain')} onClick={() => { setErr(''); begin(); }} />]} />;
  else if (askClose) card = <Card width={560} title={tr('machines.close.title', { name })} text={tr('machines.close.text')}
    extra={<View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 4 }}><Toggle on={always} onChange={setAlways} /><Text style={{ fontSize: 13, color: p.text }}>{tr('machines.close.always')}</Text></View>} actions={[
    <Btn key="c" label={tr('machines.cancel')} onClick={() => setAskClose(false)} />,
    <Btn key="k" label={tr('machines.close.keep')} onClick={() => { if (always) writeMeta(dir, { onClose: 'keep' }); finish('keep'); }} />,
    <Btn key="s" label={tr('machines.cmd.shutdown')} onClick={() => { if (always) writeMeta(dir, { onClose: 'shutdown' }); finish('shutdown'); }} />,
    <Btn key="u" primary label={tr('machines.cmd.suspend')} disabled={st?.state === 'starting'} onClick={() => { if (always) writeMeta(dir, { onClose: 'suspend' }); finish('suspend'); }} />,
  ]} />;
  else if (job) card = <Card title={tr(`machines.job.${job.kind}`, { name })} text={job.pct >= 0 ? tr('machines.job.pct', { pct: job.pct }) : tr('machines.job.wait')} />;
  else if (closing === 'suspend') card = <Card title={tr('machines.job.suspend', { name })} text={tr('machines.job.wait')} />;
  else if (closing) card = <Card title={tr('machines.win.shuttingDown', { name })} text={tr('machines.win.shuttingDownHint')} actions={[<Btn key="f" danger label={tr('machines.cmd.poweroff')} onClick={() => cmd('poweroff')} />]} />;
  else if (!sid && st) {
    if (v.why) failed = <Failed name={name} v={v} st={st} log={log || [st.detail, st.lastError, st.stderr].filter(Boolean).join('\n')} onStart={() => begin()} onSettings={() => openConfigure(m0)}
      onAnyway={() => begin({ resumeAnyway: true })} onDiscard={() => begin({ discardState: true })} />;
    else if (v.key === 'suspended') card = <OffView dir={dir} name={name} title={tr('machines.state.suspended')} hint={tr('machines.win.suspendedHint')} label={tr('machines.cmd.resume')} onPlay={() => begin()} />;
    else card = <OffView dir={dir} name={name} title={tr(`machines.state.${v.key}`)} hint={tr('machines.win.playHint')} label={tr('machines.start')} onPlay={() => begin()} />;
  } else if (!st || st.state === 'starting' || (st.display !== 'up' && st.display !== 'none' && st.display !== 'lost' && (st.state === 'running' || st.state === 'paused'))) {
    card = <Card title={tr('machines.win.starting', { name })} text={tr('machines.win.startingHint')} />;
  } else if (st.display === 'lost') {
    card = <Card title={tr('machines.win.lost')} text={tr('machines.win.lostHint')} actions={[<Btn key="d" primary label={tr('machines.win.reconnect')} onClick={() => lord.vm.cmd(sid, 'display')} />]} />;
  } else if (st.display === 'none') card = <Card title={tr('machines.win.noDisplay')} text={tr('machines.win.noDisplayHint')} />;
  else if (st.state === 'stopping') card = <Card title={tr('machines.state.stopping')} text={tr('machines.win.shuttingDownHint')} />;

  return (
    <View ref={root} style={{ flex: 1, backgroundColor: '#000' }}>
      <View ref={box} onMouseDown={(e) => { setShellKeys(false); down(e); }} onMouseMove={move} onWheel={onWheel} style={{ flex: 1, marginTop: full ? 0 : BAR }}>
        {st?.media && st.display === 'up' ? <MediaView media={st.media} style={{ flex: 1 }} /> : null}
      </View>
      {shell && !full ? (
        <View onMouseDown={() => setShellKeys(true)} style={{ height: 260, borderTopWidth: 2, borderColor: shellKeys ? '#0a84ff' : p.line, backgroundColor: '#101114' }}>
          <View style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, backgroundColor: '#1b1c20' }}>
            <View style={{ paddingHorizontal: 6, height: 18, borderRadius: 4, justifyContent: 'center', backgroundColor: '#3a2a5c' }}>
              <Text style={{ fontSize: 11, fontWeight: 700, color: '#d9c9ff' }}>{tr('machines.shell.badge')}</Text>
            </View>
            <Text style={{ flex: 1, fontSize: 12, color: '#c9c9d1', textAlign: 'start' }}>{tr('machines.shell.title', { name })}</Text>
            <Text style={{ fontSize: 11, color: '#8a8a94' }}>{shellKeys ? tr('machines.shell.keysHere') : tr('machines.shell.clickHere')}</Text>
            <Btn label="×" onClick={() => { setShell(false); setShellKeys(false); }} style={{ height: 20, paddingHorizontal: 8 }} />
          </View>
          {st?.console ? <Terminal ref={shellRef} session={st.console} style={{ flex: 1 }} />
            : <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 12, color: '#8a8a94' }}>{a.reset ? tr('machines.shell.connecting') : tr('machines.shell.off')}</Text></View>}
        </View>
      ) : null}
      {st?.state === 'paused' && !card ? (
        <View style={{ position: 'absolute', top: BAR + 12, left: 0, right: 0, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 12, height: 26, borderRadius: 13, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.6)' }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{tr('machines.state.paused')}</Text>
          </View>
        </View>
      ) : null}
      {card}
      {failed}
      {!card && !failed && ((noticeOn && noticeText) || cdErr) ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 16, alignItems: 'center' }}>
          <View style={{ maxWidth: 560, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingVertical: 9, borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.94)' }}>
            <Text style={{ flex: 1, fontSize: 13, lineHeight: 1.4, color: '#f2f2f5', textAlign: 'start' }}>{cdErr ? tr('machines.tb.cdFailed', { why: cdErr }) : noticeText}</Text>
            {cdErr ? <Btn label={tr('machines.ok')} onClick={() => setCdErr('')} style={{ height: 28 }} />
              : ['network-back', 'identity'].includes(st.notice) ? <Btn label={tr('machines.ok')} onClick={() => setNoticeGone(st.noticeSeq)} style={{ height: 28 }} />
              : <Btn label={tr('machines.cmd.logs')} onClick={() => openLogs(m0)} style={{ height: 28 }} />}
          </View>
        </View>
      ) : null}
      {pickCd ? <FilePicker title={tr('machines.new.pickIso')} filter={/\.iso$/i} onClose={() => setPickCd(false)}
        onPick={(x) => { setPickCd(false); cdTo(Array.isArray(x) ? x[0] : x); }} /> : null}
      {full ? <FloatingBar ctl={ctl} name={name} /> : <View style={{ position: 'absolute', left: 0, right: 0, top: 0 }}><Header ctl={ctl} name={name} /></View>}
    </View>
  );
}
