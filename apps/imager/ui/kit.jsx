// kit.jsx — the LordOS Imager's small building blocks, shared by the native app and the desktop
// app (desktop/imager builds the same files against a DOM version of the few SDK primitives used).
// Only View, Text, Image, ScrollView, TextInput, Button, Toggle, Segmented, SearchField, Lumo,
// useTheme, useKeyboard, t, typeScale and spacing come from the SDK.
//
// Keyboard: Tab / Shift+Tab move a focus ring through every Press and Field of the visible step,
// Enter or Space activates, Escape goes back (docs/IMAGER.md, "Accessibility"). The desktop app
// uses the browser's own focus instead (platform.domFocus) and gets real screen-reader labels.
import { View, Text, ScrollView, TextInput, Lumo, useTheme, useKeyboard, typeScale, spacing, createStore, useStore } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { platform } from '../platform.js';

// ---- focus ----

const focus = createStore({ id: null, tick: 0 });
let order = [];                       // ids in registration order for this frame
let actions = new Map();              // id → { activate, field }
let seen = new Set();

export function resetFocusOrder() { order = []; seen = new Set(); }
function register(id, a) {
  if (!seen.has(id)) { seen.add(id); order.push(id); }
  actions.set(id, a);
}
export const focusTo = (id) => focus.set({ id, tick: focus.get().tick + 1 });
export function useFocused(id) {
  const f = useStore(focus);
  return f.id === id ? f.tick : 0;
}

// The step root calls this once per render (before its children register)
export function useFocusKeys({ onEscape } = {}) {
  resetFocusOrder();
  useKeyboard((e) => {
    if (platform.domFocus) return;
    if (e.key === 'Tab') {
      e.preventDefault?.();                    // its own order, not the SDK's (sdk/focus.js)
      const ids = order.filter((x) => actions.has(x));
      if (!ids.length) return;
      const cur = ids.indexOf(focus.get().id);
      const next = cur < 0 ? (e.shift ? ids.length - 1 : 0) : (cur + (e.shift ? ids.length - 1 : 1)) % ids.length;
      focusTo(ids[next]);
      return;
    }
    const a = actions.get(focus.get().id);
    if ((e.key === 'Enter' || e.key === ' ') && a && !a.field) { a.activate?.(); return; }
    if (e.key === 'Escape') { if (focus.get().id) focusTo(null); else onEscape?.(); }
  });
}

let autoId = 0;
const useId = (id) => { const r = useRef(id || `f${++autoId}`); return id || r.current; };

// A clickable area with a focus ring and a label for screen readers (desktop).
export function Press({ id, label, role = 'button', onClick, disabled, selected, style, children, ringRadius }) {
  const t = useTheme();
  const fid = useId(id);
  const f = useFocused(fid);
  if (!disabled && onClick) register(fid, { activate: onClick });
  const ring = f && !platform.domFocus ? { boxShadow: `0 0 0 2px ${t.windowBg}, 0 0 0 4px ${t.accent}` } : null;
  return (
    <View onClick={disabled ? undefined : (e) => { focusTo(null); onClick?.(e); }}
      {...platform.a11y({ label, role, selected, disabled, onActivate: disabled ? null : onClick })}
      style={[{ opacity: disabled ? 0.45 : 1 }, style, ring, ringRadius ? { borderRadius: ringRadius } : null]}>
      {children}
    </View>
  );
}

// A labelled text field (with its error or hint underneath)
export function Field({ id, label, value, onChange, placeholder, secure, error, hint, width, suffix, onSubmit, mono }) {
  const t = useTheme();
  const fid = useId(id);
  const f = useFocused(fid);
  register(fid, { field: true });
  return (
    <View style={{ gap: 5 }}>
      {label ? <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{label}</Text> : null}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <View style={{ width: width || undefined, flex: width ? 0 : 1, borderRadius: t.controlRadius + 2,
          boxShadow: f && !platform.domFocus ? `0 0 0 2px ${t.accent}` : 'none' }}>
          <TextInput key={f ? `f${f}` : 'n'} autoFocus={!!f} value={value} onChange={onChange} placeholder={placeholder} secure={secure} eye={false}
            onSubmit={onSubmit} style={mono ? { fontFamily: 'monospace' } : undefined} {...platform.a11y({ label: label || placeholder, role: 'textbox', invalid: !!error })} />
        </View>
        {suffix ? <Text style={{ fontSize: typeScale.body, color: t.textDim }}>{suffix}</Text> : null}
      </View>
      {error ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a', lineHeight: 1.35 }}>{error}</Text>
        : hint ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{hint}</Text> : null}
    </View>
  );
}

