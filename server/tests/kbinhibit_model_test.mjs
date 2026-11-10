// node server/tests/kbinhibit_model_test.mjs — a LordOS window holding the desktop's shortcuts (sdk/kbinhibitmodel.js):
// the same keys as kbi_route in server/src/wl/kbinhibit.c, and a release goes where its press went.
import assert from 'node:assert/strict';
import { routeKey, keyOutcome, shortcutsWho, SHIFT, ALT, CTRL, META } from '../../sdk/kbinhibitmodel.js';

const ESC = 1, TAB = 15, L = 38, Q = 16, A = 30, F1 = 59, F2 = 60, F4 = 62, F12 = 88, DEL = 111, SUPER = 125, VOLUP = 115, PRINT = 99;

// active: ❖ shortcuts, ⌥Tab and ❖ alone reach the window
assert.equal(routeKey({ code: TAB, mods: META }, true), 'app');
assert.equal(routeKey({ code: TAB, mods: ALT }, true), 'app');
assert.equal(routeKey({ code: TAB, mods: ALT | SHIFT }, true), 'app');
assert.equal(routeKey({ code: SUPER, mods: META }, true), 'app');
assert.equal(routeKey({ code: Q, mods: META, key: 'q' }, true), 'app');
assert.equal(routeKey({ code: F4, mods: ALT }, true), 'app');
assert.equal(routeKey({ code: DEL, mods: CTRL | ALT }, true), 'app');
assert.equal(routeKey({ code: ESC, mods: META | SHIFT }, true), 'app');
assert.equal(routeKey({ code: ESC, mods: 0 }, true), 'app');
assert.equal(routeKey({ code: A, mods: 0, key: 'a' }, true), 'app');
assert.equal(routeKey({ code: F2, mods: META | CTRL | ALT }, true), 'app');
// …but ❖Esc, the lock, the console switch and the media row stay LordOS's
assert.equal(routeKey({ code: ESC, mods: META }, true), 'toggle');
assert.equal(routeKey({ code: L, mods: META, key: 'l' }, true), 'lord');
assert.equal(routeKey({ code: L, mods: META, key: 'L' }, true), 'lord');
assert.equal(routeKey({ code: Q, mods: META | CTRL, key: 'q' }, true), 'lord');
assert.equal(routeKey({ code: F1, mods: CTRL | ALT }, true), 'lord');
assert.equal(routeKey({ code: F12, mods: CTRL | ALT | SHIFT }, true), 'lord');
assert.equal(routeKey({ code: VOLUP, mods: 0 }, true), 'lord');
assert.equal(routeKey({ code: PRINT, mods: 0 }, true), 'lord');
// off after ❖Esc: everything as usual, and ❖Esc turns it back on
assert.equal(routeKey({ code: TAB, mods: META }, false), 'lord');
assert.equal(routeKey({ code: ESC, mods: META }, false), 'toggle');

// presses, repeats and releases
const pressed = new Map();
const down = (code, repeat = false) => ({ code, down: true, repeat });
const up = (code) => ({ code, down: false });
assert.equal(keyOutcome(pressed, down(SUPER), 'app'), 'app');
assert.equal(keyOutcome(pressed, down(SUPER, true), null), 'app');        // its repeats follow, whatever holds now
assert.equal(keyOutcome(pressed, down(ESC), 'toggle'), 'toggle');
assert.equal(keyOutcome(pressed, down(ESC, true), 'app'), 'taken');      // ❖Esc's repeats and release are nobody's
assert.equal(keyOutcome(pressed, up(ESC), null), 'taken');
assert.equal(keyOutcome(pressed, up(SUPER), null), 'app');               // ❖ pressed before it turned off: still the window's
assert.equal(pressed.size, 0);
assert.equal(keyOutcome(pressed, down(TAB), 'lord'), null);
assert.equal(keyOutcome(pressed, up(TAB), null), null);                  // pressed for LordOS, released for LordOS
assert.equal(keyOutcome(pressed, down(A), null), null);                  // no window holding them
assert.equal(keyOutcome(pressed, up(A), 'app'), null);
// whose decision: what lord-server checked (a Flatpak app's id, the program it runs) or a LordOS app's id;
// the first format's names ("desktop:<app_id>", "exe:<path>") are ignored
assert.deepEqual(shortcutsWho('flatpak:org.virt_manager.virt-viewer'), { kind: 'flatpak', id: 'org.virt_manager.virt-viewer', short: 'org.virt_manager.virt-viewer' });
assert.deepEqual(shortcutsWho('program:/usr/bin/remote-viewer'), { kind: 'program', path: '/usr/bin/remote-viewer', short: 'remote-viewer' });
assert.deepEqual(shortcutsWho('machines'), { kind: 'lord', id: 'machines' });
assert.equal(shortcutsWho('desktop:org.gnome.Boxes'), null);
assert.equal(shortcutsWho('exe:/usr/bin/remmina'), null);
assert.equal(shortcutsWho('flatpak:../x'), null);
assert.equal(shortcutsWho('flatpak:a/b'), null);
assert.equal(shortcutsWho('program:rel'), null);
assert.equal(shortcutsWho('program:/'), null);
assert.equal(shortcutsWho(''), null);
assert.equal(shortcutsWho(undefined), null);
console.log('kbinhibit_model_test: ok');
