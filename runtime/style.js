// style.js — converts CSS/React-style styles into axia-server's native properties.
// Colors → ARGB number, shorthands (margin, padding, border, boxShadow, background…) → simple properties.

const NAMED = {
  transparent: 0x00000000, white: 0xffffffff, black: 0xff000000, red: 0xffff0000,
  green: 0xff008000, blue: 0xff0000ff, gray: 0xff808080, grey: 0xff808080,
  yellow: 0xffffff00, orange: 0xffffa500, purple: 0xff800080,
};

const clamp255 = (n) => Math.max(0, Math.min(255, Math.round(n)));

export function color(v) {
  if (typeof v === 'number') return v >>> 0;
  const s = String(v).trim().toLowerCase();
  if (s in NAMED) return NAMED[s] >>> 0;
  if (s[0] === '#') {
    let h = s.slice(1);
    if (h.length === 3 || h.length === 4) h = [...h].map((c) => c + c).join('');
    const rgb = parseInt(h.slice(0, 6), 16);
    const a = h.length === 8 ? parseInt(h.slice(6, 8), 16) : 255;
    return ((a << 24) >>> 0) + rgb;
  }
  const m = s.match(/^rgba?\(([^)]+)\)$/);
  if (m) {
    const p = m[1].split(/[\s,/]+/).filter(Boolean).map(parseFloat);
    const a = p.length > 3 ? clamp255(p[3] * 255) : 255;
    return (((a << 24) >>> 0) + (clamp255(p[0]) << 16) + (clamp255(p[1]) << 8) + clamp255(p[2])) >>> 0;
  }
  return 0;
}

