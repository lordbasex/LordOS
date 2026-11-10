// triggers.js — a trigger tried on a sample line, for its sheet in Settings (pure; server/tests/terminal_triggers_test.mjs).
// It works as state.js's runTriggers does on the screen's lines: the expression without flags, the first match, and
// \0…\9 in the parameter replaced by the match and its groups. freshLines says which of the screen's lines
// runTriggers tries: the ones printed since its last look.

// the actions, in the order the sheet offers them; `param` says what the parameter is for that action
export const TRIGGER_ACTIONS = [
  { id: 'notify', param: 'text' }, { id: 'attention', param: 'text' }, { id: 'bell', param: null }, { id: 'send', param: 'keys' },
  { id: 'capture', param: null }, { id: 'highlight', param: 'colors' }, { id: 'mark', param: null }, { id: 'title', param: 'text' },
  { id: 'stop', param: null },
];

const MAX_SAMPLE = 2000;

// { error: true } for an expression that doesn't compile; { match: null } when the sample doesn't match; otherwise
// { match, groups, result }: the text matched, its groups, and the parameter as the action would use it
export function triggerPreview(regex, param, sample) {
  let re;
  try { re = new RegExp(String(regex || '')); } catch (e) { return { error: true }; }
  if (!regex) return { error: true };
  const m = String(sample || '').slice(0, MAX_SAMPLE).match(re);
  if (!m) return { match: null };
  const result = String(param || '').replace(/\\(\d)/g, (_, i) => m[+i] ?? '');
  return { match: m[0], groups: m.slice(1).map((g) => g ?? ''), result };
}

// what the action does with that result, for the sheet's line under the sample
export function previewLine(action, p, sample) {
  if (!p || p.error || p.match == null) return null;
  const line = String(sample || '').trim();
  switch (action) {
    case 'notify': case 'attention': return { kind: 'notice', text: p.result || line };
    case 'title': return { kind: 'title', text: p.result || p.match };
    case 'send': return { kind: 'send', text: p.result };
    case 'capture': return { kind: 'capture', text: line };
    case 'highlight': return { kind: 'highlight', text: p.match };
    default: return { kind: action, text: '' };
  }
}

// The screen as term.text(sid, 'screenLines') gives it, "ROW\tTEXT" per line (ROW is the absolute row the line starts
// on), with the terminal's size: what freshLines compares from one output event to the next.
export function screenSnapshot(text, info) {
  const lines = [];
  for (const s of String(text || '').split('\n')) {
    const tab = s.indexOf('\t');
    if (tab > 0) lines.push({ row: Number(s.slice(0, tab)), text: s.slice(tab + 1) });
  }
  return { cols: info?.cols || 0, rows: info?.rows || 0, lines };
}

const countTexts = (lines) => {
  const n = new Map();
  for (const l of lines || []) n.set(l.text, (n.get(l.text) || 0) + 1);
  return n;
};

// The texts of the lines triggers try now, in screen order: each line once per time it was printed. A line is new
// when its row held something else at the last look; it is not when it only scrolled (it keeps its row) or a
// full-screen program moved it (a line of the last look with that text left its row while still on the screen).
// The first look tries every line; a resize renumbers the rows, so then lines go by how many times each text was
// on the screen.
export function freshLines(prev, cur) {
  const out = [];
  if (!prev || prev.cols !== cur.cols || prev.rows !== cur.rows) {
    const left = countTexts(prev?.lines);
    for (const l of cur.lines) {
      if (!l.text.trim()) continue;
      const n = left.get(l.text) || 0;
      if (n > 0) left.set(l.text, n - 1);
      else out.push(l.text);
    }
    return out;
  }
  const before = new Map(prev.lines.map((l) => [l.row, l.text]));
  const now = new Map(cur.lines.map((l) => [l.row, l.text]));
  const top = cur.lines.length ? cur.lines[0].row : Infinity;
  const moved = new Map();
  for (const l of prev.lines) {
    if (l.row >= top && l.text.trim() && now.get(l.row) !== l.text) moved.set(l.text, (moved.get(l.text) || 0) + 1);
  }
  for (const l of cur.lines) {
    if (!l.text.trim() || before.get(l.row) === l.text) continue;
    const n = moved.get(l.text) || 0;
    if (n > 0) moved.set(l.text, n - 1);
    else out.push(l.text);
  }
  return out;
}
