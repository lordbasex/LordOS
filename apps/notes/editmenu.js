// editmenu.js — the Notes Edit and Format menus and the editor's right-click menu (GitHub #117). Each row sends the
// editor's own key (NoteEditor's onKey), so a menu does exactly what the keys do. In the reading view there is no
// selection: Undo, Redo, Cut and Copy are off, and Paste and Select All first open the editor (the caller does that).

// action → [key, shift]
export const EDIT_KEYS = {
  undo: ['z', false], redo: ['z', true], cut: ['x', false], copy: ['c', false], paste: ['v', false], selectAll: ['a', false],
  bold: ['b', false], italic: ['i', false], link: ['k', false], h1: ['1', false], h2: ['2', false],
  bullets: ['8', true], numbers: ['7', true], task: ['9', true], code: ['c', true],
};

// the key event an action sends to the editor
export function editKey(action) {
  const k = EDIT_KEYS[action];
  return k ? { ctrl: true, key: k[0], base: k[0], char: '', shift: k[1], fromMenu: true } : null;
}

// actions that work from the reading view (they switch to editing first)
export const fromReading = (action) => action === 'paste' || action === 'selectAll';

// Edit menu. editing: the editor is open; selected: text is selected (null when it is not known, as in the menu
// bar, which is not told about every selection change: Cut and Copy stay on and do nothing without one);
// empty: the note has no text; none: no note is open (every row is off).
export function editItems(tr, { editing, selected = null, empty = false, none = false }, run) {
  const it = (action, label, hint, off) => ({ label: tr(label), hint, disabled: !!(off || none), onClick: () => run(action) });
  const noSel = !editing || selected === false;
  return [
    it('undo', 'notes.cmd.undo', '❖Z', !editing),
    it('redo', 'notes.cmd.redo', '❖⇧Z', !editing),
    '-',
    it('cut', 'notes.cmd.cut', '❖X', noSel),
    it('copy', 'notes.cmd.copy', '❖C', noSel),
    it('paste', 'notes.cmd.paste', 'Ctrl+V', false),
    '-',
    it('selectAll', 'notes.cmd.selectAll', '❖A', empty),
  ];
}

// Format menu: on only while editing
export function formatItems(tr, { editing }, run) {
  const it = (action, label, hint) => ({ label: tr(label), hint, disabled: !editing, onClick: () => run(action) });
  return [
    it('bold', 'notes.fmt.bold', '❖B'), it('italic', 'notes.fmt.italic', '❖I'), it('link', 'notes.fmt.link', '❖K'), '-',
    it('h1', 'notes.fmt.h1', '❖1'), it('h2', 'notes.fmt.h2', '❖2'),
    it('bullets', 'notes.fmt.bullets', '❖⇧8'), it('numbers', 'notes.fmt.numbers', '❖⇧7'),
    it('task', 'notes.fmt.task', '❖⇧9'), it('code', 'notes.fmt.code', '❖⇧C'),
  ];
}

// the editor's right-click menu: Cut, Copy, Paste and Select All, then the extra rows (Speech) after a separator
export function contextItems(tr, { selected, empty }, run, extra = []) {
  const rows = editItems(tr, { editing: true, selected, empty }, run).slice(3);
  return extra.length ? [...rows, '-', ...extra] : rows;
}
