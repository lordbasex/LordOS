// Add widget: the desktop widget catalog (sdk/deskwidgets.jsx) as cards over a display, opened from the empty state's
// "Add widget", a free slot, or the wallpaper's right-click menu. A click puts the widget where it was asked for (the
// right-click point), else near the top of that display's work area; Escape or a click outside closes it.
import { View, Text, Layer, addListener, useStore, createStore, widgetKinds, widgetCatalog, workAreaOf, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Surface } from './Surface.jsx';

// { display, x?, y? } while the catalog is open
export const widgetPicker = createStore(null);
let cascade = 0;

const abs = (s) => ({ position: 'absolute', ...s });
// a small drawing of each kind (no emoji, no images)
function KindGlyph({ type, t }) {
  const a = t.accent, dim = t.textDim;
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

function Card({ t, kind, onPick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onPick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 176, height: 118, padding: 12, gap: 8, borderRadius: 14, backgroundColor: hover ? t.hover : t.chipBg,
        border: `1px solid ${hover ? t.accent : t.separator}` }}>
      <KindGlyph type={kind.type} t={t} />
      <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(kind.nameKey)}</Text>
      <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(kind.descKey)}</Text>
    </View>
  );
}

export function WidgetPicker({ t }) {
  const p = useStore(widgetPicker);
  useStore(widgetCatalog);
  const ref = useRef(null);
  useEffect(() => {
    if (!p) return undefined;
    const offDown = addListener('pointerdown', (e) => { for (let n = e.target; n; n = n.parent) if (n === ref.current) return; widgetPicker.set(null); });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') widgetPicker.set(null); });
    return () => { offDown(); offKey(); };
  }, [p]);
  if (!p) return null;
  const area = workAreaOf(p.display);
  const kinds = widgetKinds();
  const cols = Math.min(4, kinds.length);
  const W = cols * 176 + (cols - 1) * 10 + 36;
  const pick = (k) => {
    widgetPicker.set(null);
    const x = p.x ?? area.x + area.w - 340 - (cascade % 5) * 24;
    const y = p.y ?? area.y + 40 + (cascade % 5) * 24;
    cascade++;
    k.add(x, y);
  };
  return (
    <Layer name="overlay">
      <Surface ref={ref} t={t} radius={20} style={{ position: 'absolute', left: Math.round(area.x + (area.w - W) / 2), top: Math.round(area.y + Math.max(24, area.h * 0.16)),
        width: W, padding: 18, gap: 12 }}>
        <View style={{ gap: 2 }}>
          <Text style={{ fontSize: 16, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr('shell.widgets.title')}</Text>
          <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('shell.widgets.subtitle')}</Text>
        </View>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 10 }}>
          {kinds.map((k) => <Card key={k.type} t={t} kind={k} onPick={() => pick(k)} />)}
        </View>
      </Surface>
    </Layer>
  );
}
