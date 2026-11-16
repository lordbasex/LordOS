// The activity's group frame on one display: a frame in the activity's colour behind its windows, sized to the work
// area (model/geometry.js), over a dimmed scrim (never a live blur: it covers the whole work area). Its header names the
// activity (and the display, with several) and holds the activity's pinned apps and its actions: Change layout, Add
// window, Save layout and Move to another activity (the window in front). In a split layout a divider sits in the gap
// between the tiles: dragging it shows a line (moved with lord.setStyle, no React render per pointer move) and the
// tiles change once, on release. With no window on the display, a short empty state offers the same two ways in.
import {
  View, Text, Layer, addListener, useStore, windows, wm, desktopList, desktopName, displayFor, launchApp, useLauncherApps, t as tr,
} from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';
import { ActivityIcon, PlusGlyph, SaveGlyph, MoveGlyph, LayoutMini, LAYOUT_CELLS } from '../glyphs.jsx';
import { LAYOUTS, splitterAt, ratioFrom, TILE_GAP } from '../model/layouts.js';
import { layoutOf, setLayout, saveLayout, tileable } from '../tiler.js';
import { Surface } from './Surface.jsx';
import { noteLaunch } from '../state.js';

const alpha = (hex, a) => `${hex}${Math.round(a * 255).toString(16).padStart(2, '0')}`;

function HeaderButton({ t, label, onClick, children, anchor, active, wide }) {
  const [hover, setHover] = useState(false);
  return (
    <View ref={anchor} onClick={onClick} title={wide ? undefined : label} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: wide ? 10 : 7, borderRadius: 9,
        backgroundColor: active ? t.hover : hover ? t.hover : 'transparent' }}>
      {children}
      {wide ? <Text style={{ fontSize: 12, fontWeight: 500, color: t.frameText, whiteSpace: 'nowrap' }}>{label}</Text> : null}
    </View>
  );
}

// the layout picker: a miniature and a name per layout
function LayoutPicker({ t, at, current, onPick, onClose }) {
  const ref = useRef(null);
  useEffect(() => addListener('pointerdown', (e) => { for (let n = e.target; n; n = n.parent) if (n === ref.current) return; onClose(); }), []);
  return (
    <Layer name="popover">
      <Surface ref={ref} t={t} radius={14} style={{ position: 'absolute', left: at.x, top: at.y, width: 300, padding: 10, gap: 8 }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, textAlign: 'start', marginHorizontal: 4 }}>{tr('shell.ws.layout.title')}</Text>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
          {LAYOUTS.map((l) => (
            <View key={l} onClick={() => { onPick(l); onClose(); }}
              style={{ width: 88, paddingVertical: 8, borderRadius: 10, alignItems: 'center', gap: 6, backgroundColor: l === current ? t.accent : t.hover }}>
              <LayoutMini cells={LAYOUT_CELLS[l]} w={44} h={28} color={l === current ? t.onAccent : t.text} fill={l === current ? t.onAccent : t.text} active />
              <Text style={{ fontSize: 11, color: l === current ? t.onAccent : t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 80 }}>{tr(`shell.ws.layout.${l}`)}</Text>
            </View>
          ))}
        </View>
      </Surface>
    </Layer>
  );
}

