// Storage: a volume's bounded tree, drawn as a squarified treemap in an Image, a folder tree with logical and
// allocated sizes, and a summary by type. Selecting a rectangle only selects it.
import { View, Image, cleaner, useAppWindow, useStore, windows, wm, userDirs, t as tr, formatBytes, formatDate, formatNumber, revealPath, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { Label, Glass, GlassButton, Pill, Hero, Bar, Enter, PagedList, usePal } from './ui.jsx';

const SAFE = '#35c98f', REVIEW = '#f0b13c';
const COLOURS = ['#7b2fe0', '#2a6fe0', '#c0349f', '#0f8f86', '#b8541a', '#a02c56', '#55577f', '#3f7fb5'];

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

// One view per area, 28 at most: the rest are shown together, so areas can rise in and stay crisp.
// bands rising from the bottom of a tile: its share that can go, regenerable first
function Bands({ item, h }) {
  const whole = item.allocated || 1, safe = Math.min(1, (item.safe || 0) / whole), review = Math.min(1 - safe, (item.review || 0) / whole);
  return (
    <>
      {safe > 0 ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: Math.max(3, h * safe), backgroundColor: SAFE, opacity: 0.6 }} /> : null}
      {review > 0 ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: safe > 0 ? Math.max(3, h * safe) : 0, height: Math.max(3, h * review), backgroundColor: REVIEW, opacity: 0.55 }} /> : null}
    </>
  );
}

function Legend() {
  const dot = (c, key) => (
    <View key={key} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
      <View style={{ width: 10, height: 10, borderRadius: 3, backgroundColor: c }} />
      <Label dim size={typeScale.callout}>{tr(key)}</Label>
    </View>
  );
  return <View style={{ flexDirection: 'row', gap: spacing.m }}>{dot(SAFE, 'cleaner.map.legendSafe')}{dot(REVIEW, 'cleaner.map.legendReview')}</View>;
}

function Treemap({ data, onOpen }) {
  const [sel, setSel] = useState(null);
  const { id } = useAppWindow() || {};
  const win = useStore(windows, (ws) => ws.find((w) => w.id === id));
  const geo = win ? wm.frame(win) : null;
  const W = geo ? Math.max(240, Math.round(geo.w - 236 - 76)) : 0, H = geo ? Math.max(160, Math.round(geo.h - 230)) : 0;
  const rects = useMemo(() => {
    const kids = data.children.filter((c) => c.allocated > 0);
    const items = kids.slice(0, 26).map((c) => ({ ...c, v: c.allocated }));
    const rest = kids.slice(26).reduce((a, c) => a + c.allocated, 0);
    if (rest > 0) items.push({ id: -2, name: tr('cleaner.map.others'), v: rest, allocated: rest, logical: rest });
    if (data.ownAllocated > 0) items.push({ id: -1, name: tr('cleaner.map.looseFiles'), v: data.ownAllocated, allocated: data.ownAllocated, logical: data.ownLogical });
    items.sort((a, b) => b.v - a.v);
    return squarify(items, 0, 0, W, H);
  }, [data, W, H]);
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s }}>
      <View style={{ width: W, height: H, borderRadius: 16, overflow: 'hidden', direction: 'ltr' }}>
        {rects.map((r, i) => (
          <Enter key={r.item.id} delay={Math.min(i * 40, 360)} x={0} y={14} ms={500} style={{ position: 'absolute', left: r.x, top: r.y, width: r.w, height: r.h, padding: 1 }}>
            <View onClick={() => setSel(r.item)} style={{ flex: 1, borderRadius: 12, backgroundColor: COLOURS[i % COLOURS.length], padding: r.w > 70 && r.h > 34 ? 10 : 0, overflow: 'hidden',
              opacity: sel && sel.id !== r.item.id ? 0.7 : 1, border: sel && sel.id === r.item.id ? '2px solid #ffffff' : 'none' }}>
              <Bands item={r.item} h={r.h} />
              {r.w > 70 && r.h > 34 ? <Label size={typeScale.callout} weight={600} lines={1} style={{ color: '#ffffff' }}>{r.item.name}</Label> : null}
              {r.w > 70 && r.h > 50 ? <Label size={typeScale.caption} lines={1} style={{ color: '#ffffff' }}>{formatBytes(r.item.allocated)}</Label> : null}
            </View>
          </Enter>
        ))}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, height: 28 }}>
        {sel ? (
          <>
            <View style={{ flex: 1, minWidth: 0 }}><Label lines={1}>{tr('cleaner.map.selected', { name: sel.name, logical: formatBytes(sel.logical), allocated: formatBytes(sel.allocated) })}</Label></View>
            {sel.id >= 0 && sel.hasKids ? <GlassButton small label={tr('cleaner.map.open')} onClick={() => { const i = sel.id; setSel(null); onOpen(i); }} /> : null}
          </>
        ) : rects.some((r) => r.item.safe > 0 || r.item.review > 0) ? <Legend /> : <Label dim size={typeScale.callout}>{tr('cleaner.map.hint')}</Label>}
      </View>
    </View>
  );
}

