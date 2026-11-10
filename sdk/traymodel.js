// traymodel.js — the tray icons of Linux apps without a screen (sdk/trayitems.jsx; lord-server's side is
// server/src/ui/sni.c). Tested by server/tests/tray_model_test.mjs.
//
// An item is what lord-server sends: { id, title, tooltip, status, icon, pix, pw, ph, menu, isMenu }. `icon` is a PNG
// of the installed icon themes ('' when the app only sent pixels) and `pix` goes up with each new pixmap (0: none).

// A Passive item is one the app says needs no place in the bar (a sync client that is idle, for one).
export const trayShown = (item) => !!item && item.status !== 'Passive';

// The letter drawn when the app sent neither an icon name LordOS has nor pixels.
export function trayInitial(item) {
  const s = String((item && (item.title || item.tooltip)) || '').trim();
  return s ? Array.from(s)[0].toUpperCase() : '•';
}

// What the menu says first: the tooltip, or the title.
export const trayHeader = (item) => (item && (item.tooltip || item.title)) || '';

// A layout from lord-server ([{ id, label, enabled, toggle, on, sub } | { sep: true }]) as <ContextMenu> items. The
// SDK's menu opens one level of submenus: a deeper one is laid out inside its parent, under its label as a header.
// Separators at the ends or next to each other are dropped. pick(id) runs when an entry is chosen.
export function trayMenu(entries, pick, depth = 0) {
  const out = [];
  for (const e of Array.isArray(entries) ? entries : []) {
    if (!e || typeof e !== 'object') continue;
    if (e.sep) { if (out.length && out[out.length - 1] !== '-') out.push('-'); continue; }
    const label = String(e.label || '');
    if (Array.isArray(e.sub)) {
      const sub = trayMenu(e.sub, pick, depth + 1);
      if (depth === 0) out.push({ label, disabled: !e.enabled || !sub.length, submenu: sub.length ? sub : undefined });
      else if (sub.length) out.push({ header: label }, ...sub);
      continue;
    }
    out.push({ label, disabled: !e.enabled, checked: !!e.toggle && !!e.on, onClick: () => pick(e.id) });
  }
  while (out.length && out[out.length - 1] === '-') out.pop();
  return out;
}

// Where the menu opens for an item whose box is [x, y, w, h] on display d ({ x, y, w, h }). The menu lives in a frame
// the size of the display, placed from the item's box (an <Overlay> starts there); below the item when the bar is in
// the top half of the display and above it otherwise (<ContextMenu> keeps it inside `bounds`).
export function trayMenuAt(box, d, gap = 4) {
  const [x, y, w, h] = box;
  const below = y + h / 2 < d.y + d.h / 2;
  return {
    frame: { position: 'absolute', left: d.x - x, top: d.y - y, width: d.w, height: d.h, pointerEvents: 'box-none' },
    x: x - d.x,
    y: below ? y - d.y + h + gap : 1e6,
    bounds: { w: d.w, h: below ? d.h : Math.max(0, y - d.y - gap + 4) },
  };
}

// A menu that comes from the app is shown only if it was asked for that item a moment ago: an app that answers late,
// or sends a layout by itself, opens nothing.
export const TRAY_MENU_WAIT = 5000;
export const trayMenuWanted = (asked, id, now) => !!asked && asked.id === id && now - asked.at >= 0 && now - asked.at < TRAY_MENU_WAIT;
