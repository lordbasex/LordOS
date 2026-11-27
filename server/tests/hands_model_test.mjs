// hands_model_test.mjs — Hands' reader (sdk/handsmodel.js, docs/HANDS.md): the UI tree as text with ref_N ids. Refs stay
// the same for a node while it lives; a password's value, a protected node's text, a handsDeny node and the system band
// never come out; find; the windows list and the allowlist; and the prompt's texts claim nothing is verified.
import { serializeTree, findNodes, makeRefs, roleOf, windowsList, matchApp, filterLog, allowed, clean, blackoutNodes, optionIndex } from '../../sdk/handsmodel.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

let pass = 0, fail = 0;
const yes = (name, v) => { if (v) pass++; else { fail++; console.log(`FAIL ${name}`); } };
const eq = (name, a, b) => yes(`${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`, JSON.stringify(a) === JSON.stringify(b));

// a host tree as runtime/index.js keeps it
let nextId = 1;
const boxes = new Map();
function node(type, props = {}, kids = [], box = [0, 0, 100, 20]) {
  const n = { id: nextId++, type, props, style: {}, first: null, next: null, text: undefined };
  boxes.set(n.id, box);
  let prev = null;
  for (const k of kids) { if (prev) prev.next = k; else n.first = k; prev = k; k.parent = n; }
  return n;
}
const raw = (text) => { const n = node('raw', {}, []); n.text = text; return n; };
const text = (s, box) => node('text', {}, [raw(s)], box);
const box = (n) => boxes.get(n.id) || null;
const noop = () => {};

const field = node('view', { accessibilityRole: 'textfield', accessibilityLabel: 'Search', accessibilityValue: 'groceries', onClick: noop }, [text('groceries')], [10, 40, 200, 28]);
const pwd = node('view', { accessibilityRole: 'textfield', accessibilityLabel: 'Password', accessibilityValue: 'hunter2', accessibilityState: { secure: true }, protected: true, onClick: noop },
  [text('•••••••')], [10, 80, 200, 28]);
const save = node('view', { accessibilityRole: 'button', onClick: noop }, [text('Save')], [220, 40, 60, 28]);
const denied = node('view', { handsDeny: true }, [node('view', { accessibilityRole: 'switch', accessibilityState: { checked: true }, onClick: noop }, [text('Let an AI agent use this computer')])]);
const plain = node('view', { onClick: noop }, [text('Open recent')], [10, 120, 120, 20]);
const hiddenNode = node('view', { accessibilityRole: 'button', onClick: noop }, [text('Hidden')], [0, 0, 50, 20]);
hiddenNode.hidden = true;
const win = node('view', { lordWin: 'notes', accessibilityLabel: 'Notes' }, [field, pwd, save, denied, plain, hiddenNode], [0, 0, 800, 600]);
const base = node('view', {}, [win], [0, 0, 1280, 800]);
const sysPrompt = node('view', { accessibilityRole: 'button', onClick: noop }, [text('Allow access to your camera')], [500, 300, 200, 40]);
const system = node('view', {}, [sysPrompt], [0, 0, 1280, 800]);
const menu = node('view', {}, [node('view', { accessibilityRole: 'menuitem', onClick: noop }, [text('Import…')], [20, 30, 160, 24])], [0, 0, 1280, 800]);
const roots = [{ node: base, z: 0 }, { node: menu, z: 50 }, { node: system, z: 100 }];

const refs = makeRefs();
const a = serializeTree(roots, { box, refs }).text;
const b = serializeTree(roots, { box, refs }).text;
eq('refs stay the same between reads', a, b);
yes('a window with its id', /window "Notes" id=notes \[ref_\d+\]/.test(a));
yes('a text field with its value', a.includes('textfield "Search"') && a.includes('value="groceries"'));
yes('a button', /button "Save" \[ref_\d+\]/.test(a));
yes('a clickable view named by its text', /clickable "Open recent"/.test(a));
yes('a menu item from the popover layer', /menuitem "Import…"/.test(a));
yes('a password\'s value never', !a.includes('hunter2'));
yes('a protected node shows as protected, nothing inside', a.includes('protected "Password"') && !a.includes('•••'));
yes('a handsDeny node and its inside never', !a.includes('Let an AI agent'));
yes('the system band never (a permission prompt)', !a.includes('camera'));
yes('a hidden node never', !a.includes('Hidden'));
yes('boxes', a.includes('@220,40 60x28'));
const saveRef = /button "Save" \[(ref_\d+)\]/.exec(a)[1];
yes('a ref names its node', refs.nodeOf(saveRef) === save);
save.deleted = true;
yes('a deleted node\'s ref is gone', refs.nodeOf(saveRef) === null);
save.deleted = false;
yes('… back', refs.nodeOf(saveRef) === save);
const again = makeRefs();
yes('a new reader starts again', serializeTree(roots, { box, refs: again }).text.includes('[ref_1]'));

