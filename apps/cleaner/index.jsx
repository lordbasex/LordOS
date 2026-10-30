// Cleaner — explains what uses the disk and helps remove what can safely go
// (docs/CLEANER-ARCHITECTURE.md). The rule is analyse, review, plan, confirm, execute, verify: a scan
// only reads, and the app holds candidate ids and display paths, never authority. Scanning runs in the
// sandboxed lord-clean helper; the plan and its execution stay in lord-server (lord.clean).
import {
  View, Text, ScrollView, Button, SidebarItem, EmptyState, ConfirmDialog, Hairline, useTheme, cleaner, t as tr,
  formatBytes, formatNumber, spacing, typeScale,
} from '@lordos/sdk';
import { useCallback, useEffect, useRef, useState } from 'react';

const PAGE = 200;
const TICK_MS = 200;
const STATUSES = ['done', 'changed', 'skipped', 'denied', 'failed', 'cancelled', 'unsupported'];

// ---- icon: a broom with a spark (drawn with View) ----
export function Icon({ size }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, overflow: 'hidden', direction: 'ltr',
      background: 'linear-gradient(180deg, #17a398 0%, #0b6e6e 100%)' }}>
      <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.12, width: s * 0.06, height: s * 0.46, borderRadius: s * 0.03, backgroundColor: '#f4e3c1' }} />
      <View style={{ position: 'absolute', left: s * 0.39, top: s * 0.54, width: s * 0.22, height: s * 0.08, borderRadius: s * 0.02, backgroundColor: '#ffffff' }} />
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.62, width: s * 0.4, height: s * 0.24,
        borderBottomLeftRadius: s * 0.1, borderBottomRightRadius: s * 0.1, background: 'linear-gradient(180deg, #ffd166 0%, #f2a33a 100%)' }} />
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.26, width: s * 0.11, height: s * 0.11, borderRadius: s * 0.015,
        backgroundColor: '#ffffff', transform: 'rotate(45deg)' }} />
      <View style={{ position: 'absolute', left: s * 0.71, top: s * 0.4, width: s * 0.07, height: s * 0.07, borderRadius: s * 0.01,
        backgroundColor: '#ffffff', opacity: 0.8, transform: 'rotate(45deg)' }} />
    </View>
  );
}

function Label({ children, dim, size = typeScale.body, weight = 400, mono, lines }) {
  const th = useTheme();
  return (
    <Text style={{ fontSize: size, fontWeight: weight, color: dim ? th.textDim : th.text, textAlign: 'start',
      ...(mono ? { fontFamily: 'monospace' } : null), ...(lines ? { whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', flexShrink: 1 } : null) }}>{children}</Text>
  );
}

function Counter({ label, value }) {
  const th = useTheme();
  return (
    <View style={{ flex: 1, minWidth: 130, padding: spacing.m, gap: spacing.xs, borderRadius: th.groupRadius, backgroundColor: th.control }}>
      <Label dim size={typeScale.callout}>{label}</Label>
      <Label size={typeScale.title} weight={600}>{value}</Label>
    </View>
  );
}

function Check({ on }) {
  const th = useTheme();
  return (
    <View style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center', flexShrink: 0,
      backgroundColor: on ? th.accent : 'transparent', border: `1.5px solid ${on ? th.accent : th.textDim}` }}>
      {on ? <Text style={{ fontSize: 11, lineHeight: 12, color: '#fff' }}>✓</Text> : null}
    </View>
  );
}

function Row({ item, on, onToggle }) {
  const [hover, setHover] = useState(false);
  const th = useTheme();
  return (
    <View onClick={onToggle} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s, height: 30, borderRadius: 6,
        backgroundColor: hover ? th.hover : 'transparent' }}>
      {onToggle ? <Check on={on} /> : null}
      <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{item.path}</Label></View>
      {item.linked ? <Label dim size={typeScale.caption}>{tr('cleaner.linked')}</Label> : null}
      {item.status
        ? <Label dim size={typeScale.callout}>{tr(`cleaner.status.${item.status}`)}</Label>
        : <Label dim size={typeScale.callout}>{formatBytes(item.allocated)}</Label>}
    </View>
  );
}

// The list of scan candidates or plan items, a page at a time.
function usePages(kind, key) {
  const [items, setItems] = useState([]);
  const [total, setTotal] = useState(0);
  const load = useCallback((offset) => {
    const r = cleaner.results(kind, offset, PAGE);
    setItems((old) => (offset ? [...old, ...r.items] : r.items));
    setTotal(r.total);
  }, [kind]);
  useEffect(() => { if (key) load(0); else { setItems([]); setTotal(0); } }, [key, load]);
  return { items, total, more: () => load(items.length) };
}

