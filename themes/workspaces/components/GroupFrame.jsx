// The activity's group frame on one display: a frame in the activity's colour behind its windows, sized to the work
// area (model/geometry.js), over a dimmed scrim (never a live blur: it covers the whole work area). Its header names the
// activity (and the display, with several) and holds the activity's pinned apps and its actions: Change layout, Add
// window, Save layout and Move to another activity (the window in front). In a split layout a divider sits in the gap
// between the tiles: dragging it shows a line (moved with lord.setStyle, no React render per pointer move) and the
// tiles change once, on release. With no window on the display, a short empty state offers the same two ways in.
import {
  View, Text, Layer, addListener, useStore, windows, wm, windowTitle, windowIcon, desktopList, desktopName, displayFor, launchApp, useLauncherApps, deskWidgetsShown, widgetDisplayKey, t as tr,
} from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';
import { ActivityIcon, PlusGlyph, SaveGlyph, MoveGlyph, LockGlyph, LayoutMini, LAYOUT_CELLS } from '../glyphs.jsx';
import { LAYOUTS, splitterAt, ratioFrom, TILE_GAP } from '../model/layouts.js';
import { layoutOf, setLayout, saveLayout, tileable, isLocked, setLocked, tiled } from '../tiler.js';
import { Surface } from './Surface.jsx';
import { noteLaunch, groupHidden } from '../state.js';

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

// The frame itself: only a thin border in the accent (no brighter top edge: the maintainer found the doubled line at
// the top unconvincing, 0.8.8.10), the same for every activity, so a
// switch never repaints this display-sized box; the activity's own colour is on its header chip and its pill. No fill:
// a scrim over the whole work area was the costly part of a switch on the CPU path (docs/WORKSPACES.md, "Performance").
const FrameBox = memo(function FrameBox({ t, f }) {
  return (
    <View style={{ position: 'absolute', left: f.x, top: f.y, width: f.w, height: f.h, borderRadius: 18, border: `1.5px solid ${alpha(t.accent, 0.45)}`, pointerEvents: 'none' }} />
  );
});

// how many windows of this activity show on this display (a narrow selector: other windows' changes don't re-render)
// The windows of this activity minimized on this display, as a string of ids (a narrow selector, like countOn). A
// minimized window keeps its frame, so it stays on the display it was on and comes back there.
const minimizedOn = (desk, d, hid) => (ws) => ws.filter((w) => w.desktop === desk && w.minimized && !w.pip && !hid.has(w.id) && tileable({ ...w, minimized: false, maximized: false })
  && displayFor(w)?.id === d.id).map((w) => w.id).join('\n');
// where each display's tray is, for the minimize effect (index.jsx): display id → the tray's node
export const trayNodes = new Map();

// The minimized windows of a display, in its frame header: one icon each; a click brings the window back where it was.
function MinimizedTray({ t, d, ids }) {
  const ref = useRef(null);
  useEffect(() => { if (ref.current) trayNodes.set(d.id, ref.current.id); return () => { if (trayNodes.get(d.id) === ref.current?.id) trayNodes.delete(d.id); }; });
  const list = ids.map((id) => wm.get(id)).filter(Boolean);
  return (
    <View ref={ref} title={tr('shell.ws.minimized.title')} style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 4, borderRadius: 12,
      backgroundColor: t.chipBg, display: list.length ? 'flex' : 'none' }}>
      {list.length ? <Text style={{ fontSize: 11, color: t.textDim, paddingHorizontal: 4, whiteSpace: 'nowrap' }}>{tr('shell.ws.minimized.label')}</Text> : null}
      {list.slice(0, 8).map((w) => {
        const Icon = windowIcon(w);
        return (
          <View key={w.id} title={windowTitle(w)} onClick={() => wm.focus(w.id)}
            style={{ width: 26, height: 26, borderRadius: 7, alignItems: 'center', justifyContent: 'center' }}>
            {Icon ? <Icon size={18} /> : <View style={{ width: 14, height: 14, borderRadius: 4, backgroundColor: t.textDim }} />}
          </View>
        );
      })}
      {list.length > 8 ? <Text style={{ fontSize: 11, color: t.textDim, paddingHorizontal: 4 }}>{`+${list.length - 8}`}</Text> : null}
    </View>
  );
}