// The divider of a split layout: a target in the gap between the tiles; while dragged, a line in the drag layer.
function Splitter({ t, desk, dkey, layout, ratio, area, count }) {
  const x = splitterAt(layout, area, ratio, count);
  const line = useRef(null);
  const [drag, setDrag] = useState(false);
  const [hover, setHover] = useState(false);
  useEffect(() => {
    if (!drag) return undefined;
    let last = x;
    const offMove = addListener('pointermove', (e) => {
      last = Math.max(area.x + area.w * 0.25, Math.min(area.x + area.w * 0.75, e.x));
      if (line.current) try { lord.setStyle(line.current.id, 'left', last - 1); } catch (err) { /* gone */ }
    });
    const offUp = addListener('pointerup', () => { setDrag(false); setLayout(desk, dkey, null, ratioFrom(layout, area, last, count)); });
    return () => { offMove(); offUp(); };
  }, [drag]);
  if (x == null) return null;
  return (
    <>
      <View onMouseDown={(e) => { if (!e.button) { e.stopPropagation(); setDrag(true); } }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
        style={{ position: 'absolute', left: x - TILE_GAP / 2, top: area.y, width: TILE_GAP, height: area.h, cursor: 'ew-resize', alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: 4, height: 44, borderRadius: 2, backgroundColor: hover || drag ? t.accent : t.separator }} />
      </View>
      {drag ? (
        <Layer name="drag">
          <View ref={line} style={{ position: 'absolute', left: x - 1, top: area.y, width: 3, height: area.h, borderRadius: 2, backgroundColor: t.accent, pointerEvents: 'none' }} />
        </Layer>
      ) : null}
    </>
  );
}

// The frame itself: only a border in the accent (Crystal adds a brighter top edge), the same for every activity, so a
// switch never repaints this display-sized box; the activity's own colour is on its header chip and its pill. No fill:
// a scrim over the whole work area was the costly part of a switch on the CPU path (docs/WORKSPACES.md, "Performance").
const FrameBox = memo(function FrameBox({ t, f }) {
  return (
    <View style={{ position: 'absolute', left: f.x, top: f.y, width: f.w, height: f.h, borderRadius: 18, border: `1.5px solid ${alpha(t.accent, 0.45)}`, pointerEvents: 'none' }}>
      {t.mat.edge ? <View style={{ position: 'absolute', left: 24, right: 24, top: 0, height: 1, backgroundColor: alpha(t.accent, 0.85) }} /> : null}
    </View>
  );
});

// how many windows of this activity show on this display (a narrow selector: other windows' changes don't re-render)
const countOn = (desk, d) => (ws) => ws.filter((w) => w.desktop === desk && !w.minimized && !w.fullscreen && !w.pip && tileable({ ...w, maximized: false }) && displayFor(w)?.id === d.id).length;

export const GroupFrame = memo(function GroupFrame({ t, plan, d, dkey, desk, meta, multi, canMove, onAddWindow, onEdit, showEmpty }) {
  const count = useStore(windows, countOn(desk, d));
  const [picker, setPicker] = useState(null);
  const [moveMenu, setMoveMenu] = useState(null);
  const [saved, setSaved] = useState(false);
  const layoutBtn = useRef(null);
  const apps = useLauncherApps();
  const { layout, ratio } = layoutOf(desk, dkey);
  const f = plan.frame, hd = plan.header, area = plan.work;
  useEffect(() => { if (!saved) return undefined; const id = setTimeout(() => setSaved(false), 1600); return () => clearTimeout(id); }, [saved]);
  const pins = meta.pins.map((k) => apps.find((a) => a.id === k)).filter(Boolean);
  const openPicker = () => {
    const n = layoutBtn.current;
    const [x, y, w, h] = n ? lord.getLayout(n.id) : [hd.x + hd.w - 300, hd.y + 30, 0, 0];
    setPicker({ x: Math.max(d.x + 8, Math.min(x + w - 300, d.x + d.w - 308)), y: y + h + 6 });
  };
  return (
    <>
      <FrameBox t={t} f={f} />
      <View style={{ position: 'absolute', left: hd.x, top: hd.y, width: hd.w, height: hd.h, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 4, pointerEvents: 'box-none' }}>
        <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 8, paddingStart: 4, paddingEnd: 12, borderRadius: 12, backgroundColor: t.chipBg, flexShrink: 1, minWidth: 0 }}>
          <View style={{ width: 26, height: 26, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: alpha(meta.color, 0.22) }}>
            <ActivityIcon icon={meta.icon} color={meta.color} bg={t.solidBg} size={14} />
          </View>
          <View onDoubleClick={() => onEdit(desk)} style={{ flexShrink: 1, minWidth: 0 }}>
            <Text style={{ fontSize: 14, fontWeight: 700, color: t.frameText, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>
              {multi ? tr('shell.ws.frame.onDisplay', { activity: desktopName(desk), display: d.name || String(d.id) }) : desktopName(desk)}
            </Text>
          </View>
        </View>
        {pins.length ? (
          <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 3, borderRadius: 12, backgroundColor: t.chipBg }}>
            {pins.map((a) => (
              <View key={a.id} title={a.name} onClick={(e) => { noteLaunch(e.x, e.y); launchApp(a.id); }}
                style={{ width: 26, height: 26, borderRadius: 7, alignItems: 'center', justifyContent: 'center' }}>
                <a.Icon size={18} />
              </View>
            ))}
          </View>
        ) : null}
        <View style={{ flex: 1 }} />
        <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 3, borderRadius: 12, backgroundColor: t.chipBg }}>
          {saved ? <Text style={{ fontSize: 11.5, color: t.textDim, whiteSpace: 'nowrap', paddingHorizontal: 6 }}>{tr('shell.ws.layout.saved')}</Text> : null}
          <HeaderButton t={t} wide anchor={layoutBtn} active={!!picker} label={tr(`shell.ws.layout.${layout}`)} onClick={openPicker}>
            <LayoutMini cells={LAYOUT_CELLS[layout]} w={20} h={14} color={t.frameText} fill={t.frameText} active />
          </HeaderButton>
          <HeaderButton t={t} label={tr('shell.ws.layout.add')} onClick={(e) => onAddWindow(d, e)}><PlusGlyph color={t.frameText} size={12} /></HeaderButton>
          <HeaderButton t={t} label={tr('shell.ws.layout.save')} onClick={() => { saveLayout(desk); setSaved(true); }}><SaveGlyph color={t.frameText} bg={t.solidBg} size={14} /></HeaderButton>
          {canMove ? (
            <HeaderButton t={t} label={tr('shell.ws.layout.move')} onClick={(e) => { const id = wm.focused(); if (id) setMoveMenu({ x: e.x, y: e.y + 12, id }); }}><MoveGlyph color={t.frameText} size={14} /></HeaderButton>
          ) : null}
        </View>
      </View>
      {count >= 2 ? <Splitter t={t} desk={desk} dkey={dkey} layout={layout} ratio={ratio} area={area} count={count} /> : null}
      {/* kept mounted and only shown or hidden: an activity switch then changes one style here, not a whole card */}
      {showEmpty ? (
        <View style={{ position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'box-none',
          display: count === 0 ? 'flex' : 'none' }}>
          <Surface t={t} radius={18} still style={{ alignItems: 'center', gap: 10, maxWidth: 440, paddingHorizontal: 24, paddingVertical: 20 }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: t.frameText, textAlign: 'center' }}>{tr('shell.ws.empty.title')}</Text>
            <Text style={{ fontSize: 12.5, color: t.textDim, textAlign: 'center' }}>{tr('shell.ws.empty.body')}</Text>
            <View style={{ flexDirection: 'row', gap: 8, marginTop: 4 }}>
              <View onClick={(e) => onAddWindow(d, e)} style={{ height: 32, paddingHorizontal: 14, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: t.accent }}>
                <PlusGlyph color={t.onAccent} size={11} />
                <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.onAccent }}>{tr('shell.ws.layout.add')}</Text>
              </View>
              {pins.length ? (
                <View onClick={(e) => { noteLaunch(e.x, e.y); for (const a of pins) launchApp(a.id); }}
                  style={{ height: 32, paddingHorizontal: 14, borderRadius: 10, justifyContent: 'center', backgroundColor: t.hover, border: `1px solid ${t.separator}` }}>
                  <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.frameText }}>{tr('shell.ws.empty.openPins')}</Text>
                </View>
              ) : null}
            </View>
          </Surface>
        </View>
      ) : null}
      {picker && <LayoutPicker t={t} at={picker} current={layout} onPick={(l) => setLayout(desk, dkey, l)} onClose={() => setPicker(null)} />}
      {moveMenu && <Menu t={t} x={moveMenu.x} y={moveMenu.y} onClose={() => setMoveMenu(null)}
        items={desktopList.get().filter((x) => x !== desk).map((x) => ({ label: desktopName(x), onClick: () => wm.moveToDesktop(moveMenu.id, x) }))} />}
    </>
  );
});
