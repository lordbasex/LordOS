// node server/tests/shellx_model_test.mjs — the shell's side of the Wayland shell extras (sdk/shellxmodel.js): where a
// window following a drag goes, the tag as part of a remembered place's key, a restored window's frame, the bell, blur boxes.
import assert from 'node:assert/strict';
import { chromeInset, dragFrameAt, tagKey, placeKeyOf, descriptionTitle, restoredFrame, bellAction, blurBoxes, remembersPlace, fullscreenRectOf } from '../../sdk/shellxmodel.js';

// the theme's frame: 1 px borders and a 41 px title bar (themes/default chrome { w: 2, h: 43 })
const chrome = { w: 2, h: 43 };
assert.deepEqual(chromeInset(chrome, false), { left: 1, top: 42, w: 2, h: 43 });
assert.deepEqual(chromeInset(chrome, true), { left: 0, top: 0, w: 0, h: 0 });

// Classic and Workspaces: 1 px borders, a 30 px title bar and a 26 px menu strip under it for a window with
// menus (every Linux window has its Edit menu with Speech). The strip is part of the frame: left out, the app got
// 26 px less than it asked for and lost 26 more on each of its own resizes (400, 374, 348 on a fullscreen exit).
const classic = { w: 2, h: 32, menu: 26 };
assert.deepEqual(chromeInset(classic, false, true), { left: 1, top: 57, w: 2, h: 58 });
assert.deepEqual(chromeInset(classic, false, false), { left: 1, top: 31, w: 2, h: 32 });      // no menus: no strip
assert.deepEqual(chromeInset(classic, true, true), { left: 0, top: 0, w: 0, h: 0 });          // its own frame: no strip either
assert.deepEqual(chromeInset(chrome, false, true), { left: 1, top: 42, w: 2, h: 43 });        // Flow: no strip (menu bar)
assert.deepEqual(chromeInset(chrome, false), chromeInset(chrome, false, false));
// the app's size round trip (sdk/wayland.js: map and 'geometry' make the frame, the theme's layout gives the content back)
{
  const inset = chromeInset(classic, false, true);
  let content = { w: 600, h: 400 };
  for (let i = 0; i < 3; i++) {                  // three fullscreen exits: the app says its size again each time
    const frame = { w: content.w + inset.w, h: content.h + inset.h };
    content = { w: frame.w - classic.w, h: frame.h - classic.h - classic.menu };
  }
  assert.deepEqual(content, { w: 600, h: 400 });
}
assert.deepEqual(restoredFrame({ x: 333, y: 222, w: 400, h: 300 }, chromeInset(classic, false, true)), { x: 332, y: 165, w: 402, h: 358 });

// xdg-toplevel-drag: the geometry point (20, 10) under the pointer; lord-server's geometry corrects the inset
const f = { x: 100, y: 100, w: 400, h: 300 };
assert.deepEqual(dragFrameAt(f, { gx: 101, gy: 142 }, 20, 10, 500, 400, chromeInset(chrome, false)), { x: 479, y: 348 });
assert.deepEqual(dragFrameAt(f, null, 20, 10, 500, 400, chromeInset(chrome, false)), { x: 479, y: 348 });   // assumed inset
assert.deepEqual(dragFrameAt(f, { gx: 100, gy: 100 }, 20, 10, 500, 400, chromeInset(chrome, true)), { x: 480, y: 390 });

// xdg-toplevel-tag: the app's words, kept to plain characters, beside its own app's key only
assert.equal(tagKey('main window'), 'main window');
assert.equal(tagKey('  e-mail   composer '), 'e-mail composer');
assert.equal(tagKey('Ventana principal'), 'Ventana principal');
assert.equal(tagKey('a/b\\c#d"e'), 'abcde');
assert.equal(tagKey('x'.repeat(100)).length, 64);
assert.equal(tagKey(null), '');
assert.equal(tagKey('###'), '');
assert.equal(placeKeyOf('linux:org.gnome.Nautilus', 'preferences'), 'linux:org.gnome.Nautilus#preferences');
assert.equal(placeKeyOf('linux:org.gnome.Nautilus', ''), 'linux:org.gnome.Nautilus');
assert.equal(placeKeyOf('linux:org.gnome.Nautilus', '///'), 'linux:org.gnome.Nautilus');
assert.equal(placeKeyOf('', 'main'), '');
assert.equal(descriptionTitle('  Main\nwindow '), 'Main window');
assert.equal(descriptionTitle(42), '');

// xdg-session-management: the frame around a restored geometry
assert.deepEqual(restoredFrame({ x: 333, y: 222, w: 400, h: 300 }, chromeInset(chrome, true)), { x: 333, y: 222, w: 400, h: 300 });
assert.deepEqual(restoredFrame({ x: 333, y: 222, w: 400, h: 300 }, chromeInset(chrome, false)), { x: 332, y: 180, w: 402, h: 343 });
assert.equal(restoredFrame(null, chromeInset(chrome, true)), null);
assert.equal(restoredFrame({ x: 1, y: 2, w: 0, h: 5 }, chromeInset(chrome, true)), null);
assert.equal(restoredFrame({ x: 'a', y: 2, w: 3, h: 5 }, chromeInset(chrome, true)), null);

// xdg-system-bell: the sound unless Do Not Disturb; attention for a window that isn't in front
assert.deepEqual(bellAction({ window: true, focused: false, dnd: false }), { sound: true, attention: true });
assert.deepEqual(bellAction({ window: true, focused: true, dnd: false }), { sound: true, attention: false });
assert.deepEqual(bellAction({ window: true, focused: false, dnd: true }), { sound: false, attention: true });
assert.deepEqual(bellAction({ window: false, dnd: false }), { sound: true, attention: false });

// ext-background-effect: only sane boxes, at most 16
assert.deepEqual(blurBoxes([[10, 10, 100, 50], [0, 0, 0, 5], [1, 2, 3], 'x', [1.4, 2.6, 3, 4]]), [{ x: 10, y: 10, w: 100, h: 50 }, { x: 1, y: 3, w: 3, h: 4 }]);
assert.equal(blurBoxes(Array.from({ length: 40 }, () => [0, 0, 1, 1])).length, 16);
assert.deepEqual(blurBoxes(null), []);
// the toolkit audit's windows are never remembered (BUG-12, BUG-24 of the ws170 QA); every other app is
for (const k of ['linux:lordos-toolkit-audit', 'linux:lordos-toolkit-audit-qt6', 'desktop:lordos-toolkit-audit', 'linux:lordos-toolkit-audit-gtk3#main', 'linux:Lordos-toolkit-audit-gtk3']) assert.equal(remembersPlace(k), false, k);
for (const k of ['linux:org.gnome.TextEditor', 'desktop:firefox-esr', 'linux:lordos-toolkit-auditor', 'linux:python3', 'linux:gtk3.py']) assert.equal(remembersPlace(k), true, k);
assert.equal(remembersPlace(''), false);
// a Linux window's fullscreen box from lord-server's event (BUG-30b of the ws170 QA)
assert.deepEqual(fullscreenRectOf({ x: 1920, y: 0, w: 1920, h: 1080 }), { x: 1920, y: 0, w: 1920, h: 1080 });
assert.equal(fullscreenRectOf({}), null);
assert.equal(fullscreenRectOf({ x: 0, y: 0, w: 0, h: 1080 }), null);
assert.equal(fullscreenRectOf(null), null);
console.log('shellx-model: ok');
