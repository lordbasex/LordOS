// Scan, Cleanup, Large files, Activity and Settings.
import {
  View, ScrollView, TextInput, ConfirmDialog, admin, cleaner, CLEANER_THEMES, t as tr, formatBytes, formatNumber, formatDate, formatTime, revealPath, useSettings, typeScale, spacing,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Label, Glass, GlassButton, Pill, Check, Bar, Hero, Enter, Tile, PagedList, CountUp, ProgressRing, usePal } from './ui.jsx';

const n = (v) => formatNumber(v);
const when = (sec) => { const d = new Date(sec * 1000); return `${formatDate(d, 'short')} ${formatTime(d)}`; };
export const prefsOf = (s) => ({ exclude: [], largeMB: 100, oldDays: 365, ...(s.cleaner || {}) });

export function DiskCard({ status, last }) {
  if (!(status.total > 0)) return null;
  const used = status.total - status.free;
  return (
    <Glass style={{ padding: spacing.m, gap: spacing.s, marginTop: spacing.s }}>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between', gap: spacing.m }}>
        <Label weight={600}>{tr('cleaner.disk.free', { size: formatBytes(status.free) })}</Label>
        <Label dim>{tr('cleaner.disk.total', { size: formatBytes(status.total) })}</Label>
      </View>
      <Bar value={used / status.total} />
      <Label dim size={typeScale.callout}>{last ? tr('cleaner.lastScan', { when: when(last.at), size: formatBytes(last.allocated) }) : tr('cleaner.neverScanned')}</Label>
    </Glass>
  );
}

export function ScanHero({ status, last }) {
  return (
    <Hero art="scan" title={tr('cleaner.hero.scan.title')} line={tr('cleaner.hero.scan.line')} features={[
      ['cleanup', tr('cleaner.hero.scan.f1')], ['storage', tr('cleaner.hero.scan.f2')], ['duplicates', tr('cleaner.hero.scan.f3')],
    ]}>
      <DiskCard status={status} last={last} />
    </Hero>
  );
}

// no total is known while walking: real counters, and a percentage only once a plan runs
export function Working({ info }) {
  const scan = info.scan, plan = info.plan;
  if (info.job === 'execute') {
    return <ProgressRing progress={plan.items ? plan.processed / plan.items : 0} big={`${Math.round((plan.processed / (plan.items || 1)) * 100)} %`} small={tr('cleaner.workingCount', { done: n(plan.processed), total: n(plan.items) })} />;
  }
  if (info.job === 'plan') return <ProgressRing big="…" small={tr('cleaner.preparing')} />;
  return <ProgressRing big={n(scan ? scan.seen : 0)} small={tr('cleaner.seen')} under={<Label size={15} weight={600}>{scan && scan.hashed ? tr('cleaner.hashing', { n: n(scan.hashed) }) : tr('cleaner.foundCount', { n: n(scan ? scan.candidates : 0), dirs: n(scan ? scan.dirs : 0) })}</Label>} />;
}

// ---- Cleanup: categories, each with its reason, what will happen, and its files on request ----

const ACTION_KEY = { delete: 'cleaner.action.delete', trash: 'cleaner.action.trash', none: 'cleaner.action.none' };

function RuleRow({ rule, scanId, on, onToggle, onEmpty, delay, blocked, models, setModels }) {
  const [open, setOpen] = useState(false);
  const selectable = rule.action !== 'none' && !blocked && !rule.explicit;
  return (
    <Enter delay={delay} x={0} y={14} ms={500}>
      <Glass lift style={{ padding: spacing.m, gap: spacing.s }}>
        <View style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
          {selectable ? <Check on={on} onClick={onToggle} /> : <View style={{ width: 22 }} />}
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Label weight={600}>{tr(`cleaner.ruleName.${rule.id}`)}</Label>
            <Label dim size={typeScale.callout}>{blocked ? tr(`cleaner.blocked.${blocked}`) : tr(rule.text)}</Label>
            <View style={{ flexDirection: 'row', gap: spacing.s, marginTop: 2, flexWrap: 'wrap' }}>
              <Tag label={tr(`cleaner.risk.${rule.risk}`)} />
              <Tag label={tr(ACTION_KEY[rule.action])} />
            </View>
          </View>
          <View style={{ alignItems: 'flex-end', gap: 2, flexShrink: 0 }}>
            <Label weight={600}>{formatBytes(rule.allocated)}</Label>
            <Label dim size={typeScale.callout}>{tr('cleaner.files', { count: rule.files })}</Label>
          </View>
          {rule.category === 'trash' ? <GlassButton small label={tr('cleaner.emptyTrash')} onClick={onEmpty} /> : null}
          <GlassButton small label={open ? '▴' : '▾'} onClick={() => setOpen(!open)} />
        </View>
        {open ? (
          <View style={{ height: 170 }}>
            <PagedList rowH={28} reset={scanId} fetch={(o, l) => cleaner.results(scanId, o, l, { rule: rule.id })} render={(it) => (
              <View key={it.id} style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
                {rule.explicit && rule.action !== 'none' ? <Check on={models.has(it.id)} onClick={() => setModels((o) => { const x = new Set(o); if (x.has(it.id)) x.delete(it.id); else x.add(it.id); return x; })} /> : null}
                <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{it.path}</Label></View>
                <Label dim size={typeScale.callout}>{formatBytes(it.allocated)}</Label>
              </View>
            )} />
          </View>
        ) : null}
      </Glass>
    </Enter>
  );
}

