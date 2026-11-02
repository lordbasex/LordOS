// sdk.jsx — the few parts of @lordos/sdk the imager's UI uses, for a browser view (the Tauri app's
// window). The shared UI (apps/imager) is built against this module instead of LordOS's engine:
// View, Text, Image, ScrollView, TextInput, Button, Toggle, Segmented, SearchField, Lumo,
// LordOSLogo, useTheme, useLocale, t, tOptional, formatDate, formatNumber, createStore, useStore,
// useKeyboard, typeScale, spacing. Styles are LordOS's (React Native-like objects, arrays of them)
// turned into CSS; colors and sizes are Lord Flow's (themes/default/tokens.js). docs/IMAGER.md.
import { createElement as h, useEffect, useMemo, useState, useSyncExternalStore, useRef } from 'react';
import { catalogs } from './catalogs.js';

// ---- stores ----

export function createStore(initial) {
  let v = initial;
  const subs = new Set();
  return {
    get: () => v,
    set: (next) => { v = typeof next === 'function' ? next(v) : next; for (const f of subs) f(); },
    subscribe: (f) => { subs.add(f); return () => subs.delete(f); },
  };
}
export const useStore = (s) => useSyncExternalStore(s.subscribe, s.get);

// ---- theme: Lord Flow, light and dark (themes/default/tokens.js) ----

const ACCENTS = { azul: '#0a84ff', violeta: '#bf5af2', rosa: '#ff375f', naranja: '#ff9f0a', verde: '#30d158', grafito: '#8e8e93' };
function makeTheme(mode) {
  const dark = mode === 'dark';
  return {
    mode, accent: ACCENTS.azul, accents: ACCENTS,
    text: dark ? '#f5f5f7' : '#1d1d1f', textDim: dark ? 'rgba(235,235,245,0.55)' : 'rgba(60,60,67,0.6)',
    windowBg: dark ? '#1f1f23' : '#ffffff', sidebarBg: dark ? '#26262b' : '#f3f3f6',
    separator: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)', hover: dark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)',
    control: dark ? '#3a3a40' : '#e3e3e8', inputBg: dark ? '#2a2a2f' : '#ffffff',
    controlRadius: 7, controlRadiusSmall: 6, groupRadius: 10, menuRadius: 8,
  };
}
const media = typeof window !== 'undefined' && window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
export const appearance = createStore('auto');          // 'auto' | 'light' | 'dark' (App options)
const systemDark = createStore(!!media?.matches);
media?.addEventListener?.('change', (e) => systemDark.set(e.matches));
export function useTheme() {
  const a = useStore(appearance);
  const sys = useStore(systemDark);
  const mode = a === 'auto' ? (sys ? 'dark' : 'light') : a;
  return useMemo(() => makeTheme(mode), [mode]);
}

// ---- language ----

