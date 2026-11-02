// Duplicates: exact copies in the folders the user chose. Nothing is selected by itself; the selection menu picks
// copies by group. axia-server keeps
// at least one independent copy of every group and compares bytes right before acting.
import { View, cleaner, t as tr, formatBytes, formatNumber, formatDate, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useMemo } from 'react';
import { Label, Glass, GlassButton, Pill, Check, Hero, PagedList, CountUp, SelectBar, ORB_SPACE } from './ui.jsx';

// which copies to pick; axia-server keeps one of each group whatever is picked
export const picks = (groups, smart) => {
  const by = (keep) => groups.flatMap((g) => { const k = keep(g); return g.filter((m) => m !== k).map((m) => m.id); });
  const newest = (g) => g.reduce((a, m) => (m.mtime > a.mtime ? m : a)), oldest = (g) => g.reduce((a, m) => (m.mtime < a.mtime ? m : a));
  return [{ key: 'smart', ids: by(smart || oldest) }, { key: 'older', ids: by(newest) }, { key: 'newer', ids: by(oldest) }];
};

export function DupHero({ folders, chosen, toggle }) {
  return (
    <Hero art="duplicates" title={tr('cleaner.hero.dups.title')} line={tr('cleaner.hero.dups.line')} features={[['large', tr('cleaner.hero.dups.f1')], ['duplicates', tr('cleaner.hero.dups.f2')], ['activity', tr('cleaner.hero.dups.f3')]]}>
      <View style={{ flexDirection: 'row', gap: spacing.xs, flexWrap: 'wrap', marginTop: spacing.s }}>
        {folders.map((f) => <Pill key={f} label={f} on={chosen.has(f)} onClick={() => toggle(f)} />)}
      </View>
    </Hero>
  );
}

export function DupList({ scan, selected, setSelected }) {
  const fetch = useMemo(() => {
    // groups flattened into rows, a fixed height each
    const rows = [], groups = [];
    let total = 0;
    for (let off = 0; off < scan.groups; off += 100) {
      const r = cleaner.groups(scan.id, off, 100);
      total = r.total;
      for (const g of r.groups) {
        rows.push({ head: true, id: `g${g.id}`, g });
        for (const m of g.members) rows.push({ ...m, gid: g.id });
        groups.push(g.members);
      }
    }
    return { rows, total, options: picks(groups), size: new Map(groups.flat().map((m) => [m.id, m.allocated])) };
  }, [scan.id]);
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: ORB_SPACE }}>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end' }}>
        <View style={{ flex: 1 }}>
          <Label size={32} weight={300}>{tr('cleaner.dups.title')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.dups.hint')}</Label>
        </View>
        <View style={{ alignItems: 'flex-end' }}>
          <Label dim size={typeScale.callout}>{tr('cleaner.dups.reclaim', { count: fetch.total })}</Label>
          <CountUp value={scan.reclaim} format={formatBytes} />
        </View>
      </View>
      {fetch.rows.length ? <SelectBar selected={selected} options={fetch.options} onPick={(ids) => setSelected(new Set(ids))} size={[...selected].reduce((a, i) => a + (fetch.size.get(i) || 0), 0)} /> : null}
      {!fetch.rows.length ? <Label dim>{tr('cleaner.dups.none')}</Label> : (
        <Glass style={{ flex: 1, minHeight: 0, padding: spacing.xs }}>
          <PagedList rowH={34} reset={scan.id} fetch={(o, l) => ({ items: fetch.rows.slice(o, o + l), total: fetch.rows.length })} render={(r) => (r.head ? (
            <View key={r.id} style={{ height: 34, flexDirection: 'row', alignItems: 'flex-end', paddingBottom: 4, paddingHorizontal: spacing.s }}>
              <Label size={typeScale.callout} weight={600}>{tr('cleaner.dups.group', { size: formatBytes(r.g.size), count: r.g.members.length })}</Label>
            </View>
          ) : (
            <View key={r.id} onClick={() => setSelected((old) => { const x = new Set(old); if (x.has(r.id)) x.delete(r.id); else x.add(r.id); return x; })}
              style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s }}>
              <Check on={selected.has(r.id)} />
              <View style={{ flex: 1, minWidth: 0 }}><Label mono size={typeScale.callout} lines={1}>{r.path}</Label></View>
              <Label dim size={typeScale.caption}>{formatDate(new Date(r.mtime * 1000), 'short')}</Label>
              <GlassButton small label={tr('cleaner.show')} onClick={() => revealPath(r.abs)} />
            </View>
          ))} />
        </Glass>
      )}
    </View>
  );
}
