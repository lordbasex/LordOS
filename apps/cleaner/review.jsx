// What a full scan found: one headline, what it is made of, "Review all", and a card per module with its real
// number and the real things behind it. A card appears only for what the scan really found.
import { View, Image, media, cleaner, t as tr, formatBytes, formatNumber, typeScale, spacing } from '@lordos/sdk';
import { useMemo } from 'react';
import { Label, Glass, GlassButton, Enter, CountUp, Tile, Bar, usePal } from './ui.jsx';
import { folderKinds, folderKind } from './storage.jsx';

const TYPE_ICON = { image: 'pictures', video: 'videos', audio: 'music', document: 'documents', code: 'code' };
const ROWS = 4;

function Preview({ items }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', gap: spacing.s, height: 118, overflow: 'hidden' }}>
      {items.slice(0, ROWS).map((it) => {
        const src = it.type === 'image' ? media.thumb(it.abs, 144) : null;
        return (
          <View key={it.id} style={{ width: 80, gap: 3 }}>
            <View style={{ width: 80, height: 80, borderRadius: 12, overflow: 'hidden', backgroundColor: p.track, alignItems: 'center', justifyContent: 'center', gap: 3 }}>
              {src ? <Image src={src} style={{ width: 80, height: 80 }} /> : <Image src={`asset:cleaner/folder-${TYPE_ICON[it.type] || 'folder'}.png`} style={{ width: 28, height: 28 }} />}
              {src ? null : <Label size={typeScale.caption} dim lines={1} align="center" style={{ paddingHorizontal: 4 }}>{(it.path.split('.').pop() || '').slice(0, 6)}</Label>}
            </View>
            <Label size={typeScale.caption} lines={1} align="center">{it.path.split('/').pop()}</Label>
            <Label size={typeScale.caption} dim lines={1} align="center">{formatBytes(it.allocated)}</Label>
          </View>
        );
      })}
    </View>
  );
}

// A named part of the total: its name, its size and a bar of its share
function Part({ icon, name, size, share }) {
  return (
    <View style={{ gap: 3 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
        {icon ? <Image src={`asset:cleaner/folder-${icon}.png`} style={{ width: 16, height: 16, flexShrink: 0 }} /> : null}
        <View style={{ flex: 1, minWidth: 0 }}><Label size={typeScale.callout} lines={1}>{name}</Label></View>
        <Label dim size={typeScale.callout}>{formatBytes(size)}</Label>
      </View>
      <Bar value={share} h={3} />
    </View>
  );
}

function Card({ tile, title, big, line, onReview, delay, more, children }) {
  return (
    <Enter delay={delay} x={0} y={14} ms={500} style={{ flex: 1, minWidth: 0, minHeight: 0 }}>
      <Glass style={{ flex: 1, minHeight: 0, padding: spacing.l, gap: spacing.s, overflow: 'hidden' }}>
        <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'center' }}>
          <Tile name={tile} size={36} />
          <View style={{ flex: 1, minWidth: 0 }}>
            <Label size={15} weight={600} lines={1}>{title}</Label>
            <Label dim size={typeScale.callout} lines={1}>{line}</Label>
          </View>
          {big ? <Label size={20} weight={300}>{big}</Label> : null}
        </View>
        <View style={{ flex: 1, minHeight: 0, gap: spacing.s, overflow: 'hidden' }}>{children}</View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <View style={{ flex: 1, minWidth: 0 }}>{more ? <Label dim size={typeScale.caption} lines={1}>{more}</Label> : null}</View>
          <GlassButton small label={tr('cleaner.review')} onClick={onReview} />
        </View>
      </Glass>
    </Enter>
  );
}

// What the total is made of: a bar with one length per module
function Makeup({ parts, total }) {
  const p = usePal();
  return (
    <Enter delay={40} x={0} y={8} ms={420}>
      <View style={{ gap: spacing.xs }}>
        <View style={{ flexDirection: 'row', height: 10, borderRadius: 5, overflow: 'hidden', backgroundColor: p.track, gap: 2 }}>
          {parts.map((m) => <View key={m.id} style={{ flexGrow: m.size / total, flexShrink: 1, flexBasis: 0, backgroundColor: p.module[m.id] }} />)}
        </View>
        <View style={{ flexDirection: 'row', gap: spacing.l, flexWrap: 'wrap' }}>
          {parts.map((m) => (
            <View key={m.id} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
              <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: p.module[m.id] }} />
              <Label dim size={typeScale.callout}>{`${tr(`cleaner.section.${m.id}`)} · ${formatBytes(m.size)}`}</Label>
            </View>
          ))}
        </View>
      </View>
    </Enter>
  );
}

