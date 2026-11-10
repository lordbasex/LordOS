// iconsrc.js — the <Image src> for the icon file of a Linux app (lord-server finds it: server/src/iconfind.c). A PNG
// goes as it is; an SVG is drawn by lord-media at the size it's shown, "svg:<px>:<rrggbb|->:<path>"
// (server/src/media/svgicon.h), and a symbolic one (a "-symbolic" name or a symbolic/ folder) takes `color`, as it
// does in other desktops: a tray's volume icon in the bar's text color. Tested by server/tests/iconsrc_test.mjs.

const MIN = 16, MAX = 256;

export const isSvg = (path) => typeof path === 'string' && /\.svg$/i.test(path);
export const isSymbolic = (path) => isSvg(path) && (/-symbolic(-rtl|-ltr)?\.svg$/i.test(path) || /\/symbolic\//.test(path));

// '#rgb', '#rrggbb', '#rrggbbaa', 'rgb(…)' or 'rgba(…)' → 'rrggbb' (alpha dropped), or '' if it isn't one of those.
export function hexColor(c) {
  const s = String(c || '').trim().toLowerCase();
  let m = /^#([0-9a-f]{3})$/.exec(s);
  if (m) return m[1].split('').map((d) => d + d).join('');
  m = /^#([0-9a-f]{6})([0-9a-f]{2})?$/.exec(s);
  if (m) return m[1];
  m = /^rgba?\(\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*(?:,\s*[\d.]+\s*)?\)$/.exec(s);
  if (m) return m.slice(1, 4).map((v) => Math.max(0, Math.min(255, Math.round(+v))).toString(16).padStart(2, '0')).join('');
  return '';
}

// size: the box in px; scale: how many pixels it gets per px (2 keeps it sharp on a display at 200 %).
export function iconSrc(path, size = 32, color = null, scale = 2) {
  if (!isSvg(path) || path[0] !== '/') return path || '';
  const px = Math.max(MIN, Math.min(MAX, Math.ceil((+size || 32) * (+scale || 1))));
  const hex = isSymbolic(path) ? hexColor(color) : '';
  return `svg:${px}:${hex || '-'}:${path}`;
}
