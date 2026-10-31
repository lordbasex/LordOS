// The way every section acts: a plan built in lord-server, a confirmation short enough to read, the
// execution, and a result that leads with a summary.
import { View, Text, addListener, cleaner, t as tr, formatBytes, formatNumber, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Label, Glass, GlassButton, PagedList, Enter, CountUp, useLoop, usePal } from './ui.jsx';

const n = (v) => formatNumber(v);
const SHOWN = 5;     // names in a confirmation; the dialog must fit the window
const REASONS = ['changed', 'skipped', 'denied', 'failed', 'cancelled', 'unsupported'];

function Dialog({ title, rows, note, okLabel, onOk, onCancel, danger }) {
  const p = usePal();
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onCancel(); }), [onCancel]);
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: p.scrim, alignItems: 'center', justifyContent: 'center' }}>
      <Enter x={0} y={14}>
        <View onClick={(e) => e.stopPropagation()} style={{ width: 460, padding: spacing.xxl, borderRadius: 20, backgroundColor: p.panel, border: `1px solid ${p.line}`, boxShadow: '0 24px 60px rgba(8,4,30,0.5)' }}>
          <Label size={20} weight={600} style={{ marginBottom: spacing.m }}>{title}</Label>
          {rows.map(([a, b], i) => (
            <View key={i} style={{ flexDirection: 'row', justifyContent: 'space-between', gap: spacing.m, paddingVertical: 10, borderTop: `1px solid ${p.line}` }}>
              <View style={{ flex: 1, minWidth: 0 }}><Label>{a}</Label></View>
              {b ? <Label weight={600}>{b}</Label> : null}
            </View>
          ))}
          {note ? <Label dim size={typeScale.callout} style={{ marginTop: spacing.xs, marginBottom: spacing.l }}>{note}</Label> : <View style={{ height: spacing.l }} />}
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
            <GlassButton label={tr('cleaner.cancel')} onClick={onCancel} />
            {onOk ? <GlassButton danger={danger} label={okLabel} onClick={onOk} /> : null}
          </View>
        </View>
      </Enter>
    </View>
  );
}

export function PlanConfirm({ plan, onOk, onCancel }) {
  const left = plan.items - plan.counts.ready;
  const rows = [];
  if (plan.deleteItems) rows.push([tr('cleaner.confirm.rowDelete', { n: n(plan.deleteItems) }), formatBytes(plan.deleteAllocated)]);
  if (plan.trashItems) rows.push([tr('cleaner.confirm.rowTrash', { n: n(plan.trashItems) }), formatBytes(plan.trashAllocated)]);
  if (left > 0) rows.push([tr('cleaner.confirm.rowLeft', { n: n(left) }), '']);
  const named = plan.named ? cleaner.results('plan', 0, 30).items.filter((i) => i.status === 'ready') : [];
  for (const i of named.slice(0, SHOWN)) rows.push([i.path.split('/').pop(), formatBytes(i.allocated)]);
  if (plan.counts.ready > SHOWN && named.length) rows.push([tr('cleaner.confirm.more', { n: n(plan.counts.ready - Math.min(SHOWN, named.length)) }), '']);
  const leftovers = named.some((i) => i.rule === 'app-leftovers'), projects = named.some((i) => i.rule.startsWith('project-'));
  const notes = [plan.deleteItems ? tr('cleaner.confirm.noteDelete') : '', plan.trashItems ? tr('cleaner.confirm.noteTrash') : '', plan.named ? tr(leftovers ? 'cleaner.leftovers.warn' : projects ? 'cleaner.projects.warn' : 'cleaner.models.warn') : '',
    ...plan.warnings.filter((w) => w === 'linksElsewhere' || w === 'scanIncomplete' || w === 'keepOneCopy').map((w) => tr(`cleaner.warn.${w}`))].filter(Boolean);
  if (!plan.counts.ready) return <Dialog title={tr('cleaner.noneReady')} rows={rows} note={notes.join(' ')} onCancel={onCancel} />;
  return <Dialog title={tr(plan.deleteItems ? 'cleaner.confirm.titleDelete' : 'cleaner.confirm.title', { count: plan.counts.ready })} rows={rows} note={notes.join(' ')} danger={plan.deleteItems > 0}
    okLabel={tr(plan.deleteItems ? 'cleaner.confirm.okDelete' : 'cleaner.confirm.ok')} onOk={onOk} onCancel={onCancel} />;
}

export function AskDialog({ title, note, okLabel, onOk, onCancel }) {
  return <Dialog title={title} rows={[]} note={note} danger okLabel={okLabel} onOk={onOk} onCancel={onCancel} />;
}

