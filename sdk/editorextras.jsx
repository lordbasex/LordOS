// editorextras.jsx — spelling and ✨ AI for an app's own text editor (Notes, Mail's body; docs/SPELLING.md): what the
// SDK's TextInput and TextArea do by themselves, in pieces an editor that draws its own rows can use. Offsets here are
// UTF-16 (those editors index their strings directly).
//
//   const ranges = useEditorSpell(text, on, caret)            the misspelled words, [[a, b]]
//   <EditorSquiggles ranges rows first last xOf top left />   their wavy lines on the visible rows
//   spellHitAt(ranges, text, off)                             the word under a right click, or null
//   editorMenuRows({ hit, selected, onReplaceWord, openWriting, refresh })   rows before and after the editor's own
//
// ✨ AI goes through the shell's panel (writing.jsx WritingPanel): the app renders it with the job openWriting gave and
// gets the text back only in onReplace, after the person's click.
import { View } from '@lordos/runtime';
import { useSpellRanges, spellSuggest, spellAddWord, spellIgnore } from './spell.js';
import { spellMenuItems, rangeAt, toUtf16 } from './spellmodel.js';
import { writingMenuFor } from './writing.jsx';
import { t as tr } from './i18n.js';

const cpIndex = (text, off) => [...String(text || '').slice(0, off)].length;

export function useEditorSpell(text, on, caret = -1) {
  const r = useSpellRanges(text || '', !!on, caret >= 0 ? cpIndex(text, caret) : -1);
  return toUtf16(text, r);
}

export const squiggleColorFor = (mode) => (mode === 'dark' ? '#ff6961' : '#e0352b');

// rows: [{ start, end }] (UTF-16); xOf(row, off) the x of an offset in a row; top(r) the y of the line under row r
export function EditorSquiggles({ ranges, rows, first, last, xOf, top, left = 0, color }) {
  if (!ranges || !ranges.length) return null;
  const out = [];
  for (let r = first; r <= last; r++) {
    const row = rows[r];
    if (!row) continue;
    for (const [a, b] of ranges) {
      const s = Math.max(a, row.start), e = Math.min(b, row.end);
      if (e <= s) continue;
      const x0 = xOf(row, s), x1 = xOf(row, e);
      if (x1 - x0 > 0.5) out.push(<View key={`sq${r}-${a}`} style={{ position: 'absolute', left: left + x0, top: top(r), width: x1 - x0, height: 3, wavyColor: color, pointerEvents: 'none' }} />);
    }
  }
  return out;
}

export function spellHitAt(ranges, text, off) {
  const r = rangeAt(ranges, off);
  return r ? { word: text.slice(r[0], r[1]), a: r[0], b: r[1] } : null;
}

// The rows around an editor's own: the spelling rows of hit first (its suggestions come later: refresh(rows) is
// called with the rows again), then base, then ✨ AI when selected text exists and Writing is ready.
export function editorMenuRows({ base, hit, selected, onReplaceWord, openWriting, refresh }) {
  const writing = openWriting ? writingMenuFor({ secure: false, selected, open: openWriting }) : null;
  const build = (spell) => [
    ...(spell ? spellMenuItems({ word: spell.word, suggestions: spell.suggestions, loading: spell.loading, tr,
      onReplace: (w) => onReplaceWord(hit.a, hit.b, w), onAdd: (w) => spellAddWord(w), onIgnore: (w) => spellIgnore(w) }) : []),
    ...base,
    ...(writing ? ['-', writing] : []),
  ];
  if (!hit) return build(null);
  spellSuggest(hit.word).then((list) => refresh?.(build({ word: hit.word, loading: false, suggestions: list })));
  return build({ word: hit.word, loading: true, suggestions: [] });
}