// ---- bubbles: the largest folders as glass spheres, 14 at most, the rest together ----

// what a folder is, by its name: the icon and the colour of its bubble. In the home folder its own folders count too,
// under whatever name the user's language gave them.
const NAMED = { __proto__: null, '.cache': 'cache', '.config': 'config', '.local': 'data', '.var': 'data', Downloads: 'downloads', Trash: 'trash', '.Trash': 'trash',
  node_modules: 'code', '.git': 'code', '.npm': 'code', '.cargo': 'code', '.rustup': 'code', '.gradle': 'code', '.m2': 'code', go: 'code', src: 'code', Projects: 'code', projects: 'code' };
const HUE = { documents: 'blue', pictures: 'pink', music: 'violet', videos: 'rose', downloads: 'teal', desktop: 'indigo', code: 'orange', cache: 'rose', config: 'indigo', data: 'teal', trash: 'orange', others: 'indigo' };
const CYCLE = ['sky', 'violet', 'teal', 'blue', 'orange', 'pink', 'indigo', 'rose'];

export function folderKinds() {
  const u = userDirs(), kinds = Object.create(null);     // a folder may be called "constructor"
  for (const k of ['documents', 'pictures', 'music', 'videos', 'desktop']) if (u[k]) kinds[u[k].split('/').pop()] = k;
  return kinds;
}
export const folderKind = (c, kinds) => (c.id === -2 ? 'others' : (kinds && kinds[c.name]) || NAMED[c.name] || 'folder');

// A small circle with a check: the bubble itself opens, this only selects.
function BubbleCheck({ on, r, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ position: 'absolute', top: r * 0.293 - 11, right: r * 0.293 - 11, width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
      backgroundColor: on ? p.accent : p.glass, border: `1px solid ${on ? p.accent : p.line}` }}>
      {on ? <Label size={typeScale.caption} weight={700} style={{ color: p.btnText }}>✓</Label> : null}
    </View>
  );
}

export function pack(items, W, H) {
  const total = items.reduce((a, b) => a + b.v, 0) || 1;
  for (let k = 0.72; k > 0.1; k *= 0.93) {
    const placed = [];
    let ok = true;
    for (const it of items) {
      const r = Math.max(36, Math.sqrt(it.v / total) * Math.min(W, H) * k);
      let done = false;
      for (let t = 0; t < 900 && !done; t++) {
        const a = t * 0.42, d = t * 1.6, x = W / 2 + Math.cos(a) * d * (W / H), y = H / 2 + Math.sin(a) * d;
        if (x - r < 0 || y - r < 0 || x + r > W || y + r > H) continue;
        if (placed.every((c) => Math.hypot(c.x - x, c.y - y) >= c.r + r + 6)) { placed.push({ item: it, x, y, r }); done = true; }
      }
      if (!done) { ok = false; break; }
    }
    if (ok) return placed;
  }
  return [];
}