function Tag({ label }) {
  const p = usePal();
  return (
    <View style={{ paddingHorizontal: 8, height: 20, borderRadius: 8, justifyContent: 'center', backgroundColor: p.track }}>
      <Label size={typeScale.caption} weight={600}>{label}</Label>
    </View>
  );
}

export function CleanupList({ scan, chosen, setChosen, onEmpty, blockedOf, models, setModels, onRemoveModels }) {
  const pal = usePal();
  const rows = scan.rules.filter((r) => r.files > 0 || r.root === 'busy');
  const cats = [...new Set(rows.map((r) => r.category))];
  const incomplete = !scan.complete;
  let i = 0;
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: 96 }}>
      <View style={{ flexDirection: 'row', gap: spacing.l, alignItems: 'flex-end' }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Label size={32} weight={300}>{tr('cleaner.cleanup.title')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.estimateNote')}</Label>
        </View>
        <View style={{ alignItems: 'flex-end' }}>
          <Label dim size={typeScale.callout}>{tr('cleaner.estimated')}</Label>
          <CountUp value={scan.estimated} format={formatBytes} color={pal.accent} />
        </View>
      </View>
      {incomplete ? <Label>{tr(scan.limit ? `cleaner.incomplete.${scan.limit}` : 'cleaner.incomplete')}</Label> : null}
      {!rows.length ? <Label dim>{tr('cleaner.emptyHint')}</Label> : (
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: spacing.s }}>
          {cats.map((c) => (
            <View key={c} style={{ gap: spacing.s }}>
              <Label dim size={typeScale.callout} weight={600}>{tr(`cleaner.category.${c}`)}</Label>
              {rows.filter((r) => r.category === c).map((r) => {
                const k = i++;
                return <RuleRow key={r.id} rule={r} scanId={scan.id} delay={Math.min(k * 45, 360)} on={chosen.has(r.id)} blocked={blockedOf(r)} onEmpty={onEmpty} models={models} setModels={setModels}
                  onToggle={() => setChosen((o) => { const x = new Set(o); if (x.has(r.id)) x.delete(r.id); else x.add(r.id); return x; })} />;
              })}
              {c === 'ai' && models.size ? <GlassButton label={tr('cleaner.models.remove', { count: models.size })} onClick={onRemoveModels} /> : null}
            </View>
          ))}
          <SystemCard />
        </ScrollView>
      )}
    </View>
  );
}

// ---- the operating system's own files: closed actions in the admin helper, for administrators ----

const SYSTEM = [['journal', 'clean-journal'], ['apt', 'clean-apt-cache'], ['vartmp', 'clean-var-tmp']];