export const locale = createStore({ lang: 'en' });
function lookup(lang, key) {
  const dot = key.indexOf('.');
  const ns = key.slice(0, dot), rest = key.slice(dot + 1);
  const c = catalogs[lang]?.[ns];
  return c ? c[rest] : undefined;
}
const plural = (lang, n) => (lang === 'pt' ? (n === 0 || n === 1 ? 'one' : 'other') : n === 1 ? 'one' : 'other');
export function t(key, vars = {}) {
  const lang = locale.get().lang;
  let v = lookup(lang, key);
  if (v === undefined) v = lookup('en', key);
  if (v === undefined) return key;
  if (typeof v === 'object') v = v[plural(lang, Number(vars.count ?? vars.n ?? 0))] ?? v.other ?? '';
  return String(v).replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
}
export function tOptional(key, vars) { const r = t(key, vars); return r === key ? '' : r; }
export function useLocale() { const l = useStore(locale); return { t, lang: l.lang }; }
export const setLanguage = (lang) => { locale.set({ lang: ['en', 'es', 'pt'].includes(lang) ? lang : 'en' }); document.documentElement.lang = locale.get().lang; };
export const direction = () => 'ltr';
const intlLang = () => ({ en: 'en-US', es: 'es-AR', pt: 'pt-BR' }[locale.get().lang] || 'en-US');
export function formatNumber(n, digits = null) {
  return new Intl.NumberFormat(intlLang(), digits == null ? {} : { minimumFractionDigits: digits, maximumFractionDigits: digits }).format(n);
}
export function formatDate(d, style = 'short') {
  return new Intl.DateTimeFormat(intlLang(), style === 'long' ? { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' } : { dateStyle: 'medium' }).format(d);
}

export const typeScale = { caption: 11, body: 13, callout: 12, headline: 15, title: 17, largeTitle: 22 };
export const spacing = { xxs: 2, xs: 4, s: 8, m: 12, l: 16, xl: 20, xxl: 24, xxxl: 32 };

// ---- styles: LordOS / React Native objects → CSS ----

const UNITLESS = new Set(['flex', 'flexGrow', 'flexShrink', 'opacity', 'fontWeight', 'zIndex', 'order']);
const LOGICAL = {
  paddingHorizontal: ['paddingLeft', 'paddingRight'], paddingVertical: ['paddingTop', 'paddingBottom'],
  marginHorizontal: ['marginLeft', 'marginRight'], marginVertical: ['marginTop', 'marginBottom'],
  marginInlineStart: ['marginInlineStart'], marginInlineEnd: ['marginInlineEnd'],
  borderStartWidth: ['borderInlineStartWidth'], borderEndWidth: ['borderInlineEndWidth'],
  borderStartColor: ['borderInlineStartColor'], borderEndColor: ['borderInlineEndColor'],
  borderBottomStartRadius: ['borderEndStartRadius'], borderTopStartRadius: ['borderStartStartRadius'],
  insetInlineStart: ['insetInlineStart'], insetInlineEnd: ['insetInlineEnd'],
};
function flatten(style, out = {}) {
  if (!style) return out;
  if (Array.isArray(style)) { for (const s of style) flatten(s, out); return out; }
  Object.assign(out, style);
  return out;
}
function css(style, base) {
  const s = flatten(style, { ...base });
  const o = {};
  for (const [k, v] of Object.entries(s)) {
    if (v === undefined || v === null) continue;
    let val = v;
    if (k === 'flex' && typeof v === 'number') { o.flexGrow = v; o.flexShrink = v ? 1 : 0; o.flexBasis = v ? 0 : 'auto'; continue; }
    if (k === 'lineHeight' && typeof v === 'number') { o.lineHeight = v < 4 ? String(v) : `${v}px`; continue; }
    if (k === 'transform' && Array.isArray(v)) { val = v.map((x) => Object.entries(x).map(([f, a]) => `${f}(${typeof a === 'number' && f !== 'scale' ? `${a}px` : a})`).join(' ')).join(' '); }
    if (k === 'border' || k === 'background' || k === 'boxShadow') { o[k] = val; continue; }
    if (typeof val === 'number' && !UNITLESS.has(k)) val = `${val}px`;
    if (LOGICAL[k]) { for (const p of LOGICAL[k]) o[p] = val; if (k.startsWith('border') && k.endsWith('Width')) o[LOGICAL[k][0].replace('Width', 'Style')] = 'solid'; continue; }
    if (/^border(Top|Bottom|Left|Right)Width$/.test(k)) o[k.replace('Width', 'Style')] = 'solid';
    o[k] = val;
  }
  return o;
}
const VIEW = { display: 'flex', flexDirection: 'column', position: 'relative', boxSizing: 'border-box', minWidth: 0, flexShrink: 0 };
const PASS = /^(aria-|data-)|^(role|tabIndex|onKeyDown|title|id)$/;
function passProps(props) {
  const o = {};
  for (const [k, v] of Object.entries(props)) if (PASS.test(k)) o[k] = v;
  return o;
}
function events(props) {
  const o = {};
  if (props.onClick) o.onClick = (e) => { e.stopPropagation(); props.onClick(e); };
  for (const k of ['onMouseEnter', 'onMouseLeave', 'onMouseDown', 'onWheel']) if (props[k]) o[k] = props[k];
  return o;
}

export function View(props) {
  const st = css(props.style, VIEW);
  if (props.onClick && !st.cursor) st.cursor = 'pointer';
  return h('div', { style: st, ...events(props), ...passProps(props) }, props.children);
}
export function Text(props) {
  const st = css(props.style, { display: 'block', boxSizing: 'border-box', whiteSpace: 'pre-wrap', overflowWrap: 'anywhere', minWidth: 0, flexShrink: 1 });
  if (props.numberOfLines) Object.assign(st, { display: '-webkit-box', WebkitLineClamp: props.numberOfLines, WebkitBoxOrient: 'vertical', overflow: 'hidden' });
  return h('div', { style: st, ...events(props), ...passProps(props) }, props.children);
}
// asset:mascot/x.png → ./assets/mascot/x.png (copied next to the page by build-web.mjs)
const src = (s) => String(s || '').replace(/^asset:/, './assets/');
export function Image(props) {
  return h('img', { src: src(props.src), alt: props.alt || '', draggable: false, style: css(props.style, { display: 'block', objectFit: 'contain', flexShrink: 0 }), ...passProps(props) });
}
export function ScrollView(props) {
  return h('div', { className: 'lx-scroll', style: css(props.style, { ...VIEW, overflowY: 'auto', overflowX: 'hidden', flexShrink: 1, minHeight: 0 }), ...passProps(props) },
    h('div', { style: css(props.contentStyle, { display: 'flex', flexDirection: 'column', boxSizing: 'border-box', minHeight: 'min-content' }) }, props.children));
}

export function TextInput({ value, onChange, placeholder, onSubmit, onCancel, autoFocus, style, secure, ...rest }) {
  const th = useTheme();
  const ref = useRef(null);
  useEffect(() => { if (autoFocus) ref.current?.focus(); }, [autoFocus]);
  return h('input', {
    ref, value: value ?? '', placeholder, type: secure ? 'password' : 'text', spellCheck: false, autoComplete: secure ? 'new-password' : 'off',
    onChange: (e) => onChange?.(e.target.value),
    onKeyDown: (e) => { if (e.key === 'Enter') onSubmit?.(); else if (e.key === 'Escape') onCancel?.(); },
    className: 'lx-input',
    style: css(style, { height: 28, width: '100%', boxSizing: 'border-box', padding: '0 10px', borderRadius: th.controlRadius, fontSize: 13, color: th.text,
      backgroundColor: th.inputBg, border: `1px solid ${th.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.14)'}`, outline: 'none', fontFamily: 'inherit' }),
    ...passProps(rest),
  });
}

export function Button({ label, onClick, primary, style, small, plain, disabled, destructive, icon }) {
  const th = useTheme();
  const fg = primary ? '#fff' : destructive ? '#ff453a' : th.text;
  const bg = primary ? th.accent : plain ? 'transparent' : th.control;
  return h('button', {
    type: 'button', disabled, onClick: (e) => { e.stopPropagation(); onClick?.(e); }, className: plain ? 'lx-button lx-plain' : 'lx-button',
    style: css(style, { height: small ? 22 : 28, padding: `0 ${small ? 10 : 14}px`, borderRadius: small ? th.controlRadiusSmall : th.controlRadius, border: 'none',
      display: 'inline-flex', gap: 6, alignItems: 'center', justifyContent: 'center', backgroundColor: bg, color: fg, fontSize: small ? 12 : 13, fontWeight: 500,
      fontFamily: 'inherit', whiteSpace: 'nowrap', cursor: disabled ? 'default' : 'pointer', opacity: disabled ? 0.4 : 1, flexShrink: 0 }),
  }, icon ? h('span', null, icon) : null, label);
}
export function Toggle({ on, onChange, small }) {
  const th = useTheme();
  const w = small ? 36 : 42, hh = small ? 20 : 24, k = hh - 4;
  return h('div', { role: 'switch', 'aria-checked': !!on, tabIndex: 0, onClick: () => onChange(!on), style: { width: w, height: hh, borderRadius: hh / 2, position: 'relative', backgroundColor: on ? th.accent : th.control, cursor: 'pointer' } },
    h('div', { style: { position: 'absolute', top: 2, left: on ? w - k - 2 : 2, width: k, height: k, borderRadius: k / 2, backgroundColor: '#fff', boxShadow: '0 1px 3px rgba(0,0,0,0.35)' } }));
}
export function Segmented({ options, value, onChange }) {
  const th = useTheme();
  return h('div', { role: 'radiogroup', style: { display: 'flex', backgroundColor: th.control, borderRadius: th.controlRadius + 1, padding: 2 } },
    options.map(([v, label]) => h('div', { key: v, role: 'radio', 'aria-checked': value === v, tabIndex: 0, onClick: () => onChange(v),
      style: { padding: '0 14px', height: 24, borderRadius: th.controlRadius - 1, display: 'flex', alignItems: 'center', backgroundColor: value === v ? th.accent : 'transparent', color: value === v ? '#fff' : th.text, fontSize: 12, fontWeight: 600, cursor: 'pointer' } }, label)));
}
export function SearchField(props) { return h(TextInput, props); }

// ---- brand ----

const POSES = new Set(['wave', 'shield', 'celebrate', 'confused', 'sleep', 'run']);
export function Lumo({ pose = 'confused', size = 96, alt, style }) {
  const p = POSES.has(pose) ? pose : 'confused';
  return h(Image, { src: `asset:mascot/lumo-${p}-256.png`, alt: alt || t(`mascot.${p}`), style: [{ width: size, height: size }, style] });
}
export function LordOSLogo({ size = 64 }) {
  return h(Image, { src: 'asset:icons/lordos-icon-128.png', alt: 'LordOS', style: { width: size, height: size, borderRadius: size * 0.22, boxShadow: '0 2px 6px rgba(0,0,0,0.3)' } });
}

// ---- keyboard ----

export function useKeyboard(fn) {
  const ref = useRef(fn);
  ref.current = fn;
  useEffect(() => {
    const on = (e) => ref.current({ key: e.key, shift: e.shiftKey, ctrl: e.ctrlKey, alt: e.altKey, meta: e.metaKey, preventDefault: () => e.preventDefault() });
    window.addEventListener('keydown', on);
    return () => window.removeEventListener('keydown', on);
  }, []);
}

// unused by the imager on this platform, present so the shared files import cleanly
export const admin = null, disks = null, fs = null, settings = null, keyboard = null, sound = null, notify = () => 0, FilePicker = () => null;