function Bubbles({ data, W, H, onOpen, selected, onSelect, kinds }) {
  const p = usePal();
  const circles = useMemo(() => {
    const kids = data.children.filter((c) => c.allocated > 0);
    for (let n = 13; n > 0; n -= 2) {         // fewer bubbles when they don't fit a small window, never none
      const items = kids.slice(0, n).map((c) => ({ ...c, v: c.allocated }));
      const rest = kids.slice(n).reduce((a, c) => a + c.allocated, 0) + Math.max(0, data.ownAllocated);
      if (rest > 0) items.push({ id: -2, name: tr('cleaner.map.others'), v: rest, allocated: rest, logical: rest });
      const placed = pack(items, W, H);
      if (placed.length) return placed;
    }
    return [];
  }, [data, W, H]);
  const whole = data.node.allocated || 1;
  return (
    <View style={{ width: W, height: H, direction: 'ltr' }}>
      {circles.map((c, i) => {
        const kind = folderKind(c.item, kinds), on = selected && selected.id === c.item.id, share = Math.round((c.item.allocated / whole) * 100);
        return (
          <Enter key={c.item.id} delay={Math.min(i * 40, 360)} x={0} y={0} scale={0.7} ms={500} style={{ position: 'absolute', left: c.x - c.r, top: c.y - c.r, width: c.r * 2, height: c.r * 2 }}>
            <View onClick={() => { if (c.item.hasKids) onOpen(c.item.id); else onSelect(c.item); }} style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 8, gap: 1 }}>
              <Image src={`asset:cleaner/bubble-${HUE[kind] || CYCLE[i % CYCLE.length]}.png`} style={{ position: 'absolute', left: 0, top: 0, width: c.r * 2, height: c.r * 2 }} />
              {on ? <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, borderRadius: c.r, border: `2px solid ${p.accent}` }} /> : null}
              {c.r > 46 ? <Image src={`asset:cleaner/folder-${kind}.png`} style={{ width: c.r > 90 ? 40 : c.r > 62 ? 30 : 22, height: c.r > 90 ? 40 : c.r > 62 ? 30 : 22, marginBottom: 2 }} /> : null}
              <Label size={c.r > 70 ? typeScale.body : typeScale.caption} weight={600} lines={1} align="center">{c.item.name}</Label>
              <Label size={c.r > 70 ? typeScale.callout : typeScale.caption} dim align="center">{formatBytes(c.item.allocated)}</Label>
              {c.r > 80 ? <Label size={typeScale.caption} dim align="center">{tr('cleaner.map.share', { pct: formatNumber(share) })}</Label> : null}
              {c.item.id >= 0 ? <BubbleCheck on={on} r={c.r} onClick={(e) => { e.stopPropagation(); onSelect(c.item); }} /> : null}
            </View>
          </Enter>
        );
      })}
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
        <Glass key={v.id} lift onClick={() => onChange(v.id)} style={{ padding: spacing.s, gap: 4, opacity: value === v.id ? 1 : 0.65 }}>
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
    <Hero art="storage" title={tr('cleaner.hero.storage.title')} line={tr('cleaner.hero.storage.line')} features={[['storage', tr('cleaner.hero.storage.f1')], ['settings', tr('cleaner.hero.storage.f2')]]}>
      <VolumeChips volumes={volumes} value={volume} onChange={setVolume} />
    </Hero>
  );
}