const inter = serializeTree(roots, { box, refs, filter: 'interactive' }).text;
yes('interactive: no texts', !/^\s*text /m.test(inter) && inter.includes('button "Save"'));
yes('interactive: no password value either', !inter.includes('hunter2'));

// even a caller that passed the system band, or a secure field without protected, leaks nothing
const sneaky = node('view', { accessibilityRole: 'textfield', accessibilityValue: 'secret', accessibilityState: { secure: true }, onClick: noop }, [], [0, 0, 10, 10]);
const t2 = serializeTree([{ node: node('view', {}, [sneaky]), z: 0 }], { box, refs }).text;
yes('a secure field without protected: no value', !t2.includes('secret'));
// a getter is never run
const trap = { accessibilityRole: 'button' };
let ran = false;
Object.defineProperty(trap, 'accessibilityLabel', { get() { ran = true; return 'x'; }, enumerable: true });
serializeTree([{ node: node('view', {}, [node('view', trap, [])]), z: 0 }], { box, refs });
yes('a getter in props is never run', !ran);
eq('roles', [roleOf(text('x')), roleOf(node('surface')), roleOf(node('view', { accessibilityRole: 'evil' }))], ['text', 'app', '']);
eq('clean: controls and bidi', clean('a‮b\nc\u0007d'), 'a b c d');

// a zero-size anchor (a theme's menu bar) still shows what it holds
{
  const item = node('view', { onClick: noop }, [text('File')], [10, 2, 40, 24]);
  const anchor = node('view', {}, [item], [0, 0, 0, 0]);
  yes('what a zero-size anchor holds is read', serializeTree([{ node: node('view', {}, [anchor]), z: 20 }], { box, refs }).text.includes('clickable "File"'));
}

// clipped out of sight: a ScrollView's row below its box
const rowOut = node('view', { accessibilityRole: 'row', onClick: noop }, [text('Far away')], [0, 900, 100, 20]);
const scroller = node('view', {}, [rowOut], [0, 0, 300, 300]);
scroller.style = { overflow: 'hidden' };
yes('clipped out of sight: left out', !serializeTree([{ node: scroller, z: 0 }], { box, refs }).text.includes('Far away'));

const f = findNodes(roots, 'save button', { box, refs }).text;
yes('find: the Save button first', f.startsWith('button "Save"'));
yes('find: never the system band', !findNodes(roots, 'camera', { box, refs }).text);
yes('find: never a password value', !findNodes(roots, 'hunter2', { box, refs }).text);

const list = windowsList([{ id: 'notes', app: 'notes', title: 'Notes', x: 1, y: 2, w: 3, h: 4 }, { id: 'files#2', app: 'files', x: 0, y: 0, w: 9, h: 9 }],
  { focusedId: 'notes', allow: ['notes'] });
eq('windows with an allowlist', list.map((w) => [w.id, w.focused]), [['notes', true]]);
yes('allowed: empty list = every app', allowed([], 'x') && !allowed(['a'], 'x'));
eq('an app by name, accents ignored', matchApp([{ id: 'notes', name: 'Notas' }], 'notas')?.id, 'notes');
// (ws170 QA BUG-17) set_value on a pop-up (Settings → AI → Voice → Language) or a segmented control
const OPTS = [['es', 'Español'], ['en', 'English'], ['pt', 'Português'], ['fr', 'Français', true], ['de', 'Deutsch']];
eq('an option by its value', optionIndex(OPTS, 'pt'), 2);
eq('… by its label', optionIndex(OPTS, 'English'), 1);
eq('… by its label, case and accents ignored', optionIndex(OPTS, 'espanol'), 0);
eq('… by the one label it starts', optionIndex(OPTS, 'Deu'), 4);
eq('a disabled one never', optionIndex(OPTS, 'Français'), -1);
eq('none, or more than one meant', [optionIndex(OPTS, 'Klingon'), optionIndex([['a', 'Port A'], ['b', 'Port B']], 'Port')], [-1, -1]);
eq('numbers as values', optionIndex([[25, '25 %'], [50, '50 %']], '50'), 1);
// (ws170 QA BUG-23) a Linux app as the launcher's search finds it: Name[es] shown, its .desktop Name, its id
const LA = [{ id: 'notes', name: 'Notas' }, { id: 'desktop:lordos-toolkit-audit', desktopId: 'lordos-toolkit-audit', name: 'Auditoría de toolkits', aliases: ['Toolkit Audit'] }];
eq('a Linux app by its shown name', matchApp(LA, 'Auditoría de toolkits')?.id, 'desktop:lordos-toolkit-audit');
eq('… by its .desktop Name in English', matchApp(LA, 'Toolkit Audit')?.id, 'desktop:lordos-toolkit-audit');
eq('… by desktop:<id> and by <id>', [matchApp(LA, 'desktop:lordos-toolkit-audit')?.id, matchApp(LA, 'lordos-toolkit-audit')?.id], ['desktop:lordos-toolkit-audit', 'desktop:lordos-toolkit-audit']);
eq('… by a word of its name', matchApp(LA, 'toolkits')?.id, 'desktop:lordos-toolkit-audit');
eq('no app of that name: null', matchApp(LA, 'Calculadora científica'), null);
eq('an exact id wins over a name', matchApp([{ id: 'x', name: 'notes' }, { id: 'notes', name: 'Notas' }], 'notes')?.id, 'notes');
eq('log lines that hold the pattern', filterLog('a error 1\nb ok\nc ERROR 2', 'error', 1), 'c ERROR 2');

