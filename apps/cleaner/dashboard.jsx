// The full scan: one tile per module that really runs. The tile being scanned grows large in the middle with its own
// colour, and finished ones shrink aside with their result. Every number is the module's real result, the line
// under the title is the real path being read. Moves are left, top, width and height through lord.setStyle.
import { View, Image, t as tr, formatBytes, formatNumber, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useRef } from 'react';
import { Label, tween, usePal, midTrim } from './ui.jsx';
import { foundSoFar } from './sections.jsx';

function rects(W, H, active) {
  const big = { x: W * 0.26, y: H * 0.02, w: W * 0.74 - 8, h: H * 0.88 };
  let slot = 0;
  return (i) => {
    if (i === active) return big;
    const h = (H * 0.88 - 2 * 10) / 3, r = { x: 0, y: H * 0.02 + slot * (h + 10), w: W * 0.26 - 12, h };
    slot++;
    return r;
  };
}

function Tile({ mod, rect, active, result, now, seen, scan }) {
  const p = usePal();
  const box = useRef(null), last = useRef(rect);
  useEffect(() => {
    const a = last.current, b = rect;
    last.current = b;
    if (a === b || (a.x === b.x && a.w === b.w && a.y === b.y)) return;
    tween(box.current, { left: [a.x, b.x], top: [a.y, b.y], width: [a.w, b.w], height: [a.h, b.h] }, { ms: 520 });
  }, [rect.x, rect.y, rect.w, rect.h]);
  return (
    <View ref={box} style={{ position: 'absolute', left: rect.x, top: rect.y, width: rect.w, height: rect.h, borderRadius: 22, overflow: 'hidden', padding: spacing.l, gap: spacing.xs,
      backgroundColor: active ? p.module[mod.id] : p.glass, border: `1px solid ${p.line}` }}>
      <Image src={`asset:cleaner/hero-${mod.art}.png`} style={active ? { position: 'absolute', right: 40, bottom: 56, width: 280, height: 267 } : { position: 'absolute', right: -26, bottom: -30, width: 118, height: 113, opacity: result ? 1 : 0.5 }} />
      <Label size={active ? 40 : typeScale.body} weight={active ? 300 : 600} style={active ? { color: '#ffffff' } : null}>{tr(`cleaner.section.${mod.id}`)}</Label>
      {active ? (
        <>
          {foundSoFar(scan).map((r, i) => <Label key={r.id} size={typeScale.callout} lines={1} style={{ color: '#ffffff', position: 'absolute', left: spacing.l, top: 76 + i * 22 }}>{`${tr(`cleaner.ruleName.${r.id}`)}  ${formatBytes(r.allocated)}`}</Label>)}
          <Label size={typeScale.callout} mono lines={1} style={{ color: '#ffffff', opacity: 0.85, position: 'absolute', left: spacing.l, right: spacing.l, bottom: spacing.xl + 18 }}>{midTrim(now)}</Label>
          <Label size={typeScale.callout} style={{ color: '#ffffff', opacity: 0.85, position: 'absolute', left: spacing.l, bottom: spacing.l }}>{tr('cleaner.seenCount', { n: formatNumber(seen) })}</Label>
        </>
      ) : result ? (
        <>
          <Label size={24} weight={600}>{result.big}</Label>
          <Label size={typeScale.caption} dim>{result.label}</Label>
        </>
      ) : <Label size={typeScale.caption} dim>{tr('cleaner.waiting')}</Label>}
    </View>
  );
}

export const MODULES = [{ id: 'cleanup', kind: 'rules', art: 'cleanup' }, { id: 'storage', kind: 'tree', art: 'storage' }, { id: 'duplicates', kind: 'dups', art: 'duplicates' }];

export function moduleResult(id, scan) {
  if (id === 'cleanup') return { big: formatBytes(scan.estimated), label: tr('cleaner.estimated') };
  if (id === 'storage') return { big: formatBytes(scan.types.reduce((a, t) => a + t.allocated, 0)), label: tr('cleaner.dash.storage', { n: formatNumber(scan.dirs) }) };
  return { big: formatBytes(scan.reclaim), label: tr('cleaner.dups.reclaim', { count: scan.groups }) };
}

export function Dashboard({ W, H, step, results, scan }) {
  const at = rects(W, H, step);
  return (
    <View style={{ flex: 1, minHeight: 0 }}>
      {MODULES.map((m, i) => <Tile key={m.id} mod={m} rect={at(i)} active={i === step} result={results[m.id]} now={scan ? scan.now : ''} seen={scan ? scan.seen : 0} scan={i === step ? scan : null} />)}
    </View>
  );
}