export function ReviewBoard({ rules, tree, dups, go }) {
  const large = useMemo(() => (tree ? cleaner.results(tree.id, 0, ROWS, { minSize: 10 * 1048576 }) : null), [tree && tree.id]);
  const copies = useMemo(() => {
    if (!dups || !dups.groups) return [];
    return cleaner.groups(dups.id, 0, ROWS).groups.map((g) => g.members[0]);
  }, [dups && dups.id]);
  const home = useMemo(() => (tree ? cleaner.tree(tree.id, 0, ROWS) : null), [tree && tree.id]);
  const kinds = useMemo(folderKinds, []);
  const cats = rules ? rules.rules.filter((r) => r.files > 0 && r.action !== 'none' && !r.explicit).sort((a, b) => b.allocated - a.allocated) : [];
  const cleanup = rules ? rules.estimated : 0, reclaim = dups ? dups.reclaim : 0, total = cleanup + reclaim;
  const parts = [{ id: 'cleanup', size: cleanup }, { id: 'duplicates', size: reclaim }].filter((m) => m.size > 0);
  const cards = [
    cats.length ? (d) => (
      <Card key="cleanup" tile="cleanup" delay={d} title={tr('cleaner.section.cleanup')} big={formatBytes(cleanup)} line={tr('cleaner.review.cats', { count: cats.length })} more={cats.length > ROWS ? tr('cleaner.review.more', { n: formatNumber(cats.length - ROWS) }) : ''} onReview={() => go('cleanup')}>
        {cats.slice(0, ROWS).map((r) => <Part key={r.id} name={tr(`cleaner.ruleName.${r.id}`)} size={r.allocated} share={cats[0].allocated ? r.allocated / cats[0].allocated : 0} />)}
      </Card>
    ) : null,
    large && large.total ? (d) => (
      <Card key="large" tile="large" delay={d} title={tr('cleaner.section.large')} line={tr('cleaner.review.large', { count: large.total })} onReview={() => go('large')}>
        <Preview items={large.items} />
      </Card>
    ) : null,
    dups && dups.groups ? (d) => (
      <Card key="duplicates" tile="duplicates" delay={d} title={tr('cleaner.section.duplicates')} big={formatBytes(reclaim)} line={tr('cleaner.review.groups', { count: dups.groups })} onReview={() => go('duplicates')}>
        <Preview items={copies} />
      </Card>
    ) : null,
    tree ? (d) => (
      <Card key="storage" tile="storage" delay={d} title={tr('cleaner.section.storage')} line={tr('cleaner.review.storage', { n: formatNumber(tree.dirs) })} onReview={() => go('storage')}>
        {home ? home.children.slice(0, ROWS).map((c) => <Part key={c.id} icon={folderKind(c, kinds)} name={c.name} size={c.allocated} share={home.node.allocated ? c.allocated / home.node.allocated : 0} />) : null}
      </Card>
    ) : null,
  ].filter(Boolean);
  const rows = [];
  for (let i = 0; i < cards.length; i += 2) rows.push(cards.slice(i, i + 2));
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.m, paddingBottom: spacing.xl }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <CountUp value={total} format={(v) => tr('cleaner.review.headline', { size: formatBytes(v) })} size={32} weight={300} />
          <Label dim size={typeScale.callout}>{tr('cleaner.estimateNote')}</Label>
        </View>
        <GlassButton label={tr('cleaner.reviewAll')} onClick={() => go('cleanup')} />
      </View>
      {parts.length ? <Makeup parts={parts} total={total} /> : null}
      <View style={{ flex: 1, minHeight: 0, gap: spacing.m }}>
        {rows.map((row, i) => (
          <View key={i} style={{ flex: 1, minHeight: 0, maxHeight: 280, flexDirection: 'row', gap: spacing.m }}>
            {row.map((card, j) => card((i * 2 + j) * 80))}
          </View>
        ))}
      </View>
    </View>
  );
}