function Pulse() {
  const p = usePal();
  const ring = useRef(null);
  useLoop((t) => { if (ring.current) { const x = (t % 2.6) / 2.6, s = 1 + (1 - (1 - x) ** 2) * 0.55; lord.setStyle(ring.current.id, 'scaleX', s); lord.setStyle(ring.current.id, 'scaleY', s); lord.setStyle(ring.current.id, 'opacity', 0.7 * (1 - x)); } });
  return (
    <Enter x={0} y={14} style={{ width: 112, height: 112, alignItems: 'center', justifyContent: 'center' }}>
      <View ref={ring} style={{ position: 'absolute', left: 0, top: 0, width: 112, height: 112, borderRadius: 56, border: `2px solid ${p.accent}`, opacity: 0 }} />
      <View style={{ width: 112, height: 112, borderRadius: 56, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(90deg, ${p.barA}, ${p.barB})` }}>
        <Text style={{ fontSize: 48, fontWeight: 700, color: '#ffffff' }}>✓</Text>
      </View>
    </Enter>
  );
}
function Stat({ label, value, delay }) {
  return (
    <Enter delay={delay} y={14} x={0} style={{ width: 170 }}>
      <Glass style={{ padding: spacing.m, gap: 2 }}>
        <Label size={18} weight={600}>{value}</Label>
        <Label dim size={typeScale.callout}>{label}</Label>
      </Glass>
    </Enter>
  );
}

export function PlanResult({ plan, onDone }) {
  const [detail, setDetail] = useState(false);
  const [gen, setGen] = useState(0);
  const others = REASONS.filter((k) => plan.counts[k] > 0);
  const notDone = others.reduce((a, k) => a + plan.counts[k], 0);
  const restored = plan.counts.restored;
  return (
    <View style={{ flex: 1, minHeight: 0, alignItems: 'center', justifyContent: detail ? 'flex-start' : 'center', gap: spacing.s, paddingBottom: spacing.m }}>
      <Pulse />
      <CountUp value={plan.deletedAllocated || plan.trashedAllocated} format={(v) => tr(plan.deletedAllocated ? 'cleaner.result.freedBig' : 'cleaner.result.trashedBig', { size: formatBytes(v) })} size={36} weight={300} />
      <Label dim>{tr('cleaner.result.free', { before: formatBytes(plan.freeBefore), after: formatBytes(plan.freeAfter) })}</Label>
      <View style={{ flexDirection: 'row', gap: spacing.s, marginTop: spacing.s, flexWrap: 'wrap', justifyContent: 'center' }}>
        <Stat delay={0} label={tr('cleaner.result.freedSub', { n: n(plan.deleted) })} value={formatBytes(plan.deletedAllocated)} />
        <Stat delay={80} label={tr('cleaner.result.trashedSub', { n: n(plan.trashed) })} value={formatBytes(plan.trashedAllocated)} />
        <Stat delay={160} label={tr('cleaner.result.skipped')} value={n(notDone)} />
      </View>
      {others.map((k) => <Label key={k} dim size={typeScale.callout}>{tr(`cleaner.reason.${k}`, { n: n(plan.counts[k]) })}</Label>)}
      {restored ? <Label dim size={typeScale.callout}>{tr('cleaner.result.restored', { n: n(restored) })}</Label> : null}
      {plan.trashed ? <Label dim size={typeScale.callout}>{tr('cleaner.trashNote')}</Label> : null}
      <View style={{ flexDirection: 'row', gap: spacing.s }}>
        <GlassButton small label={tr(detail ? 'cleaner.hideDetails' : 'cleaner.showDetails')} onClick={() => setDetail(!detail)} />
        <GlassButton small label={tr('cleaner.close')} onClick={onDone} />
      </View>
      {detail ? (
        <Glass style={{ flex: 1, minHeight: 80, padding: spacing.xs, alignSelf: 'stretch' }}>
          <PagedList rowH={32} reset={`${plan.id}.${gen}`} fetch={(o, l) => cleaner.results('plan', o, l)} render={(it) => (
            <View key={it.id} style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s }}>
              <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{it.path}</Label></View>
              {it.restorable ? <GlassButton small label={tr('cleaner.restore')} onClick={() => { cleaner.restore(plan.id, it.id); setGen(gen + 1); }} /> : null}
              {it.status === 'restored' ? <GlassButton small label={tr('cleaner.showInFiles')} onClick={() => revealPath(it.abs)} /> : null}
              <Label dim size={typeScale.callout}>{tr(`cleaner.status.${it.status}${it.status === 'done' ? (it.restorable ? 'Trash' : 'Delete') : ''}`)}</Label>
            </View>
          )} />
        </Glass>
      ) : null}
    </View>
  );
}