function SystemCard() {
  const [sizes, setSizes] = useState(null);
  const [ask, setAsk] = useState(null);
  const [msg, setMsg] = useState('');
  const read = () => admin.run('clean-status', '', null, { quiet: true }).then((t) => {
    const o = {};
    for (const kv of String(t).split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
    setSizes(o);
  }).catch(() => setSizes(null));
  useEffect(() => { read(); }, []);
  if (!sizes) return null;
  const run = () => {
    const [k, action] = ask;
    setAsk(null);
    admin.run(action, '').then(() => { setMsg(tr('cleaner.system.done', { name: tr(`cleaner.system.${k}`) })); read(); }).catch((e) => setMsg(e.message));
  };
  return (
    <View style={{ gap: spacing.s }}>
      <Label dim size={typeScale.callout} weight={600}>{tr('cleaner.category.system')}</Label>
      {msg ? <Label dim size={typeScale.callout}>{msg}</Label> : null}
      {SYSTEM.map(([k, action]) => (
        <Glass key={k} style={{ padding: spacing.m, flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Label weight={600}>{tr(`cleaner.system.${k}`)}</Label>
            <Label dim size={typeScale.callout}>{tr(`cleaner.system.${k}Text`)}</Label>
          </View>
          <Label weight={600}>{formatBytes(sizes[k] || 0)}</Label>
          <GlassButton small label={tr('cleaner.clean')} onClick={() => setAsk([k, action])} />
        </Glass>
      ))}
      {ask ? <ConfirmDialog destructive title={tr(`cleaner.system.${ask[0]}`)} detail={tr('cleaner.system.confirm')} okLabel={tr('cleaner.clean')} cancelLabel={tr('cleaner.cancel')} onOk={run} onCancel={() => setAsk(null)} /> : null}
    </View>
  );
}

// ---- Large files: found by a tree scan, filtered in lord-server ----

export function LargeHero() {
  return <Hero art="large" title={tr('cleaner.hero.large.title')} line={tr('cleaner.hero.large.line')} features={[['large', tr('cleaner.hero.large.f1')], ['activity', tr('cleaner.hero.large.f2')], ['cleanup', tr('cleaner.hero.large.f3')]]} />;
}

export function LargeList({ scan, prefs, selected, setSelected }) {
  const [minMB, setMinMB] = useState(prefs.largeMB);
  const [days, setDays] = useState(0);
  const [type, setType] = useState('');
  const filter = useMemo(() => ({ minSize: minMB * 1048576, ...(days ? { olderThanDays: days } : null), ...(type ? { type } : null) }), [minMB, days, type]);
  const reset = `${scan.id}.${minMB}.${days}.${type}`;
  const total = useMemo(() => cleaner.results(scan.id, 0, 1, filter).total, [reset]);
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: 96 }}>
      <Label size={32} weight={300}>{tr('cleaner.large.title')}</Label>
      <View style={{ flexDirection: 'row', gap: spacing.xs, flexWrap: 'wrap' }}>
        {[1, 10, 100, 1024].map((m) => <Pill key={m} label={formatBytes(m * 1048576)} on={minMB === m} onClick={() => setMinMB(m)} />)}
        <View style={{ width: spacing.s }} />
        {[0, 180, 365, 730].map((d) => <Pill key={d} label={tr(d ? 'cleaner.large.older' : 'cleaner.large.anyAge', { count: d })} on={days === d} onClick={() => setDays(d)} />)}
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.xs, flexWrap: 'wrap' }}>
        {['', 'video', 'image', 'audio', 'document', 'archive', 'code', 'other'].map((k) => <Pill key={k} label={tr(`cleaner.type.${k || 'all'}`)} on={type === k} onClick={() => setType(k)} />)}
      </View>
      <Label dim size={typeScale.callout}>{tr('cleaner.large.count', { count: total })}{scan.limit === 'results' ? ` ${tr('cleaner.incomplete.results')}` : ''}</Label>
      <Glass style={{ flex: 1, minHeight: 0, padding: spacing.xs }}>
        <PagedList rowH={38} reset={reset} fetch={(o, l) => cleaner.results(scan.id, o, l, filter)} render={(it) => (
          <View key={it.id} onClick={() => setSelected((old) => { const x = new Set(old); if (x.has(it.id)) x.delete(it.id); else x.add(it.id); return x; })}
            style={{ height: 38, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s }}>
            <Check on={selected.has(it.id)} />
            <View style={{ flex: 1, minWidth: 0 }}>
              <Label mono size={typeScale.callout} lines={1}>{it.path}</Label>
              <Label dim size={typeScale.caption}>{formatDate(new Date(it.mtime * 1000), 'short')}{it.links > 1 ? ` · ${tr('cleaner.linked')}` : ''}</Label>
            </View>
            <Label size={typeScale.callout} weight={600}>{formatBytes(it.allocated)}</Label>
            <GlassButton small label={tr('cleaner.show')} onClick={() => revealPath(it.abs)} />
          </View>
        )} />
      </Glass>
    </View>
  );
}

// ---- Activity ----