export function StorageView({ scan, volume, onReview }) {
  const [node, setNode] = useState(0);
  const [back, setBack] = useState([]);
  const [ahead, setAhead] = useState([]);
  const [view, setView] = useState('bubbles');
  const [sel, setSel] = useState(null);
  const { id } = useAppWindow() || {};
  const win = useStore(windows, (ws) => ws.find((w) => w.id === id));
  const geo = win ? wm.frame(win) : null;
  const W = geo ? Math.max(260, Math.round(geo.w - 236 - 76 - 228)) : 400, H = geo ? Math.max(200, Math.round(geo.h - 232)) : 300;
  const data = useMemo(() => cleaner.tree(scan.id, node, 500), [scan.id, node]);
  const kinds = useMemo(() => (data && !data.trail.length && volume && volume.id === 'home' ? folderKinds() : null), [data, volume]);
  const go = (n) => { setBack([...back, node]); setAhead([]); setNode(n); setSel(null); };
  const undo = () => { setAhead([node, ...ahead]); setNode(back[back.length - 1]); setBack(back.slice(0, -1)); setSel(null); };
  const redo = () => { setBack([...back, node]); setNode(ahead[0]); setAhead(ahead.slice(1)); setSel(null); };
  const top = useMemo(() => cleaner.tree(scan.id, 0, 1), [scan.id]);
  if (!data) return <Label dim>{tr('cleaner.map.empty')}</Label>;
  // what isn't under this scan: the disk's used space past what was walked
  const whole = top ? top.node : data.node;
  const summary = volume && volume.total > 0 ? tr('cleaner.map.summary', { free: formatBytes(volume.free), safe: formatBytes(whole.safe || 0), review: formatBytes(whole.review || 0), other: formatBytes(Math.max(0, volume.total - volume.free - whole.allocated)) }) : '';
  const rel = (name) => [...data.trail.map((t) => t.name), name].join('/');
  const real = !!sel && sel.id >= 0;           // "Others" is no folder: it can be looked at, not opened or reviewed
  return (
    <View style={{ flex: 1, minHeight: 0, gap: spacing.s, paddingBottom: spacing.m }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
        <GlassButton small label="‹" disabled={!back.length} onClick={undo} />
        <GlassButton small label="›" disabled={!ahead.length} onClick={redo} />
        <View style={{ flex: 1, minWidth: 0, paddingHorizontal: spacing.s }}>
          <Label size={20} weight={300} lines={1}>{data.trail.length ? data.trail[data.trail.length - 1].name : tr('cleaner.section.storage')}</Label>
        </View>
        {[['bubbles', 'cleaner.map.viewBubbles'], ['map', 'cleaner.map.viewMap'], ['tree', 'cleaner.map.viewTree'], ...(scan.savedAt ? [] : [['types', 'cleaner.map.viewTypes']])].map(([k, key]) => <Pill key={k} label={tr(key)} on={view === k} onClick={() => setView(k)} />)}
      </View>
      <Label dim size={typeScale.callout} lines={1}>{scan.savedAt ? tr('cleaner.map.savedAt', { when: formatDate(new Date(scan.savedAt * 1000), 'short') }) : summary}</Label>
      {!scan.complete ? <Label size={typeScale.callout}>{tr(scan.limit ? `cleaner.incomplete.${scan.limit}` : 'cleaner.incomplete')}</Label> : null}
      <View style={{ flex: 1, minHeight: 0, flexDirection: 'row', gap: spacing.m }}>
        {view === 'bubbles' ? (
          <Glass style={{ width: 216, padding: spacing.m, gap: spacing.s }}>
            <Label weight={600} lines={1}>{volume ? (volume.id === 'home' ? tr('cleaner.map.home') : volume.id === 'root' ? tr('cleaner.map.root') : volume.path) : data.base}</Label>
            {volume && volume.total > 0 ? <Bar value={1 - volume.free / volume.total} /> : null}
            <Label dim size={typeScale.caption}>{tr('cleaner.map.totals', { logical: formatBytes(data.node.logical), allocated: formatBytes(data.node.allocated), files: formatNumber(data.node.files) })}</Label>
            {data.children.slice(0, 12).map((c) => (
              <View key={c.id} onClick={c.hasKids ? () => go(c.id) : undefined} style={{ flexDirection: 'row', gap: spacing.xs, height: 24, alignItems: 'center' }}>
                <Image src={`asset:cleaner/folder-${folderKind(c, kinds)}.png`} style={{ width: 18, height: 18, flexShrink: 0 }} />
                <View style={{ flex: 1, minWidth: 0 }}><Label size={typeScale.callout} lines={1}>{c.name}</Label></View>
                <Label dim size={typeScale.caption}>{formatBytes(c.allocated)}</Label>
              </View>
            ))}
          </Glass>
        ) : null}
        {view === 'bubbles' ? <Bubbles data={data} W={W} H={H} kinds={kinds} onOpen={go} selected={sel} onSelect={(c) => setSel(sel && sel.id === c.id ? null : c)} />
          : view === 'map' ? <Treemap data={data} onOpen={go} /> : view === 'tree' ? <View style={{ flex: 1 }}><Folders scanId={scan.id} data={data} onOpen={go} /></View> : <View style={{ flex: 1 }}><Types scan={scan} /></View>}
      </View>
      {view === 'bubbles' ? (
        <Glass style={{ height: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: spacing.m }}>
          <View style={{ flex: 1, minWidth: 0 }}><Label size={typeScale.callout} lines={1}>{sel ? tr('cleaner.map.selectedShort', { name: sel.name, size: formatBytes(sel.allocated) }) : tr('cleaner.map.noneSelected')}</Label></View>
          <GlassButton small label={tr('cleaner.map.review')} disabled={!real || !!scan.savedAt} onClick={() => onReview(rel(sel.name))} />
          <GlassButton small label={tr('cleaner.show')} disabled={!real} onClick={() => revealPath(`${data.base === '/' ? '' : data.base}/${rel(sel.name)}/.`)} />
        </Glass>
      ) : null}
    </View>
  );
}
