// layers.js — ONE stacking order for the whole desktop (docs/LAYERS.md).
//
// The engine's zIndex only orders siblings, so a menu drawn inside the menu bar can never rise
// above a Dock drawn after it. Instead every shell surface goes into a named layer: a root node of
// the screen's size (runtime/index.js `layerContainer`) that the content enters through a React
// portal. Paint order and hit testing follow the same order (lord-server's hit test walks the
// children in paint order, and a layer is `pointerEvents: 'box-none'`: it never takes a click
// itself), so a click on a menu over the Dock goes to the menu.
//
//   base (0)      the theme's own tree: wallpaper, desktop widgets, windows (tree order)
//   dock          the Dock / the Classic taskbar
//   menubar       the top menu bar
//   floating      fullscreen and picture-in-picture windows (above the Dock and the bar)
//   overlay       full-screen shell overlays: the app launcher, search
//   popover       menus, context menus, popovers, Control Center, select lists, panels
//   modal         shell sheets and confirmations (Empty Trash), the print dialog
//   tooltip       tooltips
//   drag          drag images and the «poof»
//   notification  notification banners and toasts, the privacy pill in the corner (0.8.9.6.2: above every layer a theme
//                 draws in, so no tooltip, drag image or community theme's surface covers them)
//   system        system prompts: permissions, passwords and the keyring, Bluetooth pairing,
//                 «keep this display?», the file chooser
//   agent         Hands' banner, "Claude / Lumo is using your computer" with Stop (sdk/hands.jsx, docs/HANDS.md): part of
//                 the system band (100 and above), which an AI agent never reads, clicks or sees, and no theme reaches; it
//                 doesn't take the keyboard
//   input         the input method's candidate window and the on-screen keyboard (they serve the field below)
//   capture       Screenshots' area selection (❖⇧4): a still of the whole screen, so it covers every
//                 layer, sheets and prompts included, and the selection works whatever is open
//
// Inside one layer, what opened last is on top. The lock screen and the login screen are another
// session (lord-sessiond), above all of this.
//
//   <Layer name="popover">…</Layer>          a shell surface (themes, startShell)
//   <Overlay anchor={ref} onClose={…}>…</Overlay>
//       a list, popover or menu opened from a control: its children are positioned against the
//       anchor's box (`position: 'absolute', top: h + 4, left: 0` keeps meaning «below the
//       button»), drawn above the whole window and, in the desktop's process, above the Dock and
//       other windows. In an isolated app's process it goes to the window's own overlay host
//       (the app's surface is the window, so it is clipped to it). Clicking outside or Escape
//       calls onClose.
import { createElement as h, Fragment, useContext, useEffect, useRef } from 'react';
import { View, layerContainer, createPortal, addListener } from '@lordos/runtime';
import { AppWindowContext } from './components.jsx';
import { direction } from './i18n.js';
import { wm } from './windows.js';

export const LAYERS = Object.freeze({
  dock: 10, menubar: 20, floating: 30, overlay: 40, popover: 50, modal: 60,
  tooltip: 80, drag: 90, notification: 95, system: 100, agent: 105, input: 110, capture: 120,
});

let shell = false;
// startShell turns the layers on (the desktop's process). Elsewhere (an isolated app's process)
// <Layer> draws its children in place and <Overlay> uses the window's overlay host.
export function enableShellLayers() { shell = true; }
export const shellLayers = () => shell;

// A window's sheet: a modal layer opened from inside an app's window (Settings → Wallpaper's file dialog) is drawn
// in a box of the layer's size that is registered as the window's (wm.addSheet), so Screenshots' window capture
// paints it over the window instead of leaving it out with the other layers (issue #58).
function SheetBox({ win, children }) {
  const box = useRef(null);
  useEffect(() => wm.addSheet(win, box.current?.id), [win]);
  // handsWin: the window it belongs to, so an allowlisted agent's screenshot of that window shows it (sdk/hands.jsx)
  return h(View, { ref: box, handsWin: win, style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } }, children);
}

export function Layer({ name, children }) {
  const z = LAYERS[name];
  if (z == null) throw new Error(`unknown layer: ${name}`);
  const win = useContext(AppWindowContext);
  const c = shell ? layerContainer(z) : null;
  if (!c) return h(Fragment, null, children);
  return createPortal(name === 'modal' && win?.id ? h(SheetBox, { win: win.id }, children) : children, c);
}

const insideOf = (inst, ref) => {
  for (let n = inst; n; n = n.parent) if (n === ref) return true;
  return false;
};
const boxOf = (inst) => { try { return inst ? lord.getLayout(inst.id) : null; } catch (e) { return null; } };

export function Overlay({ anchor, children, onClose, layer = 'popover' }) {
  const win = useContext(AppWindowContext);
  const box = useRef(null);
  const close = useRef(onClose);
  close.current = onClose;
  const a = anchor && typeof anchor === 'object' && 'current' in anchor ? anchor.current : anchor;
  useEffect(() => {
    if (!onClose) return undefined;
    const offPtr = addListener('pointerdown', (e) => {
      const an = anchor && typeof anchor === 'object' && 'current' in anchor ? anchor.current : anchor;
      if (insideOf(e.target, box.current) || insideOf(e.target, an)) return;
      // geometric too: the anchor may be a wrapper whose children are not its own hit target
      const r = boxOf(an);
      if (r && e.x >= r[0] && e.x < r[0] + r[2] && e.y >= r[1] && e.y < r[1] + r[3]) return;
      close.current?.();
    });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') close.current?.(); });
    return () => { offPtr(); offKey(); };
  }, [!!onClose]);
  const target = shell ? layerContainer(LAYERS[layer] || LAYERS.popover) : win?.overlayHost || null;
  const r = boxOf(a);
  // no layer: drawn in place, as before (a window's host exists from its first frame on)
  if (!target || !r) return win?.id && !target ? null : h(Fragment, null, children);
  const o = shell ? [0, 0] : boxOf(target) || [0, 0];
  return createPortal(h(View, {
    ref: box,
    protected: !!win?.protected,         // a protected window's list stays the protected fill in screenshots
    handsWin: win?.id || undefined,      // the window it belongs to (Hands' allowlisted screenshots)
    style: { position: 'absolute', left: r[0] - o[0], top: r[1] - o[1], width: r[2], height: r[3], pointerEvents: 'box-none', direction: direction() },
  }, children), target);
}
