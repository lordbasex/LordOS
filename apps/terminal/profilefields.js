// profilefields.js — two of a profile's fields as Preferences edits them and the Terminal uses them (pure;
// server/tests/terminal_profilefields_test.mjs).
//   initialKeys(text) → what is typed into the shell when a session opens: each line, with Enter after the last one too,
//                       so the command runs without a "\n" at its end (a "\n" written as two characters still counts as
//                       a line break, as profiles saved before 0.8.8.8 have it)
//   parseTags(text) / tagsText(tags) → the profile's tags as one field, separated by commas

export function initialKeys(text) {
  const s = String(text || '').replace(/\\n/g, '\n').replace(/\r\n?/g, '\n').replace(/\n+$/, '');
  return s.trim() ? `${s.replace(/\n/g, '\r')}\r` : '';
}

const MAX_TAG = 100;

export function parseTags(text) {
  const out = [];
  for (const raw of String(text || '').split(',')) {
    const tag = raw.replace(/[\x00-\x1f\x7f]/g, '').trim().slice(0, MAX_TAG);
    if (tag && !out.some((x) => x.toLowerCase() === tag.toLowerCase())) out.push(tag);
  }
  return out;
}

export const tagsText = (tags) => (Array.isArray(tags) ? tags.filter((x) => typeof x === 'string' && x.trim()).join(', ') : '');

// programExit: what a program opened from the launcher (a terminal app: htop, mc…) does to its tab when it exits. lord-server
// turns it into the end of the line it types ('close' "; exit", 'closeIfOk' " && exit", 'keep' nothing); any other value is
// the default, 'close'.
export const PROGRAM_EXIT = ['close', 'closeIfOk', 'keep'];
export const programExitOf = (p) => (PROGRAM_EXIT.includes(p?.programExit) ? p.programExit : 'close');
