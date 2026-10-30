// Cleaner — explains what uses the disk and helps remove what can safely go (docs/CLEANER.md). The rule is
// analyse, review, plan, confirm, execute, verify: a scan only reads, and the app holds candidate ids and display
// paths, never authority. Scanning runs in the sandboxed lord-clean helper; plans and their execution stay in
// lord-server (lord.clean). Sections appear only for what works.
import {
  View, cleaner, wm, windows, useStore, useAppWindow, useSettings, userDirs, t as tr, spacing,
} from '@lordos/sdk';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Label, Sky, Tints, Tile, Orb, Enter, PalProvider, usePal, tween } from './ui.jsx';
import { Dashboard, MODULES, moduleResult } from './dashboard.jsx';
import { ReviewBoard } from './review.jsx';
import { PlanConfirm, PlanResult, AskDialog } from './flow.jsx';
import { Working, ScanHero, CleanupList, LargeHero, LargeList, Activity, Preferences, prefsOf } from './sections.jsx';
import { StorageHero, StorageView } from './storage.jsx';
import { DupHero, DupList } from './duplicates.jsx';

// ---- icon: a broom with a spark (drawn with View) ----
export function Icon({ size }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, overflow: 'hidden', direction: 'ltr',
      background: 'linear-gradient(160deg, #1b1f6b 0%, #0d1138 100%)' }}>
      <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.12, width: s * 0.06, height: s * 0.46, borderRadius: s * 0.03, backgroundColor: '#EEEAFF' }} />
      <View style={{ position: 'absolute', left: s * 0.39, top: s * 0.54, width: s * 0.22, height: s * 0.08, borderRadius: s * 0.02, backgroundColor: '#ffffff' }} />
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.62, width: s * 0.4, height: s * 0.24,
        borderBottomLeftRadius: s * 0.1, borderBottomRightRadius: s * 0.1, background: 'linear-gradient(180deg, #FFF1A8 0%, #FFB000 100%)' }} />
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.26, width: s * 0.11, height: s * 0.11, borderRadius: s * 0.015,
        backgroundColor: '#ffffff', transform: 'rotate(45deg)' }} />
      <View style={{ position: 'absolute', left: s * 0.71, top: s * 0.4, width: s * 0.07, height: s * 0.07, borderRadius: s * 0.01,
        backgroundColor: '#ffffff', opacity: 0.8, transform: 'rotate(45deg)' }} />
    </View>
  );
}

const TICK_MS = 200;
const ART = { cleanup: 'cleanup', large: 'large', storage: 'storage', duplicates: 'duplicates' };
const SECTIONS = ['scan', 'cleanup', 'large', 'storage', 'duplicates'];
const BOTTOM = ['activity', 'settings'];

function SideItem({ id, section, go, rail, badge }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const on = section === id;
  return (
    <View onClick={() => go(id)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 46, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: spacing.m, borderRadius: 13, border: `1px solid ${on ? p.line : 'transparent'}`,
        backgroundColor: on || hover ? p.glass : 'transparent' }}>
      <Tile name={id} size={28} />
      {rail ? null : <Label size={14} lines={1}>{tr(`cleaner.section.${id}`)}</Label>}
      {badge ? <View style={{ position: 'absolute', left: 30, top: 2, paddingHorizontal: 5, height: 16, borderRadius: 8, justifyContent: 'center', backgroundColor: p.accent }}><Label size={10} weight={700} style={{ color: p.btnText }}>{badge}</Label></View> : null}
    </View>
  );
}

function Lights({ id }) {
  const [hover, setHover] = useState(false);
  const dot = (c, act) => <View onMouseDown={(e) => e.stopPropagation()} onClick={() => act(id)} style={{ width: 12, height: 12, borderRadius: 6, backgroundColor: c, opacity: hover ? 1 : 0.85 }} />;
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ flexDirection: 'row', gap: 8, paddingVertical: 6, direction: 'ltr' }}>
      {dot('#ff5f57', wm.close)}{dot('#febc2e', wm.minimize)}{dot('#28c840', wm.toggleMaximize)}
    </View>
  );
}
const KIND = { scan: 'rules', cleanup: 'rules', large: 'tree', storage: 'tree', duplicates: 'dups' };

