// A drawer: a panel in the frame's colour that slides out of an edge of the frame, with small concave fillets where
// it meets the frame once it is fully out, so frame and panel read as one shape. The slide changes only translateX/Y
// and opacity with lord.setStyle from animation frames (no React render per frame); React renders once when it starts
// and once when it ends. With reduce motion it appears and disappears at once.
import { View } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { F, hiddenOffset } from '../model.js';
import { setStyle, tween } from '../cae.js';
import { Fillet } from './Frame.jsx';

const RAD = 26;
const MS = 300;

function radiusFor(edge, rtl) {
  if (edge === 'top') return { borderBottomLeftRadius: RAD, borderBottomRightRadius: RAD };
  if (edge === 'bottom') return { borderTopLeftRadius: RAD, borderTopRightRadius: RAD };
  return rtl ? { borderTopRightRadius: RAD, borderBottomRightRadius: RAD } : { borderTopLeftRadius: RAD, borderBottomLeftRadius: RAD };
}

function filletsFor(r, rtl) {
  const { x, y, w, h, edge } = r;
  if (edge === 'top') return [[x - F, y, 'tr'], [x + w, y, 'tl']];
  if (edge === 'bottom') return [[x - F, y + h - F, 'br'], [x + w, y + h - F, 'bl']];
  return rtl ? [[x, y - F, 'bl'], [x, y + h, 'tl']] : [[x + w - F, y - F, 'br'], [x + w - F, y + h, 'tr']];
}

// rect: from model.js drawerRect; open: wanted state; reduce: reduce motion; onClosed: called once it is gone.
export function Drawer({ color, rect, rtl, open, reduce, onClosed, children }) {
  const [mounted, setMounted] = useState(open);
  const [settled, setSettled] = useState(0);     // 0 hidden, 1 out: what React draws between slides
  const box = useRef(null);
  const p = useRef(0);
  const live = useRef({});
  live.current = { rect, rtl, onClosed };

  useEffect(() => {
    if (open && !mounted) { setMounted(true); return undefined; }
    if (!mounted) return undefined;
    const to = open ? 1 : 0;
    if (p.current === to) { if (!open) { setMounted(false); live.current.onClosed?.(); } return undefined; }
    if (!open) setSettled(-1);                   // the fillets go before it slides back
    const from = p.current;
    const off = hiddenOffset(live.current.rect, live.current.rtl);
    return tween(reduce ? 0 : MS * Math.abs(to - from), (k) => {
      const v = from + (to - from) * k;
      p.current = v;
      const n = box.current;
      setStyle(n, 'translateX', Math.round(off.x * (1 - v)));
      setStyle(n, 'translateY', Math.round(off.y * (1 - v)));
      setStyle(n, 'opacity', Math.min(1, v * 1.4));
    }, () => {
      if (open) setSettled(1);
      else { setSettled(0); setMounted(false); live.current.onClosed?.(); }
    });
  }, [open, mounted]);

  if (!mounted || !rect) return null;
  const off = hiddenOffset(rect, rtl);
  const out = settled !== 0;                     // out, or sliding back (the tween moves it from there)
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, width: 0, height: 0, pointerEvents: 'box-none' }}>
      {settled === 1 ? filletsFor(rect, rtl).map(([x, y, c]) => <Fillet key={c} x={x} y={y} r={F} corner={c} color={color} />) : null}
      <View ref={box} onMouseDown={(e) => e?.stopPropagation?.()}
        style={{ position: 'absolute', left: rect.x, top: rect.y, width: rect.w, height: rect.h, backgroundColor: color, overflow: 'hidden',
          translateX: out ? 0 : off.x, translateY: out ? 0 : off.y, opacity: out ? 1 : 0, ...radiusFor(rect.edge, rtl) }}>
        {children}
      </View>
    </View>
  );
}
