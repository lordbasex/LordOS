// Storage: a volume's bounded tree, drawn as a squarified treemap in an Image, a folder tree with logical and
// allocated sizes, and a summary by type. Selecting a rectangle only selects it.
import { View, Image, cleaner, t as tr, formatBytes, formatNumber, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Label, Glass, GlassButton, Pill, Hero, Bar, Enter, PagedList } from './ui.jsx';

const PALETTE = [[180, 108, 255], [94, 234, 212], [255, 209, 102], [125, 211, 252], [253, 164, 175], [190, 242, 100], [124, 92, 255], [255, 255, 255]];

// squarified layout of values (largest first) in x, y, w, h
export function squarify(items, x, y, w, h) {
  const out = [];
  const total = items.reduce((a, b) => a + b.v, 0);
  if (!(total > 0) || w <= 0 || h <= 0) return out;
  const scale = (w * h) / total;
  let i = 0;
  while (i < items.length) {
    const side = Math.min(w, h);
    let sum = 0, best = Infinity, k = i;
    for (; k < items.length; k++) {
      const a = items[k].v * scale, s2 = sum + a;
      const lo = items[i].v * scale, hi = a;
      const worst = Math.max((side * side * Math.max(lo, hi)) / (s2 * s2), (s2 * s2) / (side * side * Math.min(lo, hi)));
      if (worst > best) break;
      best = worst;
      sum = s2;
    }
    const thick = sum / side;
    let off = 0;
    for (let j = i; j < k; j++) {
      const len = (items[j].v * scale) / thick;
      if (w >= h) out.push({ item: items[j], x, y: y + off, w: thick, h: len });
      else out.push({ item: items[j], x: x + off, y, w: len, h: thick });
      off += len;
    }
    if (w >= h) { x += thick; w -= thick; } else { y += thick; h -= thick; }
    i = k;
  }
  return out;
}

function paint(rects, W, H, sel) {
  const data = new Uint8ClampedArray(W * H * 4);
  rects.forEach((r, n) => {
    const c = PALETTE[n % PALETTE.length];
    const x0 = Math.round(r.x) + 1, y0 = Math.round(r.y) + 1, x1 = Math.round(r.x + r.w) - 1, y1 = Math.round(r.y + r.h) - 1;
    const on = sel === r.item.id;
    for (let y = Math.max(0, y0); y < Math.min(H, y1); y++) {
      const shade = 1.08 - ((y - y0) / Math.max(1, y1 - y0)) * 0.34;
      const a = on ? 255 : 205;
      let o = (y * W + Math.max(0, x0)) * 4;
      for (let x = Math.max(0, x0); x < Math.min(W, x1); x++, o += 4) { data[o] = c[0] * shade; data[o + 1] = c[1] * shade; data[o + 2] = c[2] * shade; data[o + 3] = a; }
    }
  });
  return { width: W, height: H, data };
}

function Treemap({ data, onOpen }) {
  const box = useRef(null);
  const [size, setSize] = useState({ w: 0, h: 0 });
  const [sel, setSel] = useState(null);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!box.current) return;
      const [, , w, h] = lord.getLayout(box.current.id);
      if (w > 0 && (Math.abs(w - size.w) > 2 || Math.abs(h - size.h) > 2)) setSize({ w: Math.round(w), h: Math.round(h) });
    }, 30);
    return () => clearTimeout(id);
  });
  const rects = useMemo(() => {
    const items = data.children.filter((c) => c.allocated > 0).slice(0, 120).map((c) => ({ ...c, v: c.allocated }));
    if (data.ownAllocated > 0) items.push({ id: -1, name: tr('cleaner.map.looseFiles'), v: data.ownAllocated, allocated: data.ownAllocated, logical: data.ownLogical });
    items.sort((a, b) => b.v - a.v);
    return squarify(items, 0, 0, size.w, size.h);
  }, [data, size]);
  const pixels = useMemo(() => (size.w ? paint(rects, size.w, size.h, sel && sel.id) : null), [rects, size, sel]);
  const click = (e) => {
    const [x, y] = lord.getLayout(box.current.id);
    const hit = rects.find((r) => e.x - x >= r.x && e.x - x < r.x + r.w && e.y - y >= r.y && e.y - y < r.y + r.h);
    setSel(hit ? hit.item : null);
  };
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <View ref={box} onClick={click} style={{ flex: 1, minHeight: 0, borderRadius: 12, overflow: 'hidden', direction: 'ltr' }}>
        {pixels ? <Image pixels={pixels} style={{ position: 'absolute', left: 0, top: 0, width: size.w, height: size.h }} /> : null}
        {rects.filter((r) => r.w > 90 && r.h > 34).slice(0, 18).map((r) => (
          <View key={r.item.id} style={{ position: 'absolute', left: r.x + 8, top: r.y + 6, width: r.w - 14, pointerEvents: 'none' }}>
            <Label size={typeScale.callout} weight={600} lines={1} style={{ color: '#12122A' }}>{r.item.name}</Label>
            <Label size={typeScale.caption} lines={1} style={{ color: '#12122A' }}>{formatBytes(r.item.allocated)}</Label>
          </View>
        ))}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, height: 28 }}>
        {sel ? (
          <>
            <View style={{ flex: 1, minWidth: 0 }}><Label lines={1}>{tr('cleaner.map.selected', { name: sel.name, logical: formatBytes(sel.logical), allocated: formatBytes(sel.allocated) })}</Label></View>
            {sel.id >= 0 && sel.hasKids ? <GlassButton small label={tr('cleaner.map.open')} onClick={() => { const i = sel.id; setSel(null); onOpen(i); }} /> : null}
          </>
        ) : <Label dim size={typeScale.callout}>{tr('cleaner.map.hint')}</Label>}
      </View>
    </View>
  );
}