// A switch row: label, optional detail, the Toggle at the trailing edge
export function SwitchRow({ id, label, detail, on, onChange, disabled }) {
  const t = useTheme();
  return (
    <Press id={id} label={label} role="switch" selected={on} disabled={disabled} onClick={() => onChange(!on)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 8 }} ringRadius={8}>
      <View style={{ flex: 1, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text }}>{label}</Text>
        {detail ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{detail}</Text> : null}
      </View>
      <View style={{ pointerEvents: 'none' }}><ToggleLook on={on} /></View>
    </Press>
  );
}
function ToggleLook({ on }) {
  const t = useTheme();
  return (
    <View style={{ width: 36, height: 20, borderRadius: 10, direction: 'ltr', backgroundColor: on ? t.accent : t.control }}>
      <View style={{ position: 'absolute', top: 2, left: on ? 18 : 2, width: 16, height: 16, borderRadius: 8, backgroundColor: '#fff', boxShadow: '0 1px 3px rgba(0,0,0,0.35)' }} />
    </View>
  );
}

// A row of choices (a segmented control that takes part in the focus order)
export function Choice({ id, options, value, onChange, label, small, fill }) {
  const t = useTheme();
  return (
    <View {...platform.a11y({ label, role: 'radiogroup' })}
      style={{ flexDirection: 'row', backgroundColor: t.control, borderRadius: t.controlRadius + 1, padding: 2, alignSelf: fill ? 'stretch' : 'flex-start' }}>
      {options.map(([v, text]) => {
        const on = value === v;
        return (
          <Press key={v} id={id ? `${id}-${v}` : undefined} label={text} role="radio" selected={on} onClick={() => onChange(v)}
            style={{ paddingHorizontal: small ? 10 : 14, height: small ? 20 : 24, borderRadius: Math.max(2, t.controlRadius - 1), justifyContent: 'center',
              alignItems: 'center', flexGrow: fill ? 1 : 0, backgroundColor: on ? t.accent : 'transparent' }}>
            <Text style={{ fontSize: small ? 11 : 12, fontWeight: 600, whiteSpace: 'nowrap', color: on ? '#fff' : t.text }}>{text}</Text>
          </Press>
        );
      })}
    </View>
  );
}

// A selectable card (devices, coolers, themes)
export function Card({ id, selected, onClick, disabled, label, children, style }) {
  const t = useTheme();
  const box = t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)';
  return (
    <Press id={id} label={label} role="radio" selected={selected} onClick={onClick} disabled={disabled} ringRadius={t.groupRadius + 2}
      style={[{ borderRadius: t.groupRadius + 2, padding: spacing.l, gap: spacing.s, backgroundColor: selected ? (t.mode === 'dark' ? 'rgba(10,132,255,0.10)' : 'rgba(10,132,255,0.06)') : box,
        border: `${selected ? 2 : 1}px solid ${selected ? t.accent : t.separator}`, margin: selected ? 0 : 1 }, style]}>
      {children}
    </Press>
  );
}

// An inset group, as in Settings
export function Group({ title, footer, children, style }) {
  const t = useTheme();
  const box = t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)';
  const rows = (Array.isArray(children) ? children.flat() : [children]).filter(Boolean);
  return (
    <View style={[{ gap: 6 }, style]}>
      {title ? <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginInlineStart: spacing.s }}>{title}</Text> : null}
      <View style={{ borderRadius: t.groupRadius, backgroundColor: box, border: `1px solid ${t.separator}`, paddingHorizontal: spacing.m }}>
        {rows.map((r, i) => (
          <View key={i} style={i ? { borderTopWidth: 1, borderTopColor: t.separator } : null}>{r}</View>
        ))}
      </View>
      {footer ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, marginInlineStart: spacing.s, lineHeight: 1.4 }}>{footer}</Text> : null}
    </View>
  );
}

export function KeyValue({ label, value, mono }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 32, paddingVertical: 5 }}>
      <Text style={{ width: 150, fontSize: typeScale.body, color: t.textDim }}>{label}</Text>
      <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, fontFamily: mono ? 'monospace' : undefined, textAlign: 'start' }}>{value}</Text>
    </View>
  );
}

// A small rounded label: tone 'ok' | 'warn' | 'bad' | 'info' | 'dim'
export function Badge({ tone = 'info', children }) {
  const t = useTheme();
  const c = { ok: '#30d158', warn: '#ff9f0a', bad: '#ff453a', info: t.accent, dim: t.textDim }[tone];
  return (
    <View style={{ paddingHorizontal: 7, height: 18, borderRadius: 9, justifyContent: 'center', backgroundColor: `${c}22`, border: `1px solid ${c}55` }}>
      <Text style={{ fontSize: 10.5, fontWeight: 600, color: c, whiteSpace: 'nowrap' }}>{children}</Text>
    </View>
  );
}

