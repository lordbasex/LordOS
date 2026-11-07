// iterm.js — import profiles and color schemes exported by other terminal emulators.
//
// Accepted formats:
//   · Profile JSON ("Save Profile as JSON" or "Copy All Profiles as JSON"); also Dynamic
//     Profiles ({ "Profiles": [...] }).
//   · .itermcolors color presets (XML plist).
//   · The full com.googlecode.iterm2.plist preferences (XML or binary): every profile
//     ("New Bookmarks"), the default profile and the global shortcuts.
//
// Returns { profiles, schemes, keyMap, defaultGuid, warnings }: the caller decides what to
// keep. What LordOS doesn't have is reported in `warnings`, never made up.
import { t as T } from '@lordos/sdk';
import { DEFAULT_PROFILE, newProfileId } from './profiles.js';

// ---- plist XML ----

function parseXmlPlist(text) {
  const re = /<(\/?)([a-zA-Z]+)(\s*\/)?>|([^<]+)/g;
  const tokens = [];
  let m;
  while ((m = re.exec(text))) {
    if (m[4] != null) { if (m[4].trim()) tokens.push({ text: m[4] }); continue; }
    tokens.push({ tag: m[2], close: !!m[1], empty: !!m[3] });
  }
  let i = 0;
  const unescape = (s) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'")
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(+n)).replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&amp;/g, '&');
  const textUntil = (tag) => {
    let s = '';
    while (i < tokens.length && !(tokens[i].tag === tag && tokens[i].close)) { if (tokens[i].text != null) s += tokens[i].text; i++; }
    i++;
    return unescape(s);
  };
  const value = () => {
    while (i < tokens.length && (tokens[i].text != null || tokens[i].tag === 'plist' || tokens[i].tag === 'xml')) i++;
    const t = tokens[i++];
    if (!t) return null;
    if (t.empty) return t.tag === 'true' ? true : t.tag === 'false' ? false : t.tag === 'dict' ? {} : t.tag === 'array' ? [] : '';
    switch (t.tag) {
      case 'dict': {
        const o = {};
        while (i < tokens.length && !(tokens[i].tag === 'dict' && tokens[i].close)) {
          if (tokens[i].tag === 'key' && !tokens[i].close) { i++; const k = textUntil('key'); o[k] = value(); } else i++;
        }
        i++;
        return o;
      }
      case 'array': {
        const a = [];
        while (i < tokens.length && !(tokens[i].tag === 'array' && tokens[i].close)) {
          if (tokens[i].text != null) { i++; continue; }
          a.push(value());
        }
        i++;
        return a;
      }
      case 'string': case 'date': case 'data': return textUntil(t.tag);
      case 'real': case 'integer': return Number(textUntil(t.tag));
      case 'true': i++; return true;
      case 'false': i++; return false;
      default: return null;
    }
  };
  return value();
}

// ---- plist binario (bplist00) ----

function parseBinaryPlist(buf) {
  const d = new DataView(buf);
  const n = buf.byteLength;
  if (n < 40) throw new Error(T('terminal.iterm.short'));
  const offSize = d.getUint8(n - 26), refSize = d.getUint8(n - 25);
  const big = (o) => d.getUint32(o) * 2 ** 32 + d.getUint32(o + 4);
  const numObjects = big(n - 24), top = big(n - 16), tableOff = big(n - 8);
  const uint = (o, size) => { let v = 0; for (let k = 0; k < size; k++) v = v * 256 + d.getUint8(o + k); return v; };
  const offsetOf = (idx) => uint(tableOff + idx * offSize, offSize);
  const seen = new Set();
  const readLen = (o, low) => {
    if (low !== 15) return [low, o + 1];
    const m = d.getUint8(o + 1), size = 1 << (m & 15);
    return [uint(o + 2, size), o + 2 + size];
  };
  const obj = (idx) => {
    if (idx >= numObjects) throw new Error(T('terminal.iterm.badRef'));
    if (seen.has(idx)) throw new Error(T('terminal.iterm.circular'));
    seen.add(idx);
    try {
      const o = offsetOf(idx);
      const marker = d.getUint8(o), hi = marker >> 4, low = marker & 15;
      switch (hi) {
        case 0: return low === 8 ? false : low === 9 ? true : null;
        case 1: return uint(o + 1, 1 << low);
        case 2: return low === 2 ? d.getFloat32(o + 1) : d.getFloat64(o + 1);
        case 3: return d.getFloat64(o + 1);
        case 4: { const [len] = readLen(o, low); return `<${len} bytes>`; }
        case 5: { const [len, s] = readLen(o, low); let t = ''; for (let k = 0; k < len; k++) t += String.fromCharCode(d.getUint8(s + k)); return t; }
        case 6: { const [len, s] = readLen(o, low); let t = ''; for (let k = 0; k < len; k++) t += String.fromCharCode(d.getUint16(s + 2 * k)); return t; }
        case 8: return uint(o + 1, low + 1);
        case 10: { const [len, s] = readLen(o, low); const a = []; for (let k = 0; k < len; k++) a.push(obj(uint(s + k * refSize, refSize))); return a; }
        case 13: {
          const [len, s] = readLen(o, low);
          const r = {};
          for (let k = 0; k < len; k++) r[obj(uint(s + k * refSize, refSize))] = obj(uint(s + (len + k) * refSize, refSize));
          return r;
        }
        default: return null;
      }
    } finally { seen.delete(idx); }
  };
  return obj(top);
}

