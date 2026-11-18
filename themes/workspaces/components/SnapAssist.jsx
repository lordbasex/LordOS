// The chooser of the free layout's magnet (../magnet.js): over the empty half of a display, the windows that can take
// it, as a list of icons and titles. A click snaps the window there; Escape, a click outside the list, or the first
// window leaving its half closes it.
import { View, Text, addListener, useStore, windows, windowTitle, windowIcon, getApp, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { assist, choose } from '../magnet.js';
import { Surface } from './Surface.jsx';

const ROW_H = 40;
const MAX_ROWS = 8;

function Row({ t, w, onPick }) {
  const [hover, setHover] = useState(false);
  const Icon = windowIcon(w, getApp);
  return (
    <View onClick={onPick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: ROW_H, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, borderRadius: 10, backgroundColor: hover ? t.hover : 'transparent' }}>
      {Icon ? <Icon size={22} /> : <View style={{ width: 22, height: 22 }} />}
      <Text style={{ flex: 1, fontSize: 13, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{windowTitle(w)}</Text>
    </View>
  );
}

export function SnapAssist({ t }) {
  const a = useStore(assist);
  const all = useStore(windows);
  useEffect(() => {
    if (!a) return undefined;
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') assist.set(null); });
    const offDown = addListener('pointerdown', (e) => {
      const r = a.rect;
      if (e.x < r.x || e.x >= r.x + r.w || e.y < r.y || e.y >= r.y + r.h) assist.set(null);
    });
    return () => { offKey(); offDown(); };
  }, [a]);
  if (!a) return null;
  const list = a.ids.map((id) => all.find((w) => w.id === id)).filter(Boolean).slice(0, MAX_ROWS);
  if (!list.length) return null;
  const r = a.rect;
  const w = Math.min(360, r.w - 48);
  return (
    <View style={{ position: 'absolute', left: r.x + 6, top: r.y + 6, width: r.w - 12, height: r.h - 12, borderRadius: 14, alignItems: 'center', justifyContent: 'center',
      backgroundColor: t.mode === 'dark' ? 'rgba(124,92,255,0.10)' : 'rgba(91,63,224,0.08)', border: `2px dashed ${t.accent}` }}>
      <Surface t={t} radius={16} style={{ width: w, padding: 10, gap: 2 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.textDim, textAlign: 'start', marginHorizontal: 10, marginBottom: 6 }}>{tr('shell.ws.snap.pick')}</Text>
        {list.map((x) => <Row key={x.id} t={t} w={x} onPick={() => choose(x.id)} />)}
      </Surface>
    </View>
  );
}