export function Notice({ tone = 'warn', title, children, style }) {
  const t = useTheme();
  const c = { ok: '#30d158', warn: '#ff9f0a', bad: '#ff453a', info: t.accent }[tone];
  return (
    <View {...platform.a11y({ role: tone === 'bad' ? 'alert' : 'note', label: title })}
      style={[{ flexDirection: 'row', gap: spacing.m, padding: spacing.m, borderRadius: t.groupRadius, backgroundColor: `${c}14`, border: `1px solid ${c}44` }, style]}>
      <View style={{ width: 4, borderRadius: 2, backgroundColor: c }} />
      <View style={{ flex: 1, gap: 3 }}>
        {title ? <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{title}</Text> : null}
        {children ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{children}</Text> : null}
      </View>
    </View>
  );
}

export function ProgressBar({ value, height = 8, tone }) {
  const t = useTheme();
  const v = Math.max(0, Math.min(1, value || 0));
  return (
    <View {...platform.a11y({ role: 'progressbar', value: Math.round(v * 100) })}
      style={{ height, borderRadius: height / 2, backgroundColor: t.control, overflow: 'hidden', direction: 'ltr' }}>
      <View style={{ width: `${(v * 100).toFixed(2)}%`, height, borderRadius: height / 2,
        background: tone === 'ok' ? '#30d158' : `linear-gradient(90deg, #B46CFF, ${t.accent})` }} />
    </View>
  );
}

// Lumo's tip next to the step: a pose and a short sentence in a bubble (one Lumo per view)
export function Helper({ pose = 'wave', title, text, compact }) {
  const t = useTheme();
  const bubble = t.mode === 'dark' ? 'rgba(180,108,255,0.12)' : 'rgba(110,75,255,0.07)';
  return (
    <View {...platform.a11y({ role: 'note', label: title || text })} style={{ flexDirection: 'row', alignItems: 'flex-end', gap: spacing.s }}>
      <Lumo pose={pose} size={compact ? 52 : 64} />
      <View style={{ flex: 1, padding: spacing.m, borderRadius: 12, borderBottomStartRadius: 3, backgroundColor: bubble, border: `1px solid ${t.mode === 'dark' ? 'rgba(180,108,255,0.28)' : 'rgba(110,75,255,0.18)'}`, gap: 3 }}>
        {title ? <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{title}</Text> : null}
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.45 }}>{text}</Text>
      </View>
    </View>
  );
}

// A searchable list (time zones, keyboard layouts, countries)
export function PickList({ id, items, value, onPick, placeholder, height = 150 }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const n = q.trim().toLowerCase();
  // the chosen one first, then the rest (searching keeps the list order)
  const ordered = value ? [...items.filter(([k]) => k === value), ...items.filter(([k]) => k !== value)] : items;
  const shown = (n ? items.filter(([key, label]) => `${key} ${label}`.toLowerCase().includes(n)) : ordered).slice(0, 400);
  const sel = items.find(([k]) => k === value);
  return (
    <View style={{ gap: 6 }}>
      <Field id={id ? `${id}-q` : undefined} value={q} onChange={setQ} placeholder={placeholder} />
      <View style={{ height, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.02)' }}>
        <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 4 }}>
          {shown.map(([key, label]) => (
            <View key={key} onClick={() => onPick(key)} {...platform.a11y({ role: 'option', label, selected: key === value, onActivate: () => onPick(key) })}
              style={{ minHeight: 24, justifyContent: 'center', paddingHorizontal: 8, borderRadius: 5, backgroundColor: key === value ? t.accent : 'transparent' }}>
              <Text style={{ fontSize: typeScale.callout, color: key === value ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{label}</Text>
            </View>
          ))}
        </ScrollView>
      </View>
      {sel ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{sel[1]}</Text> : null}
    </View>
  );
}

export function SectionTitle({ children, style }) {
  const t = useTheme();
  return <Text style={[{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }, style]}>{children}</Text>;
}

// Re-render every `ms` while mounted (progress timers)
export function useTick(ms, on = true) {
  const [, set] = useState(0);
  useEffect(() => {
    if (!on) return undefined;
    const iv = setInterval(() => set((x) => x + 1), ms);
    return () => clearInterval(iv);
  }, [ms, on]);
}
