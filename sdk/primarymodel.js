// primarymodel.js — the primary selection of the SDK's text fields (components.jsx TextInput, TextArea), no screen: what
// is selected is published as the primary selection (lord.primary.set; Linux apps paste it with a middle click), and a
// middle click pastes the primary selection where it lands. A field's state is { value, pos, anchor } (textfieldmodel.js).
//   primaryStep(last, s, secure) → { last, text }: text is what to publish now ('' for nothing), last what to keep for the
//                                   next call. A password field publishes nothing; the same selection is published once,
//                                   until it is gone (so selecting it again after another app took it publishes it again)
//   pasteAt(s, pos, text, oneLine) → the state after a middle click at pos: text goes in there, not over the selection
//                                   (which is let go), and the caret goes after it; oneLine: line breaks become spaces
import { selRange, selectedText, insertAt } from './textfieldmodel.js';

export function primaryStep(last, s, secure = false) {
  const r = secure ? null : selRange(s);
  if (!r) return { last: null, text: '' };
  const text = selectedText(s);
  const key = `${r[0]}:${r[1]}:${text}`;
  return key === last ? { last, text: '' } : { last: key, text };
}

export function pasteAt(s, pos, text, oneLine = false) {
  const t = oneLine ? String(text || '').replace(/\r?\n/g, ' ') : String(text || '').replace(/\r\n?/g, '\n');
  const value = s.value || '';
  if (!t) return { value, pos: s.pos, anchor: s.anchor };
  const r = insertAt(value, pos, t);
  return { value: r.value, pos: r.pos, anchor: null };
}
