// The way every section acts: a plan built in lord-server, a confirmation short enough to read, the
// execution, and a result that leads with a summary.
import { View, ConfirmDialog, cleaner, t as tr, formatBytes, formatNumber, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useState } from 'react';
import { Label, Glass, GlassButton, PagedList, Enter, CountUp } from './ui.jsx';

const n = (v) => formatNumber(v);
const REASONS = ['changed', 'skipped', 'denied', 'failed', 'cancelled', 'unsupported'];

export function PlanConfirm({ plan, onOk, onCancel }) {
  const leftAlone = plan.items - plan.counts.ready;
  const lines = [
    plan.deleteItems ? tr('cleaner.confirm.lineDelete', { n: n(plan.deleteItems), size: formatBytes(plan.deleteAllocated) }) : '',
    plan.trashItems ? tr('cleaner.confirm.lineTrash', { n: n(plan.trashItems), size: formatBytes(plan.trashAllocated) }) : '',
    leftAlone > 0 ? tr('cleaner.confirm.lineLeft', { n: n(leftAlone) }) : '',
    ...plan.warnings.filter((w) => w === 'linksElsewhere' || w === 'scanIncomplete' || w === 'keepOneCopy').map((w) => tr(`cleaner.warn.${w}`)),
  ].filter(Boolean);
  if (!plan.counts.ready) {
    return <ConfirmDialog title={tr('cleaner.noneReady')} detail={lines.join('\n')} okLabel={tr('cleaner.close')} cancelLabel={tr('cleaner.cancel')} onOk={onCancel} onCancel={onCancel} />;
  }
  return (
    <ConfirmDialog title={tr(plan.deleteItems ? 'cleaner.confirm.titleDelete' : 'cleaner.confirm.title', { count: plan.counts.ready })} destructive={plan.deleteItems > 0}
      detail={lines.join('\n')} okLabel={tr(plan.deleteItems ? 'cleaner.confirm.okDelete' : 'cleaner.confirm.ok')} cancelLabel={tr('cleaner.cancel')} onOk={onOk} onCancel={onCancel} />
  );
}

function Stat({ label, value, sub, delay }) {
  return (
    <Enter delay={delay} y={14} x={0} style={{ flex: 1, minWidth: 180 }}>
      <Glass style={{ padding: spacing.l, gap: spacing.xs }}>
        <Label dim size={typeScale.callout}>{label}</Label>
        {value}
        {sub ? <Label dim size={typeScale.callout}>{sub}</Label> : null}
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
    <View style={{ flex: 1, minHeight: 0, gap: spacing.m, paddingBottom: spacing.m }}>
      <Enter x={0} y={8}><Label size={typeScale.title} weight={600}>{tr('cleaner.finished')}</Label></Enter>
      <View style={{ flexDirection: 'row', gap: spacing.m, flexWrap: 'wrap' }}>
        <Stat delay={0} label={tr('cleaner.result.freed')} value={<CountUp value={plan.deletedAllocated} format={formatBytes} />}
          sub={tr('cleaner.result.freedSub', { n: n(plan.deleted) })} />
        <Stat delay={80} label={tr('cleaner.result.trashed')} value={<CountUp value={plan.trashedAllocated} format={formatBytes} />}
          sub={tr('cleaner.result.trashedSub', { n: n(plan.trashed) })} />
        <Stat delay={160} label={tr('cleaner.result.skipped')} value={<CountUp value={notDone} format={n} />}
          sub={notDone ? tr('cleaner.result.skippedSub') : tr('cleaner.result.nothingSkipped')} />
      </View>
      {others.map((k) => <Label key={k} dim>{tr(`cleaner.reason.${k}`, { n: n(plan.counts[k]) })}</Label>)}
      {restored ? <Label dim>{tr('cleaner.result.restored', { n: n(restored) })}</Label> : null}
      <Label dim>{tr('cleaner.result.free', { before: formatBytes(plan.freeBefore), after: formatBytes(plan.freeAfter) })}</Label>
      {plan.trashed ? <Label dim>{tr('cleaner.trashNote')}</Label> : null}
      <View style={{ flexDirection: 'row', gap: spacing.s }}>
        <GlassButton small label={tr(detail ? 'cleaner.hideDetails' : 'cleaner.showDetails')} onClick={() => setDetail(!detail)} />
        <GlassButton small label={tr('cleaner.close')} onClick={onDone} />
      </View>
      {detail ? (
        <Glass style={{ flex: 1, minHeight: 80, padding: spacing.xs }}>
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
