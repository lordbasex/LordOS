// screenshots_toolbar_model_test.mjs — the Screenshots toolbar's logic (apps/screenshots/toolbarmodel.js): its options
// as they are saved and read back (an older "only copy to the clipboard" included), the modes and the keyboard's way
// through the bar, where a capture or a recording is saved, and the remembered selection with its handles.
//
//   node tests/screenshots_toolbar_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../apps/screenshots/toolbarmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- options ----
const def = M.normalizeOptions(undefined);
eq('defaults', def, { mode: 'area', saveTo: 'pictures', otherDir: '', timer: 0, mic: false, micSource: '', thumbnail: true, remember: false, pointer: true, lastRect: null, barPos: null });
eq('the panel\'s "only copy" is Save to Clipboard', M.normalizeOptions({ clipboard: true, timer: 5, mic: true }).saveTo, 'clipboard');
eq('the panel\'s timer and microphone are kept', [M.normalizeOptions({ clipboard: true, timer: 5, mic: true }).timer, M.normalizeOptions({ timer: 5, mic: true }).mic], [5, true]);
eq('Save to wins over the old key', M.normalizeOptions({ clipboard: true, saveTo: 'desktop' }).saveTo, 'desktop');
eq('an unknown Save to falls back', M.normalizeOptions({ saveTo: 'nowhere' }).saveTo, 'pictures');
eq('a timer that is not offered is none', M.normalizeOptions({ timer: 7 }).timer, 0);
eq('an unknown mode falls back', M.normalizeOptions({ mode: 'video' }).mode, 'area');
eq('a mode is kept', M.normalizeOptions({ mode: 'recordArea' }).mode, 'recordArea');
eq('thumbnail and pointer can be off', [M.normalizeOptions({ thumbnail: false }).thumbnail, M.normalizeOptions({ pointer: false }).pointer], [false, false]);
eq('remember only when true', [M.normalizeOptions({ remember: 'yes' }).remember, M.normalizeOptions({ remember: true }).remember], [false, true]);
eq('another folder: absolute, no ..', [M.normalizeOptions({ otherDir: 'rel/x' }).otherDir, M.normalizeOptions({ otherDir: '/home/a/../b' }).otherDir, M.normalizeOptions({ otherDir: '/home/a/Shots/' }).otherDir], ['', '', '/home/a/Shots']);
eq('a broken last rect is dropped', M.normalizeOptions({ lastRect: { x: 'a', y: 0, w: 10, h: 10 } }).lastRect, null);
eq('a tiny last rect is dropped', M.normalizeOptions({ lastRect: { x: 0, y: 0, w: 2, h: 10 } }).lastRect, null);
eq('a last rect is kept, rounded', M.normalizeOptions({ lastRect: { x: 10.4, y: 20.6, w: 100, h: 50, display: 'DP-1' } }).lastRect, { x: 10, y: 21, w: 100, h: 50, display: 'DP-1' });
eq('a bar place is kept', M.normalizeOptions({ barPos: { display: 'HDMI-A-1', dx: 300, dy: 900 } }).barPos, { display: 'HDMI-A-1', dx: 300, dy: 900 });
eq('a broken bar place is dropped', M.normalizeOptions({ barPos: { dx: 1 } }).barPos, null);
// what is saved reads back the same (persistence round trip)
const saved = JSON.parse(JSON.stringify(M.normalizeOptions({ mode: 'window', saveTo: 'other', otherDir: '/home/a/S', timer: 10, mic: true, micSource: 'alsa_input.usb', thumbnail: false, remember: true, pointer: false })));
eq('round trip', M.normalizeOptions(saved), saved);

// ---- modes ----
eq('record modes', [M.isRecordMode('recordFull'), M.isRecordMode('recordArea'), M.isRecordMode('area')], [true, true, false]);
eq('primary button', [M.primaryOf('full'), M.primaryOf('recordArea')], ['capture', 'record']);
eq('next mode', M.nextMode('area', 1), 'recordFull');
eq('next mode wraps', M.nextMode('recordArea', 1), 'full');
eq('previous mode wraps', M.nextMode('full', -1), 'recordArea');
eq('unknown mode starts at the first', M.nextMode('x', 1), 'full');

// ---- the keyboard through the bar ----
const stops = M.barStops();
eq('stops', stops, ['close', 'full', 'window', 'area', 'recordFull', 'recordArea', 'color', 'text', 'options', 'primary']);
eq('the first → is the close button', M.stepStop(stops, null, 1), 'close');
eq('the first ← is the primary button', M.stepStop(stops, null, -1), 'primary');
eq('→ goes on', M.stepStop(stops, 'area', 1), 'recordFull');
eq('→ stops at the end', M.stepStop(stops, 'primary', 1), 'primary');
eq('← stops at the start', M.stepStop(stops, 'close', -1), 'close');

