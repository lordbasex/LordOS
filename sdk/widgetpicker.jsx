// Add widget, for every theme: the desktop widget catalog (sdk/deskwidgets.jsx) in a panel centred on a display, opened
// from a theme's "Add widget" (Workspaces' empty state, the wallpaper's right-click menu in each theme). A search field on top (name and description, in the
// person's language), the categories with their counts on the side, and the cards in a scrolling list, by category.
// Each card has a button per size (small 1×1, medium 2×1, large 2×2, wide 4×1, extra large 4×2, drawn as its block of
// cells); a click puts the widget on the first free block of that display's grid, in the activity in view. The arrow
// keys move between cards and Enter adds the highlighted one at its first size; Escape or a click outside closes it.
// A theme can give its own panel (Panel: a component taking ref, style and children; Workspaces passes its glass
// Surface); otherwise a plain one in the theme's colours. Its tokens are the theme's (useTheme), chipBg falling back to
// the control colour.
// A kind with a Chooser (registerWidgetKind, sdk/deskwidgets.jsx; the Notes app's sticky note: a new note or one of
// the notes already made) shows it in the same panel after its card is clicked, under a ‹ Back row with the kind's
// name: <Chooser t x y display area done /> where x, y is where the widget goes and done() closes the catalog.
import { forwardRef, useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { Layer } from './layers.js';
import { ScrollView } from './components.jsx';
import { SearchField } from './controls.jsx';
import { useStore, createStore } from './store.js';
import { useTheme } from './theme.js';
import { workAreaOf } from './windows.js';
import { widgetKinds, widgetCatalog, addDeskWidget } from './deskwidgets.jsx';
import { direction, t as tr } from './i18n.js';

const PlainPanel = forwardRef(function PlainPanel({ t, radius = 22, style, children }, ref) {
  return (
    <View ref={ref} style={{ borderRadius: radius, backgroundColor: t.popupBg || t.windowBg, border: `1px solid ${t.separator}`,
      boxShadow: '0 18px 40px rgba(0,0,0,0.35)', ...style }}>{children}</View>
  );
});

// { display, x?, y? } while the catalog is open
export const widgetPicker = createStore(null);
let cascade = 0;

const abs = (s) => ({ position: 'absolute', ...s });
// a small drawing of each kind (no emoji, no images)
function KindGlyph({ type, t }) {
  const a = t.accent, dim = t.textDim;
  if (type === 'video') return (
    <View style={{ width: 34, height: 34, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ width: 34, height: 24, borderRadius: 6, backgroundColor: '#1d1d22', border: `2px solid ${dim}`, alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: 0, height: 0, marginLeft: 3, borderTopWidth: 6, borderBottomWidth: 6, borderLeftWidth: 9, borderTopColor: 'transparent', borderBottomColor: 'transparent', borderLeftColor: '#E5484D' }} />
      </View>
    </View>
  );
  if (type === 'timer') return (
    <View style={{ width: 34, height: 34, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 28, height: 28, borderRadius: 14, border: `3px solid ${a}` }} />
      <View style={abs({ left: 15.5, top: 9, width: 3, height: 9, borderRadius: 1.5, backgroundColor: t.text })} />
      <View style={abs({ left: 14, top: 0, width: 6, height: 4, borderRadius: 1, backgroundColor: t.text })} />
    </View>
  );
  if (type === 'recent') return (
    <View style={{ width: 34, height: 34, gap: 4, justifyContent: 'center' }}>
      {[0, 1, 2].map((i) => <View key={i} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, direction: 'ltr' }}><View style={{ width: 9, height: 7, borderRadius: 2, backgroundColor: i ? dim : a }} /><View style={{ flex: 1, height: 3, borderRadius: 2, backgroundColor: dim }} /></View>)}
    </View>
  );
  if (type === 'updates') return (
    <View style={{ width: 34, height: 34, borderRadius: 17, backgroundColor: '#22A06B', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 18, fontWeight: 700, color: '#ffffff' }}>↑</Text>
    </View>
  );
  if (type === 'launcher') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', flexWrap: 'wrap', gap: 4, direction: 'ltr' }}>
      {['#F97316', a, '#22A06B', '#E5484D'].map((c) => <View key={c} style={{ width: 15, height: 15, borderRadius: 4, backgroundColor: c }} />)}
    </View>
  );
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
  if (type === 'devices') return (
    <View style={{ width: 34, height: 34, flexDirection: 'row', flexWrap: 'wrap', gap: 4, direction: 'ltr' }}>
      {['#3dd68c', '#3dd68c', '#f5b43c', '#3dd68c'].map((c, i) => <View key={i} style={{ width: 15, height: 15, borderRadius: 8, border: `2.5px solid ${c}` }} />)}
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

// the side list's categories, in order; a kind not listed here (a package's) goes to "other"
export const CATEGORIES = ['time', 'system', 'devices', 'network', 'productivity', 'media', 'other'];
const CATEGORY = {
  clock: 'time', calendar: 'time', weather: 'time', timer: 'time',
  system: 'system', temps: 'system', disks: 'system', updates: 'system',
  battery: 'devices', devices: 'devices',
  network: 'network', vpn: 'network',
  claude: 'productivity', reminders: 'productivity', note: 'productivity', clipboard: 'productivity', recent: 'productivity', launcher: 'productivity',
  video: 'media', nowplaying: 'media', readaloud: 'media', soundwave: 'media',
};
export const categoryOf = (type) => CATEGORY[type] || 'other';
// lower case without accents, so "calendario" finds "Calendário" and "reloj" finds "Reloj"
export const fold = (s) => String(s || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
export function matches(kind, query, text = tr) {
  const q = fold(query).trim();
  if (!q) return true;
  const hay = fold(`${text(kind.nameKey)} ${text(kind.descKey)}`);
  return q.split(/\s+/).every((w) => hay.includes(w));
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
function Card({ t, kind, onPick, on }) {
  const [hover, setHover] = useState(false);
  const lit = hover || on;
  const sizes = kind.sizes || null;
  return (
    <View onClick={sizes ? undefined : () => onPick(null)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 176, minHeight: 132, padding: 12, gap: 6, borderRadius: 16, backgroundColor: lit ? t.hover : t.chipBg,
        border: `1px solid ${lit ? t.accent : t.separator}` }}>
      <KindGlyph type={kind.type} t={t} />
      <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(kind.nameKey)}</Text>
      <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(kind.descKey)}</Text>
      {sizes ? <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4, marginTop: 'auto' }}>{sizes.map((sz) => <SizeChip key={sz} t={t} size={sz} onPick={() => onPick(sz)} />)}</View> : null}
    </View>
  );
}

