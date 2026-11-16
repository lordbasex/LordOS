// Add widget: the desktop widget catalog (sdk/deskwidgets.jsx) as cards over a display, opened from the empty state's
// "Add widget" or the wallpaper's right-click menu. Each card has a button per size (small 1×1, medium 2×1, large 2×2,
// wide 4×1, extra large 4×2, drawn as its block of cells); a click puts the widget on the first free block of that display's grid,
// in the activity in view. Escape or a click outside closes it.
import { View, Text, Layer, addListener, useStore, createStore, widgetKinds, widgetCatalog, workAreaOf, addDeskWidget, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Surface } from './Surface.jsx';

// { display, x?, y? } while the catalog is open
export const widgetPicker = createStore(null);
let cascade = 0;

const abs = (s) => ({ position: 'absolute', ...s });
// a small drawing of each kind (no emoji, no images)
function KindGlyph({ type, t }) {
  const a = t.accent, dim = t.textDim;
  if (type === 'vpn') return (
    <View style={{ width: 34, height: 34, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ width: 34, height: 18, borderRadius: 9, backgroundColor: '#3fbf6e', justifyContent: 'center' }}>
        <View style={abs({ left: 18, top: 2, width: 14, height: 14, borderRadius: 7, backgroundColor: '#f3f6fb' })} />
      </View>
    </View>
  );
  if (type === 'weather') return (
    <View style={{ width: 34, height: 34, direction: 'ltr' }}>
      <View style={abs({ left: 14, top: 2, width: 18, height: 18, borderRadius: 9, backgroundColor: '#FFC531' })} />
      <View style={abs({ left: 2, top: 18, width: 28, height: 12, borderRadius: 6, backgroundColor: t.text })} />
      <View style={abs({ left: 8, top: 11, width: 14, height: 14, borderRadius: 7, backgroundColor: t.text })} />
    </View>
  );
  if (type === 'clock') return (
    <View style={{ width: 34, height: 34, borderRadius: 17, border: `2.5px solid ${a}` }}>
      <View style={abs({ left: 15, top: 6, width: 2.5, height: 12, backgroundColor: t.text, borderRadius: 2 })} />
      <View style={abs({ left: 15, top: 15, width: 10, height: 2.5, backgroundColor: a, borderRadius: 2 })} />
    </View>
  );
  if (type === 'system') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', alignItems: 'flex-end', gap: 4, direction: 'ltr' }}>
      {[14, 26, 18, 32].map((h, i) => <View key={i} style={{ width: 5.5, height: h, borderRadius: 2, backgroundColor: i === 3 ? a : dim }} />)}
    </View>
  );
  if (type === 'calendar') return (
    <View style={{ width: 34, height: 32, borderRadius: 6, border: `2px solid ${dim}`, overflow: 'hidden' }}>
      <View style={{ height: 8, backgroundColor: a }} />
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 3, padding: 3, direction: 'ltr' }}>
        {Array.from({ length: 8 }, (_, i) => <View key={i} style={{ width: 4, height: 4, borderRadius: 1, backgroundColor: i === 5 ? a : dim }} />)}
      </View>
    </View>
  );
  if (type === 'note') return <View style={{ width: 30, height: 30, borderRadius: 3, backgroundColor: '#F7D774', boxShadow: '0 3px 6px rgba(0,0,0,0.25)' }} />;
  if (type === 'nowplaying') return (
    <View style={{ width: 34, height: 34, borderRadius: 8, backgroundColor: a, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 0, height: 0, marginLeft: 4, borderTopWidth: 8, borderBottomWidth: 8, borderLeftWidth: 13, borderTopColor: 'transparent', borderBottomColor: 'transparent', borderLeftColor: '#ffffff' }} />
    </View>
  );
  if (type === 'readaloud') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', alignItems: 'center', gap: 3, direction: 'ltr' }}>
      {[10, 22, 30, 18, 26, 12].map((h, i) => <View key={i} style={{ width: 3, height: h, borderRadius: 2, backgroundColor: i % 2 ? a : t.text }} />)}
    </View>
  );
  if (type === 'network') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', alignItems: 'flex-end', gap: 3, direction: 'ltr' }}>
      {[8, 15, 23, 31].map((h, i) => <View key={i} style={{ width: 6, height: h, borderRadius: 2, backgroundColor: i < 3 ? a : dim }} />)}
    </View>
  );
  if (type === 'disks') return (
    <View style={{ width: 34, height: 34, gap: 4, justifyContent: 'center' }}>
      {[0.7, 0.4].map((f, i) => <View key={i} style={{ height: 9, borderRadius: 3, backgroundColor: dim, direction: 'ltr' }}><View style={{ width: `${f * 100}%`, height: 9, borderRadius: 3, backgroundColor: a }} /></View>)}
    </View>
  );
  if (type === 'temps') return (
    <View style={{ width: 34, height: 34, alignItems: 'center' }}>
      <View style={{ width: 10, height: 22, borderRadius: 5, border: `2px solid ${dim}`, overflow: 'hidden', justifyContent: 'flex-end' }}><View style={{ height: 11, backgroundColor: '#E5484D' }} /></View>
      <View style={{ width: 16, height: 16, borderRadius: 8, marginTop: -5, backgroundColor: '#E5484D' }} />
    </View>
  );
  if (type === 'battery') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', alignItems: 'center', direction: 'ltr' }}>
      <View style={{ width: 28, height: 16, borderRadius: 4, border: `2px solid ${dim}`, padding: 2 }}><View style={{ width: '70%', height: '100%', borderRadius: 2, backgroundColor: '#22A06B' }} /></View>
      <View style={{ width: 3, height: 7, borderRadius: 1, backgroundColor: dim }} />
    </View>
  );
  if (type === 'reminders') return (
    <View style={{ width: 34, height: 34, gap: 5, justifyContent: 'center' }}>
      {[0, 1, 2].map((i) => <View key={i} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, direction: 'ltr' }}><View style={{ width: 7, height: 7, borderRadius: 4, border: `1.5px solid ${a}` }} /><View style={{ flex: 1, height: 3, borderRadius: 2, backgroundColor: dim }} /></View>)}
    </View>
  );
  if (type === 'clipboard') return (
    <View style={{ width: 28, height: 34, borderRadius: 5, border: `2px solid ${dim}`, alignItems: 'center', paddingTop: 7, gap: 4 }}>
      <View style={abs({ left: 7, top: -4, width: 10, height: 6, borderRadius: 2, backgroundColor: a })} />
      {[16, 12, 14].map((w, i) => <View key={i} style={{ width: w, height: 3, borderRadius: 2, backgroundColor: dim }} />)}
    </View>
  );
  if (type === 'claude') return (
    <View style={{ width: 34, height: 34, alignItems: 'center', justifyContent: 'center' }}>
      {[0, 45, 90, 135].map((deg) => <View key={deg} style={abs({ left: 15, top: 3, width: 4, height: 28, borderRadius: 2, backgroundColor: '#D97757', transform: `rotate(${deg}deg)` })} />)}
    </View>
  );
  return <View style={{ width: 30, height: 30, borderRadius: 8, backgroundColor: dim }} />;
}