// ---- colores ----

const clamp01 = (v) => Math.max(0, Math.min(1, Number(v) || 0));
const toHex = (r, g, b) => `#${[r, g, b].map((v) => Math.round(clamp01(v) * 255).toString(16).padStart(2, '0')).join('')}`;

// Display P3 → sRGB (recent exports store colors in P3 by default).
function p3ToSrgb(r, g, b) {
  const lin = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const gam = (c) => (c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055);
  const [R, G, B] = [lin(r), lin(g), lin(b)];
  return [
    gam(clamp01(1.2249401 * R - 0.2249404 * G)),
    gam(clamp01(-0.0420569 * R + 1.0420571 * G)),
    gam(clamp01(-0.0196376 * R - 0.0786361 * G + 1.0982735 * B)),
  ];
}

function color(c) {
  if (!c || typeof c !== 'object') return null;
  let r = clamp01(c['Red Component']), g = clamp01(c['Green Component']), b = clamp01(c['Blue Component']);
  if (c['Color Space'] === 'P3') [r, g, b] = p3ToSrgb(r, g, b);
  return toHex(r, g, b);
}

// Colores del perfil; si tiene variantes claro/oscuro ("(Dark)"), se usa la oscura.
function schemeFrom(o, name) {
  const dark = o['Use Separate Colors for Light and Dark Mode'];
  const get = (k) => color((dark && o[`${k} (Dark)`]) || o[k]);
  const ansi = [];
  for (let i = 0; i < 16; i++) ansi.push(get(`Ansi ${i} Color`));
  if (ansi.some((x) => !x)) return null;
  const s = { name, fg: get('Foreground Color') || '#c7c7c7', bg: get('Background Color') || '#000000', ansi };
  const opt = { cursor: 'Cursor Color', cursorText: 'Cursor Text Color', selection: 'Selection Color',
    selectionText: 'Selected Text Color', bold: 'Bold Color', link: 'Link Color', guide: 'Cursor Guide Color' };
  for (const [k, key] of Object.entries(opt)) { const v = get(key); if (v) s[k] = v; }
  if (s.bold && (o['Use Bold Color'] === false || (dark && o['Use Bold Color (Dark)'] === false))) delete s.bold;
  if (!s.cursor) s.cursor = s.fg;
  if (!s.selection) s.selection = '#b5d5ff';
  return s;
}

// ---- teclas ----

// The special-key character codes used in the exported "Keyboard Map".
const NS_KEYS = {
  0xf700: 'ArrowUp', 0xf701: 'ArrowDown', 0xf702: 'ArrowLeft', 0xf703: 'ArrowRight',
  0xf727: 'Insert', 0xf728: 'Delete', 0xf729: 'Home', 0xf72b: 'End', 0xf72c: 'PageUp', 0xf72d: 'PageDown',
  0x0d: 'Enter', 0x03: 'Enter', 0x09: 'Tab', 0x19: 'Tab', 0x1b: 'Escape', 0x7f: 'Backspace', 0x08: 'Backspace', 0x20: 'Space',
};
for (let i = 0; i < 12; i++) NS_KEYS[0xf704 + i] = `F${i + 1}`;
const SHIFTED = { '{': '[', '}': ']', '|': '\\', ':': ';', '"': "'", '<': ',', '>': '.', '?': '/', '~': '`',
  '!': '1', '@': '2', '#': '3', $: '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0', _: '-', '+': '=' };

