// sqlview.js — SQL drawn in color outside the editor (the CREATE syntax, an ALTER before it runs), with the Editor's
// own SQL tokenizer and palette (apps/editor/syntax.js), so both read the same.
import { languageById, tokenizeLine, paletteFor } from '../editor/syntax.js';

const SQL = languageById('sql');

// text → lines of [text, color]
export function highlight(text, dark) {
  const pal = paletteFor(dark ? 'dark' : 'light');
  let state = 'root';
  return String(text || '').split('\n').map((line) => {
    const r = tokenizeLine(SQL, line, state);
    state = r.state;
    return r.tokens.map(([s, type]) => [s, pal[type] || pal.text]);
  });
}