function Folders({ scanId, data, onOpen }) {
  const rows = data.children;
  return (
    <Glass style={{ flex: 1, minHeight: 0, padding: spacing.xs }}>
      <View style={{ flexDirection: 'row', height: 26, alignItems: 'center', paddingHorizontal: spacing.s, gap: spacing.s }}>
        <View style={{ flex: 1 }}><Label dim size={typeScale.callout}>{tr('cleaner.map.folder')}</Label></View>
        <View style={{ width: 90 }}><Label dim size={typeScale.callout} align="end">{tr('cleaner.logical')}</Label></View>
        <View style={{ width: 90 }}><Label dim size={typeScale.callout} align="end">{tr('cleaner.allocated')}</Label></View>
      </View>
      <PagedList rowH={32} reset={`${scanId}.${data.node.id}`} fetch={(o, l) => ({ items: rows.slice(o, o + l), total: rows.length })} render={(c) => (
        <View key={c.id} onClick={c.hasKids ? () => onOpen(c.id) : undefined} style={{ height: 32, flexDirection: 'row', alignItems: 'center', paddingHorizontal: spacing.s, gap: spacing.s }}>
          <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
            <Label size={typeScale.callout} lines={1}>{`${c.hasKids ? '▸ ' : '   '}${c.name}`}</Label>
            <Bar value={data.node.allocated ? c.allocated / data.node.allocated : 0} style={{ height: 3 }} />
          </View>
          <View style={{ width: 90 }}><Label dim size={typeScale.callout} align="end">{formatBytes(c.logical)}</Label></View>
          <View style={{ width: 90 }}><Label size={typeScale.callout} align="end">{formatBytes(c.allocated)}</Label></View>
        </View>
      )} />
    </Glass>
  );
}

function Types({ scan }) {
  const max = Math.max(1, ...scan.types.map((t) => t.allocated));
  return (
    <Glass style={{ padding: spacing.m, gap: spacing.s }}>
      {scan.types.slice().sort((a, b) => b.allocated - a.allocated).map((t) => (
        <View key={t.type} style={{ gap: 3 }}>
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <View style={{ flex: 1 }}><Label size={typeScale.callout}>{tr(`cleaner.type.${t.type}`)}</Label></View>
            <Label dim size={typeScale.callout}>{tr('cleaner.files', { count: t.files })}</Label>
            <View style={{ width: 86 }}><Label dim size={typeScale.callout} align="end">{formatBytes(t.logical)}</Label></View>
            <View style={{ width: 86 }}><Label size={typeScale.callout} align="end">{formatBytes(t.allocated)}</Label></View>
          </View>
          <Bar value={t.allocated / max} style={{ height: 5 }} />
        </View>
      ))}
    </Glass>
  );
}

export function VolumeChips({ volumes, value, onChange }) {
  return (
    <View style={{ gap: spacing.xs, marginTop: spacing.s }}>
      {volumes.map((v) => (
        <Glass key={v.id} lift onClick={() => onChange(v.id)} style={{ padding: spacing.s, gap: 4, opacity: value === v.id ? 1 : 0.6 }}>
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <View style={{ flex: 1, minWidth: 0 }}><Label size={typeScale.callout} weight={600} lines={1}>{v.id === 'home' ? tr('cleaner.map.home') : v.id === 'root' ? tr('cleaner.map.root') : v.path}</Label></View>
            <Label dim size={typeScale.caption}>{tr('cleaner.disk.free', { size: formatBytes(v.free) })}</Label>
          </View>
          <Bar value={v.total > 0 ? 1 - v.free / v.total : 0} style={{ height: 4 }} />
        </Glass>
      ))}
    </View>
  );
}

export function StorageHero({ volumes, volume, setVolume }) {
  return (
    <Hero title={tr('cleaner.hero.storage.title')} line={tr('cleaner.hero.storage.line')}>
      <VolumeChips volumes={volumes} value={volume} onChange={setVolume} />
    </Hero>
  );
}

export function StorageView({ scan }) {
  const [node, setNode] = useState(0);
  const [view, setView] = useState('map');
  useEffect(() => setNode(0), [scan.id]);
  const data = useMemo(() => cleaner.tree(scan.id, node, 500), [scan.id, node]);
  if (!data) return <Label dim>{tr('cleaner.map.empty')}</Label>;
  return (
    <Enter style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Label size={typeScale.title} weight={600} lines={1}>{data.trail.length ? data.trail[data.trail.length - 1].name : data.base}</Label>
          <Label dim size={typeScale.callout}>{tr('cleaner.map.totals', { logical: formatBytes(data.node.logical), allocated: formatBytes(data.node.allocated), files: formatNumber(data.node.files) })}</Label>
        </View>
        {node ? <GlassButton small label={tr('cleaner.map.up')} onClick={() => setNode(data.node.up)} /> : null}
        <GlassButton small label={tr('cleaner.show')} onClick={() => revealPath(`${data.base === '/' ? '' : data.base}/${data.trail.map((t) => t.name).join('/')}/.`)} />
        {[['map', 'cleaner.map.viewMap'], ['tree', 'cleaner.map.viewTree'], ['types', 'cleaner.map.viewTypes']].map(([k, key]) => <Pill key={k} label={tr(key)} on={view === k} onClick={() => setView(k)} />)}
      </View>
      {!scan.complete ? <Label size={typeScale.callout}>{tr(scan.limit ? `cleaner.incomplete.${scan.limit}` : 'cleaner.incomplete')}</Label> : null}
      {view === 'map' ? <Treemap data={data} onOpen={setNode} /> : view === 'tree' ? <Folders scanId={scan.id} data={data} onOpen={setNode} /> : <Types scan={scan} />}
    </Enter>
  );
}
