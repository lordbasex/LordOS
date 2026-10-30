// What a full scan found: one headline, "Review all", and a card per module with a real statement and real
// previews. A card appears only for what the scan really found.
import { View, Image, media, cleaner, t as tr, formatBytes, formatNumber, typeScale, spacing } from '@lordos/sdk';
import { useMemo } from 'react';
import { Label, Glass, GlassButton, Enter, CountUp, Tile, usePal } from './ui.jsx';

function Preview({ items }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', gap: spacing.xs, height: 72 }}>
      {items.slice(0, 4).map((it) => {
        const src = it.type === 'image' ? media.thumb(it.abs, 144) : null;
        return (
          <View key={it.id} style={{ width: 72, height: 72, borderRadius: 12, overflow: 'hidden', backgroundColor: p.track, alignItems: 'center', justifyContent: 'center' }}>
            {src ? <Image src={src} style={{ width: 72, height: 72 }} /> : <Label size={typeScale.caption} dim lines={1} style={{ paddingHorizontal: 4 }}>{(it.path.split('.').pop() || '').slice(0, 6)}</Label>}
          </View>
        );
      })}
    </View>
  );
}

function Card({ tile, title, line, items, onReview, delay }) {
  return (
    <Enter delay={delay} x={0} y={14} ms={500} style={{ flex: 1, minWidth: 220 }}>
      <Glass style={{ padding: spacing.l, gap: spacing.s }}>
        <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center' }}>
          <Tile name={tile} size={28} />
          <View style={{ flex: 1, minWidth: 0 }}><Label size={15} weight={600} lines={1}>{title}</Label></View>
        </View>
        <Label dim size={typeScale.callout}>{line}</Label>
        {items && items.length ? <Preview items={items} /> : null}
        <View style={{ flexDirection: 'row' }}><GlassButton small label={tr('cleaner.review')} onClick={onReview} /></View>
      </Glass>
    </Enter>
  );
}

export function ReviewBoard({ rules, tree, dups, go }) {
  const large = useMemo(() => (tree ? cleaner.results(tree.id, 0, 4, { minSize: 10 * 1048576 }) : null), [tree && tree.id]);
  const copies = useMemo(() => {
    if (!dups || !dups.groups) return [];
    return cleaner.groups(dups.id, 0, 4).groups.map((g) => g.members[0]);
  }, [dups && dups.id]);
  const cats = rules ? rules.rules.filter((r) => r.files > 0 && r.action !== 'none' && !r.explicit) : [];
  const total = (rules ? rules.estimated : 0) + (dups ? dups.reclaim : 0);
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.m }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <CountUp value={total} format={(v) => tr('cleaner.review.headline', { size: formatBytes(v) })} size={32} weight={300} />
          <Label dim size={typeScale.callout}>{tr('cleaner.estimateNote')}</Label>
        </View>
        <GlassButton label={tr('cleaner.reviewAll')} onClick={() => go('cleanup')} />
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.m, flexWrap: 'wrap' }}>
        {cats.length ? <Card tile="cleanup" delay={0} title={tr('cleaner.section.cleanup')} line={tr('cleaner.review.cleanup', { size: formatBytes(rules.estimated), count: cats.length })} onReview={() => go('cleanup')} /> : null}
        {large && large.total ? <Card tile="large" delay={80} title={tr('cleaner.section.large')} line={tr('cleaner.review.large', { count: large.total })} items={large.items} onReview={() => go('large')} /> : null}
        {dups && dups.groups ? <Card tile="duplicates" delay={160} title={tr('cleaner.section.duplicates')} line={tr('cleaner.review.dups', { size: formatBytes(dups.reclaim), count: dups.groups })} items={copies} onReview={() => go('duplicates')} /> : null}
        {tree ? <Card tile="storage" delay={240} title={tr('cleaner.section.storage')} line={tr('cleaner.review.storage', { n: formatNumber(tree.dirs) })} onReview={() => go('storage')} /> : null}
      </View>
    </View>
  );
}
