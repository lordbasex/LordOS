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
//   notification  notification banners and toasts
//   tooltip       tooltips
//   drag          drag images and the «poof»
//   system        system prompts: permissions, passwords and the keyring, Bluetooth pairing,
//                 «keep this display?», the file chooser
//   input         the input method's candidate window and the on-screen keyboard (they serve the field below)
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

export const LAYERS = Object.freeze({
  dock: 10, menubar: 20, floating: 30, overlay: 40, popover: 50, modal: 60,
  notification: 70, tooltip: 80, drag: 90, system: 100, input: 110,
});

let shell = false;
// startShell turns the layers on (the desktop's process). Elsewhere (an isolated app's process)
// <Layer> draws its children in place and <Overlay> uses the window's overlay host.
export function enableShellLayers() { shell = true; }
export const shellLayers = () => shell;

export function Layer({ name, children }) {
  const z = LAYERS[name];
  if (z == null) throw new Error(`unknown layer: ${name}`);
  const c = shell ? layerContainer(z) : null;
  return c ? createPortal(children, c) : h(Fragment, null, children);
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
    protected: !!win?.protected,         // a protected window's list stays black in screenshots
    style: { position: 'absolute', left: r[0] - o[0], top: r[1] - o[1], width: r[2], height: r[3], pointerEvents: 'box-none', direction: direction() },
  }, children), target);
}
