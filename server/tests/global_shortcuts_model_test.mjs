// global_shortcuts_model_test.mjs — the sheet and the Settings rows of Linux apps' global shortcuts
// (sdk/globalshortcutsmodel.js).
import { comboOfKey, problemOf, sheetRows, setChoice, answerOf, byApp, isModifierCode } from '../../sdk/globalshortcutsmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

// a key press names the combination the way lord-server writes it (gs_format)
const key = (code, base, mods = {}) => ({ code, base, ctrl: false, alt: false, shift: false, meta: false, ...mods });
eq('ctrl shift a', comboOfKey(key(30, 'A', { ctrl: true, shift: true })), 'Ctrl+Shift+A');
eq('the layout letter: A on AZERTY', comboOfKey(key(16, 'a', { ctrl: true })), 'Ctrl+A');
eq('a non-Latin layout: the position', comboOfKey(key(30, 'ф', { ctrl: true })), 'Ctrl+A');
eq('a digit by position, shifted', comboOfKey(key(2, '!', { alt: true, shift: true })), 'Alt+Shift+1');
eq('a digit on AZERTY', comboOfKey(key(3, 'é', { ctrl: true })), 'Ctrl+2');
eq('zero', comboOfKey(key(11, '0', { ctrl: true })), 'Ctrl+0');
eq('F9 alone', comboOfKey(key(67, '')), 'F9');
eq('F13', comboOfKey(key(183, '')), 'F13');
eq('F24', comboOfKey(key(194, '')), 'F24');
eq('F11', comboOfKey(key(87, '')), 'F11');
eq('space', comboOfKey(key(57, ' ', { ctrl: true, alt: true })), 'Ctrl+Alt+Space');
eq('order is Ctrl Alt Shift Meta', comboOfKey(key(30, 'a', { meta: true, shift: true, alt: true, ctrl: true })), 'Ctrl+Alt+Shift+Meta+A');
eq('page up', comboOfKey(key(104, '', { ctrl: true })), 'Ctrl+PageUp');
eq('slash', comboOfKey(key(53, '/', { ctrl: true })), 'Ctrl+/');
eq('a modifier alone', comboOfKey(key(29, '', { ctrl: true })), '');
eq('super alone', comboOfKey(key(125, '', { meta: true })), '');
eq('an unknown key', comboOfKey(key(240, '')), '');
eq('nothing', comboOfKey(null), '');
eq('modifiers', [29, 97, 42, 54, 56, 100, 125, 126].every(isModifierCode) && !isModifierCode(30), true);

// the problems the person hears about
const shell = new Set(['Ctrl+Space', 'F10', 'Ctrl+Alt+Space']);
const shellTaken = (c) => shell.has(c);
eq('free', problemOf({ trigger: 'Ctrl+Shift+A', kept: 0, owner: '' }, { shellTaken }), null);
eq('not a trigger', problemOf(null), 'invalid');
eq('empty', problemOf({ trigger: '', kept: 0, owner: '' }), 'invalid');
eq('logo', problemOf({ trigger: 'Meta+Space', kept: 1, owner: '' }), 'logo');
eq('system', problemOf({ trigger: 'Alt+Tab', kept: 2, owner: '' }), 'system');
eq('typing', problemOf({ trigger: 'A', kept: 3, owner: '' }), 'typing');
eq('theme', problemOf({ trigger: 'Alt+F4', kept: 4, owner: '' }), 'theme');
eq('an unknown kept code is still kept', problemOf({ trigger: 'X', kept: 9, owner: '' }), 'system');
eq('the shell has it', problemOf({ trigger: 'Ctrl+Space', kept: 0, owner: '' }, { shellTaken }), 'shell');
eq('another app has it', problemOf({ trigger: 'Ctrl+Shift+A', kept: 0, owner: 'desktop:x' }, { shellTaken }), 'owner');
eq('kept beats the shell', problemOf({ trigger: 'Meta+Space', kept: 1, owner: '' }, { shellTaken: () => true }), 'logo');
eq('another row has it', problemOf({ trigger: 'Ctrl+B', kept: 0, owner: '' }, { others: ['Ctrl+B'] }), 'duplicate');

// the sheet: nothing is bound without the person; each row starts on a free proposal, a decision made
// before, or none
const req = { id: 4, app: 'desktop:com.example.Talk', shortcuts: [
  { id: 'ptt', description: 'Push to talk', proposed: 'Ctrl+Shift+T', kept: 0, owner: '', current: '' },
  { id: 'mute', description: 'Mute', proposed: 'Meta+M', kept: 1, owner: '', current: '' },
  { id: 'search', description: 'Search', proposed: 'Ctrl+Space', kept: 0, owner: '', current: '' },
  { id: 'rec', description: 'Record', proposed: 'Alt+F9', kept: 0, owner: 'desktop:com.example.Rec', current: '' },
  { id: 'none', description: 'No proposal', proposed: '', kept: 0, owner: '', current: '' },
  { id: 'twice', description: 'Same as ptt', proposed: 'Ctrl+Shift+T', kept: 0, owner: '', current: '' },
  { id: 'old', description: 'Chosen before', proposed: 'Ctrl+Shift+O', kept: 0, owner: '', current: 'Ctrl+Alt+O' },
] };
const rows = sheetRows(req, { shellTaken });
eq('choices', rows.map((r) => r.choice), ['Ctrl+Shift+T', '', '', '', '', '', 'Ctrl+Alt+O']);
eq('notes', rows.map((r) => r.note), [null, 'logo', 'shell', 'owner', null, 'duplicate', null]);
eq('owner named', rows[3].owner, 'desktop:com.example.Rec');
eq('description kept', rows[0].description, 'Push to talk');
eq('no shortcuts', sheetRows({ id: 1 }), []);
eq('garbage', sheetRows(null), []);
const changed = setChoice(rows, 1, 'Ctrl+Alt+M');
eq('a new choice clears the note', [changed[1].choice, changed[1].note], ['Ctrl+Alt+M', null]);
eq('the others stay', changed[0], rows[0]);
eq('cleared', setChoice(rows, 0, '')[0].choice, '');
eq('the answer', answerOf(changed), [
  { id: 'ptt', trigger: 'Ctrl+Shift+T' }, { id: 'mute', trigger: 'Ctrl+Alt+M' }, { id: 'search', trigger: '' },
  { id: 'rec', trigger: '' }, { id: 'none', trigger: '' }, { id: 'twice', trigger: '' }, { id: 'old', trigger: 'Ctrl+Alt+O' }]);

// Settings: grouped per app, in order
eq('by app', byApp([
  { app: 'desktop:a', id: 'one', description: 'One', trigger: 'Ctrl+Alt+1' },
  { app: 'desktop:b', id: 'two', description: 'Two', trigger: '' },
  { app: 'desktop:a', id: 'three', description: 'Three', trigger: 'F9' },
  { app: '', id: 'x' }, null,
]), [
  { app: 'desktop:a', items: [{ id: 'one', description: 'One', trigger: 'Ctrl+Alt+1' }, { id: 'three', description: 'Three', trigger: 'F9' }] },
  { app: 'desktop:b', items: [{ id: 'two', description: 'Two', trigger: '' }] },
]);
eq('by app of nothing', byApp(undefined), []);

console.log(`global-shortcuts-model-test: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
