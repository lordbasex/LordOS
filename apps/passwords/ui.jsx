// ui.jsx — small pieces of Passwords: category and item icons, sheets (dialogs), the strength
// bar, a QR code image, a multi-line field, a file picker and pop-up menus. Spacing, type and
// controls follow docs/DESIGN.md; icons are drawn (glyphs.jsx), never color emoji.
import { View, Text, Image, useTheme, useStore, createStore, Button, ScrollView, TextInput, ToolbarButton, takeTextFocus, releaseTextFocus, fs, t, Overlay } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { builtinById } from './templates.js';
import { appMode } from './api.js';
import { Glyph, Tile, iconKind, tint } from './glyphs.jsx';

// A picked file is a path (shell: axia-server opens it, validated like lord.fs) or { fd, name }
// (app process: the system dialog opened it). fileReq builds the request options for either.
export function fileReq(...items) {
  const files = [], fds = [];
  for (const [p, mode] of items) {
    if (!p) continue;
    if (typeof p === 'string') files.push({ path: p, mode }); else fds.push(p.fd);
  }
  return fds.length ? { fds } : { files };
}
export const fileName = (p) => (typeof p === 'string' ? p : p?.name || '');

export const RED = '#ff453a', ORANGE = '#ff9f0a', GREEN = '#30d158', YELLOW = '#ffd60a';

// ---- icons ----

// A category's tile: its color and its glyph (a user category stores the glyph's name; older
// versions stored an emoji, mapped by iconKind).
export function CatIcon({ cat, size = 28 }) {
  const b = cat ? builtinById(cat.id) : null;
  const color = cat?.color || b?.color || '#8e8e93';
  return <Tile kind={iconKind(cat?.icon || b?.icon)} color={color} size={size} />;
}

// The site's initial (or the title's) on the category color: like site icons, without downloading anything
export function EntryIcon({ entry, cat, size = 32 }) {
  const b = cat ? builtinById(cat.id) : null;
  const color = cat?.color || b?.color || '#8e8e93';
  const host = String(entry.url || '').replace(/^[a-z]+:\/\//i, '').replace(/^www\./, '');
  const useLetter = host || ['internet', 'general', 'email', 'apps', 'secrets', 'keyring'].includes(entry.cat);
  if (!useLetter) return <CatIcon cat={cat} size={size} />;
  const ch = ([...(host || entry.title || '?')][0] || '?').toUpperCase();
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.26, alignItems: 'center', justifyContent: 'center',
      background: `linear-gradient(180deg, ${tint(color, 0.18)}, ${tint(color, -0.12)})` }}>
      <Text style={{ fontSize: Math.round(size * 0.46), fontWeight: 600, color: '#fff' }}>{ch}</Text>
    </View>
  );
}

export const shade = tint;

// ---- sheet (a dialog over the window) ----

// Sheets are drawn over the whole window (not inside the pane that opens them): each <Sheet>
// registers itself in `overlays` and <Overlays /> (at the end of the app) shows them.
const overlays = createStore([]);           // [{ key, node }]
let overlaySeq = 0;

export function Sheet(props) {
  // inline: the panel goes in the layout where it is written, without the dimmed overlay. The lock
  // and create screens use it so the dialog does not cover the whole (small) form behind it.
  if (props.inline) return <SheetPanel {...props} inline />;
  return <OverlaySheet {...props} />;
}

function OverlaySheet(props) {
  const key = useRef(null);
  if (!key.current) key.current = `s${++overlaySeq}`;
  useEffect(() => {
    const node = <SheetBox {...props} />;
    overlays.set((l) => (l.some((x) => x.key === key.current) ? l.map((x) => (x.key === key.current ? { key: x.key, node } : x)) : [...l, { key: key.current, node }]));
  });
  useEffect(() => () => overlays.set((l) => l.filter((x) => x.key !== key.current)), []);
  return null;
}

