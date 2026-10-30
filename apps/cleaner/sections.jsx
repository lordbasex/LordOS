// Scan, Cleanup, Large files, Activity and Settings.
import {
  View, ScrollView, TextInput, cleaner, t as tr, formatBytes, formatNumber, formatDate, formatTime, revealPath, useSettings, typeScale, spacing,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Label, Glass, GlassButton, Pill, Check, Bar, Hero, Enter, PagedList, CountUp } from './ui.jsx';

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

export function ScanHero({ status, last, scanning, scan }) {
  return (
    <Hero title={tr(scanning ? 'cleaner.scanning' : 'cleaner.hero.scan.title')} line={tr('cleaner.hero.scan.line')}>
      {scanning ? (
        <Glass style={{ padding: spacing.m, flexDirection: 'row', gap: spacing.xl, marginTop: spacing.s }}>
          <Counter label={tr('cleaner.seen')} value={n(scan ? scan.seen : 0)} />
          <Counter label={tr('cleaner.dirs')} value={n(scan ? scan.dirs : 0)} />
          <Counter label={tr('cleaner.found')} value={n(scan ? scan.candidates : 0)} />
        </Glass>
      ) : <DiskCard status={status} last={last} />}
    </Hero>
  );
}

function Counter({ label, value }) {
  return <View style={{ gap: 2 }}><Label dim size={typeScale.callout}>{label}</Label><Label size={20} weight={600}>{value}</Label></View>;
}

// ---- Cleanup: categories, each with its reason, what will happen, and its files on request ----

const ACTION_KEY = { delete: 'cleaner.action.delete', trash: 'cleaner.action.trash', none: 'cleaner.action.none' };

function RuleRow({ rule, scanId, on, onToggle, onEmpty, delay, blocked }) {
  const [open, setOpen] = useState(false);
  const selectable = rule.action !== 'none' && !blocked;
  return (
    <Enter delay={delay} x={0} y={12}>
      <Glass style={{ padding: spacing.m, gap: spacing.s }}>
        <View style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
          {selectable ? <Check on={on} onClick={onToggle} /> : <View style={{ width: 20 }} />}
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Label weight={600}>{tr(`cleaner.ruleName.${rule.id}`)}</Label>
            <Label dim size={typeScale.callout}>{blocked ? tr(`cleaner.blocked.${blocked}`) : tr(rule.text)}</Label>
            <View style={{ flexDirection: 'row', gap: spacing.s, marginTop: 2, flexWrap: 'wrap' }}>
              <Tag label={tr(`cleaner.risk.${rule.risk}`)} />
              <Tag label={tr(ACTION_KEY[rule.action])} strong={rule.action === 'delete'} />
            </View>
          </View>
          <View style={{ alignItems: 'flex-end', gap: 2, flexShrink: 0 }}>
            <Label weight={600}>{formatBytes(rule.allocated)}</Label>
            <Label dim size={typeScale.callout}>{tr('cleaner.files', { count: rule.files })}</Label>
          </View>
          {rule.id === 'trash' ? <GlassButton small label={tr('cleaner.emptyTrash')} onClick={onEmpty} /> : null}
          <GlassButton small label={open ? '▴' : '▾'} onClick={() => setOpen(!open)} />
        </View>
        {open ? (
          <View style={{ height: 170 }}>
            <PagedList rowH={28} reset={scanId} fetch={(o, l) => cleaner.results(scanId, o, l, { rule: rule.id })} render={(it) => (
              <View key={it.id} style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
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

function Tag({ label, strong }) {
  return (
    <View style={{ paddingHorizontal: 8, height: 18, borderRadius: 9, justifyContent: 'center', opacity: strong ? 1 : 0.75 }}>
      <Label size={typeScale.caption} weight={500}>{label}</Label>
    </View>
  );
}

export function CleanupList({ scan, chosen, setChosen, onEmpty, blockedOf }) {
  const rows = scan.rules.filter((r) => r.files > 0);
  const cats = [...new Set(rows.map((r) => r.category))];
  const incomplete = !scan.complete;
  let i = 0;
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <View style={{ flexDirection: 'row', gap: spacing.l, alignItems: 'flex-end' }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Label size={typeScale.title} weight={600}>{tr('cleaner.cleanup.title')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.estimateNote')}</Label>
        </View>
        <View style={{ alignItems: 'flex-end' }}>
          <Label dim size={typeScale.callout}>{tr('cleaner.estimated')}</Label>
          <CountUp value={scan.estimated} format={formatBytes} />
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
                return <RuleRow key={r.id} rule={r} scanId={scan.id} delay={Math.min(k * 45, 360)} on={chosen.has(r.id)} blocked={blockedOf(r.id)} onEmpty={onEmpty}
                  onToggle={() => setChosen((o) => { const x = new Set(o); if (x.has(r.id)) x.delete(r.id); else x.add(r.id); return x; })} />;
              })}
            </View>
          ))}
        </ScrollView>
      )}
    </View>
  );
}

// ---- Large files: found by a tree scan, filtered in lord-server ----

export function LargeHero() {
  return <Hero title={tr('cleaner.hero.large.title')} line={tr('cleaner.hero.large.line')} />;
}

export function LargeList({ scan, prefs, selected, setSelected }) {
  const [minMB, setMinMB] = useState(prefs.largeMB);
  const [days, setDays] = useState(0);
  const [type, setType] = useState('');
  const filter = useMemo(() => ({ minSize: minMB * 1048576, ...(days ? { olderThanDays: days } : null), ...(type ? { type } : null) }), [minMB, days, type]);
  const reset = `${scan.id}.${minMB}.${days}.${type}`;
  const total = useMemo(() => cleaner.results(scan.id, 0, 1, filter).total, [reset]);
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <Label size={typeScale.title} weight={600}>{tr('cleaner.large.title')}</Label>
      <View style={{ flexDirection: 'row', gap: spacing.xs, flexWrap: 'wrap' }}>
        {[10, 100, 1024].map((m) => <Pill key={m} label={formatBytes(m * 1048576)} on={minMB === m} onClick={() => setMinMB(m)} />)}
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
    return <Hero title={tr('cleaner.activity.title')} line={tr('cleaner.activity.empty')} />;
  }
  return (
    <Enter style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <View style={{ flex: 1 }}><Label size={typeScale.title} weight={600}>{tr('cleaner.activity.title')}</Label></View>
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
      <Label size={typeScale.title} weight={600}>{tr('cleaner.prefs.title')}</Label>
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