export default function Cleaner() { return <PalProvider><Body /></PalProvider>; }

function Body() {
  const p = usePal();
  const { id: winId, args } = useAppWindow() || {};
  const win = useStore(windows, (ws) => ws.find((w) => w.id === winId));
  const [s, setSettings] = useSettings();
  const prefs = prefsOf(s);
  const [section, setSection] = useState((args && args.section) || 'scan');
  const [status, setStatus] = useState(() => cleaner.status());
  const [gen, setGen] = useState(0);
  const [polling, setPolling] = useState(false);
  const [error, setError] = useState('');
  const [chosen, setChosen] = useState(() => new Set());
  const [picked, setPicked] = useState(() => new Set());
  const [models, setModels] = useState(() => new Set());
  const [confirm, setConfirm] = useState(false);
  const [emptyAsk, setEmptyAsk] = useState(false);
  const [resultOf, setResultOf] = useState('');
  const planShown = useRef(0);
  const [volume, setVolume] = useState('home');
  const [pipe, setPipe] = useState(null);        // the full scan: { step, results } or null
  const pipeRef = useRef(null);
  const [board, setBoard] = useState(false);
  const [under, setUnder] = useState('');
  const side = useRef(null);
  const volumes = useMemo(() => cleaner.volumes(), [gen]);
  const dirs = useMemo(() => {
    const u = userDirs();
    return [...new Set(['documents', 'pictures', 'music', 'videos', 'downloads'].map((k) => u[k]).filter(Boolean))].map((p) => p.slice(u.home.length + 1)).filter(Boolean);
  }, []);
  const [folders, setFolders] = useState(() => new Set(dirs.slice(0, 1)));

  const kind = KIND[section];
  const info = cleaner.poll(kind) || { state: 'idle' };
  const scan = info.scan;
  const plan = info.plan;
  const running = info.state === 'running';
  const ready = !!scan && !scan.running && scan.id > 0 && !(info.job === 'scan' && (info.state === 'cancelled' || info.state === 'failed'));
  const planHere = plan && scan && plan.scan === scan.id;

  useEffect(() => {
    if (!polling) return undefined;
    let t = 0;
    const tick = () => {
      const p = cleaner.poll();
      setGen((g) => g + 1);
      if (p && p.state === 'running') t = setTimeout(tick, TICK_MS);
      else {
        setStatus(cleaner.status());
        if (p && p.job === 'scan' && p.state === 'done' && p.scan.kind === 'rules') {
          setSettings({ cleaner: { ...prefsOf(s), lastScan: { at: Math.floor(Date.now() / 1000), allocated: p.scan.estimated, candidates: p.scan.candidates } } });
          setChosen(new Set(p.scan.rules.filter((r) => r.risk === 'regenerable' && r.action === 'delete' && r.files > 0 && !r.explicit).map((r) => r.id)));
          setSection((c) => (c === 'cleanup' || q ? c : c));
        }
        const q = pipeRef.current;
        if (q && p && p.job === 'scan' && p.state === 'done') {
          const results = { ...q.results, [MODULES[q.step].id]: moduleResult(MODULES[q.step].id, p.scan) };
          const step = q.step + 1;
          if (step < MODULES.length) {
            pipeRef.current = { step, results };
            setPipe(pipeRef.current);
            startModule(step);
            t = setTimeout(tick, TICK_MS);
            return;
          }
          pipeRef.current = null;
          setPipe(null);
          setBoard(true);
        } else if (q) { pipeRef.current = null; setPipe(null); }
        setPolling(false);
      }
    };
    t = setTimeout(tick, TICK_MS);
    return () => clearTimeout(t);
  }, [polling]);
  useEffect(() => () => cleaner.cancel(true), []);

  const attempt = (fn) => { setError(''); try { fn(); } catch (e) { setError(e.message); } setGen((g) => g + 1); };
  const go = (id) => { setSection(id); setResultOf(''); setError(''); setUnder(''); };
  const watch = () => setPolling(true);

  const startModule = (step) => {
    const m = MODULES[step];
    if (m.kind === 'rules') cleaner.start('rules', { exclude: prefs.exclude });
    else if (m.kind === 'tree') cleaner.start('tree', { volume: 'home', exclude: prefs.exclude });
    else cleaner.start('dups', { folders: dirs.slice(0, 8), exclude: prefs.exclude });
  };
  const startAll = () => attempt(() => {
    setBoard(false);
    setResultOf('');
    planShown.current = 0;
    pipeRef.current = { step: 0, results: {} };
    setPipe(pipeRef.current);
    startModule(0);
    setPolling(true);
  });
  useEffect(() => {
    if (!side.current) return;
    const to = pipe ? 76 : 236, from = pipe ? 236 : 76;
    tween(side.current, { width: [from, to] }, { ms: 420 });
  }, [!!pipe]);
  const start = () => attempt(() => {
    setPicked(new Set());
    setResultOf('');
    planShown.current = 0;
    if (kind === 'rules') cleaner.start('rules', { exclude: prefs.exclude });
    else if (kind === 'tree') cleaner.start('tree', { volume, exclude: prefs.exclude });
    else cleaner.start('dups', { folders: [...folders], exclude: prefs.exclude });
    watch();
  });
  const stop = () => { cleaner.cancel(false); setGen((g) => g + 1); };
  const makePlan = () => attempt(() => {
    if (kind === 'rules') cleaner.plan(scan.id, { rules: [...chosen] });
    else cleaner.plan(scan.id, [...picked]);
    setResultOf('');
    watch();
  });
  useEffect(() => {
    if (plan && plan.state === 'ready' && planShown.current !== plan.id) { planShown.current = plan.id; setConfirm(true); }
    if (plan && plan.state === 'finished' && resultOf !== plan.id) setResultOf(plan.id);
  }, [plan && plan.id, plan && plan.state]);
  const execute = () => attempt(() => { setConfirm(false); cleaner.execute(plan.id); watch(); });
  const emptyTrash = () => attempt(() => { setEmptyAsk(false); cleaner.emptyTrash(); watch(); });
  useEffect(() => {
    if (info.job === 'emptyTrash' && info.state === 'done' && !polling && section === 'cleanup') attempt(() => { cleaner.start('rules', { exclude: prefs.exclude }); watch(); });
  }, [info.job, info.state]);

  const showResult = planHere && resultOf === plan.id && plan.state === 'finished';
  const showing = (() => {
    if (section === 'activity') return <Activity gen={gen} />;
    if (section === 'settings') return <Preferences />;
    if (showResult) return <PlanResult key={plan.id} plan={plan} onDone={() => { setResultOf(''); start(); }} />;
    if (pipe) {
      const l = cleaner.poll();
      const g = win ? wm.frame(win) : { w: 1100, h: 700 };
      return <Dashboard W={g.w - 76 - 76} H={g.h - 150} step={pipe.step} results={pipe.results} scan={l.job === 'scan' ? l.scan : null} />;
    }
    if (running && info.job !== 'emptyTrash') return <Working info={cleaner.poll()} art={ART[section] || 'scan'} />;
    if (section === 'scan' && board) return <ReviewBoard rules={cleaner.poll('rules').scan} tree={cleaner.poll('tree').scan} dups={cleaner.poll('dups').scan} go={go} />;
    if (section === 'scan' || (section === 'cleanup' && !ready)) return <ScanHero key={section} status={status} last={prefs.lastScan} />;
    if (section === 'cleanup') return <CleanupList key={scan.id} scan={scan} chosen={chosen} setChosen={setChosen} onEmpty={() => setEmptyAsk(true)} blockedOf={(r) => (r.root === 'busy' ? 'busy' : '')} models={models} setModels={setModels}
      onRemoveModels={() => attempt(() => { cleaner.plan(scan.id, [...models]); setModels(new Set()); setResultOf(''); watch(); })} />;
    if (section === 'large') return ready ? <LargeList key={`${scan.id}.${under}`} scan={scan} prefs={prefs} selected={picked} setSelected={setPicked} under={under} clearUnder={() => setUnder('')} /> : <LargeHero />;
    if (section === 'storage') return ready ? <StorageView key={scan.id} scan={scan} onScan={start} volume={volumes.find((v) => v.id === volume)} onReview={(rel) => { setSection('large'); setUnder(rel); }} /> : <StorageHero volumes={volumes} volume={volume} setVolume={setVolume} />;
    return ready ? <DupList key={scan.id} scan={scan} selected={picked} setSelected={setPicked} />
      : <DupHero folders={dirs} chosen={folders} toggle={(f) => setFolders((o) => { const x = new Set(o); if (x.has(f)) x.delete(f); else x.add(f); return x; })} />;
  })();

  // what the round button does here
  const orb = (() => {
    if (section === 'activity' || section === 'settings' || showResult) return null;
    if (running) return info.job === 'scan' ? { label: tr('cleaner.stop'), onClick: () => { pipeRef.current = null; setPipe(null); stop(); }, busy: true } : null;
    if (section === 'scan' && board) return null;
    if (section === 'scan') return { label: tr('cleaner.scan'), onClick: startAll, disabled: !status.available };
    if (section === 'cleanup' && ready) return { label: tr('cleaner.clean'), onClick: makePlan, disabled: !chosen.size };
    if (section === 'storage' && ready) return null;
    if ((section === 'large' || section === 'duplicates') && ready && picked.size) return { label: tr('cleaner.orb.trash'), onClick: makePlan };
    if (section === 'duplicates' && !folders.size) return { label: tr('cleaner.scan'), disabled: true };
    return { label: tr('cleaner.scan'), onClick: start, disabled: !status.available };
  })();

  const failed = info.state === 'failed' && info.error ? tr(`cleaner.error.${info.error}`) : '';

  return (
    <View style={{ flex: 1, flexDirection: 'row', overflow: 'hidden', borderRadius: 12 }}>
      <Sky />
      <Tints section={section} />
      <View onMouseDown={() => wm.beginMove(winId)} onDoubleClick={() => wm.toggleMaximize(winId)} style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 34 }} />
      <View ref={side} style={{ width: 236, flexShrink: 0, paddingHorizontal: 14, paddingTop: 14, paddingBottom: 14, gap: 4, overflow: 'hidden', backgroundColor: p.side, borderInlineEnd: `1px solid ${p.line}` }}>
        <View style={{ paddingHorizontal: 6, marginBottom: spacing.m }}><Lights id={winId} /></View>
        {SECTIONS.map((id) => <SideItem key={id} id={id} section={section} go={go} rail={!!pipe} badge={pipe && id === 'scan' ? `${pipe.step + 1}/${MODULES.length}` : ''} />)}
        <View style={{ flex: 1 }} />
        {BOTTOM.map((id) => <SideItem key={id} id={id} section={section} go={go} rail={!!pipe} />)}
      </View>
      <View style={{ flex: 1, minWidth: 0, paddingHorizontal: 38, paddingTop: 34 }}>
        {board && section === 'scan' && !pipe ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', marginBottom: spacing.s, marginTop: -18 }}>
            <View onClick={startAll} style={{ width: 120 }}><Label size={13} style={{ color: p.accent }}>{tr('cleaner.startOver')}</Label></View>
            <View style={{ flex: 1 }}><Label size={13} weight={600} align="center">{tr('cleaner.section.scan')}</Label></View>
            <View style={{ width: 120 }} />
          </View>
        ) : null}
        {!status.available ? <Label>{tr('cleaner.unavailable')}</Label> : null}
        {error ? <Label>{error}</Label> : null}
        {failed ? <Label>{failed}</Label> : null}
        <Enter key={section} style={{ flex: 1, minHeight: 0 }}>{showing}</Enter>
        {orb && orb.label ? <Orb key={orb.busy ? 'busy' : 'idle'} label={orb.label} onClick={orb.onClick} disabled={orb.disabled} busy={orb.busy} /> : null}
      </View>
      {confirm && plan && plan.state === 'ready' ? <PlanConfirm plan={plan} onOk={execute} onCancel={() => setConfirm(false)} /> : null}
      {emptyAsk ? <AskDialog title={tr('cleaner.empty.title')} note={tr('cleaner.empty.detail')} okLabel={tr('cleaner.empty.ok')} onOk={emptyTrash} onCancel={() => setEmptyAsk(false)} /> : null}
    </View>
  );
}