export default function Cleaner() {
  const th = useTheme();
  const [available] = useState(() => cleaner.available());
  const [free, setFree] = useState(() => cleaner.status().free);
  const [info, setInfo] = useState(() => cleaner.poll());
  const [polling, setPolling] = useState(false);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState(() => new Set());
  const [confirm, setConfirm] = useState(false);
  const planShown = useRef(0);

  const scan = info && info.scan;
  const plan = info && info.plan;
  const running = !!info && info.state === 'running';
  const scanDone = !!scan && info.job !== 'none' && !(info.job === 'scan' && info.state !== 'done');
  const finished = !!plan && plan.state === 'finished';

  const refresh = useCallback(() => {
    const p = cleaner.poll();
    setInfo(p);
    return p;
  }, []);

  // poll only while a job runs; nothing is left running when the app closes
  useEffect(() => {
    if (!polling) return undefined;
    let id = 0;
    const tick = () => {
      const p = refresh();
      if (p && p.state === 'running') id = setTimeout(tick, TICK_MS);
      else { setPolling(false); setFree(cleaner.status().free); }
    };
    id = setTimeout(tick, TICK_MS);
    return () => clearTimeout(id);
  }, [polling, refresh]);
  useEffect(() => () => cleaner.cancel(true), []);

  const attempt = (fn) => {
    setError('');
    try { fn(); } catch (e) { setError(e.message); refresh(); }
  };
  const startScan = () => attempt(() => {
    cleaner.start();
    setSelected(new Set());
    setConfirm(false);
    planShown.current = 0;
    refresh();
    setPolling(true);
  });
  const stop = () => { cleaner.cancel(false); refresh(); };

  const list = usePages('scan', scanDone && !finished ? scan.id : 0);
  const outcome = usePages('plan', finished ? plan.id : 0);

  const toggle = (id) => setSelected((old) => {
    const next = new Set(old);
    if (next.has(id)) next.delete(id); else next.add(id);
    return next;
  });
  const selectAll = async () => {
    const all = new Set();
    for (let off = 0; off < list.total; off += 500) {
      cleaner.results('scan', off, 500).items.forEach((c) => all.add(c.id));
      await new Promise((r) => setTimeout(r, 0));     // long lists: the compositor keeps drawing
    }
    setSelected(all);
  };
  const askPlan = () => attempt(() => {
    cleaner.plan(scan.id, [...selected]);
    refresh();
    setPolling(true);
  });
  // the plan's checks ran in lord-server: its totals, not the app's, go in the confirmation
  useEffect(() => {
    if (plan && plan.state === 'ready' && planShown.current !== plan.id) { planShown.current = plan.id; setConfirm(true); }
  }, [plan]);
  const execute = () => attempt(() => {
    setConfirm(false);
    cleaner.execute(plan.id);
    refresh();
    setPolling(true);
  });

  const scanning = running && info.job === 'scan';
  const notOffered = scan ? Object.values(scan.skipped).reduce((a, b) => a + b, 0) - scan.skipped.nomatch : 0;
  const incompleteText = scan && !scan.complete && scanDone
    ? tr(scan.limit === 'files' ? 'cleaner.incomplete.files' : scan.limit === 'depth' ? 'cleaner.incomplete.depth' : 'cleaner.incomplete') : '';
  const planCounts = plan ? plan.counts : null;
  const notReady = plan ? plan.items - plan.counts.ready : 0;

  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      <View style={{ width: 180, padding: spacing.s, gap: 2, backgroundColor: th.sidebarBg }}>
        <SidebarItem label={tr('cleaner.overview')} selected />
        <Hairline edge="end" />
      </View>

      <View style={{ flex: 1, minWidth: 0, padding: spacing.xl, gap: spacing.m }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
          <View style={{ flex: 1, minWidth: 0, gap: spacing.xs }}>
            <Label size={typeScale.title} weight={600}>{tr('cleaner.overview')}</Label>
            <Label dim>{tr('cleaner.intro')}</Label>
          </View>
          {running && !finished
            ? <Button label={tr('cleaner.cancel')} onClick={stop} />
            : <Button primary label={tr(scan && !finished ? 'cleaner.scanAgain' : 'cleaner.scan')} onClick={startScan} disabled={!available} />}
        </View>

        {!available ? <Label dim>{tr('cleaner.unavailable')}</Label> : null}
        {error ? <Label>{error}</Label> : null}
        {info && info.state === 'failed' && info.error ? <Label>{tr(`cleaner.error.${info.error}`)}</Label> : null}
        {info && info.job === 'scan' && info.state === 'cancelled' ? <Label dim>{tr('cleaner.cancelled')}</Label> : null}

        {scanning ? (
          <View style={{ gap: spacing.s }}>
            <Label dim>{tr('cleaner.scanning')}</Label>
            <View style={{ flexDirection: 'row', gap: spacing.s, flexWrap: 'wrap' }}>
              <Counter label={tr('cleaner.seen')} value={formatNumber(scan ? scan.seen : 0)} />
              <Counter label={tr('cleaner.dirs')} value={formatNumber(scan ? scan.dirs : 0)} />
              <Counter label={tr('cleaner.found')} value={formatNumber(scan ? scan.candidates : 0)} />
            </View>
          </View>
        ) : null}

        {finished ? (
          <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
            <Label weight={600}>{tr('cleaner.finished')}</Label>
            <View style={{ flexDirection: 'row', gap: spacing.m, flexWrap: 'wrap' }}>
              {STATUSES.filter((k) => planCounts[k] > 0).map((k) => <Label key={k} dim>{tr(`cleaner.count.${k}`, { n: formatNumber(planCounts[k]) })}</Label>)}
            </View>
            <Label dim>{tr('cleaner.freeBefore', { size: formatBytes(plan.freeBefore) })}</Label>
            <Label>{tr('cleaner.freeAfter', { size: formatBytes(plan.freeAfter) })}</Label>
            <Label dim>{tr('cleaner.trashNote')}</Label>
            <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 2 }}>
              {outcome.items.map((it) => <Row key={it.id} item={it} />)}
              {outcome.items.length < outcome.total ? <Button small label={tr('cleaner.showMore')} onClick={outcome.more} /> : null}
            </ScrollView>
          </View>
        ) : null}

        {!finished && running && info.job === 'plan' ? <Label dim>{tr('cleaner.preparing')}</Label> : null}
        {!finished && running && info.job === 'execute'
          ? <Label>{tr('cleaner.working', { done: formatNumber(plan.processed), total: formatNumber(plan.items) })}</Label> : null}

        {!finished && !scanning && scanDone ? (
          <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
            <View style={{ flexDirection: 'row', gap: spacing.s, flexWrap: 'wrap' }}>
              <Counter label={tr('cleaner.found')} value={formatNumber(scan.candidates)} />
              <Counter label={tr('cleaner.allocated')} value={formatBytes(scan.allocated)} />
              <Counter label={tr('cleaner.estimated')} value={formatBytes(scan.estimated)} />
            </View>
            <Label dim size={typeScale.callout}>{tr('cleaner.freeSpace', { size: formatBytes(free) })}</Label>
            <Label dim size={typeScale.callout}>{tr('cleaner.estimateNote')}</Label>
            {incompleteText ? <Label>{incompleteText}</Label> : null}
            {notOffered > 0 ? <Label dim size={typeScale.callout}>{tr('cleaner.notOffered', { n: formatNumber(notOffered) })}</Label> : null}
            {scan.candidates === 0
              ? <EmptyState title={tr('cleaner.empty')} hint={tr('cleaner.emptyHint')} />
              : (
                <>
                  {scan.rules.filter((r) => r.files > 0).map((r) => <Label key={r.id} dim size={typeScale.callout}>{tr(r.text)}</Label>)}
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
                    <Button small label={tr('cleaner.selectAll')} onClick={selectAll} />
                    <Button small label={tr('cleaner.selectNone')} onClick={() => setSelected(new Set())} disabled={!selected.size} />
                    <View style={{ flex: 1 }}><Label dim size={typeScale.callout}>{tr('cleaner.selected', { n: formatNumber(selected.size) })}</Label></View>
                    <Button primary label={tr('cleaner.moveToTrash')} onClick={askPlan} disabled={!selected.size || running} />
                  </View>
                  <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 2 }}>
                    {list.items.map((it) => <Row key={it.id} item={it} on={selected.has(it.id)} onToggle={() => toggle(it.id)} />)}
                    {list.items.length < list.total ? <Button small label={tr('cleaner.showMore')} onClick={list.more} /> : null}
                  </ScrollView>
                </>
              )}
          </View>
        ) : null}

        {!scan && !running && available ? <Label dim>{tr('cleaner.freeSpace', { size: formatBytes(free) })}</Label> : null}
      </View>

      {confirm && plan && plan.counts.ready > 0 ? (
        <ConfirmDialog
          title={tr('cleaner.confirm.title', { count: plan.counts.ready })}
          detail={[
            tr('cleaner.confirm.detail', { size: formatBytes(plan.allocated) }),
            notReady > 0 ? tr('cleaner.confirm.notReady', { n: formatNumber(notReady) }) : '',
            ...plan.warnings.filter((w) => w !== 'trashKeepsSpace').map((w) => tr(`cleaner.warn.${w}`)),
          ].filter(Boolean).join(' ')}
          okLabel={tr('cleaner.confirm.ok')}
          cancelLabel={tr('cleaner.cancel')}
          onOk={execute}
          onCancel={() => setConfirm(false)}
        />
      ) : null}
      {confirm && plan && plan.counts.ready === 0 ? (
        <ConfirmDialog title={tr('cleaner.noneReady')} okLabel={tr('cleaner.backToList')} cancelLabel={tr('cleaner.cancel')}
          onOk={() => setConfirm(false)} onCancel={() => setConfirm(false)} />
      ) : null}
    </View>
  );
}
