// Cleaner — explains what uses the disk and helps remove what can safely go (docs/CLEANER.md). The rule is
// analyse, review, plan, confirm, execute, verify: a scan only reads, and the app holds candidate ids and display
// paths, never authority. Scanning runs in the sandboxed lord-clean helper; plans and their execution stay in
// lord-server (lord.clean). Sections appear only for what works.
import {
  View, cleaner, wm, windows, useStore, useAppWindow, useSettings, useWindowButtons, themes, userDirs, t as tr, formatBytes, spacing,
} from '@lordos/sdk';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Label, Sky, Tints, Tile, Orb, Enter, Pill, GlassButton, PalProvider, usePal, tween } from './ui.jsx';
import { Dashboard, MODULES, moduleResult } from './dashboard.jsx';
import { ReviewBoard } from './review.jsx';
import { PlanConfirm, PlanResult, AskDialog } from './flow.jsx';
import { Working, ScanHero, CleanupList, allIds, LargeHero, LargeList, Activity, Preferences, prefsOf } from './sections.jsx';
import { StorageHero, StorageView } from './storage.jsx';
import { DupHero, DupList } from './duplicates.jsx';
import { SimilarHero, SimilarList } from './similar.jsx';
import { AppsView } from './apps.jsx';

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
// a scan of a nearly empty disk ends at once: each step still stays long enough for its animation to be seen
const MIN_SCAN_MS = 1800, MIN_STEP_MS = 2600;
const ART = { cleanup: 'cleanup', large: 'large', storage: 'storage', duplicates: 'duplicates' };
const SECTIONS = ['scan', 'cleanup', 'large', 'storage', 'duplicates', 'apps'];
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
const KIND = { scan: 'rules', cleanup: 'rules', large: 'tree', storage: 'tree', duplicates: 'dups', apps: 'rules' };

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
  const [emptied, setEmptied] = useState('');
  const [resultOf, setResultOf] = useState('');
  const planShown = useRef(0);
  const [volume, setVolume] = useState('home');
  const [pipe, setPipe] = useState(null);        // the full scan: { step, results } or null
  const pipeRef = useRef(null);
  const [board, setBoard] = useState(false);
  const [under, setUnder] = useState('');
  const side = useRef(null);
  const [hold, setHold] = useState(false);
  const holdTimer = useRef(0), stepAt = useRef(0);
  useEffect(() => () => clearTimeout(holdTimer.current), []);
  const volumes = useMemo(() => cleaner.volumes(), [gen]);
  const dirs = useMemo(() => {
    const u = userDirs();
    const all = [...new Set(['documents', 'pictures', 'music', 'videos', 'downloads'].map((k) => u[k]).filter(Boolean))].map((p) => p.slice(u.home.length + 1)).filter(Boolean);
    return all.filter((d) => !all.some((o) => d.startsWith(`${o}/`)));   // a scan refuses a folder inside another
  }, []);
  const [folders, setFolders] = useState(() => new Set(dirs.slice(0, 1)));
  const [similar, setSimilar] = useState(false);
  const [imageFolders, setImageFolders] = useState(() => new Set(dirs.slice(1, 2)));
  const pickFrom = (set) => (f) => set((o) => { const x = new Set(o); if (x.has(f)) x.delete(f); else x.add(f); return x; });

  const kind = section === 'duplicates' && similar ? 'images' : KIND[section];
  const info = cleaner.poll(kind) || { state: 'idle' };
  const scan = info.scan;
  const plan = info.plan;
  const running = info.state === 'running';
  // a section's results stay in memory until its next scan; "Start over" only stops showing them
  const [hidden, setHidden] = useState({});
  const ready = !!scan && !scan.running && scan.id > 0 && hidden[kind] !== scan.id && !(info.job === 'scan' && (info.state === 'cancelled' || info.state === 'failed'));
  const planHere = plan && scan && plan.scan === scan.id;
  useEffect(() => { setPicked(new Set()); setModels(new Set()); }, [scan && scan.id]);   // ids belong to one scan, however it started

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
          pipeRef.current = { step: q.step, results };
          setPipe(pipeRef.current);
          t = setTimeout(() => {
            if (!pipeRef.current) { setPolling(false); return; }      // stopped while the step was still showing
            if (step < MODULES.length) {
              pipeRef.current = { step, results };
              setPipe(pipeRef.current);
              stepAt.current = Date.now();
              startModule(step);
              t = setTimeout(tick, TICK_MS);
              return;
            }
            pipeRef.current = null;
            setPipe(null);
            setBoard(true);
            setPolling(false);
          }, Math.max(0, MIN_STEP_MS - (Date.now() - stepAt.current)));
          return;
        } else if (q) { pipeRef.current = null; setPipe(null); }
        setPolling(false);
      }
    };
    t = setTimeout(tick, TICK_MS);
    return () => clearTimeout(t);
  }, [polling]);
  useEffect(() => () => cleaner.cancel(true), []);

  const attempt = (fn) => { setError(''); try { fn(); } catch (e) { setError(e.message); } setGen((g) => g + 1); };
  const go = (id) => {
    if (KIND[id] !== KIND[section]) setPicked(new Set());     // ids belong to one scan: another list's would name other files
    setSection(id); setResultOf(''); setError(''); setUnder(''); setEmptied('');
  };
  const watch = () => setPolling(true);

  const startModule = (step) => {
    const m = MODULES[step];
    if (m.kind === 'rules') cleaner.start('rules', { exclude: prefs.exclude });
    else if (m.kind === 'tree') cleaner.start('tree', { volume: 'home', exclude: prefs.exclude, remember: prefs.rememberMap !== false });
    else cleaner.start('dups', { folders: dirs.slice(0, 8), exclude: prefs.exclude });
  };
  const startAll = () => attempt(() => {
    setBoard(false);
    setResultOf('');
    planShown.current = 0;
    pipeRef.current = { step: 0, results: {} };
    setPipe(pipeRef.current);
    stepAt.current = Date.now();
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
    else if (kind === 'tree') cleaner.start('tree', { volume, exclude: prefs.exclude, remember: prefs.rememberMap !== false });
    else if (kind === 'images') cleaner.start('images', { folders: [...imageFolders], exclude: prefs.exclude });
    else cleaner.start('dups', { folders: [...folders], exclude: prefs.exclude });
    setHold(true);
    clearTimeout(holdTimer.current);
    holdTimer.current = setTimeout(() => setHold(false), MIN_SCAN_MS);
    watch();
  });
  const stop = () => { cleaner.cancel(false); setGen((g) => g + 1); };
  const startOver = () => { setHidden((h) => ({ ...h, [kind]: scan.id })); setPicked(new Set()); setModels(new Set()); setResultOf(''); setError(''); setUnder(''); };
  const makePlan = () => attempt(() => {
    const whole = scan.rules ? scan.rules.filter((r) => chosen.has(r.id)) : [];
    if (kind === 'rules' && section !== 'apps' && (models.size || whole.some((r) => r.explicit))) {
      // what is chosen by name goes first: the confirmation names the plan's first items
      const ids = new Set(models);
      for (const r of [...whole].sort((a, b) => b.explicit - a.explicit)) for (const id of allIds(scan.id, { rule: r.id })) ids.add(id);
      cleaner.plan(scan.id, [...ids]);
    } else if (kind === 'rules' && section !== 'apps') cleaner.plan(scan.id, { rules: [...chosen] });
    else cleaner.plan(scan.id, [...picked]);
    setResultOf('');
    watch();
  });
  useEffect(() => {
    if (plan && plan.state === 'ready' && planShown.current !== plan.id) { planShown.current = plan.id; setConfirm(true); }
    if (plan && plan.state === 'finished' && resultOf !== plan.id) {
      setResultOf(plan.id);
      // what ran changed the disk: another section's list would offer files that are gone
      const old = {};
      for (const k of ['rules', 'tree', 'dups', 'images']) { const o = cleaner.poll(k).scan; if (o && o.id !== plan.scan) old[k] = o.id; }
      setHidden((h) => ({ ...h, ...old }));
      setBoard(false);
    }
  }, [plan && plan.id, plan && plan.state]);
  const execute = () => attempt(() => { setConfirm(false); setModels(new Set()); cleaner.execute(plan.id); watch(); });
  const freeAtEmpty = useRef(0);
  const emptying = useRef(false);
  const emptyTrash = () => attempt(() => { setEmptyAsk(false); setEmptied(''); freeAtEmpty.current = cleaner.status().free; emptying.current = true; cleaner.emptyTrash(); watch(); });
  useEffect(() => {
    if (!emptying.current || info.job !== 'emptyTrash' || info.state !== 'done' || polling) return;
    emptying.current = false;
    setEmptied(tr('cleaner.empty.done', { size: formatBytes(Math.max(0, cleaner.status().free - freeAtEmpty.current)) }));
    setResultOf('');
    if (['cleanup', 'large', 'duplicates'].includes(section)) start();
  }, [info.job, info.state, polling]);

  // the map of an earlier session, once, only until a real scan replaces it
  const triedSaved = useRef(false);
  useEffect(() => {
    if (section !== 'storage' || triedSaved.current || ready || running || prefs.rememberMap === false) return;
    triedSaved.current = true;
    const kept = cleaner.saved();
    if (!kept) return;
    setVolume(kept.volume);
    attempt(() => { cleaner.start('tree', { volume: kept.volume, saved: true }); watch(); });
  }, [section]);

  const showResult = planHere && resultOf === plan.id && plan.state === 'finished';
  const showing = (() => {
    if (section === 'activity') return <Activity gen={gen} />;
    if (section === 'settings') return <Preferences />;
    if (showResult) return <PlanResult key={plan.id} plan={plan} onDone={() => { setResultOf(''); start(); }} onEmpty={() => setEmptyAsk(true)} onRestored={() => setGen((g) => g + 1)} />;
    if (pipe) {
      const l = cleaner.poll();
      const g = win ? wm.frame(win) : { w: 1100, h: 700 };
      return <Dashboard W={g.w - 76 - 76} H={g.h - 150} step={pipe.step} results={pipe.results} scan={l.job === 'scan' ? l.scan : null} />;
    }
    if ((running || hold) && info.job !== 'emptyTrash') return <Working info={cleaner.poll()} art={ART[section] || 'scan'} />;
    if (section === 'scan' && board) return <ReviewBoard rules={cleaner.poll('rules').scan} tree={cleaner.poll('tree').scan} dups={cleaner.poll('dups').scan} go={go} />;
    if (section === 'scan' || (section === 'cleanup' && !ready)) return <ScanHero key={section} status={status} last={prefs.lastScan} />;
    if (section === 'cleanup') return <CleanupList key={scan.id} scan={scan} chosen={chosen} setChosen={setChosen} onEmpty={() => setEmptyAsk(true)} blockedOf={(r) => (r.root === 'busy' ? 'busy' : '')} models={models} setModels={setModels} />;
    if (section === 'large') return ready && !scan.savedAt ? <LargeList key={`${scan.id}.${under}`} scan={scan} prefs={prefs} selected={picked} setSelected={setPicked} under={under} clearUnder={() => setUnder('')} /> : <LargeHero />;
    if (section === 'storage') return ready ? <StorageView key={scan.id} scan={scan} volume={volumes.find((v) => v.id === volume)} onReview={(rel) => { setSection('large'); setUnder(rel); }} /> : <StorageHero volumes={volumes} volume={volume} setVolume={setVolume} />;
    if (section === 'apps') return <AppsView scan={ready ? scan : null} onFind={start} selected={picked} setSelected={setPicked} />;
    if (kind === 'images') return ready ? <SimilarList key={scan.id} scan={scan} selected={picked} setSelected={setPicked} /> : <SimilarHero folders={dirs} chosen={imageFolders} toggle={pickFrom(setImageFolders)} />;
    return ready ? <DupList key={scan.id} scan={scan} selected={picked} setSelected={setPicked} />
      : <DupHero folders={dirs} chosen={folders} toggle={pickFrom(setFolders)} />;
  })();

  // what the round button does here
  const orb = (() => {
    if (section === 'activity' || section === 'settings' || showResult) return null;
    if (pipe) return { label: tr('cleaner.stop'), onClick: () => { pipeRef.current = null; setPipe(null); if (running) stop(); }, busy: true };
    if (hold && !running) return null;
    if (running) return info.job === 'scan' ? { label: tr('cleaner.stop'), onClick: () => { pipeRef.current = null; setPipe(null); stop(); }, busy: true } : null;
    if (section === 'scan' && board) return null;
    if (section === 'scan') return { label: tr('cleaner.scan'), onClick: startAll, disabled: !status.available };
    if (section === 'cleanup' && ready) return { label: tr('cleaner.clean'), onClick: makePlan, disabled: !chosen.size && !models.size };
    if (section === 'storage' && ready) return null;
    if ((section === 'large' || section === 'duplicates' || section === 'apps') && ready && picked.size) return { label: tr('cleaner.orb.trash'), onClick: makePlan };
    if ((section === 'large' || section === 'duplicates') && ready) return { label: tr('cleaner.orb.trash'), disabled: true };
    if (section === 'apps') return null;
    if (section === 'duplicates' && !(similar ? imageFolders : folders).size) return { label: tr('cleaner.scan'), disabled: true };
    return { label: tr('cleaner.scan'), onClick: start, disabled: !status.available };
  })();

  const failed = info.state === 'failed' && info.error ? tr(`cleaner.error.${info.error}`) : '';

  // own frame: the window moves from the top edge and the sidebar's row, which stay free of controls. Lord Classic
  // draws its own buttons on the top edge (themes/classic/components/Window.jsx), so that edge is taller. In Lord Flow
  // the lights are the sidebar's, unless the window buttons go at the end: the theme draws them there (issue #58).
  // the theme that is running (settings.theme is null until one is chosen in Appearance)
  const classic = themes.current() === 'classic', topEdge = classic ? 34 : 14;
  const lightsAtStart = useWindowButtons(true).atStart, ownLights = !classic && lightsAtStart;
  const move = () => wm.beginMove(winId), zoom = () => wm.toggleMaximize(winId);

  return (
    <View style={{ flex: 1, flexDirection: 'row', overflow: 'hidden', borderRadius: 12 }}>
      <Sky />
      <Tints section={section} />
      <View ref={side} style={{ width: 236, flexShrink: 0, paddingHorizontal: 14, paddingTop: 14, paddingBottom: 14, gap: 4, overflow: 'hidden', backgroundColor: p.side, borderInlineEnd: `1px solid ${p.line}` }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, marginBottom: spacing.m }}>{ownLights ? <Lights id={winId} /> : null}<View onMouseDown={move} onDoubleClick={zoom} style={{ flex: 1, alignSelf: 'stretch', minHeight: 16 }} /></View>
        {SECTIONS.map((id) => <SideItem key={id} id={id} section={section} go={go} rail={!!pipe} badge={pipe && id === 'scan' ? `${pipe.step + 1}/${MODULES.length}` : ''} />)}
        <View style={{ flex: 1 }} />
        {BOTTOM.map((id) => <SideItem key={id} id={id} section={section} go={go} rail={!!pipe} />)}
      </View>
      <View style={{ flex: 1, minWidth: 0, paddingHorizontal: 38, paddingTop: classic ? 52 : 34 }}>
        {board && section === 'scan' && !pipe ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', marginBottom: spacing.s, marginTop: -18 }}>
            <View onClick={() => setBoard(false)} style={{ width: 170 }}><Label size={13} lines={1} style={{ color: p.accent }}>{tr('cleaner.startOver')}</Label></View>
            <View style={{ flex: 1 }}><Label size={13} weight={600} align="center">{tr('cleaner.section.scan')}</Label></View>
            <View style={{ width: 170, alignItems: 'flex-end' }}><GlassButton small label={tr('cleaner.rescan')} onClick={startAll} /></View>
          </View>
        ) : null}
        {!status.available ? <Label>{tr('cleaner.unavailable')}</Label> : null}
        {error ? <Label>{error}</Label> : null}
        {failed ? <Label>{failed}</Label> : null}
        {['cleanup', 'large', 'storage', 'duplicates'].includes(section) && !running && !hold && !showResult && (ready || section === 'duplicates') ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs, marginBottom: spacing.s, marginTop: -18 }}>
            {ready ? <View onClick={startOver}><Label size={13} lines={1} style={{ color: p.accent }}>{tr('cleaner.startOver')}</Label></View> : null}
            {section === 'duplicates' ? <Pill label={tr('cleaner.dups.exact')} on={!similar} onClick={() => { setSimilar(false); setPicked(new Set()); }} /> : null}
            {section === 'duplicates' ? <Pill label={tr('cleaner.similar.tab')} on={similar} onClick={() => { setSimilar(true); setPicked(new Set()); }} /> : null}
            <View style={{ flex: 1 }} />
            {ready ? <GlassButton small label={tr('cleaner.rescan')} onClick={start} /> : null}
          </View>
        ) : null}
        {emptied ? <Label size={13} style={{ marginBottom: spacing.xs }}>{emptied}</Label> : null}
        <Enter key={section} style={{ flex: 1, minHeight: 0 }}>{showing}</Enter>
        {orb && orb.label ? <Orb key={orb.busy ? 'busy' : 'idle'} label={orb.label} onClick={orb.onClick} disabled={orb.disabled} busy={orb.busy} /> : null}
      </View>
      {confirm && plan && plan.state === 'ready' ? <PlanConfirm plan={plan} onOk={execute} onCancel={() => setConfirm(false)} /> : null}
      {emptyAsk ? <AskDialog title={tr('cleaner.empty.title')} note={tr('cleaner.empty.detail')} okLabel={tr('cleaner.empty.ok')} onOk={emptyTrash} onCancel={() => setEmptyAsk(false)} /> : null}
      <View onMouseDown={move} onDoubleClick={zoom} style={{ position: 'absolute', left: 0, right: 0, top: 0, height: topEdge }} />
    </View>
  );
}