// Splits on spaces/commas outside parentheses: "0 8px 24px rgba(0,0,0,.4)".
function tokens(s, sep = /\s/) {
  const out = [];
  let depth = 0, cur = '';
  for (const ch of String(s)) {
    if (ch === '(') depth++;
    if (ch === ')') depth--;
    if (depth === 0 && sep.test(ch)) { if (cur.trim()) out.push(cur.trim()); cur = ''; } else cur += ch;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

const num = (v) => (typeof v === 'number' ? v : parseFloat(v) || 0);

function dim(v) {
  if (typeof v === 'number') return v;
  const s = String(v).trim();
  if (s === 'auto' || s.endsWith('%')) return s;
  return parseFloat(s) || 0;
}

function box(v) {
  if (typeof v === 'number') return [v, v, v, v];
  const p = tokens(v).map(num);
  if (p.length === 1) return [p[0], p[0], p[0], p[0]];
  if (p.length === 2) return [p[0], p[1], p[0], p[1]];
  if (p.length === 3) return [p[0], p[1], p[2], p[1]];
  return p.slice(0, 4);
}

const ANGLES = { 'to top': 0, 'to right': 90, 'to bottom': 180, 'to left': 270,
  'to top right': 45, 'to bottom right': 135, 'to bottom left': 225, 'to top left': 315 };

function gradient(v) {
  const m = String(v).match(/^linear-gradient\((.*)\)$/s);
  if (!m) return null;
  const parts = tokens(m[1], /,/);
  let angle = 180;
  if (/deg$/.test(parts[0])) angle = parseFloat(parts.shift());
  else if (parts[0] in ANGLES) angle = ANGLES[parts.shift()];
  const stop = (p) => color(tokens(p)[0]);
  return [angle, stop(parts[0]), stop(parts[parts.length - 1])];
}

const SIDES = ['Left', 'Top', 'Right', 'Bottom', 'Start', 'End'];
const COLOR_KEYS = new Set(['backgroundColor', 'borderColor', 'color', 'shadowColor', ...SIDES.map((s) => `border${s}Color`)]);
const DIM_KEYS = new Set(['width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight',
  'left', 'top', 'right', 'bottom', 'flexBasis', 'start', 'end']);
// CSS logical names → the engine's (React Native) names
const ALIASES = {
  marginInlineStart: 'marginStart', marginInlineEnd: 'marginEnd',
  paddingInlineStart: 'paddingStart', paddingInlineEnd: 'paddingEnd',
  insetInlineStart: 'start', insetInlineEnd: 'end',
  borderInlineStartWidth: 'borderStartWidth', borderInlineEndWidth: 'borderEndWidth',
  borderInlineStartColor: 'borderStartColor', borderInlineEndColor: 'borderEndColor',
  borderStartStartRadius: 'borderTopStartRadius', borderStartEndRadius: 'borderTopEndRadius',
  borderEndStartRadius: 'borderBottomStartRadius', borderEndEndRadius: 'borderBottomEndRadius',
  lineClamp: 'numberOfLines', WebkitLineClamp: 'numberOfLines',
};

// "1px solid #ccc" → [width, color]; "none" → [0, transparent]
function borderParts(v) {
  if (typeof v === 'number') return [v, null];
  if (v === 'none' || v === 0) return [0, 0];
  const p = tokens(v);
  const w = p.find((t) => /^-?[\d.]/.test(t));
  const c = p.find((t) => !/^-?[\d.]/.test(t) && !/^(solid|dashed|dotted|double|none)$/.test(t));
  return [w !== undefined ? num(w) : 1, c !== undefined ? color(c) : null];
}

function angle(v) {
  if (typeof v === 'number') return v;
  const s = String(v).trim(), n = parseFloat(s) || 0;
  if (s.endsWith('rad')) return n * 180 / Math.PI;
  if (s.endsWith('turn')) return n * 360;
  return n;                                          // deg (default)
}

const ORIGIN_WORDS = { left: '0%', top: '0%', center: '50%', right: '100%', bottom: '100%' };
function origin(v, out) {
  const p = Array.isArray(v) ? v : tokens(String(v));
  let x = p[0] ?? '50%', y = p[1] ?? '50%';
  if (p.length === 1 && (x === 'top' || x === 'bottom')) { y = x; x = '50%'; }
  if (x === 'top' || x === 'bottom' || y === 'left' || y === 'right') [x, y] = [y, x];
  out.transformOriginX = dim(ORIGIN_WORDS[x] ?? x);
  out.transformOriginY = dim(ORIGIN_WORDS[y] ?? y);
}
const WEIGHTS = { normal: 400, medium: 500, semibold: 600, bold: 700 };

function expand(k, v, out) {
  if (k in ALIASES) k = ALIASES[k];
  switch (k) {
    case 'margin': case 'padding': {
      const [t, r, b, l] = box(v);
      out[k + 'Top'] = t; out[k + 'Right'] = r; out[k + 'Bottom'] = b; out[k + 'Left'] = l;
      return;
    }
    case 'marginHorizontal': case 'paddingHorizontal': {
      const base = k.replace('Horizontal', '');
      out[base + 'Left'] = out[base + 'Right'] = num(v);
      return;
    }
    case 'marginVertical': case 'paddingVertical': {
      const base = k.replace('Vertical', '');
      out[base + 'Top'] = out[base + 'Bottom'] = num(v);
      return;
    }
    case 'inset': {
      const [t, r, b, l] = box(v);
      out.top = t; out.right = r; out.bottom = b; out.left = l;
      return;
    }
    case 'marginInline': case 'paddingInline': {
      const base = k.replace('Inline', ''), p = tokens(String(v)).map(num);
      out[base + 'Start'] = p[0] || 0; out[base + 'End'] = p.length > 1 ? p[1] : p[0] || 0;
      return;
    }
    case 'insetInline': {
      const p = tokens(String(v)).map(dim);
      out.start = p[0]; out.end = p.length > 1 ? p[1] : p[0];
      return;
    }
    case 'flex': {
      // React Native: flex > 0 grows and shrinks from a zero basis; flex: 0 is sized by its
      // width/height and inflexible; flex < 0 only shrinks (down to minWidth/minHeight)
      const f = num(v);
      if (f > 0) { out.flexGrow = f; out.flexShrink = 1; out.flexBasis = 0; }
      else if (f === 0) { out.flexGrow = 0; out.flexShrink = 0; out.flexBasis = 'auto'; }
      else { out.flexGrow = 0; out.flexShrink = -f; out.flexBasis = 'auto'; }
      return;
    }
    case 'borderTop': case 'borderRight': case 'borderBottom': case 'borderLeft':
    case 'borderStart': case 'borderEnd': case 'borderInlineStart': case 'borderInlineEnd': {
      const side = k.replace('Inline', '');
      const [w, c] = borderParts(v);
      out[side + 'Width'] = w;
      if (c !== null) out[side + 'Color'] = c;
      return;
    }
    case 'textOverflow':
      out.textOverflow = v === 'ellipsis' ? 'ellipsis' : 'clip';
      return;
    case 'transformOrigin':
      origin(v, out);
      return;
    case 'border': {
      const p = tokens(v);
      out.borderWidth = num(p[0]);
      out.borderColor = color(p[p.length - 1]);
      return;
    }
    case 'boxShadow': {
      if (v === 'none') { out.shadowColor = 0; return; }
      const p = tokens(v);
      const nums = p.filter((t) => /^-?[\d.]/.test(t)).map(num);
      const c = p.find((t) => !/^-?[\d.]/.test(t));
      out.shadowOffsetX = nums[0] || 0; out.shadowOffsetY = nums[1] || 0; out.shadowBlur = nums[2] || 0;
      out.shadowColor = c ? color(c) : 0x80000000;
      return;
    }
    case 'backdropFilter': {
      const m = String(v).match(/blur\(\s*([\d.]+)/);
      out.backdropBlur = m ? parseFloat(m[1]) : 0;
      return;
    }
    case 'background': case 'backgroundImage': {
      const g = gradient(v);
      if (g) out.gradient = g; else out.backgroundColor = color(v);
      return;
    }
    case 'transform': {
      // translate moves the box (and what layout reports); scale and rotate only paint (as in CSS)
      const list = Array.isArray(v) ? v : [];
      for (const t of list) {
        if ('translateX' in t) out.translateX = num(t.translateX);
        if ('translateY' in t) out.translateY = num(t.translateY);
        if ('scale' in t) out.scaleX = out.scaleY = num(t.scale);
        if ('scaleX' in t) out.scaleX = num(t.scaleX);
        if ('scaleY' in t) out.scaleY = num(t.scaleY);
        if ('rotate' in t) out.rotate = angle(t.rotate);
        if ('rotateZ' in t) out.rotate = angle(t.rotateZ);
      }
      if (typeof v === 'string' && v !== 'none') {
        for (const m of v.matchAll(/(\w+)\(([^)]*)\)/g)) {
          const a = m[2].split(/[\s,]+/).filter(Boolean);
          switch (m[1]) {
            case 'translate': out.translateX = num(a[0]); out.translateY = num(a[1] ?? 0); break;
            case 'translateX': out.translateX = num(a[0]); break;
            case 'translateY': out.translateY = num(a[0]); break;
            case 'scale': out.scaleX = num(a[0]); out.scaleY = num(a[1] ?? a[0]); break;
            case 'scaleX': out.scaleX = num(a[0]); break;
            case 'scaleY': out.scaleY = num(a[0]); break;
            case 'rotate': case 'rotateZ': out.rotate = angle(a[0]); break;
            default: break;
          }
        }
      }
      return;
    }
    case 'fontWeight':
      out.fontWeight = typeof v === 'number' ? v : WEIGHTS[v] || parseFloat(v) || 400;
      return;
    case 'fontFamily':
      // three families: Inter ('sans-serif'), JetBrains Mono ('monospace', or a name containing "mono")
      // and the brand face of the LordOS wordmark ('brand': Plus Jakarta Sans, see LordOSWordmark)
      out.fontFamily = /mono|courier|consol/i.test(String(v)) ? 'monospace' : /brand|jakarta/i.test(String(v)) ? 'brand' : 'sans-serif';
      return;
    default:
      if (COLOR_KEYS.has(k)) out[k] = color(v);
      else if (DIM_KEYS.has(k)) out[k] = dim(v);
      else if (typeof v === 'string' && /^-?[\d.]+(px)?$/.test(v)) out[k] = parseFloat(v);
      else out[k] = v;
  }
}

export function flatten(style) {
  if (!style) return {};
  if (Array.isArray(style)) return Object.assign({}, ...style.map(flatten));
  return style;
}

export function normalize(style) {
  const s = flatten(style), out = {};
  for (const k in s) {
    const v = s[k];
    if (v === undefined || v === null || v === false) continue;
    expand(k, v, out);
  }
  return out;
}

const same = (a, b) => a === b || (Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((x, i) => x === b[i]));

// Sends the server only the properties that changed.
export function applyStyle(id, prev, next) {
  for (const k in prev) if (!(k in next)) lord.setStyle(id, k, undefined);
  for (const k in next) if (!same(prev[k], next[k])) lord.setStyle(id, k, next[k]);
}
