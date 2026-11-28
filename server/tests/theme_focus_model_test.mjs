// theme_focus_model_test.mjs — a community theme's own text focus (sdk/themefocusmodel.js; security scan 0.8.9.6.2, F2):
// its tokens are registered under the facade's own, LordOS's real tokens never reach it, and it never takes the keys
// while a field of LordOS's, a system prompt or a dialog has them.
//
//   node tests/theme_focus_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { createThemeFocus, THEME_FOCUS_MAX, themeFocusBlocked } = await import(path.join(here, '../../sdk/themefocusmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  if (got !== want) { bad++; console.log(`✗ ${name}: want ${String(want)}, got ${String(got)}`); }
};

// a stand-in for sdk/components.jsx's focus: one owner, a handler per token, keys to the owner's handler
let focused = null, blocked = false;
const handlers = new Map();
const real = {
  take: (t, fn) => { handlers.set(t, fn); focused = t; },
  release: (t) => { handlers.delete(t); if (focused === t) focused = null; },
  owner: () => focused,
  restore: (t) => { if (handlers.has(t)) focused = t; },
};
const key = (e) => { if (focused !== null && handlers.has(focused)) handlers.get(focused)(e); };
const heard = [];
const f = createThemeFocus({ ...real, blocked: () => blocked, wrap: (fn) => (e) => fn(e) });

// the keyring prompt's password field (LordOS's own TextInput) has the keys
const secret = Symbol('lordos field');
const typedInSecret = [];
real.take(secret, (e) => typedInSecret.push(e.char));
eq('LordOS\'s field: the theme sees the stand-in, never the real token', f.owner(), f.other);
eq('… and focusToken is the same stand-in', f.owner() === secret, false);
const mine = {};
eq('takeTextFocus refused while LordOS\'s field has the keys', f.take(mine, (e) => heard.push(e.char)), false);
eq('a token object made up by the theme refused too', f.take({}, (e) => heard.push(e.char)), false);
eq('the stand-in can\'t be taken either', f.take(f.other, () => {}), false);
eq('restoreFocus(stand-in) does nothing', f.restore(f.other), false);
eq('the field keeps the keys', focused, secret);
key({ char: 'p' });
eq('the password goes to the field, not the theme', typedInSecret.join('') + '|' + heard.join(''), 'p|');

// the person clicks the theme's menu bar: LordOS's field lets go, the menu takes the keys under the facade's token
real.release(secret);
eq('no one has the keys: null', f.owner(), null);
eq('the theme\'s menu takes them', f.take(mine, (e) => heard.push(e.char)), true);
eq('focused is the facade\'s own token, not the theme\'s value', focused !== mine && focused !== null && Object.isFrozen(focused), true);
eq('the theme sees its own token as the owner', f.owner(), mine);
key({ char: 'a' });
eq('its handler hears its keys', heard.join(''), 'a');
eq('it takes them again (keepKeys)', f.take(mine, (e) => heard.push(e.char)), true);

// a system prompt opens over it (the system band) or a dialog owns the keyboard: no key reaches the theme
blocked = true;
key({ char: 'x' });
eq('a system prompt open: the key is dropped', heard.join(''), 'a');
eq('… and taking the keys is refused', f.take({}, () => {}), false);
blocked = false;

// the prompt's field takes the keys: the theme can't take them back, by its token or restoreFocus
real.take(secret, (e) => typedInSecret.push(e.char));
eq('LordOS\'s field took the keys: the theme sees the stand-in', f.owner(), f.other);
eq('re-taking with its own token refused', f.take(mine, (e) => heard.push(e.char)), false);
eq('restoreFocus(its own token) refused', f.restore(mine), false);
key({ char: 's' });
eq('the password goes to the field', typedInSecret.join('') + '|' + heard.join(''), 'ps|a');

// back to the theme when the field is gone, and release
real.release(secret);
eq('restoreFocus to its own token', f.restore(mine), true);
eq('… which has the keys', f.owner(), mine);
f.release(mine);
eq('released: no one', f.owner(), null);
eq('a released token is not restored', f.restore(mine), false);

// never sticky: a click on an app window takes the keys back (drop)
eq('the menu takes the keys again', f.take(mine, (e) => heard.push(e.char)), true);
eq('a click on an app window: dropped', f.drop(), true);
eq('… no one has the keys', f.owner(), null);
key({ char: 'z' });
eq('… and its handler hears nothing more', heard.join(''), 'a');
eq('… its token is forgotten (restoreFocus does nothing)', f.restore(mine), false);
real.take(secret, () => {});
eq('drop never touches a LordOS field\'s keys', f.drop() === false && focused === secret, true);
real.release(secret);

// the policy (themeFocusBlocked): the same as a theme's own key listeners (themekeysmodel.js)
const pol = (o) => themeFocusBlocked({ layerOpen: false, dialogOwner: false, focusedApp: null, holds: 0, ...o });
eq('nothing open, no window focused: free', pol({}), false);
eq('any keyboard layer (LordOS\'s search, a sheet, a prompt): blocked', pol({ layerOpen: true }), true);
eq('a dialog with the keyboard: blocked', pol({ dialogOwner: true }), true);
eq('a LordOS window focused (the Terminal): blocked', pol({ focusedApp: 'terminal' }), true);
eq('… even while the theme holds the keyboard', pol({ focusedApp: 'terminal', holds: 1 }), true);
eq('an unknown window focused: blocked', pol({ focusedApp: '?' }), true);
eq('a Linux app focused: blocked', pol({ focusedApp: 'linux' }), true);
eq('… unless the theme holds the keyboard (its menu)', pol({ focusedApp: 'linux', holds: 1 }), false);

// bounded: at most THEME_FOCUS_MAX tokens at once
let took = 0;
for (let i = 0; i < THEME_FOCUS_MAX + 5; i++) if (f.take(`t${i}`, () => {})) took++;
eq('at most THEME_FOCUS_MAX tokens', took, THEME_FOCUS_MAX);
eq('bad arguments refused', f.take(null, () => {}) || f.take('t-x', 'not a function'), false);

console.log(`${n - bad}/${n} theme focus checks passed`);
process.exit(bad ? 1 : 0);
