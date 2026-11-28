// model.js — the Calculator's arithmetic (pure, no eval; node server/tests/calculator_model_test.mjs).
//
//   evaluate(src)        + − × ÷ with precedence, parentheses, unary minus and % (GitHub #271):
//                        "a + b%" and "a − b%" take b percent of a (200 + 10% = 220, 200 − 10% = 180);
//                        anywhere else b% is b / 100 (200 × 10% = 20, 50% = 0.5)
//   addPercent(expr)     the % key: after a number or a ")" only
//   toggleSign(expr)     the ± key: the sign of the entry being typed (the last number), or a leading − to start one

const NUM = /\d*\.?\d+|\d+\./;

export function evaluate(src) {
  const toks = String(src).replace(/×/g, '*').replace(/÷/g, '/').replace(/−/g, '-').replace(/,/g, '.')
    .match(/\d*\.?\d+|\d+\.|[-+*/()%]/g) || [];
  let i = 0;
  const peek = () => toks[i], next = () => toks[i++];
  // a factor: { v, pct } where pct is the number before its / 100 when the factor ended with %
  const factor = () => {
    const tk = next();
    let v;
    if (tk === '-') { const f = factor(); return { v: -f.v, pct: f.pct === null ? null : -f.pct }; }
    if (tk === '(') { v = expr(); if (next() !== ')') throw new Error('syntax'); }
    else {
      if (tk === undefined || !NUM.test(tk)) throw new Error('syntax');
      v = parseFloat(tk);
    }
    if (peek() === '%') { next(); return { v: v / 100, pct: v }; }
    return { v, pct: null };
  };
  const term = () => {
    const first = factor();
    let v = first.v, alone = true;
    while (peek() === '*' || peek() === '/') { alone = false; v = next() === '*' ? v * factor().v : v / factor().v; }
    return { v, pct: alone ? first.pct : null };
  };
  const expr = () => {
    let v = term().v;
    while (peek() === '+' || peek() === '-') {
      const op = next(), r = term();
      const add = r.pct !== null ? v * r.pct / 100 : r.v;      // a + b% → a + a·b/100
      v = op === '+' ? v + add : v - add;
    }
    return v;
  };
  const v = expr();
  if (i !== toks.length || !Number.isFinite(v)) throw new Error('error');
  return Math.round(v * 1e10) / 1e10;
}

export function addPercent(expr) {
  const e = String(expr);
  return /[\d.)]$/.test(e) ? `${e}%` : e;
}

// the last number of the expression (with its %), where it starts, and whether a unary minus is in front of it
function lastEntry(e) {
  const m = /(\d*\.?\d+|\d+\.)%?$/.exec(e);
  if (!m) return null;
  const at = m.index;
  const before = e.slice(0, at);
  // a minus is the entry's own when it starts the expression or follows an operator or "("
  const unary = before.endsWith('-') && (before.length === 1 || /[-+×÷*/(]$/.test(before.slice(0, -1)));
  return { at, unary };
}

export function toggleSign(expr) {
  const e = String(expr);
  const last = lastEntry(e);
  if (last) return last.unary ? e.slice(0, last.at - 1) + e.slice(last.at) : `${e.slice(0, last.at)}-${e.slice(last.at)}`;
  // nothing typed yet, or after an operator or "(": start a negative entry; a lone − started that way goes away
  if (e === '-' || /[-+×÷*/(]-$/.test(e)) return e.slice(0, -1);
  if (e === '' || /[-+×÷*/(]$/.test(e)) return `${e}-`;
  return e;
}