// "0xf702-0x280000" → "Alt+ArrowLeft" (combinación en el formato de keys.js).
export function itermCombo(spec) {
  const parts = String(spec).split('-');
  const code = parseInt(parts[0], 16), mods = parseInt(parts[1] || '0', 16);
  if (Number.isNaN(code)) return null;
  let key = NS_KEYS[code];
  // the numeric keypad (NSNumericPadKeyMask): macOS also sets it on the arrows, which stay; a keypad
  // character or Enter has no combination here and would take the main key of the same character
  // (on a Latin American keyboard, the key that types "-" sent "/")
  if ((mods & 0x200000) && (!key || code === 0x03)) return null;
  let shift = !!(mods & 0x20000);
  if (code === 0x19) shift = true;
  if (!key) {
    if (code < 0x21 || code > 0x7e) return null;
    let ch = String.fromCharCode(code);
    if (SHIFTED[ch]) { ch = SHIFTED[ch]; shift = true; }
    if (ch >= 'A' && ch <= 'Z') { ch = ch.toLowerCase(); shift = true; }
    key = ch;
  }
  const out = [];
  if (mods & 0x100000) out.push('Super');
  if (mods & 0x40000) out.push('Ctrl');
  if (mods & 0x80000) out.push('Alt');
  if (shift) out.push('Shift');
  out.push(key);
  return out.join('+');
}

// Exported key actions (KEY_ACTION_*) that exist in LordOS.
const ITERM_ACTIONS = {
  0: 'nextTab', 2: 'prevTab', 4: 'scrollBottom', 5: 'scrollTop', 6: 'scrollLineDown', 7: 'scrollLineUp',
  8: 'scrollPageDown', 9: 'scrollPageUp', 18: 'paneLeft', 19: 'paneRight', 20: 'paneUp', 21: 'paneDown',
  23: 'fullscreen', 31: 'moveTabLeft', 32: 'moveTabRight', 34: 'find', 42: 'undoClose',
};

export function itermKeyMap(map, warnings, where) {
  const out = [];
  let skipped = 0;
  for (const [spec, a] of Object.entries(map || {})) {
    const combo = itermCombo(spec);
    const act = Number(a?.Action);
    const text = a?.Text ?? '';
    if (!combo) { skipped++; continue; }
    if (act === 10) out.push({ combo, action: 'escape', param: text });
    else if (act === 11) out.push({ combo, action: 'hex', param: text });
    else if (act === 12 || act === 36) out.push({ combo, action: 'text', param: text });
    else if (act === 13) out.push({ combo, action: 'ignore', param: '' });
    else if (act === 16) out.push({ combo, action: 'hex', param: '0x08' });
    else if (act === 17) out.push({ combo, action: 'hex', param: '0x7f' });
    else if (ITERM_ACTIONS[act]) out.push({ combo, action: ITERM_ACTIONS[act], param: '' });
    else skipped++;
  }
  if (skipped) warnings.push(`${where}: ${T('terminal.iterm.keysSkipped', { n: skipped })}`);
  return out;
}

// ---- perfil ----

const TRIGGERS = {
  AlertTrigger: 'notify', UserNotificationTrigger: 'notify', BellTrigger: 'bell', BounceTrigger: 'attention',
  SendTextTrigger: 'send', CaptureTrigger: 'capture', HighlightTrigger: 'highlight', MarkTrigger: 'mark',
  SetTitleTrigger: 'title', StopTrigger: 'stop',
};

const macPath = (p) => String(p || '').replace(/^\/Users\/[^/]+/, '~');

function nearestScrollback(o) {
  if (o['Unlimited Scrollback']) return 1000000;
  const n = Number(o['Scrollback Lines']);
  return n > 0 ? Math.min(1000000, n) : DEFAULT_PROFILE.scrollback;
}

