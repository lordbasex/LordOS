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
import { settings } from './settings.js';
import { direction } from './i18n.js';
import { windowButtonsPref, buttonsAtStart, buttonOrder, windowButtonSizePref, scaledBar } from './windowbuttons.js';

// Where a title bar's buttons go (Settings → Desktop & Dock → Title bars, issue #58; sdk/windowbuttons.js).
// themeStart: the theme's own place (Flow true, Classic false). atStart: first in the title bar's row;
// order: the buttons in the row's order ('close', 'minimize', 'maximize').
// size: Settings → Appearance → Window options → Window buttons ('normal', 'large', 'xlarge'; GitHub #151, #219), and px(n) a
// length of the theme's bar at that size (its height, a button, a glyph, a gap): the hit areas grow with what is drawn.
export function useWindowButtons(themeStart) {
  const pref = useStore(settings, windowButtonsPref);      // every window's frame: only these settings, not all of them
  const size = useStore(settings, windowButtonSizePref);
  const atStart = buttonsAtStart(pref, themeStart, direction() === 'rtl');
  return { atStart, order: buttonOrder(atStart), size, px: (n) => scaledBar(n, size) };
}
// The same outside React: a theme's chrome for configureWindows (sdk/windows.js), read when a window opens or the app
// resizes itself, so a Linux window gets its content size for the bar's height now.
export const titleBarPx = (n) => scaledBar(n, windowButtonSizePref(settings.get()));

// <SeamCover color below />: the first child of a transparent app's title bar (the Terminal, manifest "transparent"),
// and of a bar under it. At 125 %, 150 % or 175 % the edges where the frame's border, the title bar, the separator and
// the app's first row meet fall inside a pixel; each box covers only part of that pixel and the window below them is
// transparent, so the row let the desktop through (a quarter of it at 150 %: a line across the window, GitHub #162).
// This box, painted first, goes under those edges: from 1 px above and beside the bar (under the frame's border) to
// `below` px past the bar's bottom edge (into what follows it), in the bar's color, so every one of those pixels is
// opaque. An opaque window doesn't need it: its own background is behind the seams. above={0} for a bar under the
// title bar (whose cover already reaches it), so it never paints over the title bar's buttons.
export function SeamCover({ color, below = 1, above = 1 }) {
  return h(View, { style: { position: 'absolute', left: -1, right: -1, top: -above, bottom: -below, backgroundColor: color, pointerEvents: 'none' } });
}

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
    // the press focuses the window in the capture phase: a sheet, a dialog or a control inside that stops the press still
    // brings its window forward, so the keyboard follows (a Linux window in front kept it while a Settings sheet was typed into)
    <View ref={wm.frameRef(win.id, m)} lordWin={win.id} onMouseDownCapture={tv ? undefined : onMouseDown}
      style={{ position: 'absolute', left: geo.x - m, top: geo.y - m, width: geo.w + 2 * m, height: geo.h + 2 * m, display: here && (!win.minimized || tv || leaving) ? 'flex' : 'none', ...(tv ? { pointerEvents: 'box-none' } : null) }}>
      {/* protected (wm.setProtected, wm.open's `protected`): the whole window, its title bar included, is the protected fill
          in every capture, in its exact box and corners, through the overview's scale and the theme's effects */}
      <View ref={inner.current} protected={!!win.protected} style={{ position: 'absolute', left: m, top: m, right: m, bottom: m, ...style, ...place }}>
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