// ---- where it goes ----
const dirs = { home: '/home/a', pictures: '/home/a/Pictures', desktop: '/home/a/Desktop', documents: '/home/a/Documents' };
eq('pictures', M.saveDirFor('pictures', dirs, 'Screenshots'), '/home/a/Pictures/Screenshots');
eq('desktop', M.saveDirFor('desktop', dirs, 'Screenshots'), '/home/a/Desktop');
eq('documents', M.saveDirFor('documents', dirs, 'Screenshots'), '/home/a/Documents');
eq('clipboard: no file', M.saveDirFor('clipboard', dirs, 'Screenshots'), null);
eq('a recording with clipboard: the default folder', M.saveDirFor('clipboard', dirs, 'Screenshots', true), '/home/a/Pictures/Screenshots');
eq('mail and the editor: the default folder', [M.saveDirFor('mail', dirs, 'S'), M.saveDirFor('editor', dirs, 'S')], ['/home/a/Pictures/S', '/home/a/Pictures/S']);
eq('another folder', M.saveDirFor('other', dirs, 'S', false, '/home/a/Shots'), '/home/a/Shots');
eq('another folder not chosen yet: the default', M.saveDirFor('other', dirs, 'S', false, ''), '/home/a/Pictures/S');

// ---- remember last selection ----
const two = [{ name: 'eDP-1', x: 0, y: 0, w: 1440, h: 900 }, { name: 'HDMI-A-1', x: 1440, y: 0, w: 1920, h: 1080 }];
const kept = M.keepRect({ x: 1500, y: 100, w: 400, h: 300 }, two[1]);
eq('kept with its display', kept, { x: 1500, y: 100, w: 400, h: 300, display: 'HDMI-A-1' });
eq('restored on its display', M.restoreRect(kept, two), { x: 1500, y: 100, w: 400, h: 300 });
eq('its display gone: not restored', M.restoreRect(kept, [two[0], { name: 'DP-2', x: 1440, y: 0, w: 1920, h: 1080 }]), null);
eq('one display left: restored if it fits', M.restoreRect({ x: 10, y: 10, w: 100, h: 100, display: 'gone' }, [two[0]]), { x: 10, y: 10, w: 100, h: 100 });
eq('no longer fits (smaller display): not restored', M.restoreRect({ x: 1300, y: 10, w: 400, h: 100, display: 'eDP-1' }, two), null);
eq('no displays: the screen', M.restoreRect({ x: 0, y: 0, w: 50, h: 50 }, [], { width: 1280, height: 800 }), { x: 0, y: 0, w: 50, h: 50 });

// ---- handles ----
const r = { x: 100, y: 100, w: 200, h: 100 };
eq('eight handles', M.handlePoints(r).map((p) => p.id), M.HANDLES);
eq('grab the top left handle', M.hitHandle(r, 103, 98), 'nw');
eq('grab the right edge handle', M.hitHandle(r, 300, 150), 'e');
eq('grab the inside', M.hitHandle(r, 200, 150), 'inside');
eq('outside: a new one', M.hitHandle(r, 50, 50), null);
eq('no selection: nothing grabbed', M.hitHandle(null, 1, 1), null);
const b = { x: 0, y: 0, w: 1000, h: 800 };
eq('move inside', M.adjustRect(r, 'inside', 50, -20, b), { x: 150, y: 80, w: 200, h: 100 });
eq('move stops at the display edge', M.adjustRect(r, 'inside', -500, 2000, b), { x: 0, y: 700, w: 200, h: 100 });
eq('resize from the bottom right', M.adjustRect(r, 'se', 30, 40, b), { x: 100, y: 100, w: 230, h: 140 });
eq('resize from the left', M.adjustRect(r, 'w', -30, 999, b), { x: 70, y: 100, w: 230, h: 100 });
eq('resize from the top stops at the display', M.adjustRect(r, 'n', 0, -500, b), { x: 100, y: 0, w: 200, h: 200 });
eq('an edge never passes the other one', M.adjustRect(r, 'e', -500, 0, b), { x: 100, y: 100, w: 8, h: 100 });
eq('an unknown grab changes nothing', M.adjustRect(r, 'zz', 5, 5, b), r);

console.log(bad ? `screenshots-toolbar-model: ${bad} of ${n} failed` : `screenshots-toolbar-model: ${n} checks passed`);
process.exit(bad ? 1 : 0);
