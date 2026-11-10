// trayitems.jsx — the tray icons of Linux apps (StatusNotifierItem) next to LordOS's own status items. lord-server is
// the watcher and the host (server/src/ui/sni.c): it reads each item's icon, title and menu, and sends them here as the
// "sni", "sni-gone" and "sni-menu" events (sdk/hardware.jsx). A click activates the app; a right click shows its
// dbusmenu with the SDK's menu, or lets the app show its own. The pure parts are in traymodel.js.
import { createElement as h, useRef } from 'react';
import { View, Text, Image, displayAt } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { Overlay } from './layers.js';
import { ContextMenu } from './contextmenu.jsx';
import { registerStatusItem } from './statusitems.js';
import { t } from './i18n.js';
import { trayShown, trayInitial, trayHeader, trayMenu, trayMenuAt, trayMenuWanted } from './traymodel.js';
import { iconSrc } from './iconsrc.js';

const api = () => (typeof lord !== 'undefined' && lord.sni) || null;
const items = createStore({});            // id → item
const menus = createStore({});            // id → the layout to show
const off = new Map();                    // id → unregister
const pixels = new Map();                 // id → { pix, px }: a new object only for a new pixmap (<Image pixels>)
let asked = null;                         // { id, at }: the last click or right click

function pixelsOf(item) {
  const had = pixels.get(item.id);
  if (had && had.pix === item.pix) return had.px;
  let px = null;
  try { px = api() ? lord.sni.pixels(item.id) : null; } catch (e) { px = null; }
  pixels.set(item.id, { pix: item.pix, px });
  return px;
}

function boxOf(ref) {
  try { return ref.current ? lord.getLayout(ref.current.id) : null; } catch (e) { return null; }
}

function TrayMenu({ btn, item, layout, close }) {
  const box = boxOf(btn);
  if (!box) return null;
  const at = trayMenuAt(box, displayAt(box[0] + box[2] / 2, box[1] + box[3] / 2));
  const head = trayHeader(item);
  const list = trayMenu(layout, (entry) => { try { lord.sni.clicked(item.id, entry); } catch (e) { /* the app left */ } });
  const all = [...(head ? [{ header: head }] : []), ...(list.length ? list : [{ label: t('shell.tray.noMenu'), disabled: true }])];
  return h(Overlay, { anchor: btn },
    h(View, { style: at.frame }, h(ContextMenu, { x: at.x, y: at.y, bounds: at.bounds, items: all, onClose: close })));
}

function TrayItem({ id, height }) {
  const th = useTheme();
  const all = useStore(items);
  const shown = useStore(menus);
  const btn = useRef(null);
  const item = all[id];
  if (!trayShown(item)) return null;
  const point = (e) => [Math.round(e.x), Math.round(e.y)];
  const close = () => { menus.set((m) => { const n = { ...m }; delete n[id]; return n; }); };
  const onClick = (e) => {
    asked = { id, at: Date.now() };
    try { lord.sni.activate(id, ...point(e), false); } catch (err) { /* lord-server without a tray */ }
  };
  const onContextMenu = (e) => {
    asked = { id, at: Date.now() };
    try { lord.sni.menu(id, ...point(e)); } catch (err) { /* lord-server without a tray */ }
  };
  const onMouseDown = (e) => { if (e.button === 1) { try { lord.sni.activate(id, ...point(e), true); } catch (err) { /* idem */ } } };
  const px = item.icon ? null : item.pix ? pixelsOf(item) : null;
  const glyph = item.icon
    ? h(Image, { src: iconSrc(item.icon, 16, th.text), style: { width: 16, height: 16, objectFit: 'contain' } })
    : px ? h(Image, { pixels: px, style: { width: 16, height: 16, objectFit: 'contain' } })
      : h(Text, { style: { fontSize: 11, fontWeight: 700, color: th.text } }, trayInitial(item));
  return h(View, { ref: btn, onClick, onContextMenu, onMouseDown, style: { height, width: 26, alignItems: 'center', justifyContent: 'center' } },
    glyph,
    item.status === 'NeedsAttention' && !item.icon && !px
      ? h(View, { style: { position: 'absolute', right: 3, top: 3, width: 7, height: 7, borderRadius: 4, backgroundColor: '#ff453a' } }) : null,
    shown[id] ? h(TrayMenu, { btn, item, layout: shown[id], close }) : null);
}

function add(item) {
  if (!item || !item.id) return;
  items.set((s) => ({ ...s, [item.id]: item }));
  if (!off.has(item.id)) off.set(item.id, registerStatusItem(`tray:${item.id}`, (p) => h(TrayItem, { ...p, id: item.id }), 7));
}

function remove(id) {
  if (off.has(id)) { off.get(id)(); off.delete(id); }
  pixels.delete(id);
  items.set((s) => { const n = { ...s }; delete n[id]; return n; });
  menus.set((m) => { if (!(id in m)) return m; const n = { ...m }; delete n[id]; return n; });
}

// From sdk/hardware.jsx's __lordHw.
export function trayEvent(kind, data) {
  if (kind === 'sni') add(data);
  else if (kind === 'sni-gone') { if (data && data.id) remove(data.id); }
  else if (kind === 'sni-menu' && data && trayMenuWanted(asked, data.id, Date.now()) && items.get()[data.id]) {
    asked = null;
    menus.set({ [data.id]: Array.isArray(data.items) ? data.items : [] });
  }
}

// Called once by startShell: the items already there (a theme loaded again keeps lord-server's state).
export function startTray() {
  let list = [];
  try { list = api() ? lord.sni.list() : []; } catch (e) { list = []; }
  for (const item of Array.isArray(list) ? list : []) add(item);
}