const countOn = (desk, d) => (ws) => ws.filter((w) => w.desktop === desk && !w.minimized && !w.fullscreen && !w.pip && tileable({ ...w, maximized: false }) && displayFor(w)?.id === d.id).length;

export const GroupFrame = memo(function GroupFrame({ t, plan, d, dkey, dname, desk, meta, multi, canMove, onAddWindow, onAddWidget, onAddWebApp, onEdit, showEmpty }) {
  const count = useStore(windows, countOn(desk, d));
  const hid = useStore(groupHidden);           // windows another group hid aren't in the tray (state.js, groups)
  const minIds = useStore(windows, minimizedOn(desk, d, hid));
  const minimized = minIds ? minIds.split('\n') : [];
  const widgetsHere = useStore(deskWidgetsShown, (s) => (s[widgetDisplayKey(d)] || 0) > 0);
  // nothing open here: the free slots keep only their outlines (no "Free space" over the widgets or the card)
  const bare = count === 0 && !minimized.length;
  // the empty state's card only on a display with nothing at all: no window, none minimized, no widget
  const empty = bare && !widgetsHere;
  // a free slot's "Free space" and its Add window button stay out on a display with widgets too (they would sit over
  // them): the slot keeps its outline, and the frame header's + adds a window
  const quiet = bare || widgetsHere;
  // the free slots of this display's tiles (a window minimized there keeps its place): a dashed outline to fill
  const holesJson = useStore(tiled, (s) => JSON.stringify(s.holes?.[dkey] || []));
  const holes = JSON.parse(holesJson);
  // the divider splits the slots (a free one included), not only the windows shown
  const slotCount = useStore(tiled, (s) => s.counts?.[dkey] || 0);
  const slots = Math.max(count, slotCount);
  const [picker, setPicker] = useState(null);
  const [moveMenu, setMoveMenu] = useState(null);
  const [addMenu, setAddMenu] = useState(null);   // the header's "+": window, widget, web app
  const [saved, setSaved] = useState(false);
  const layoutBtn = useRef(null);
  const apps = useLauncherApps();
  const { layout, ratio } = layoutOf(desk, dkey);
  // the padlock closed: only the padlock stays in the header, and the divider can't be dragged
  const locked = isLocked(desk, dkey);
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
              {multi ? tr('shell.ws.frame.onDisplay', { activity: desktopName(desk), display: dname || d.name || String(d.id) }) : desktopName(desk)}
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
        <MinimizedTray t={t} d={d} ids={minimized} />
        <View style={{ flex: 1 }} />
        <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 3, borderRadius: 12, backgroundColor: t.chipBg }}>
          {saved && !locked ? <Text style={{ fontSize: 11.5, color: t.textDim, whiteSpace: 'nowrap', paddingHorizontal: 6 }}>{tr('shell.ws.layout.saved')}</Text> : null}
          {locked ? null : (<>
          <HeaderButton t={t} wide anchor={layoutBtn} active={!!picker} label={tr(`shell.ws.layout.${layout}`)} onClick={openPicker}>
            <LayoutMini cells={LAYOUT_CELLS[layout]} w={20} h={14} color={t.frameText} fill={t.frameText} active />
          </HeaderButton>
          <HeaderButton t={t} active={!!addMenu} label={tr('shell.ws.add.menu')} onClick={(e) => setAddMenu({ x: e.x, y: e.y + 12, e })}><PlusGlyph color={t.frameText} size={12} /></HeaderButton>
          <HeaderButton t={t} label={tr('shell.ws.layout.save')} onClick={() => { saveLayout(desk); setSaved(true); }}><SaveGlyph color={t.frameText} bg={t.solidBg} size={14} /></HeaderButton>
          {canMove ? (
            <HeaderButton t={t} label={tr('shell.ws.layout.move')} onClick={(e) => { const id = wm.focused(); if (id) setMoveMenu({ x: e.x, y: e.y + 12, id }); }}><MoveGlyph color={t.frameText} size={14} /></HeaderButton>
          ) : null}
          </>)}
          <HeaderButton t={t} active={locked} label={tr(locked ? 'shell.ws.lock.off' : 'shell.ws.lock.on')} onClick={() => { setPicker(null); setMoveMenu(null); setLocked(desk, dkey, !locked); }}>
            <LockGlyph color={locked ? t.accent : t.frameText} size={14} open={!locked} />
          </HeaderButton>
        </View>
      </View>
      {holes.map((r, i) => (
        <View key={`hole${i}`} style={{ position: 'absolute', left: r.x + 4, top: r.y + 4, width: r.w - 8, height: r.h - 8, borderRadius: 12,
          border: `2px dashed ${alpha(meta.color, 0.55)}`, alignItems: 'center', justifyContent: 'center', gap: 10, pointerEvents: 'box-none' }}>
          {/* with nothing open the empty state's card says it once, over the outlines */}
          {quiet ? null : <Text style={{ fontSize: 13, color: t.textDim, textAlign: 'center', paddingHorizontal: 16 }}>{tr('shell.ws.slot.free')}</Text>}
          {quiet ? null : (   /* locked too: the slot a minimized window left can take another app */
            <View onClick={(e) => onAddWindow(d, e)} style={{ height: 32, paddingHorizontal: 14, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: t.accent }}>
              <PlusGlyph color={t.onAccent} size={11} />
              <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.onAccent }}>{tr('shell.ws.layout.add')}</Text>
            </View>
          )}
        </View>
      ))}
      {slots >= 2 && !locked ? <Splitter t={t} desk={desk} dkey={dkey} layout={layout} ratio={ratio} area={area} count={slots} /> : null}
      {/* kept mounted and only shown or hidden: an activity switch then changes one style here, not a whole card */}
      {showEmpty ? (
        <View style={{ position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'box-none',
          display: empty ? 'flex' : 'none' }}>
          <Surface t={t} radius={18} still style={{ alignItems: 'center', gap: 10, width: Math.min(560, area.w - 48), paddingHorizontal: 24, paddingVertical: 20 }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: t.frameText, textAlign: 'center' }}>{tr('shell.ws.empty.title')}</Text>
            <Text style={{ fontSize: 12.5, color: t.textDim, textAlign: 'center' }}>{tr('shell.ws.empty.body')}</Text>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', gap: 8, marginTop: 4, alignSelf: 'stretch' }}>
              <View onClick={(e) => onAddWindow(d, e)} style={{ height: 32, paddingHorizontal: 14, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: t.accent }}>
                <PlusGlyph color={t.onAccent} size={11} />
                <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.onAccent }}>{tr('shell.ws.layout.add')}</Text>
              </View>
              {onAddWidget ? (
                <View onClick={() => onAddWidget(d)} style={{ height: 32, paddingHorizontal: 14, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 6,
                  backgroundColor: t.hover, border: `1px solid ${t.separator}` }}>
                  <PlusGlyph color={t.frameText} size={11} />
                  <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.frameText }}>{tr('shell.widgets.add')}</Text>
                </View>
              ) : null}
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
      {addMenu && <Menu t={t} x={addMenu.x} y={addMenu.y} onClose={() => setAddMenu(null)} items={[
        { label: tr('shell.ws.layout.add'), onClick: () => onAddWindow(d, addMenu.e) },
        ...(onAddWidget ? [{ label: tr('shell.widgets.add'), onClick: () => onAddWidget(d) }] : []),
        ...(onAddWebApp ? [{ label: tr('shell.ws.add.webapp'), onClick: () => onAddWebApp(d) }] : []),
      ]} />}
      {moveMenu && <Menu t={t} x={moveMenu.x} y={moveMenu.y} onClose={() => setMoveMenu(null)}
        items={desktopList.get().filter((x) => x !== desk).map((x) => ({ label: desktopName(x), onClick: () => wm.moveToDesktop(moveMenu.id, x) }))} />}
    </>
  );
});
