// osk.jsx — the on-screen keyboard's panel (docs/ON-SCREEN-KEYBOARD.md): large rounded keys, Shift, two
// pages of symbols, the theme's colors. startShell mounts it in the `input` layer and the login screen
// at the end of its tree. `keepFocus`: pressing it leaves the field, and an open menu, as they are.
// `protected`: it stays black in screenshots, recordings and remote desktop.
//
// Keys act on release (Backspace and Space on press, and they repeat while held). The rows are always
// left to right: a keyboard doesn't mirror in Arabic or Hebrew, and each key shows the active layout.
import { useEffect, useRef, useState } from 'react';
import { View, Text, mainDisplay, addListener } from '@lordos/runtime';
import { useStore } from './store.js';
import { shellTokens, useTheme } from './theme.js';
import { t as tr } from './i18n.js';
import { keyboard } from './keyboard.js';
import { osk, oskState } from './osk.js';
import { KEY, TOP_GAP, letterRows, panelSize, punctuation, symbolRows } from './oskmodel.js';
import { settings } from './settings.js';
import { menuBarPrefs } from './menubarmodel.js';

const HOLD_MS = 420, REPEAT_MS = 70, CAPS_MS = 400;

// A small keyboard, drawn (it doesn't depend on a font having ⌨).
export function OskIcon({ color, size = 16 }) {
  const dot = { width: 2, height: 2, borderRadius: 1, backgroundColor: color };
  return (
    <View style={{ width: size + 2, height: Math.round(size * 0.72), borderRadius: 3, border: `1.4px solid ${color}`, alignItems: 'center', justifyContent: 'center', gap: 1.5 }}>
      <View style={{ flexDirection: 'row', gap: 2 }}><View style={dot} /><View style={dot} /><View style={dot} /><View style={dot} /></View>
      <View style={{ width: Math.round(size * 0.55), height: 2, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

function Key({ flex = 1, h, radius, label, icon, onPress, repeat, tone, c, size }) {
  const [down, setDown] = useState(false);
  const timer = useRef(0);
  const stop = () => { clearTimeout(timer.current); clearInterval(timer.current); timer.current = 0; setDown(false); };
  useEffect(() => { if (!down) return undefined; const off = addListener('pointerup', stop); return () => { off(); clearTimeout(timer.current); clearInterval(timer.current); }; }, [down]);
  const press = () => {
    setDown(true);
    if (!repeat) return;
    onPress();
    timer.current = setTimeout(() => { timer.current = setInterval(onPress, REPEAT_MS); }, HOLD_MS);
  };
  // on release, as on a tablet: Return can close the field and the keyboard with it, and the finger
  // must not come down on what was underneath
  const release = () => { if (down && !repeat) onPress(); stop(); };
  const bg = tone === 'accent' ? c.accent : tone === 'side' ? c.side : c.key;
  return (
    <View onMouseDown={press} onMouseUp={release} onMouseLeave={() => { if (down) stop(); }}
      style={{ flex, height: h, minWidth: 0, borderRadius: radius, alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
        backgroundColor: down ? c.down : bg, boxShadow: c.shadow }}>
      {icon || <Text style={{ fontSize: size, color: tone === 'accent' ? c.onAccent : c.text, whiteSpace: 'nowrap' }}>{label}</Text>}
    </View>
  );
}

export function OskPanel() {
  const st = useStore(oskState);
  const own = useTheme();
  const t = useStore(shellTokens) || own;   // mounted outside the theme's tree (sdk/theme.js)
  const lastShift = useRef(0);
  if (!st.visible || !st.labels) return null;
  const box = st.box || (() => { const m = mainDisplay(); return { x: m.x, y: m.y, w: m.w, h: m.h }; })();
  const s = panelSize(box);
  const left = box.x + Math.round((box.w - s.width) / 2);
  const top = st.dock === 'top' ? box.y + TOP_GAP : box.y + box.h - s.height;
  const c = {
    panel: t.panelBg || t.windowBg, text: t.text, accent: t.accent,
    key: t.control, side: t.sidebarBg || t.separator, down: t.accent,
    onAccent: t.accent === '#ffffff' ? '#1d1d1f' : '#fff',
    shadow: '0 1px 1px rgba(0,0,0,0.22)',
  };
  const radius = Math.max(t.controlRadius || 0, Math.round(s.keyH * 0.2));
  const K = { h: s.keyH, radius, c, size: Math.round(Math.min(23, s.keyH * 0.4)) };
  const small = { ...K, size: Math.round(K.size * 0.68) };
  const onLetters = st.page === 0;
  const rows = onLetters ? letterRows(st.labels, st.shift) : symbolRows(st.labels, st.page);
  const [comma, dot] = punctuation(st.labels);
  const pressShift = () => {
    const now = lord.now();
    const caps = now - lastShift.current < CAPS_MS;
    lastShift.current = now;
    osk.setShift(st.shift === 2 ? 0 : caps ? 2 : st.shift ? 0 : 1);
  };
  const shiftKey = onLetters
    ? <Key {...K} flex={1.45} tone={st.shift ? 'accent' : 'side'} label={st.shift === 2 ? '⇪' : '⇧'} onPress={pressShift} />
    : <Key {...small} flex={1.45} tone="side" label={st.page === 1 ? '1/2' : '2/2'} onPress={() => osk.setPage(st.page === 1 ? 2 : 1)} />;
  const keyOf = (k, i) => <Key key={`${k.code}.${i}`} {...K} label={k.name ? tr(`shell.osk.${k.name}`) : k.label} onPress={() => osk.press(k)} />;
  return (
    <View keepFocus protected
      style={{ position: 'absolute', left, top, width: s.width, height: s.height, padding: s.pad, gap: s.gap, direction: 'ltr',
        borderTopLeftRadius: 18, borderTopRightRadius: 18, borderBottomLeftRadius: st.dock === 'top' ? 18 : 0, borderBottomRightRadius: st.dock === 'top' ? 18 : 0,
        backgroundColor: c.panel, border: `1px solid ${t.separator}`, boxShadow: '0 -6px 24px rgba(0,0,0,0.28)' }}>
      {rows.map((row, r) => (
        <View key={r} style={{ flexDirection: 'row', gap: s.gap, height: s.keyH, paddingHorizontal: r === 1 && row.length < rows[0].length ? Math.round(s.keyH * 0.4) : 0 }}>
          {r === 2 ? shiftKey : null}
          {row.map(keyOf)}
          {r === 2 ? <Key {...K} flex={1.45} tone="side" repeat label="⌫" onPress={() => osk.press({ code: KEY.BACKSPACE })} /> : null}
        </View>
      ))}
      <View style={{ flexDirection: 'row', gap: s.gap, height: s.keyH }}>
        <Key {...small} flex={1.45} tone="side" label={onLetters ? '?123' : 'ABC'} onPress={() => osk.setPage(onLetters ? 1 : 0)} />
        {keyboard.configured().length > 1 ? <Key {...small} tone="side" label={keyboard.label(keyboard.active())} onPress={osk.nextLayout} /> : null}
        <Key {...K} label={comma.label} onPress={() => osk.press(comma)} />
        <Key {...small} flex={4.6} repeat label={tr('shell.osk.space')} onPress={() => osk.press({ code: KEY.SPACE })} />
        <Key {...K} label={dot.label} onPress={() => osk.press(dot)} />
        <Key {...small} flex={1.7} tone="accent" label={tr('shell.osk.return')} onPress={() => osk.press({ code: KEY.ENTER })} />
        <Key {...small} tone="side" icon={<OskIcon color={t.text} />} onPress={osk.hide} />
      </View>
    </View>
  );
}

// In the theme's bar, next to the clock: the way to open and close it.
// Hidden with Settings → Desktop & Dock → Menu Bar → On-screen keyboard (issue #58); ❖⌃K and Settings → Keyboard →
// Show Keyboard still open it.
export function OskStatus({ color, height }) {
  const s = useStore(settings);
  if (!menuBarPrefs(s).osk) return null;
  return (
    <View keepFocus onClick={osk.toggle} title={tr('shell.osk.name')}
      style={{ height, paddingHorizontal: 6, alignItems: 'center', justifyContent: 'center' }}>
      <OskIcon color={color} size={14} />
    </View>
  );
}

// The greeter asks how much its column must rise so the password stays above the keyboard.
export function oskLift(fieldBottom, displayHeight) {
  const st = oskState.get();
  if (!st.visible) return 0;
  return Math.max(0, fieldBottom + 20 - (displayHeight - panelSize({ w: displayHeight * 2, h: displayHeight }).height));
}