export function Overlays() {
  const l = useStore(overlays);
  return l.map((x) => <View key={x.key} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }}>{x.node}</View>);
}

function SheetBox(props) {
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, zIndex: 50,
      alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.38)' }}>
      <SheetPanel {...props} />
    </View>
  );
}

function SheetPanel({ title, onClose, children, width = 460, footer, inline }) {
  const th = useTheme();
  return (
    <View style={{ width: inline ? undefined : width, maxWidth: inline ? undefined : '92%', alignSelf: inline ? 'stretch' : undefined,
      maxHeight: inline ? undefined : '92%', borderRadius: 12, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
      boxShadow: inline ? '0 4px 14px rgba(0,0,0,0.18)' : '0 18px 50px rgba(0,0,0,0.45)', overflow: 'hidden' }}>
      <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 20, borderBottomWidth: 1, borderBottomColor: th.separator }}>
        <Text style={{ flex: 1, minWidth: 0, fontSize: 15, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{title}</Text>
        {onClose && <ToolbarButton small icon="✕" iconSize={11} title={t('passwords.close')} onClick={onClose} />}
      </View>
      {inline ? <View style={{ paddingHorizontal: 20, paddingVertical: 16, gap: 12 }}>{children}</View> : (
        // a body taller than the window scrolls instead of being cut off (small windows)
        <ScrollView style={{ flex: -1 }}>
          <View style={{ paddingHorizontal: 20, paddingVertical: 16, gap: 12 }}>{children}</View>
        </ScrollView>
      )}
      {footer && <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, paddingHorizontal: 20, paddingVertical: 12, borderTopWidth: 1, borderTopColor: th.separator }}>{footer}</View>}
    </View>
  );
}

// Caption over a field (11 px, 600, dim).
export function Label({ children }) {
  const th = useTheme();
  return <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim }}>{children}</Text>;
}

// One hint line (12 px, dim).
export function Hint({ children, color }) {
  const th = useTheme();
  return <Text style={{ fontSize: 12, color: color || th.textDim, lineHeight: 17 }}>{children}</Text>;
}

// A calm warning box: a monochrome sign and the text, on a soft tint.
export function Warning({ children, tone = 'red' }) {
  const th = useTheme();
  const c = tone === 'orange' ? ORANGE : RED;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 10, paddingHorizontal: 12, paddingVertical: 10, borderRadius: 10,
      backgroundColor: tone === 'orange' ? 'rgba(255,159,10,0.12)' : 'rgba(255,69,58,0.10)', border: `1px solid ${tone === 'orange' ? 'rgba(255,159,10,0.35)' : 'rgba(255,69,58,0.32)'}` }}>
      <Text style={{ fontSize: 13, lineHeight: 17, fontWeight: 600, color: c }}>⚠</Text>
      <Text style={{ flex: 1, fontSize: 12, color: th.mode === 'dark' ? '#ffb3ad' : '#c4281c', lineHeight: 17 }}>{children}</Text>
    </View>
  );
}

// A grouped ("inset") box as in docs/DESIGN.md: rows separated by hairlines.
export function Group({ children, style }) {
  const th = useTheme();
  const rows = (Array.isArray(children) ? children.flat() : [children]).filter(Boolean);
  return (
    <View style={[{ borderRadius: 10, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)',
      border: `1px solid ${th.separator}`, paddingHorizontal: 12 }, style]}>
      {rows.map((r, i) => <View key={i} style={i ? { borderTopWidth: 1, borderTopColor: th.separator } : null}>{r}</View>)}
    </View>
  );
}

