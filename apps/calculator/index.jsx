// Calculator — sample native app: buttons and keyboard (no eval: our own parser).
import { View, Text, useTheme, useKeyboard, t as tr, formatNumber } from '@lordos/sdk';
import { useState } from 'react';

export function Icon({ size: s }) {
  const key = (x, y, c) => (
    <View key={`${x}:${y}`} style={{ position: 'absolute', left: s * x, top: s * y, width: s * 0.15, height: s * 0.15,
      borderRadius: s * 0.075, backgroundColor: c }} />
  );
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #3a3a40, #18181b)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, border: '1px solid rgba(255,255,255,0.12)' }}>
      <View style={{ position: 'absolute', left: s * 0.16, right: s * 0.16, top: s * 0.12, height: s * 0.15,
        borderRadius: s * 0.04, backgroundColor: '#9be7b0' }} />
      {[0.34, 0.53, 0.72].flatMap((y) => [key(0.16, y, '#71717a'), key(0.425, y, '#71717a'), key(0.69, y, '#ff9f0a')])}
    </View>
  );
}

// Evaluates + - × ÷ with precedence and parentheses (recursive descent).
function evaluate(src) {
  const toks = src.replace(/×/g, '*').replace(/÷/g, '/').replace(/,/g, '.').match(/\d*\.?\d+|[-+*/()]/g) || [];
  let i = 0;
  const peek = () => toks[i], next = () => toks[i++];
  const factor = () => {
    const tk = next();
    if (tk === '-') return -factor();
    if (tk === '(') { const v = expr(); next(); return v; }
    const n = parseFloat(tk);
    if (Number.isNaN(n)) throw new Error('syntax');
    return n;
  };
  const term = () => {
    let v = factor();
    while (peek() === '*' || peek() === '/') v = next() === '*' ? v * factor() : v / factor();
    return v;
  };
  const expr = () => {
    let v = term();
    while (peek() === '+' || peek() === '-') v = next() === '+' ? v + term() : v - term();
    return v;
  };
  const v = expr();
  if (i !== toks.length || !Number.isFinite(v)) throw new Error('error');
  return Math.round(v * 1e10) / 1e10;
}

const KEYS = [
  ['C', '(', ')', '÷'],
  ['7', '8', '9', '×'],
  ['4', '5', '6', '-'],
  ['1', '2', '3', '+'],
  ['0', '.', '⌫', '='],
];

export default function Calculator() {
  const t = useTheme();
  const [expr, setExpr] = useState('');
  const [result, setResult] = useState('0');

  const press = (k) => {
    if (k === 'C') { setExpr(''); setResult('0'); }
    else if (k === '⌫') setExpr((e) => e.slice(0, -1));
    else if (k === '=') {
      try { const v = String(evaluate(expr)); setResult(v); setExpr(v); } catch (e) { setResult(tr('calculator.error')); }
    } else setExpr((e) => (e + k).slice(0, 40));
  };

  useKeyboard((e) => {
    const map = { '*': '×', '/': '÷', Enter: '=', Backspace: '⌫', Escape: 'C', ',': '.' };
    const k = map[e.key] || map[e.char] || e.char;
    if (KEYS.flat().includes(k)) press(k);
  });

  // the region's decimal separator (1,5 or 1.5); internally always '.'
  const dec = formatNumber(1.5, 1)[1];
  // on screen, the typographic minus (−) instead of the hyphen
  const show = (x) => x.replace(/\./g, dec).replace(/-/g, '−');
  const shown = /\d/.test(result) ? show(result) : result;
  // the big number shrinks so it never gets clipped
  const big = shown.length <= 8 ? 44 : shown.length <= 11 ? 34 : 26;
  return (
    // the calculator is always left to right, also in Arabic or Hebrew
    <View style={{ flex: 1, padding: 16, gap: 8, direction: 'ltr', backgroundColor: t.mode === 'dark' ? '#1c1c1e' : '#f5f5f7' }}>
      <View style={{ height: 92, justifyContent: 'flex-end', alignItems: 'flex-end', paddingHorizontal: 6, marginBottom: 4, overflow: 'hidden' }}>
        <Text style={{ fontSize: 13, color: t.textDim, whiteSpace: 'nowrap' }}>{show(expr) || ' '}</Text>
        <Text style={{ fontSize: big, fontWeight: 400, color: t.text, whiteSpace: 'nowrap' }}>{shown}</Text>
      </View>
      {KEYS.map((row, r) => (
        <View key={r} style={{ flexDirection: 'row', gap: 8, flex: 1 }}>
          {row.map((k) => <Key key={k} k={k} label={k === '.' ? dec : k === '-' ? '−' : k} onPress={() => press(k)} />)}
        </View>
      ))}
    </View>
  );
}

// Key: digits, functions (C, parentheses, delete) and operators in orange; = in the accent color
// (the primary action). Lightens on hover and dims while pressed.
function Key({ k, label, onPress }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const [down, setDown] = useState(false);
  const dark = t.mode === 'dark';
  const op = '÷×-+'.includes(k), fn = 'C()⌫'.includes(k);
  const bg = k === '=' ? t.accent : op ? '#ff9f0a' : fn ? (dark ? '#4a4a50' : '#d4d4d9') : (dark ? '#333338' : '#ffffff');
  return (
    <View onMouseDown={() => setDown(true)} onMouseUp={() => setDown(false)} onClick={onPress}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => { setHover(false); setDown(false); }}
      style={{ flex: 1, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: bg,
        opacity: down ? 0.7 : hover ? 0.88 : 1, boxShadow: dark ? 'none' : '0 1px 1px rgba(0,0,0,0.08)' }}>
      <Text style={{ fontSize: op || k === '=' ? 24 : 20, fontWeight: op || k === '=' ? 500 : 400,
        color: op || k === '=' ? '#fff' : t.text }}>{label}</Text>
    </View>
  );
}