const SPANS = { s: [1, 1], m: [2, 1], l: [2, 2], w: [4, 1], xl: [4, 2] };
function SizeChip({ t, size, onPick }) {
  const [hover, setHover] = useState(false);
  const [c, r] = SPANS[size];
  return (
    <View onClick={onPick} title={tr(`shell.widgets.size.${size}`)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 30, paddingHorizontal: 7, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? t.accent : t.hover }}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', width: c * 7 + (c - 1) * 2, gap: 2, direction: 'ltr' }}>
        {Array.from({ length: c * r }, (_, i) => <View key={i} style={{ width: 7, height: 7, borderRadius: 2, backgroundColor: hover ? t.onAccent : t.textDim }} />)}
      </View>
    </View>
  );
}
function Card({ t, kind, onPick }) {
  const [hover, setHover] = useState(false);
  const sizes = kind.sizes || null;
  return (
    <View onClick={sizes ? undefined : () => onPick(null)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 176, minHeight: 132, padding: 12, gap: 6, borderRadius: 16, backgroundColor: hover ? t.hover : t.chipBg,
        border: `1px solid ${hover ? t.accent : t.separator}` }}>
      <KindGlyph type={kind.type} t={t} />
      <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(kind.nameKey)}</Text>
      <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(kind.descKey)}</Text>
      {sizes ? <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4, marginTop: 'auto' }}>{sizes.map((sz) => <SizeChip key={sz} t={t} size={sz} onPick={() => onPick(sz)} />)}</View> : null}
    </View>
  );
}

export function WidgetPicker({ t }) {
  const p = useStore(widgetPicker);
  useStore(widgetCatalog);
  const ref = useRef(null);
  const [full, setFull] = useState(false);
  useEffect(() => { setFull(false); }, [p]);
  useEffect(() => {
    if (!p) return undefined;
    const offDown = addListener('pointerdown', (e) => { for (let n = e.target; n; n = n.parent) if (n === ref.current) return; widgetPicker.set(null); });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') widgetPicker.set(null); });
    return () => { offDown(); offKey(); };
  }, [p]);
  if (!p) return null;
  const area = workAreaOf(p.display);
  const kinds = widgetKinds();
  // four columns (fewer on a narrow display); a card is 176 px plus its 1 px border on each side
  const CARD = 178, GAPX = 10;
  const cols = Math.max(1, Math.min(4, kinds.length, Math.floor((area.w - 48 - 36 + GAPX) / (CARD + GAPX))));
  const W = cols * CARD + (cols - 1) * GAPX + 36 + 2;
  const pick = (k, size) => {
    if (!k.sizes) {
      widgetPicker.set(null);
      k.add?.(p.x ?? area.x + area.w - 340 - (cascade % 5) * 24, p.y ?? area.y + 40 + (cascade % 5) * 24);
      cascade++;
      return;
    }
    if (addDeskWidget(k.type, size, p.display)) widgetPicker.set(null);
    else setFull(true);
  };
  return (
    <Layer name="overlay">
      <Surface ref={ref} t={t} radius={20} style={{ position: 'absolute', left: Math.round(area.x + (area.w - W) / 2), top: Math.round(area.y + Math.max(24, area.h * 0.16)),
        width: W, padding: 18, gap: 12 }}>
        <View style={{ gap: 2 }}>
          <Text style={{ fontSize: 16, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr('shell.widgets.title')}</Text>
          <Text style={{ fontSize: 12, color: full ? '#E5484D' : t.textDim, textAlign: 'start' }}>{tr(full ? 'shell.widgets.noRoom' : 'shell.widgets.subtitle')}</Text>
        </View>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 10 }}>
          {kinds.map((k) => <Card key={k.type} t={t} kind={k} onPick={(size) => pick(k, size)} />)}
        </View>
      </Surface>
    </Layer>
  );
}