// A checkbox with its label (16 px box, accent when on).
export function Check({ on, onChange, label, color }) {
  const th = useTheme();
  return (
    <View onClick={() => onChange(!on)} style={{ flexDirection: 'row', gap: 8, alignItems: 'center', minHeight: 24 }}>
      <View style={{ width: 16, height: 16, borderRadius: 4, border: `1px solid ${on ? 'transparent' : th.textDim}`,
        backgroundColor: on ? (color || th.accent) : th.inputBg, alignItems: 'center', justifyContent: 'center' }}>
        {on && <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff' }}>✓</Text>}
      </View>
      <Text style={{ flex: 1, fontSize: 13, color: th.text, lineHeight: 18 }}>{label}</Text>
    </View>
  );
}

// ---- strength ----

export function strengthColor(score) { return [RED, RED, ORANGE, YELLOW, GREEN][Math.max(0, Math.min(4, score | 0))]; }
export function strengthLabel(score) { return [t('passwords.strength.0'), t('passwords.strength.1'), t('passwords.strength.2'), t('passwords.strength.3'), t('passwords.strength.4')][Math.max(0, Math.min(4, score | 0))]; }

export function StrengthBar({ score, bits }) {
  const th = useTheme();
  return (
    <View style={{ gap: 6 }}>
      <View style={{ flexDirection: 'row', gap: 4, direction: 'ltr' }}>
        {[0, 1, 2, 3].map((i) => (
          <View key={i} style={{ flex: 1, height: 4, borderRadius: 2, backgroundColor: i < Math.max(1, score) ? strengthColor(score) : th.control }} />
        ))}
      </View>
      <Text style={{ fontSize: 11, fontWeight: 500, color: strengthColor(score) }}>{bits != null ? t('passwords.strength.bits', { label: strengthLabel(score), bits }) : strengthLabel(score)}</Text>
    </View>
  );
}

// ---- QR code (hex rows from axia-vault → pixels) ----

export function QrImage({ qr, scale = 5 }) {
  if (!qr || !qr.size) return null;
  const quiet = 4, n = qr.size, side = (n + quiet * 2) * scale;
  const data = new Uint8Array(side * side * 4).fill(255);
  for (let y = 0; y < n; y++) {
    const row = qr.rows[y];
    for (let x = 0; x < n; x++) {
      const nib = parseInt(row[x >> 2], 16);
      if (!((nib >> (3 - (x & 3))) & 1)) continue;
      for (let dy = 0; dy < scale; dy++) {
        let o = (((y + quiet) * scale + dy) * side + (x + quiet) * scale) * 4;
        for (let dx = 0; dx < scale; dx++, o += 4) { data[o] = 0; data[o + 1] = 0; data[o + 2] = 0; }
      }
    }
  }
  return <Image pixels={{ width: side, height: side, data }} style={{ width: side, height: side, borderRadius: 8 }} />;
}

// ---- multi-line field (notes, private keys, seed phrases) ----

export function TextArea({ value, onChange, placeholder, minHeight = 90, mono, secure }) {
  const th = useTheme();
  const token = useRef({}).current;
  const [focus, setFocus] = useState(false);
  const latest = useRef();
  latest.current = { value, onChange };
  useEffect(() => () => releaseTextFocus(token), []);
  const onKey = (e) => {
    const { value: v, onChange: set } = latest.current;
    if (e.ctrl && e.base === 'v') { try { set(v + lord.clipboard.get()); } catch (err) { /* no clipboard permission */ } return; }
    if (e.key === 'Backspace') set([...v].slice(0, -1).join(''));
    else if (e.key === 'Enter') set(`${v}\n`);
    else if (e.key === 'Tab') set(`${v}  `);
    else if (e.key === 'Escape') { releaseTextFocus(token); setFocus(false); }
    else if (e.char) set(v + e.char);
  };
  const shown = secure && !focus ? String(value || '').replace(/[^\n]/g, '•') : value;
  return (
    <View onMouseDown={() => { takeTextFocus(token, onKey, () => setFocus(false)); setFocus(true); }}
      style={{ minHeight, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 7, backgroundColor: th.inputBg, border: `1px solid ${focus ? th.accent : th.separator}` }}>
      {value ? <Text style={{ fontSize: 13, color: th.text, lineHeight: 18, fontFamily: mono ? 'monospace' : undefined }}>{shown}{focus ? '▏' : ''}</Text>
        : <Text style={{ fontSize: 13, color: th.textDim }}>{focus ? '▏' : placeholder || ''}</Text>}
    </View>
  );
}

// ---- pick a file (home folder and the user's disks: USB sticks) ----

export function FilePicker({ title, save, suggest = '', onPick, onClose, filter, inline }) {
  const th = useTheme();
  const [dir, setDir] = useState(null);            // null = places
  const [name, setName] = useState(suggest);
  const [sel, setSel] = useState(null);
  // app process: the system's own dialog (powerbox) picks the file and hands over only its fd
  useEffect(() => {
    if (!appMode) return;
    let r = null;
    try { r = lord.broker.pick(save ? 1 : 0, suggest || ''); } catch (e) { r = null; }
    if (r && r.fd >= 0) onPick({ fd: r.fd, name: r.name }); else onClose();
  }, []);
  if (appMode) return null;
  let entries = [];
  let places = [];
  if (dir === null) {
    places = [{ name: t('passwords.picker.home'), path: fs.home(), icon: 'folder' }];
    try { for (const v of fs.volumes()) places.push({ name: v.name || v.path, path: v.path, icon: 'drive' }); } catch (e) { /* no disks */ }
  } else {
    try { entries = fs.list(dir).filter((e) => e.dir || !filter || filter(e.name)); } catch (e) { entries = []; }
  }
  const up = () => { const h = fs.home(); if (dir === h || dir === null || !dir.includes('/')) setDir(null); else setDir(fs.parent(dir) || null); setSel(null); };
  const choose = () => {
    if (save) { if (dir && name) onPick(fs.join(dir, name)); return; }
    if (sel) onPick(fs.join(dir, sel));
  };
  return (
    <Sheet title={title} onClose={onClose} width={520} inline={inline} footer={[
      <Button key="c" label={t('passwords.cancel')} onClick={onClose} />,
      <Button key="o" primary label={save ? t('passwords.picker.save') : t('passwords.picker.open')} onClick={choose} />]}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <ToolbarButton icon="‹" title={t('passwords.picker.up')} onClick={up} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{dir === null ? t('passwords.picker.places') : dir}</Text>
      </View>
      <View style={{ height: inline ? 190 : 260, borderRadius: 8, border: `1px solid ${th.separator}`, overflow: 'hidden' }}>
        <ScrollView style={{ flex: 1 }}>
          {dir === null ? places.map((p) => (
            <Row key={p.path} icon={p.icon} label={p.name} onClick={() => setDir(p.path)} />
          )) : entries.map((e) => (
            <Row key={e.name} icon={e.dir ? 'folder' : 'note'} label={e.name} selected={sel === e.name}
              onClick={() => (e.dir ? (setDir(fs.join(dir, e.name)), setSel(null)) : (setSel(e.name), save && setName(e.name)))}
              onDoubleClick={() => !e.dir && onPick(fs.join(dir, e.name))} />
          ))}
          {dir !== null && !entries.length && <Text style={{ fontSize: 12, color: th.textDim, margin: 10 }}>{t('passwords.picker.empty')}</Text>}
        </ScrollView>
      </View>
      {save && <TextInput value={name} onChange={setName} placeholder={t('passwords.picker.name')} onSubmit={choose} />}
    </Sheet>
  );
}

function Row({ icon, label, onClick, onDoubleClick, selected }) {
  const th = useTheme();
  return (
    <View onClick={onClick} onDoubleClick={onDoubleClick} style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10,
      marginHorizontal: 4, borderRadius: 6, backgroundColor: selected ? th.accent : 'transparent' }}>
      <Glyph kind={icon} size={15} color={selected ? '#fff' : th.accent} />
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{label}</Text>
    </View>
  );
}