// an allowlisted screenshot: everything is black but the allowed window and the menus tied to it (fail closed)
{
  const wallpaper = node('image', {});
  const widget = node('view', {}, [text('Weather 21°')]);
  const allowedWin = node('view', { lordWin: 'notes' }, [text('My note')]);
  const otherWin = node('view', { lordWin: 'mail' }, [text('Inbox: 3 secrets')]);
  const windowsBox = node('view', {}, [allowedWin, otherWin]);
  const themeRoot = node('view', {}, [wallpaper, widget, windowsBox]);
  const dockItem = node('view', {}, [text('Terminal')]);
  const barMenu = node('view', {}, [text('File Edit')]);
  const floating = node('view', {}, [text('pip video')]);
  const search = node('view', {}, [text('Search')]);
  const myMenu = node('view', { handsWin: 'notes' }, [text('Import…')]);
  const otherMenu = node('view', { handsWin: 'mail' }, [text('Forward')]);
  const clip = node('view', {}, [text('Clipboard: password123')]);
  const emoji = node('view', {}, [text('😀')]);
  const mySheet = node('view', { handsWin: 'notes' }, [text('Delete note?')]);
  const tip = node('view', {}, [text('tooltip')]);
  const drag = node('view', {}, [text('drag')]);
  const banner = node('view', {}, [text('New mail from Ana')]);
  const prompt = node('view', {}, [text('Allow camera?')]);
  const L = (z, ...kids) => ({ node: node('view', {}, kids), z });
  const allRoots = [{ node: themeRoot, z: 0 }, L(10, dockItem), L(20, barMenu), L(30, floating), L(40, search), L(50, myMenu, otherMenu, clip, emoji),
    L(60, mySheet), L(80, tip), L(90, drag), L(95, banner), L(100, prompt)];
  const kept = (n) => n === allowedWin || (n.props.handsWin === 'notes');
  const black = new Set(blackoutNodes(allRoots, kept));
  const is = (n) => black.has(n.id);
  yes('the allowed window is not blacked', !is(allowedWin) && !is(themeRoot) && !is(windowsBox));
  yes('its menu and its sheet are not blacked', !is(myMenu) && !is(mySheet));
  yes('the wallpaper and the desktop\'s widgets: black', is(wallpaper) && is(widget));
  yes('another app\'s window: black', is(otherWin));
  for (const [z, n, name] of [[10, dockItem, 'the Dock'], [20, barMenu, 'the menu bar'], [30, floating, 'floating windows'], [40, search, 'search'],
    [50, otherMenu, 'another window\'s menu'], [50, clip, 'the clipboard panel'], [50, emoji, 'the emoji panel'], [80, tip, 'tooltips'],
    [90, drag, 'drag images'], [95, banner, 'notifications']]) yes(`layer ${z}: ${name} black`, is(n));
  yes('the system band is left to lord-server (always black there)', !is(prompt));
  const none = new Set(blackoutNodes(allRoots, () => false));
  yes('nothing allowed: every surface black', none.has(wallpaper.id) && none.has(widget.id) && none.has(windowsBox.id) && none.has(dockItem.id) && none.has(myMenu.id));
}

// the prompt and the settings claim no identity: the name and the program are reported, never verified
const root = fileURLToPath(new URL('../../', import.meta.url));
const cat = JSON.parse(readFileSync(`${root}assets/locales/en/hands.json`, 'utf8'));
const texts = Object.values(cat).flatMap((v) => (typeof v === 'string' ? [v] : Object.values(v)));
yes('no text claims a verified identity', texts.every((s) => !/verified|trusted|system program/i.test(s.replace(/not verified/gi, ''))));
yes('the prompt says the name and program are not verified', /not verified/.test(cat['prompt.reported']));
yes('the prompt says a program of the account asked', /program running on your account/.test(cat['prompt.title']));

console.log(`hands-model-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