export function Activity({ gen }) {
  const [v, setV] = useState(0);
  const items = useMemo(() => cleaner.history().slice().reverse(), [v, gen]);
  if (!items.length) {
    return <Hero art="activity" title={tr('cleaner.activity.title')} line={tr('cleaner.activity.empty')} features={[['activity', tr('cleaner.activity.f1')], ['settings', tr('cleaner.activity.f2')], ['cleanup', tr('cleaner.activity.f3')]]} />;
  }
  return (
    <Enter style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <View style={{ flex: 1 }}><Label size={32} weight={300}>{tr('cleaner.activity.title')}</Label></View>
        <GlassButton small label={tr('cleaner.activity.clear')} onClick={() => { cleaner.clearHistory(); setV(v + 1); }} />
      </View>
      <Label dim size={typeScale.callout}>{tr('cleaner.activity.privacy')}</Label>
      <ScrollView style={{ flex: 1 }} contentStyle={{ gap: spacing.s }}>
        {items.slice(0, 100).map((e, i) => (
          <Enter key={`${e.at}.${i}`} delay={Math.min(i * 40, 320)} x={0} y={10}>
            <Glass style={{ padding: spacing.m, flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
              
              <View style={{ flex: 1, minWidth: 0 }}>
                <Label weight={600}>{tr(e.action === 'emptyTrash' ? 'cleaner.activity.emptied' : 'cleaner.activity.cleaned', { n: n(e.action === 'emptyTrash' ? e.items : e.counts.done) })}</Label>
                <Label dim size={typeScale.callout}>{when(e.at)}</Label>
              </View>
              <Label dim size={typeScale.callout}>{tr('cleaner.result.free', { before: formatBytes(e.freeBefore), after: formatBytes(e.freeAfter) })}</Label>
            </Glass>
          </Enter>
        ))}
      </ScrollView>
    </Enter>
  );
}

// ---- Settings ----

export function Preferences() {
  const [s, set] = useSettings();
  const p = prefsOf(s);
  const [text, setText] = useState('');
  const save = (patch) => set({ cleaner: { ...p, ...patch } });
  const add = () => {
    const v = text.trim().replace(/^~\//, '').replace(/\/+$/, '');
    if (!v || v.startsWith('/') || v.split('/').includes('..') || p.exclude.includes(v) || p.exclude.length >= 32) return;
    save({ exclude: [...p.exclude, v] });
    setText('');
  };
  return (
    <Enter style={{ flex: 1, minHeight: 0, gap: spacing.m }}>
      <Label size={32} weight={300}>{tr('cleaner.prefs.title')}</Label>
      <ScrollView style={{ flex: 1 }} contentStyle={{ gap: spacing.m }}>
        <Glass style={{ padding: spacing.m, gap: spacing.s }}>
          <Label weight={600}>{tr('cleaner.prefs.exclude')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.prefs.excludeHint')}</Label>
          <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center' }}>
            <View style={{ flex: 1 }}><TextInput value={text} onChange={setText} onSubmit={add} placeholder={tr('cleaner.prefs.excludePlaceholder')} /></View>
            <GlassButton small label={tr('cleaner.prefs.add')} onClick={add} />
          </View>
          {p.exclude.map((x) => (
            <View key={x} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
              <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{`~/${x}`}</Label></View>
              <GlassButton small label={tr('cleaner.prefs.remove')} onClick={() => save({ exclude: p.exclude.filter((y) => y !== x) })} />
            </View>
          ))}
        </Glass>
        <Glass style={{ padding: spacing.m, gap: spacing.s, flexDirection: 'row', alignItems: 'center' }}>
          <View style={{ flex: 1 }}><Label weight={600}>{tr('cleaner.prefs.theme')}</Label></View>
          {CLEANER_THEMES.map((id) => <Pill key={id} label={tr(`cleaner.prefs.theme.${id}`)} on={(p.theme || 'noche') === id} onClick={() => save({ theme: id })} />)}
        </Glass>
        <Glass style={{ padding: spacing.m, gap: spacing.s, flexDirection: 'row', alignItems: 'center' }}>
          <View style={{ flex: 1 }}><Label weight={600}>{tr('cleaner.prefs.menuItem')}</Label></View>
          <Pill label={tr(p.menuItem === false ? 'cleaner.prefs.off' : 'cleaner.prefs.on')} on={p.menuItem !== false} onClick={() => save({ menuItem: p.menuItem === false })} />
        </Glass>
        <Glass style={{ padding: spacing.m, gap: spacing.s }}>
          <Label weight={600}>{tr('cleaner.prefs.large')}</Label>
          <View style={{ flexDirection: 'row', gap: spacing.xs }}>
            {[10, 100, 1024].map((m) => <Pill key={m} label={formatBytes(m * 1048576)} on={p.largeMB === m} onClick={() => save({ largeMB: m })} />)}
          </View>
        </Glass>
      </ScrollView>
    </Enter>
  );
}
