// Similar images: pictures that look alike in the folders the user chose, such as a resized or recompressed copy.
// Decoding happens only in the sandboxed lord-clean helper. Nothing is selected by itself, every image is shown, and
// lord-server keeps at least one image of each group. They aren't the same bytes, so what goes is the user's choice.
import { View, Image, cleaner, useThumb, t as tr, formatBytes, formatNumber, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useMemo } from 'react';
import { Label, Glass, GlassButton, Pill, Check, Hero, PagedList, CountUp, SelectBar, usePal } from './ui.jsx';
import { picks } from './duplicates.jsx';

export function SimilarHero({ folders, chosen, toggle }) {
  return (
    <Hero art="duplicates" title={tr('cleaner.hero.similar.title')} line={tr('cleaner.hero.similar.line')} features={[['large', tr('cleaner.hero.similar.f1')], ['duplicates', tr('cleaner.hero.similar.f2')], ['activity', tr('cleaner.hero.similar.f3')]]}>
      <View style={{ flexDirection: 'row', gap: spacing.xs, flexWrap: 'wrap', marginTop: spacing.s }}>
        {folders.map((f) => <Pill key={f} label={f} on={chosen.has(f)} onClick={() => toggle(f)} />)}
      </View>
    </Hero>
  );
}

const ROW = 72;

function Picture({ item }) {
  const p = usePal();
  const src = useThumb(item.abs, 128);
  return (
    <View style={{ width: 60, height: 60, borderRadius: 10, overflow: 'hidden', backgroundColor: p.track }}>
      {src ? <Image src={src} style={{ width: 60, height: 60 }} /> : null}
    </View>
  );
}

export function SimilarList({ scan, selected, setSelected }) {
  const list = useMemo(() => {
    const rows = [], groups = [];
    for (let off = 0; off < scan.groups; off += 100) {
      const r = cleaner.groups(scan.id, off, 100);
      for (const g of r.groups) {
        rows.push({ head: true, id: `g${g.id}`, g });
        for (const m of g.members) rows.push(m);
        groups.push(g.members);
      }
    }
    rows.options = picks(groups);
    rows.size = new Map(groups.flat().map((m) => [m.id, m.allocated]));
    return rows;
  }, [scan.id]);
  const toggle = (id) => setSelected((old) => { const x = new Set(old); if (x.has(id)) x.delete(id); else x.add(id); return x; });
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: 96 }}>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end' }}>
        <View style={{ flex: 1 }}>
          <Label size={32} weight={300}>{tr('cleaner.similar.title')}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.similar.hint')}</Label>
        </View>
        <View style={{ alignItems: 'flex-end' }}>
          <Label dim size={typeScale.callout}>{tr('cleaner.similar.reclaim', { count: scan.groups })}</Label>
          <CountUp value={scan.reclaim} format={formatBytes} />
        </View>
      </View>
      {!scan.complete ? <Label>{tr(scan.limit ? `cleaner.incomplete.${scan.limit}` : 'cleaner.incomplete')}</Label> : null}
      {scan.skipped.undecoded ? <Label dim size={typeScale.callout}>{tr('cleaner.similar.undecoded', { count: scan.skipped.undecoded })}</Label> : null}
      {list.length ? <SelectBar selected={selected} options={list.options} onPick={(ids) => setSelected(new Set(ids))} size={[...selected].reduce((a, i) => a + (list.size.get(i) || 0), 0)} /> : null}
      {!list.length ? <Label dim>{tr('cleaner.similar.none')}</Label> : (
        <Glass style={{ flex: 1, minHeight: 0, padding: spacing.xs }}>
          <PagedList rowH={ROW} reset={scan.id} fetch={(o, l) => ({ items: list.slice(o, o + l), total: list.length })} render={(r) => (r.head ? (
            <View key={r.id} style={{ height: ROW, justifyContent: 'flex-end', paddingBottom: 6, paddingHorizontal: spacing.s }}>
              <Label size={typeScale.callout} weight={600}>{tr('cleaner.similar.group', { count: r.g.members.length })}</Label>
            </View>
          ) : (
            <View key={r.id} onClick={() => toggle(r.id)} style={{ height: ROW, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s }}>
              <Check on={selected.has(r.id)} />
              <Picture item={r} />
              <View style={{ flex: 1, minWidth: 0 }}>
                <Label mono size={typeScale.callout} lines={1}>{r.path}</Label>
                <Label dim size={typeScale.caption}>{`${formatNumber(r.width)} × ${formatNumber(r.height)} · ${formatBytes(r.allocated)}`}</Label>
              </View>
              <GlassButton small label={tr('cleaner.show')} onClick={() => revealPath(r.abs)} />
            </View>
          ))} />
        </Glass>
      )}
    </View>
  );
}