function CategoryRow({ t, label, count, on, onPick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onPick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', height: 34, paddingHorizontal: 12, borderRadius: 10,
        backgroundColor: on ? t.hover : hover ? t.chipBg : 'transparent' }}>
      <Text style={{ flex: 1, fontSize: 13, fontWeight: on ? 600 : 500, color: t.text, textAlign: 'start' }}>{label}</Text>
      <Text style={{ fontSize: 12, color: t.textDim }}>{String(count)}</Text>
    </View>
  );
}

function BackButton({ t, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 4, height: 32, paddingHorizontal: 10, borderRadius: 10, backgroundColor: hover ? t.hover : t.chipBg }}>
      {/* ‹ is a mirrored character: right to left it is drawn as › by itself */}
      <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>‹</Text>
      <Text style={{ fontSize: 13, fontWeight: 600, color: t.text }}>{tr('shell.widgets.back')}</Text>
    </View>
  );
}

export function WidgetPicker({ t: given, Panel = PlainPanel }) {
  const th = useTheme() || {};
  const base = given || th;
  const t = { ...base, chipBg: base.chipBg || base.control || base.hover };
  const p = useStore(widgetPicker);
  useStore(widgetCatalog);
  const ref = useRef(null);
  const [full, setFull] = useState(false);
  const [query, setQuery] = useState('');
  const [cat, setCat] = useState('all');
  const [hi, setHi] = useState(-1);
  const [choosing, setChoosing] = useState(null);       // { kind, x, y } while a kind's Chooser is shown
  useEffect(() => { setFull(false); setQuery(''); setCat('all'); setHi(-1); setChoosing(null); }, [p]);
  useEffect(() => {
    if (!p) return undefined;
    const offDown = addListener('pointerdown', (e) => { for (let n = e.target; n; n = n.parent) if (n === ref.current) return; widgetPicker.set(null); });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') widgetPicker.set(null); });
    return () => { offDown(); offKey(); };
  }, [p]);
  const kinds = p ? widgetKinds() : [];
  const counts = useMemo(() => {
    const c = { all: kinds.length };
    for (const k of kinds) c[categoryOf(k.type)] = (c[categoryOf(k.type)] || 0) + 1;
    return c;
  }, [kinds.length, p]);
  if (!p) return null;
  const area = workAreaOf(p.display);
  // the cards that show: the search (over every category) or the chosen category, grouped in the side list's order
  const shown = kinds.filter((k) => (query ? matches(k, query) : cat === 'all' || categoryOf(k.type) === cat));
  const groups = CATEGORIES.map((c) => ({ c, list: shown.filter((k) => categoryOf(k.type) === c) })).filter((g) => g.list.length);
  const flat = groups.flatMap((g) => g.list);
  // centred on the display, as large as it fits: up to four columns of cards, the list scrolls inside
  const CARD = 178, GAPX = 10, SIDE = 188, PAD = 18;
  const room = Math.max(CARD, area.w - 48 - SIDE - PAD * 3);
  const cols = Math.max(1, Math.min(4, Math.floor((room + GAPX) / (CARD + GAPX))));
  const W = SIDE + PAD * 3 + cols * CARD + (cols - 1) * GAPX + 10;
  const H = Math.min(Math.max(320, area.h - 64), 720);
  const pick = (k, size) => {
    if (!k.sizes) {
      const x = p.x ?? area.x + area.w - 340 - (cascade % 5) * 24, y = p.y ?? area.y + 40 + (cascade % 5) * 24;
      if (typeof k.Chooser === 'function') { setChoosing({ kind: k, x, y }); return; }
      widgetPicker.set(null);
      k.add?.(x, y);
      cascade++;
      return;
    }
    if (addDeskWidget(k.type, size, p.display)) widgetPicker.set(null);
    else setFull(true);
  };
  const onKey = (e) => {
    const n = flat.length;
    if (!n) return;
    const rtl = direction() === 'rtl';
    const step = { ArrowRight: rtl ? -1 : 1, ArrowLeft: rtl ? 1 : -1, ArrowDown: cols, ArrowUp: -cols }[e.key];
    if (step) { setHi((h) => (h < 0 ? 0 : Math.max(0, Math.min(n - 1, h + step)))); e.preventDefault?.(); return; }
    if (e.key === 'Enter') { const k = flat[hi < 0 ? 0 : Math.min(hi, n - 1)]; if (k) pick(k, k.sizes ? k.sizes[0] : null); }
  };
  const cats = ['all', ...CATEGORIES.filter((c) => counts[c])];
  if (choosing) {
    const C = choosing.kind.Chooser;
    const done = () => { widgetPicker.set(null); cascade++; };
    return (
      <Layer name="overlay">
        <Panel ref={ref} t={t} radius={22} style={{ position: 'absolute', left: Math.round(area.x + (area.w - W) / 2), top: Math.round(area.y + (area.h - H) / 2),
          width: W, height: H, overflow: 'hidden' }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: PAD + 4, paddingTop: PAD, paddingBottom: 14, borderBottom: `1px solid ${t.separator}` }}>
            <BackButton t={t} onClick={() => setChoosing(null)} />
            <Text style={{ flex: 1, fontSize: 17, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr(choosing.kind.nameKey)}</Text>
          </View>
          <View style={{ flex: 1, minHeight: 0 }}>
            <C t={t} x={choosing.x} y={choosing.y} display={p.display} area={area} done={done} />
          </View>
        </Panel>
      </Layer>
    );
  }
  return (
    <Layer name="overlay">
      <Panel ref={ref} t={t} radius={22} style={{ position: 'absolute', left: Math.round(area.x + (area.w - W) / 2), top: Math.round(area.y + (area.h - H) / 2),
        width: W, height: H, overflow: 'hidden' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: PAD + 4, paddingTop: PAD, paddingBottom: 14, borderBottom: `1px solid ${t.separator}` }}>
          <Text style={{ fontSize: 17, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr('shell.widgets.title')}</Text>
          <SearchField value={query} onChange={(v) => { setQuery(v); setHi(-1); }} onKeyDown={onKey} autoFocus placeholder={tr('shell.widgets.search')}
            onCancel={() => { if (!query) widgetPicker.set(null); }} style={{ flex: 1, height: 36 }} />
        </View>
        <View style={{ flex: 1, flexDirection: 'row', minHeight: 0 }}>
          <View style={{ width: SIDE, padding: 10, gap: 2, borderEndWidth: 1, borderEndColor: t.separator }}>
            {cats.map((c) => <CategoryRow key={c} t={t} label={tr(`shell.widgets.cat.${c}`)} count={counts[c] || 0} on={!query && cat === c}
              onPick={() => { setCat(c); setQuery(''); setHi(-1); }} />)}
          </View>
          <ScrollView style={{ flex: 1 }} contentStyle={{ padding: PAD, paddingEnd: PAD - 4, gap: 10 }} resetKey={`${cat}|${query}`}>
            {groups.length ? groups.map((g) => (
              <View key={g.c} style={{ gap: 10, marginBottom: 8 }}>
                <Text style={{ fontSize: 11, fontWeight: 700, color: t.textDim, textAlign: 'start', paddingHorizontal: 4 }}>{tr(`shell.widgets.cat.${g.c}`).toLocaleUpperCase()}</Text>
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: GAPX }}>
                  {g.list.map((k) => <Card key={k.type} t={t} kind={k} on={flat[hi] === k} onPick={(size) => pick(k, size)} />)}
                </View>
              </View>
            )) : <Text style={{ fontSize: 13, color: t.textDim, textAlign: 'center', paddingTop: 40 }}>{tr('shell.widgets.noMatch')}</Text>}
          </ScrollView>
        </View>
        <View style={{ paddingHorizontal: PAD + 4, paddingVertical: 10, borderTop: `1px solid ${t.separator}` }}>
          <Text style={{ fontSize: 12, color: full ? '#E5484D' : t.textDim, textAlign: 'start' }}>{tr(full ? 'shell.widgets.noRoom' : 'shell.widgets.subtitle')}</Text>
        </View>
      </Panel>
    </Layer>
  );
}
