// triggers.js — a trigger tried on a sample line, for its sheet in Settings (pure; server/tests/terminal_triggers_test.mjs).
// It works as state.js's runTriggers does on the screen's lines: the expression without flags, the first match, and
// \0…\9 in the parameter replaced by the match and its groups.

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
