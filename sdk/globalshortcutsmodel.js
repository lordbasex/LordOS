// globalshortcutsmodel.js — the shell's side of a Linux app's global shortcuts (org.freedesktop.portal.
// GlobalShortcuts, server/src/ui/portal.c and gshort.c): the sheet that asks the person and the rows of
// Settings → Keyboard. Pure, so it has node tests (server/tests/global_shortcuts_model_test.mjs).
//
//   comboOfKey(e)          the combination a key press names, in the form sdk/shortcuts.js and lord-server
//                          read ('Ctrl+Shift+A', 'Alt+F9'); '' for a modifier pressed alone
//   problemOf(check, ctx)  why a combination can't be given: lord-server's verdict (check, from
//                          lord.portal.shortcutCheck), one of the shell's own shortcuts (ctx.shellTaken), or
//                          another row of the same sheet (ctx.others); null when it can
//   sheetRows(req, ctx)    the rows of the sheet: each one starts on what the person chose before, or on the
//                          key the app proposed when it is free, or on none
//   setChoice(rows, i, c)  the rows with row i on c ('' leaves it unassigned)
//   answerOf(rows)         [{ id, trigger }] for lord.portal.shortcutsChosen
//   byApp(list)            Settings: lord.portal.shortcuts() grouped per app, in the order they came
//
// lord-server checks every answer again (gs_kept, the other apps' bindings): what is refused here is only
// what the person should hear about while choosing.

// gshort.h's GS_*: why LordOS keeps a combination
export const KEPT = ['', 'logo', 'system', 'typing', 'theme'];

const MODIFIERS = new Set([29, 97, 42, 54, 56, 100, 125, 126]);
export const isModifierCode = (code) => MODIFIERS.has(code);

const LETTERS = { 16: 'q', 17: 'w', 18: 'e', 19: 'r', 20: 't', 21: 'y', 22: 'u', 23: 'i', 24: 'o', 25: 'p', 30: 'a', 31: 's', 32: 'd',
  33: 'f', 34: 'g', 35: 'h', 36: 'j', 37: 'k', 38: 'l', 44: 'z', 45: 'x', 46: 'c', 47: 'v', 48: 'b', 49: 'n', 50: 'm' };
// the names gs_format writes for keys that are not letters
const NAMES = {
  1: 'Escape', 14: 'Backspace', 15: 'Tab', 28: 'Enter', 57: 'Space', 12: '-', 13: '=', 26: '[', 27: ']', 39: ';', 40: "'", 41: '`',
  43: '\\', 51: ',', 52: '.', 53: '/', 70: 'ScrollLock', 99: 'PrintScreen', 102: 'Home', 103: 'ArrowUp', 104: 'PageUp',
  105: 'ArrowLeft', 106: 'ArrowRight', 107: 'End', 108: 'ArrowDown', 109: 'PageDown', 110: 'Insert', 111: 'Delete', 119: 'Pause',
  11: '0', 87: 'F11', 88: 'F12',
};
for (let d = 1; d <= 9; d++) NAMES[d + 1] = String(d);
for (let f = 1; f <= 10; f++) NAMES[58 + f] = `F${f}`;
for (let f = 13; f <= 24; f++) NAMES[170 + f] = `F${f}`;

// As keygrab_letter (server/src/wl/keygrab.c): the layout's letter, or the position's on a layout without
// Latin letters; any other key by its position
function keyNameOf(e) {
  const b = String(e.base || '').toLowerCase();
  if (/^[a-z]$/.test(b)) return b.toUpperCase();
  if (LETTERS[e.code] && !/^[\x20-\x7e]$/.test(b)) return LETTERS[e.code].toUpperCase();
  return NAMES[e.code] || '';
}

export function comboOfKey(e) {
  if (!e || isModifierCode(e.code)) return '';
  const key = keyNameOf(e);
  if (!key) return '';
  return `${e.ctrl ? 'Ctrl+' : ''}${e.alt ? 'Alt+' : ''}${e.shift ? 'Shift+' : ''}${e.meta ? 'Meta+' : ''}${key}`;
}

// 'invalid', 'logo', 'system', 'typing', 'theme', 'shell', 'owner' or 'duplicate'; null when it can be given
export function problemOf(check, { shellTaken = () => false, others = [] } = {}) {
  if (!check || !check.trigger) return 'invalid';
  if (check.kept) return KEPT[check.kept] || 'system';
  if (shellTaken(check.trigger)) return 'shell';
  if (check.owner) return 'owner';
  if (others.includes(check.trigger)) return 'duplicate';
  return null;
}

// req: the hw event 'portal-shortcuts' ({ id, app, comm, shortcuts: [{ id, description, proposed, kept, owner,
// current }] }). A row: { id, description, proposed, choice, note, owner } where note says why the proposed
// key was not taken ('logo', 'shell', 'owner'…) and owner who has it.
export function sheetRows(req, { shellTaken = () => false } = {}) {
  const rows = [];
  for (const s of (req && Array.isArray(req.shortcuts) ? req.shortcuts : [])) {
    const proposed = typeof s.proposed === 'string' ? s.proposed : '';
    const others = rows.map((r) => r.choice).filter(Boolean);
    const note = proposed ? problemOf({ trigger: proposed, kept: s.kept | 0, owner: s.owner || '' }, { shellTaken, others }) : null;
    const current = typeof s.current === 'string' ? s.current : '';
    const choice = current && !others.includes(current) ? current : proposed && !note ? proposed : '';
    rows.push({ id: String(s.id), description: String(s.description || ''), proposed, choice, note: choice ? null : note,
      owner: note === 'owner' ? String(s.owner || '') : '' });
  }
  return rows;
}

export function setChoice(rows, i, combo) {
  return rows.map((r, j) => (j === i ? { ...r, choice: combo || '', note: null, owner: '' } : r));
}

export const answerOf = (rows) => rows.map((r) => ({ id: r.id, trigger: r.choice || '' }));

// [{ app, items: [{ id, description, trigger }] }]
export function byApp(list) {
  const groups = new Map();
  for (const e of Array.isArray(list) ? list : []) {
    if (!e || typeof e.app !== 'string' || !e.app) continue;
    if (!groups.has(e.app)) groups.set(e.app, []);
    groups.get(e.app).push({ id: String(e.id), description: String(e.description || ''), trigger: String(e.trigger || '') });
  }
  return [...groups].map(([app, items]) => ({ app, items }));
}
