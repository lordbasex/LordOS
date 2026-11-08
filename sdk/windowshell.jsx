// windowshell.jsx — <WindowShell>: a window's node with invisible borders for resizing
// on all 4 sides and 4 corners, 6 px outside the frame and 3 px inside, with the
// cursor for each direction. Moving and resizing is done by lord-server in C (wm.beginMove/beginResize,
// docs/WINDOWS.md): the theme doesn't receive an event per frame.
//
//   <WindowShell win={w} style={{ borderRadius: 12, backgroundColor, overflow: 'hidden', boxShadow }}
//                onMouseDown={() => wm.focus(w.id)}>
//     <View onMouseDown={(e) => wm.beginMove(w.id)}>…title bar…</View>
//     <AppHost id={w.id} focused={focused} />
//   </WindowShell>
//
// A window of a hidden desktop that is still mounted (a LordOS app, useWindows mounted) stays here with
// display: none, so it keeps its state; in the overview the window takes its place in the grid.
import { createElement as h, useEffect, useRef } from 'react';
import { View } from '@lordos/runtime';
import { useStore } from './store.js';
import { wm, fxNodes, frameRadii, activeDesktop } from './windows.js';
import { onDesktop } from './desktopmodel.js';
import { desktopFx } from './desktops.js';
import { OverviewCover, OverviewPlaceholder, useOverviewPlace } from './overview.jsx';

export const WINDOW_EDGE = 6;     // px of border outside the frame
const INSIDE = 3;                 // and inside
const CORNER = 16;

// [xdg edges, cursor, style]
const GRIPS = [
  [1, 'ns-resize', (t) => ({ left: CORNER, right: CORNER, top: 0, height: t })],
  [2, 'ns-resize', (t) => ({ left: CORNER, right: CORNER, bottom: 0, height: t })],
  [4, 'ew-resize', (t) => ({ top: CORNER, bottom: CORNER, left: 0, width: t })],
  [8, 'ew-resize', (t) => ({ top: CORNER, bottom: CORNER, right: 0, width: t })],
  [5, 'nwse-resize', () => ({ left: 0, top: 0, width: CORNER, height: CORNER })],
  [10, 'nwse-resize', () => ({ right: 0, bottom: 0, width: CORNER, height: CORNER })],
  [9, 'nesw-resize', () => ({ right: 0, top: 0, width: CORNER, height: CORNER })],
  [6, 'nesw-resize', () => ({ left: 0, bottom: 0, width: CORNER, height: CORNER })],
];

// leaving: the theme's minimize effect is drawing the window on its way out.
export function WindowShell({ win, style, children, onMouseDown, resizable = true, leaving = false }) {
  const geo = wm.frame(win);
  const tv = useOverviewPlace(win.id);
  const here = onDesktop(win, useStore(activeDesktop));
  const fx = useStore(desktopFx);
  const m = resizable && !win.maximized && !tv ? WINDOW_EDGE : 0;
  const inner = useRef(null);
  if (!inner.current) inner.current = (n) => { if (n && n.id) fxNodes.set(win.id, n.id); else fxNodes.delete(win.id); };
  const radius = typeof style?.borderRadius === 'number' ? style.borderRadius : 0;
  useEffect(() => { frameRadii.set(win.id, radius); }, [win.id, radius]);
  useEffect(() => () => frameRadii.delete(win.id), [win.id]);
  const place = tv ? { transform: `translate(${tv.x - geo.x}px, ${tv.y - geo.y}px) scale(${tv.scale})`, transformOrigin: 'left top' } : fx && here ? fx : null;
  return (
    <View ref={wm.frameRef(win.id, m)} onMouseDown={tv ? undefined : onMouseDown}
      style={{ position: 'absolute', left: geo.x - m, top: geo.y - m, width: geo.w + 2 * m, height: geo.h + 2 * m, display: here && (!win.minimized || tv || leaving) ? 'flex' : 'none', ...(tv ? { pointerEvents: 'box-none' } : null) }}>
      <View ref={inner.current} style={{ position: 'absolute', left: m, top: m, right: m, bottom: m, ...style, ...place }}>
        {tv ? h(OverviewPlaceholder, { win, scale: tv.scale }) : null}
        {children}
        {tv ? h(OverviewCover, { id: win.id, scale: tv.scale, radius: win.maximized ? 0 : 8 }) : null}
      </View>
      {m > 0 && GRIPS.map(([edges, cursor, at]) => (
        <View key={edges} style={{ position: 'absolute', cursor, ...at(m + INSIDE) }}
          onMouseDown={(e) => { e.stopPropagation(); wm.focus(win.id); wm.beginResize(win.id, edges); }} />
      ))}
    </View>
  );
}