export function convertProfile(o, fonts, warnings) {
  const name = String(o.Name || 'Imported');
  const w = (msg) => warnings.push(`${name}: ${msg}`);
  const p = { ...DEFAULT_PROFILE, id: newProfileId() + Math.random().toString(36).slice(2, 5), name, itermGuid: o.Guid || '' };

  const scheme = schemeFrom(o, name);
  if (scheme) { p.colors = 'custom'; p.custom = scheme; } else w(T('terminal.iterm.noAnsi'));

  // fuente: "JetBrainsMono-Regular 13"
  const font = String(o['Normal Font'] || '');
  const fm = /^(.*)\s+([\d.]+)$/.exec(font);
  if (fm) {
    p.fontSize = Math.max(7, Math.min(40, Math.round(Number(fm[2]))));
    const f = fonts.byPs(fm[1]);
    if (f) p.font = f.family;
    else if (!/^JetBrainsMono/i.test(fm[1])) w(T('terminal.iterm.noFont', { font: fm[1] }));
  }
  if (o['Use Non-ASCII Font'] && o['Non Ascii Font']) {
    const nm = /^(.*)\s+[\d.]+$/.exec(String(o['Non Ascii Font']));
    const f = nm && fonts.byPs(nm[1]);
    if (f) p.nonAsciiFont = f.family; else w(T('terminal.iterm.noNaFont', { font: nm ? nm[1] : '' }));
  }
  if (o['ASCII Ligatures'] != null) p.ligatures = !!o['ASCII Ligatures'];
  if (o['Vertical Spacing'] != null) p.lineSpacing = Math.max(0.8, Math.min(2, Math.round(Number(o['Vertical Spacing']) * 100) / 100));
  if (o['Horizontal Spacing'] != null && Math.abs(Number(o['Horizontal Spacing']) - 1) > 0.01) w(T('terminal.iterm.hspacing'));
  if (o['Use Bold Font'] != null) p.useBoldFont = !!o['Use Bold Font'];
  if (o['Use Bright Bold'] != null) p.boldBright = !!o['Use Bright Bold'];
  if (o['Use Italic Font'] != null) p.useItalic = !!o['Use Italic Font'];
  if (o['Blink Allowed'] != null) p.blinkText = !!o['Blink Allowed'];
  if (o['Minimum Contrast'] != null) p.minimumContrast = Math.round(clamp01(o['Minimum Contrast']) * 100) / 100;

  // cursor
  const ct = Number(o['Cursor Type']);
  if (ct === 0) p.cursorStyle = 'underline'; else if (ct === 1) p.cursorStyle = 'beam'; else if (ct === 2) p.cursorStyle = 'block';
  if (o['Blinking Cursor'] != null) p.cursorBlink = !!o['Blinking Cursor'];
  if (o['Use Cursor Guide'] != null) p.cursorGuide = !!o['Use Cursor Guide'];

  // ventana
  if (o.Transparency != null) p.transparency = Math.round(Math.min(0.9, clamp01(o.Transparency)) * 100) / 100;
  if (o.Blur != null) p.blur = !!o.Blur;
  if (o.Columns > 0 && o.Rows > 0) { p.columns = Math.min(400, Number(o.Columns)); p.rows = Math.min(200, Number(o.Rows)); }
  if (o['Show Status Bar'] != null) p.statusBar = !!o['Show Status Bar'];
  if (o['Background Image Location']) w(T('terminal.iterm.bgImage'));

  // terminal
  p.scrollback = nearestScrollback(o);
  if (o['Mouse Reporting'] != null) p.mouseReporting = !!o['Mouse Reporting'];
  if (o['Option Key Sends'] != null) p.altSendsEscape = Number(o['Option Key Sends']) !== 0;
  if (o['Silence Bell']) p.bell = 'none';
  if (o['Prompt Before Closing 2'] != null) p.confirmClose = Number(o['Prompt Before Closing 2']) !== 0;
  if (o['Character Encoding'] && Number(o['Character Encoding']) !== 4) w(T('terminal.iterm.encoding'));
  if (o['Ambiguous Double Width']) w(T('terminal.iterm.ambiguous'));

  // general
  const cd = o['Custom Directory'];
  if (cd === 'Yes' && o['Working Directory']) { p.workingDir = 'custom'; p.customDir = macPath(o['Working Directory']); }
  else if (cd === 'Recycle' || cd === 'Advanced') p.workingDir = 'reuse';
  else if (cd === 'No') p.workingDir = 'home';
  if (o['Badge Text']) {
    p.badge = String(o['Badge Text']);
    if (/\\\(/.test(p.badge)) w(T('terminal.iterm.badgeVars'));
  }
  if (o['Use Tab Color'] && o['Tab Color']) p.tabColor = color(o['Tab Color']) || '';
  if (o['Initial Text']) p.initialText = String(o['Initial Text']);
  if (o['Custom Command'] === 'Yes' && o.Command) {
    // LordOS solo abre el shell del usuario: el comando se escribe en él al abrir la sesión.
    p.initialText = `${o.Command}${p.initialText ? `\n${p.initialText}` : ''}`;
    w(T('terminal.iterm.command', { cmd: o.Command }));
  }

  // cambio automático de perfil: "usuario@host", "host", "/ruta", "&trabajo"
  const bound = Array.isArray(o['Bound Hosts']) ? o['Bound Hosts'] : [];
  const rules = bound.map((r) => String(r).trim()).filter((r) => r && !r.startsWith('&')).map(macPath);
  if (bound.some((r) => String(r).startsWith('&'))) w(T('terminal.iterm.jobRules'));
  if (rules.length) p.rules = rules.join('\n');

  // disparadores
  const trig = Array.isArray(o.Triggers) ? o.Triggers : [];
  let tskip = 0;
  p.triggers = [];
  for (const t of trig) {
    const action = TRIGGERS[t.action];
    if (!action || !t.regex) { tskip++; continue; }
    let param = t.parameter == null ? '' : String(t.parameter);
    if (action === 'highlight' && typeof t.parameter === 'object') param = '';
    p.triggers.push({ regex: String(t.regex), action, param, enabled: t.disabled !== true });
  }
  if (tskip) w(T('terminal.iterm.triggersSkipped', { n: tskip }));

  // teclas del perfil
  p.keyMap = itermKeyMap(o['Keyboard Map'], warnings, name);

  if (o['Dynamic Profile Parent Name']) w(T('terminal.iterm.parent', { name: o['Dynamic Profile Parent Name'] }));
  return p;
}

// ---- entrada ----

function isColorsDict(o) { return o && typeof o === 'object' && 'Ansi 0 Color' in o && !('Name' in o && 'Guid' in o); }

// data: string (JSON / XML) o ArrayBuffer (plist binario). fonts: { byPs(name) → { family } | null }.
export function importIterm(data, fileName, fonts) {
  const warnings = [];
  let doc;
  if (typeof data !== 'string') {
    const head = String.fromCharCode(...new Uint8Array(data.slice(0, 8)));
    if (head !== 'bplist00') throw new Error(T('terminal.iterm.unknownBinary'));
    doc = parseBinaryPlist(data);
  }
  if (doc === undefined) {
    const text = data.trim();
    if (text.startsWith('{') || text.startsWith('[')) doc = JSON.parse(text);
    else if (text.startsWith('<')) doc = parseXmlPlist(text);
    else throw new Error(T('terminal.iterm.notIterm'));
  }
  const out = { profiles: [], schemes: [], keyMap: [], defaultGuid: null, warnings };
  const baseName = String(fileName || 'Imported').split('/').pop().replace(/\.(itermcolors|json|plist)$/i, '');

  if (isColorsDict(doc)) {
    const s = schemeFrom(doc, baseName);
    if (!s) throw new Error(T('terminal.iterm.no16'));
    out.schemes.push(s);
    return out;
  }
  let list = [];
  if (Array.isArray(doc)) list = doc;
  else if (Array.isArray(doc?.Profiles)) list = doc.Profiles;
  else if (Array.isArray(doc?.['New Bookmarks'])) {
    list = doc['New Bookmarks'];
    out.defaultGuid = doc['Default Bookmark Guid'] || null;
    if (doc.GlobalKeyMap) out.keyMap = itermKeyMap(doc.GlobalKeyMap, warnings, 'Atajos globales');
    if (doc['Custom Color Presets']) for (const [n, c] of Object.entries(doc['Custom Color Presets'])) { const s = schemeFrom(c, n); if (s) out.schemes.push(s); }
  } else if (doc && typeof doc === 'object' && ('Name' in doc || 'Guid' in doc)) list = [doc];
  if (!list.length && !out.schemes.length) throw new Error(T('terminal.iterm.noProfiles'));
  for (const o of list) if (o && typeof o === 'object') out.profiles.push(convertProfile(o, fonts, warnings));
  return out;
}