// ---- small pop-up menu ----

export function Choice({ value, options, onChange, width = 200 }) {
  const th = useTheme();
  const [open, setOpen] = useState(false);
  const cur = options.find((o) => o[0] === value);
  const btn = useRef(null);
  return (
    <View style={{ width }}>
      <View ref={btn} onClick={() => setOpen(!open)} style={{ height: 28, paddingHorizontal: 10, borderRadius: 7, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: th.control, border: `1px solid ${open ? th.accent : 'transparent'}` }}>
        <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{cur ? cur[1] : String(value ?? '')}</Text>
        <Text style={{ fontSize: 9, color: th.textDim }}>▼</Text>
      </View>
      {/* the list goes in the window's overlay layer (sdk/layers.js), above every field of the form */}
      {open && (
        <Overlay anchor={btn} onClose={() => setOpen(false)}>
          <View style={{ position: 'absolute', top: 32, insetInlineStart: 0, width, borderRadius: 8, padding: 5,
            backgroundColor: th.mode === 'dark' ? '#2c2c31' : '#ffffff', border: `1px solid ${th.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {options.map(([v, label]) => <ChoiceRow key={String(v)} label={label} on={v === value} onClick={() => { setOpen(false); onChange(v); }} />)}
          </View>
        </Overlay>
      )}
    </View>
  );
}

function ChoiceRow({ label, on, onClick }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 24, paddingHorizontal: 8, borderRadius: 5, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: hover ? th.accent : 'transparent' }}>
      <Text style={{ width: 12, fontSize: 11, color: hover ? '#fff' : th.text }}>{on ? '✓' : ''}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: hover ? '#fff' : th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{label}</Text>
    </View>
  );
}

// Small borderless button with a glyph (copy, show, generate): a string glyph or a drawn kind
// ({ kind: 'eye' }). label: visible text after the glyph; tip: the tooltip (icon-only buttons).
export function IconButton({ glyph, onClick, label, tip, active, color }) {
  const th = useTheme();
  const icon = glyph && typeof glyph === 'object' && glyph.kind ? <Glyph kind={glyph.kind} size={14} color={active ? th.accent : color || th.text} /> : glyph;
  return <ToolbarButton small icon={icon} iconSize={13} label={label} title={tip} active={active} color={color} onClick={onClick} />;
}

export function Chip({ label, color, onClick, onRemove, selected }) {
  const th = useTheme();
  return (
    <View onClick={onClick} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 22, paddingHorizontal: 9, borderRadius: 11,
      backgroundColor: selected ? th.accent : th.control }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: color || '#8e8e93' }} />
      <Text style={{ fontSize: 11, fontWeight: 500, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap' }}>{label}</Text>
      {onRemove && <Text onClick={(e) => { e.stopPropagation?.(); onRemove(); }} style={{ fontSize: 9, color: th.textDim, paddingInlineStart: 2 }}>✕</Text>}
    </View>
  );
}

// base64 → bytes (QuickJS has no atob)
export function b64ToBytes(s) {
  const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const map = new Uint8Array(128);
  for (let i = 0; i < 64; i++) map[A.charCodeAt(i)] = i;
  const clean = String(s).replace(/[^A-Za-z0-9+/]/g, '');
  const out = new Uint8Array(Math.floor((clean.length * 3) / 4));
  let o = 0;
  for (let i = 0; i + 1 < clean.length; i += 4) {
    const a = map[clean.charCodeAt(i)], b = map[clean.charCodeAt(i + 1)];
    const c = i + 2 < clean.length ? map[clean.charCodeAt(i + 2)] : 0, d = i + 3 < clean.length ? map[clean.charCodeAt(i + 3)] : 0;
    const v = (a << 18) | (b << 12) | (c << 6) | d;
    if (o < out.length) out[o++] = (v >> 16) & 255;
    if (o < out.length && i + 2 < clean.length) out[o++] = (v >> 8) & 255;
    if (o < out.length && i + 3 < clean.length) out[o++] = v & 255;
  }
  return out;
}
