// Screenshots: screen captures and screen recording (a thin floating toolbar, toolbar.jsx, with our own design).
//
// Shortcuts (global, also with a Linux app in front):
//   ❖⇧3                 entire screen        ❖⇧4  area (or window, with Space)
//   ❖⇧5                 the Screenshots toolbar (and stop a recording in progress)
//   Print Screen        what Options → Print Screen Key says (GitHub #202): the entire screen by default
// The screen permission starts allowed for this app (pressing the key is the request, GitHub #219; perms.c
// perm_builtin_default), and a "Don't Allow" saved in Settings → Privacy wins.
// Protected windows (wm.setProtected) come out as the protected fill, and one is never captured alone: the server does that, not this app.
// After each capture a floating thumbnail shows up; a click opens the markup editor (editor.jsx: arrow,
// box, line, text, blur, pixelate and solid-fill redaction, crop, resize, turns). Photos and Files'
// Quick Preview open the same editor on any picture (media.js openMarkup). Captures are saved in
// ~/Pictures/Screenshots by default; the toolbar's Options choose another place (toolbarmodel.js saveDirFor).
import {
  View, Text, Image, MediaView, FilePicker, clipboard,
  useAppWindow, useStore, useWindows, wm, windows, getApp,
  t as tr, media, userDirs, fs, addListener, addShortcuts, createStore, windowTitle, settings, notify, microphones, soundState, toast, errorText, dndActive, appNotifyPrefs, direction,
  workAreaOf, screenDisplays, SkinRegion, useSkinned, commandBar, pickCommand, useSettings,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import {
  captureRect, displaysToCapture, displayAt, dragRect, capturePixel, magnify, middleColor, LOUPE_PX, LOUPE_ZOOM,
  displayById, thumbFrame, editorFrame, countdownFrame, badgeFrame,
} from './geometry.js';
import { ocrLangs, cleanText } from './ocr.js';
import { Editor } from './editor.jsx';
import { APP, recording, recent, bar, options, setOptions, addRecent } from './state.js';
import { isRecordMode } from './toolbarmodel.js';
import { saveDirFor, barStops, stepStop, keepRect, restoreRect, hitHandle, adjustRect, handlePoints, printKeyLabel, refusal } from './toolbarmodel.js';
import { Toolbar, barFrame, menuRows, selectable } from './toolbar.jsx';

export function Icon({ size: s }) {
  const corner = (l, tp, w, hh) => <View style={{ position: 'absolute', left: s * l, top: s * tp, width: s * w, height: s * hh, backgroundColor: '#fff', borderRadius: s * 0.02 }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #5e6b7a, #2c333d)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* the selection frame: four corners and a lens */}
      {corner(0.18, 0.2, 0.18, 0.05)}{corner(0.18, 0.2, 0.05, 0.18)}
      {corner(0.64, 0.2, 0.18, 0.05)}{corner(0.77, 0.2, 0.05, 0.18)}
      {corner(0.18, 0.75, 0.18, 0.05)}{corner(0.18, 0.62, 0.05, 0.18)}
      {corner(0.64, 0.75, 0.18, 0.05)}{corner(0.77, 0.62, 0.05, 0.18)}
      <View style={{ position: 'absolute', left: s * 0.36, top: s * 0.38, width: s * 0.28, height: s * 0.24, borderRadius: s * 0.05, backgroundColor: 'rgba(255,255,255,0.9)' }} />
      <View style={{ position: 'absolute', left: s * 0.44, top: s * 0.43, width: s * 0.12, height: s * 0.12, borderRadius: s * 0.06, backgroundColor: '#2c333d' }} />
      <View style={{ position: 'absolute', left: s * 0.47, top: s * 0.46, width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: '#ff5f57' }} />
    </View>
  );
}

// ---- files ----

const pad = (n) => String(n).padStart(2, '0');
function stamp() {
  const d = new Date();
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}.${pad(d.getMinutes())}.${pad(d.getSeconds())}`;
}
function captureDir() {
  const dir = `${userDirs().pictures}/${tr('screenshots.folder')}`;
  try { fs.mkdir(userDirs().pictures); } catch (e) { /* already there */ }
  try { fs.mkdir(dir); } catch (e) { /* already there */ }
  return dir;
}
// where this capture (or recording) goes, per Options → Save to; null: the clipboard alone
function saveDir(o, video = false) {
  const dir = saveDirFor(o.saveTo, userDirs(), tr('screenshots.folder'), video, o.otherDir);
  if (!dir) return null;
  if (dir === `${userDirs().pictures}/${tr('screenshots.folder')}`) return captureDir();
  try { fs.mkdir(dir); } catch (e) { /* already there */ }
  return dir;
}
function freeName(dir, base, ext) {
  for (let n = 1; n < 1000; n++) {
    const p = `${dir}/${base}${n > 1 ? ` (${n})` : ''}.${ext}`;
    try { fs.stat(p); } catch (e) { return p; }
  }
  return `${dir}/${base} ${Date.now()}.${ext}`;
}
const opts = options;

// ---- capture ----

// Saved to ~/Pictures/Screenshots and on the clipboard as well (pasted into a Terminal it goes in as its path,
// which Claude Code attaches as an image); with "only copy" on, the clipboard alone. copy: false for the other
// displays of a ❖⇧3 (the clipboard keeps the first one's: the main display's, displaysToCapture). display: the one
// it was taken of (null: the main one), where its thumbnail or editor shows (issue #252).
async function finish(handle, { copy = true, display = null } = {}) {
  if (!handle) return;
  const o = opts();
  const dir = saveDir(o);
  let path = null, failed = null;
  if (copy || !dir) { const c = await media.copyImage(handle); if (!c.ok) failed = { what: 'copy', error: c.error }; }
  if (dir) {
    const r = await media.saveImage(handle, freeName(dir, tr('screenshots.fileName', { date: stamp() }), 'png'));
    if (r.ok) { path = r.path; addRecent({ path, time: Date.now() }); }
    else failed = { what: failed ? 'both' : 'save', error: r.error };
  }
  if (failed) notSaved(failed);
  // Save to Mail: a new message with the file attached; Save to the editor: it opens on the capture at once
  if (path && copy && o.saveTo === 'mail' && getApp('mail')) wm.open('mail', { new: true, args: { compose: { attachments: [path] } } });
  const at = display ? display.id : null;
  if (copy && o.saveTo === 'editor' && !failed) { openEditor(handle, path, at); return; }
  if (o.thumbnail) showThumb(handle, path, failed ? failed.what : null, dir, at);
  else if (!windows.get().some((w) => w.args?.handle === handle)) media.close(handle);
}

// The work area of a display of lord.screen.displays, by its id (null or gone: the main one), as the theme gives it
// (its menu bar and Dock left out, in every theme). The app's own windows go inside one display: they were placed
// on the whole logical space, so with two displays the editor sat across both (issue #252).
const areaOn = (id) => workAreaOf(displayById(screenDisplays(), id));

// the markup editor, on the display the capture was taken of (display: its id)
function openEditor(handle, path, display = null) {
  wm.open(APP, { instance: `edit${handle}`, args: { handle, path }, frame: editorFrame(areaOn(display)) });
}

// The capture was taken but couldn't be saved or copied (lord-media missing, a full disk…): said, never silent
function notSaved({ what, error }) {
  const title = tr(what === 'copy' ? 'screenshots.notCopied' : what === 'both' ? 'screenshots.notKept' : 'screenshots.notSaved');
  const body = errorText(error || 'media.noHelper');
  const banner = notify({ app: APP, label: tr('apps.screenshots'), title, body });
  if (!banner || dndActive() || appNotifyPrefs(APP).style === 'none') toast.set(`${title}: ${body}`);
}

// the floating thumbnail, a few seconds in the bottom corner of the display that was captured (Options → Show
// Floating Thumbnail). One per display: a ❖⇧3 on two displays shows both captures, each on its own display; with a
// single thumbnail on the main display the last capture (another display's) replaced the main one's (issue #252).
function showThumb(handle, path, failed = null, dir = null, display = null) {
  const folder = dir ? dir.split('/').pop() : '';
  const id = wm.open(APP, { instance: display == null ? 'thumb' : `thumb-${display}`, args: { handle, path, failed, folder, display } });
  wm.setPip(id, thumbFrame(areaOn(display)));
}

// takes the screen once a frame without our windows was drawn (it closes them first)
function afterHide(fn, delay = 160) {
  for (const w of windows.get()) if (w.app === APP && w.id !== `${APP}#edit` && !String(w.id).startsWith(`${APP}#edit`)) wm.close(w.id, true);
  setTimeout(fn, delay);
}

function withTimer(fn) {
  const secs = opts().timer;
  if (!secs) { fn(); return; }
  const id = wm.open(APP, { instance: 'count', args: { secs, start: Date.now() } });
  wm.setPip(id, countdownFrame(areaOn(null)));
  setTimeout(() => { wm.close(id, true); setTimeout(fn, 180); }, secs * 1000);
}

// Nothing was captured: the screen permission is denied (no answer here means the prompt was answered no), the app is
// restricted, or lord-server could not (a failed capture, a protected window alone…; the SDK passes its code on, GitHub
// #219). The person who pressed the key is told the real reason. Do Not Disturb or this app's notification settings would
// hide a banner, so then it is a toast.
function refused(err) {
  // Privacy is offered only when a permission is the reason: lord-media missing, a capture that failed or a recording
  // that didn't start is not fixed there (issue #77, GitHub #219)
  const why = refusal(err);
  const body = errorText(err || { code: why.code, params: { app: tr('apps.screenshots') } });
  const banner = notify({ app: APP, label: tr('apps.screenshots'), title: tr('screenshots.refused'), body,
    actions: !why.permission ? [] : [{ id: 'privacy', label: tr('screenshots.refused.open'), run: () => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } }) }] });
  if (!banner || dndActive() || appNotifyPrefs(APP).style === 'none') toast.set(body);
}
async function allowed(ask) {
  try {
    const got = await ask();
    if (!got) refused();
    return got;
  } catch (e) { refused(e); return null; }
}
// d: a display of lord.screen.displays (captured at its own resolution), or null for Settings' choice
const grab = (d) => allowed(() => media.screenshot(APP, d ? { display: d.id } : undefined));

// ❖⇧4 on every display at once: a capture and a full-screen overlay per display (geometry.js), sharing one
// selection; it stays inside the display it began on.
const area = createStore(null);   // { shots: [{ handle, display }], mode, start, rect, hover, pointer }
function endArea(keep) {
  const a = area.get();
  if (!a) return;
  area.set(null);
  for (const w of windows.get()) if (w.app === APP && String(w.instance || w.id).includes('overlay')) wm.close(w.id, true);
  for (const sh of a.shots) if (sh.handle !== keep) media.close(sh.handle);
}
// A window alone (issue #70): the screen painted again up to that window and no further, so what covered it
// (other windows, a menu, the Dock) is left out and the window is whole. Its sheets are painted over it (a dialog
// its app opened in the modal layer, such as Settings → Wallpaper's file dialog, issue #58). null when lord-server
// can't (a window the theme draws without a frame node, or one gone meanwhile): the still is cropped as before. A
// protected window (the vault, a password prompt, "Protect front window") is refused alone: 'protected', and nothing of it
// is kept (the still around it already has it in black).
async function windowAlone(r, shot) {
  const node = r && r.win ? wm.frameNode(r.win) : 0;
  if (!node) return null;
  try { return await media.screenshot(APP, { ...(shot.display ? { display: shot.display.id } : null), until: node, also: wm.sheetNodes(r.win) }); } catch (e) {
    if (e?.code === 'media.protectedWindow') { refused(e); return 'protected'; }
    return null;
  }
}
async function areaDone(r, at) {
  const a = area.get();
  if (!a) return;
  let shot = a.shots.find((sh) => !sh.display || (at && sh.display.id === at.id)) || a.shots[0];
  endArea(shot.handle);
  if (!r || r.w < 3 || r.h < 3) { media.close(shot.handle); return; }
  if (a.mode !== 'window' && opts().remember) setOptions({ lastRect: keepRect(r, at || displayAt(lord.screen.displays, r.x + r.w / 2, r.y + r.h / 2)) });
  if (a.mode === 'record') { media.close(shot.handle); capture.record(r); return; }
  if (a.mode === 'window') {
    const alone = await windowAlone(r, shot);
    if (alone === 'protected') { media.close(shot.handle); return; }
    if (alone) { media.close(shot.handle); shot = { ...shot, handle: alone }; }
  }
  // r is in logical pixels; the capture is at its display's physical resolution (2× at 200 %)
  const info = media.info(shot.handle);
  const p = info && captureRect(r, info.width, info.height, lord.screen, shot.display);
  if (!p) { media.close(shot.handle); return; }
  // a window keeps its own shape: the corners outside its rounded frame come out transparent (issue #70)
  const c = media.crop(shot.handle, p.x, p.y, p.w, p.h, a.mode === 'window' ? p.radius : 0);
  media.close(shot.handle);
  if (a.mode === 'text') { await copyText(c); return; }
  await finish(c, { display: shot.display });
}

// "Copy the text": the selection read by the lord-ocr helper, in the interface's languages, to the clipboard
async function copyText(handle) {
  if (!handle) return;
  try {
    const text = cleanText(await media.ocr(handle, ocrLangs(settings.get().languages || [settings.get().language])));
    if (text) { clipboard.set(text); toast.set(tr('screenshots.textCopied', { count: [...text].length })); }
    else toast.set(tr('screenshots.noText'));
  } catch (e) { toast.set(errorText(e)); } finally { media.close(handle); }
}

// The magnifier under a logical point: the capture's pixels around it, enlarged, and the middle one's color
// (media.pixels reads at most a 15 × 15 block). null off every display.
function loupeAt(a, x, y) {
  const shot = a.shots.find((sh) => !sh.display || (x >= sh.display.x && y >= sh.display.y && x < sh.display.x + sh.display.w && y < sh.display.y + sh.display.h));
  const info = shot && media.info(shot.handle);
  if (!info) return null;
  const p = capturePixel(x, y, info.width, info.height, lord.screen, shot.display);
  const half = LOUPE_PX >> 1;
  const buf = media.pixels(shot.handle, p.x - half, p.y - half, LOUPE_PX, LOUPE_PX);
  if (!buf) return null;
  const rgba = new Uint8Array(buf);
  return { data: magnify(rgba), color: middleColor(rgba).hex, px: p.x, py: p.y };
}
function copyColor(hex) {
  clipboard.set(hex);
  toast.set(tr('screenshots.colorCopied', { color: hex }));
}

export const capture = {
  // ❖⇧3: a file per display, as other systems do
  full: () => afterHide(() => withTimer(async () => {
    let first = true;
    for (const d of displaysToCapture(lord.screen)) { const h = await grab(d); if (!h) return; await finish(h, { copy: first, display: d }); first = false; }
  })),
  // mode: 'area', 'window', 'color', 'text' or 'record' (the area to record: on the main display, which is what is
  // recorded). With Remember Last Selection the last area comes back with its handles, and Enter or the button takes
  // it; an area to record is always confirmed so.
  area: (mode = 'area') => afterHide(() => (mode === 'record' ? (f) => f() : withTimer)(async () => {
    endArea();
    const shots = [];
    const main = (lord.screen.displays || []).length > 1 && !lord.screen.mirror ? lord.screen.displays.find((d) => d.main) || null : null;
    for (const d of (mode === 'record' ? [main] : displaysToCapture(lord.screen))) {
      const h = await grab(d);
      if (!h) { shots.forEach((sh) => media.close(sh.handle)); return; }
      shots.push({ handle: h, display: d });
    }
    const o = opts();
    const kept = (mode === 'area' || mode === 'record' || mode === 'text') && o.remember ? restoreRect(o.lastRect, lord.screen.displays, lord.screen) : null;
    const usable = kept && (mode !== 'record' || !main || displayAt([main], kept.x, kept.y));
    area.set({ shots, mode, start: null, rect: usable ? kept : null, hover: null, pointer: null, adjust: mode === 'record' || !!usable, grab: null });
    // in the capture layer, above every other: a sheet, menu, prompt or Live Speech that was open is part of the
    // still, and the selection is drawn over it and gets the pointer there as well
    shots.forEach((sh, i) => {
      const d = sh.display;
      const id = wm.open(APP, { instance: `overlay${i}`, args: { index: i }, ...(d ? { frame: { x: d.x, y: d.y, w: d.w, h: d.h } } : {}) });
      wm.setFullscreen(id, true, { capture: true });
    });
  })),
  // region: an area of the main display (logical pixels), or the whole screen
  async record(region = null) {
    const cur = recording.get();
    if (cur) { media.stop(cur.id); return; }
    afterHide(() => withTimer(async () => {
      const o = opts();
      const path = freeName(saveDir(o, true), tr('screenshots.recName', { date: stamp() }), 'mp4');
      const id = await allowed(() => media.screenRecord(APP, path, { mic: o.mic, ...(o.mic && o.micSource ? { source: o.micSource } : null),
        appName: tr('apps.screenshots'), cursor: o.pointer ? 1 : 0, ...(region ? { region: { x: region.x, y: region.y, w: region.w, h: region.h } } : null) }));
      if (!id) return;
      recording.set({ id, path, start: Date.now() });
      const off = media.on(id, (type, d) => {
        if (type === 'done' || type === 'closed') {
          off();
          recording.set(null);
          wm.close(`${APP}#rec`, true);
          if (type === 'done' && d.ok) addRecent({ path: d.path, time: Date.now(), video: true });
          media.close(id);
        }
      });
      const rid = wm.open(APP, { instance: 'rec' });
      wm.setPip(rid, badgeFrame(areaOn(null)));
    }), 220);
  },
};

// global shortcuts
const hasMedia = () => typeof lord !== 'undefined' && !!lord.media;
// While a selection is open its keys are shortcuts: the dispatcher takes them, so they never reach the app in
// front as well (a C or a Space used to be typed into a Terminal under the overlay).
const selecting = () => !!area.get();
const toggleWindowMode = () => area.set((x) => (x ? { ...x, mode: x.mode === 'area' ? 'window' : 'area', start: null, rect: null } : x));
const copyUnderPointer = () => { const l = area.get()?.loupe; if (l) { copyColor(l.color); endArea(); } };
const captureFull = () => capture.full();
const captureArea = () => capture.area('area');
// the Print Screen key (GitHub #202): Options → Print Screen Key; 'none' leaves the key to the app in front
const printKey = () => opts().printKey;
function pressPrintKey() {
  const a = printKey();
  if (a === 'area') capture.area('area');
  else if (a === 'window') capture.area('window');
  else if (a === 'toolbar') { if (barShown()) closeToolbar(); else openToolbar(); }
  else if (a !== 'none') capture.full();
}

// ---- the toolbar (toolbar.jsx) ----

export function openToolbar() {
  if (recording.get()) { capture.record(); return; }
  bar.set({ ring: null, menu: false, page: 'main', lit: -1 });
  const frame = barFrame(opts().barPos);
  if (wm.get(APP)) wm.setPip(APP, frame); else wm.open(APP, { pip: frame });
}
const closeToolbar = () => { bar.set({ menu: false, page: 'main', lit: -1 }); wm.close(APP, true); };
const barShown = () => !!wm.get(APP)?.pip && !area.get();
const front = () => windows.get().filter((w) => w.app !== APP && !w.minimized && !w.pip && w.desktop === wm.desktop()).pop() || null;

const barActions = {
  get hasMail() { return !!getApp('mail'); },
  close: closeToolbar,
  primary() {
    const m = opts().mode;
    if (m === 'full') capture.full();
    else if (m === 'window') capture.area('window');
    else if (m === 'recordFull') capture.record();
    else if (m === 'recordArea') capture.area('record');
    else capture.area('area');
  },
  color: () => capture.area('color'),
  text: () => capture.area('text'),
  protectFront() { const f = front(); if (f) wm.setProtected(f.id, !f.protected); },
  openRecent(r) { closeToolbar(); wm.open(r.video ? 'player' : 'photos', { new: true, args: { path: r.path } }); },
  // Other Location…: a folder chosen in the system's file dialog, in a window of its own; it is remembered
  // (the toolbar goes meanwhile, so its keys are the dialog's, and comes back after)
  chooseFolder() {
    closeToolbar();
    wm.open(APP, { instance: 'folder', frame: editorFrame(areaOn(null), 820, 540) });
  },
};

// the keys while the toolbar shows: the Options menu's rows when it is open, else the bar's items
const rowsNow = () => {
  const ws = windows.get();
  return menuRows(opts(), bar.get().page, { mics: microphones(soundState.get()), recents: recent.get(), front: front(), protectedCount: ws.filter((w) => w.protected).length, actions: barActions });
};
function pressStop(id) {
  if (id === 'close') closeToolbar();
  else if (id === 'color' || id === 'text') barActions[id]();
  else if (id === 'options') bar.set({ menu: true, page: 'main', lit: firstRow(rowsNow(), -1, 1) });
  else if (id === 'primary') barActions.primary();
  else { setOptions({ mode: id }); }
}
function firstRow(rows, from, step) {
  for (let i = from + step; i >= 0 && i < rows.length; i += step) if (selectable(rows[i])) return i;
  return from;
}
function barKey(name) {
  const b = bar.get();
  if (b.menu) {
    const rows = rowsNow();
    if (name === 'Escape') { bar.set(b.page !== 'main' ? { page: 'main', lit: 0 } : { menu: false, lit: -1 }); return; }
    if (name === 'ArrowDown' || name === 'ArrowUp') { bar.set({ lit: firstRow(rows, b.lit, name === 'ArrowDown' ? 1 : -1) }); return; }
    if ((name === 'Enter' || name === 'Space') && selectable(rows[b.lit])) {
      const row = rows[b.lit];
      row.run();
      if (!row.keep) bar.set({ menu: false, page: 'main', lit: -1 });
    }
    return;
  }
  if (name === 'Escape') { closeToolbar(); return; }
  if (name === 'ArrowLeft' || name === 'ArrowRight') {
    const step = (name === 'ArrowRight' ? 1 : -1) * (direction() === 'rtl' ? -1 : 1);
    bar.set({ ring: stepStop(barStops(), b.ring, step) });
    return;
  }
  if (name === 'ArrowDown' && b.ring === 'options') { pressStop('options'); return; }
  if (name === 'Enter' || name === 'Space') { if (b.ring) pressStop(b.ring); else if (name === 'Enter') barActions.primary(); }
}
const ringOrMenu = () => barShown() && (!!bar.get().ring || bar.get().menu);

// While a selection waits with its handles (Remember Last Selection, an area to record), Enter takes it
const adjusting = () => !!area.get()?.adjust && !!area.get()?.rect;
function confirmArea() {
  const a = area.get();
  if (a?.rect) areaDone(a.rect, displayAt((a.shots || []).map((sh) => sh.display).filter(Boolean), a.rect.x + a.rect.w / 2, a.rect.y + a.rect.h / 2));
}

addShortcuts([
  { id: 'capture.full', keys: ['Meta+Shift+3'], when: hasMedia, run: captureFull, label: 'shell.shortcuts.captureFull', group: 'system' },
  // its row in Settings → Keyboard follows the choice (the whole screen shares ❖⇧3's row); not listed when it does nothing
  { id: 'capture.printKey', keys: ['PrintScreen'], when: () => hasMedia() && printKey() !== 'none', run: pressPrintKey,
    get label() { return printKeyLabel(printKey()) || 'shell.shortcuts.captureFull'; }, listed: () => printKey() !== 'none', group: 'system' },
  { id: 'capture.area', keys: ['Meta+Shift+4'], when: hasMedia, run: captureArea, label: 'shell.shortcuts.captureArea', group: 'system' },
  { id: 'capture.panel', keys: ['Meta+Shift+5'], when: hasMedia, run: () => { if (barShown()) closeToolbar(); else openToolbar(); }, label: 'shell.shortcuts.capturePanel', group: 'system' },
  { id: 'capture.confirm', keys: ['Enter'], when: adjusting, run: confirmArea, listed: () => false },
  { id: 'capture.bar.escape', keys: ['Escape'], when: barShown, run: () => barKey('Escape'), listed: () => false },
  { id: 'capture.bar.enter', keys: ['Enter'], when: barShown, run: () => barKey('Enter'), listed: () => false },
  { id: 'capture.bar.space', keys: ['Space'], when: ringOrMenu, run: () => barKey('Space'), listed: () => false },
  { id: 'capture.bar.left', keys: ['ArrowLeft'], when: barShown, run: () => barKey('ArrowLeft'), listed: () => false },
  { id: 'capture.bar.right', keys: ['ArrowRight'], when: barShown, run: () => barKey('ArrowRight'), listed: () => false },
  { id: 'capture.bar.down', keys: ['ArrowDown'], when: ringOrMenu, run: () => barKey('ArrowDown'), listed: () => false },
  { id: 'capture.bar.up', keys: ['ArrowUp'], when: ringOrMenu, run: () => barKey('ArrowUp'), listed: () => false },
  { id: 'capture.cancel', keys: ['Escape'], when: selecting, run: () => endArea(), listed: () => false },
  { id: 'capture.windowMode', keys: ['Space'], when: selecting, run: toggleWindowMode, listed: () => false },
  { id: 'capture.copyColor', keys: ['c', 'Shift+c'], when: selecting, run: copyUnderPointer, listed: () => false },
  { id: 'capture.textMode', keys: ['t', 'Shift+t'], when: selecting, run: () => area.set((x) => (x ? { ...x, mode: x.mode === 'text' ? 'area' : 'text', start: null, rect: null } : x)), listed: () => false },
]);

// ---- the app's windows ----

function Overlay({ index }) {
  const a = useStore(area);
  const { windows: list } = useWindows();
  const shot = a?.shots[index];
  const d = shot?.display || null;              // this overlay's display (null: the main one, Settings' choice)
  const ox = d ? d.x : 0, oy = d ? d.y : 0;     // global → this overlay's pixels
  useEffect(() => { if (shot) media.view(shot.handle, { zoom: 1, cx: 0.5, cy: 0.5, background: 0xff000000 }); }, [shot?.handle]);
  useEffect(() => {
    if (index !== 0) return undefined;          // the pointer, once for every overlay
    const displays = () => (area.get()?.shots || []).map((sh) => sh.display).filter(Boolean);
    const offMove = addListener('pointermove', (e) => {
      const cur = area.get();
      if (!cur) return;
      const loupe = cur.mode !== 'window' ? loupeAt(cur, e.x, e.y) : null;
      // a handle or the inside of a selection that waits (adjust): it is resized or moved inside its display
      if (cur.grab) {
        const g = cur.grab;
        area.set({ ...cur, pointer: { x: e.x, y: e.y }, loupe, rect: adjustRect(g.rect, g.what, e.x - g.x, e.y - g.y, g.bounds) });
        return;
      }
      if (cur.start) { area.set({ ...cur, pointer: { x: e.x, y: e.y }, loupe, rect: dragRect(cur.start, e.x, e.y, cur.start.display) }); return; }
      // the window under the pointer (the topmost one), for the window mode
      const vis = list.filter((w) => !w.minimized && w.app !== APP && (w.pip || w.desktop == null || w.desktop === wm.desktop()));
      let found = null;
      for (const w of vis) { const f = wm.frame(w); if (e.x >= f.x && e.y >= f.y && e.x < f.x + f.w && e.y < f.y + f.h) found = { x: f.x, y: f.y, w: f.w, h: f.h, win: w.fullscreen ? null : w.id, radius: w.fullscreen ? 0 : wm.cornerRadius(w.id), title: windowTitle(w), display: displayAt(displays(), f.x + f.w / 2, f.y + f.h / 2) }; }
      area.set({ ...cur, pointer: { x: e.x, y: e.y }, hover: found, loupe });
    });
    const offUp = addListener('pointerup', (e) => {
      const cur = area.get();
      if (cur?.grab) { area.set({ ...cur, grab: null }); return; }
      if (!cur?.start) return;
      const r = dragRect(cur.start, e.x, e.y, cur.start.display);
      // one that waits is taken with Enter or its button; a click without a drag keeps the one there was
      if (cur.adjust) { area.set({ ...cur, start: null, rect: r.w >= 3 && r.h >= 3 ? r : cur.prev || null, prev: null }); return; }
      areaDone(r, cur.start.display);
    });
    return () => { offMove(); offUp(); };
  }, [list, index]);
  if (!a || !shot) return null;
  const down = (e) => {
    if (a.mode === 'color') { if (a.loupe) copyColor(a.loupe.color); endArea(); return; }
    const shown = (a.shots || []).map((sh) => sh.display).filter(Boolean);
    const what = a.adjust && a.rect ? hitHandle(a.rect, e.x, e.y) : null;
    if (what) {
      const dd = displayAt(shown, a.rect.x + a.rect.w / 2, a.rect.y + a.rect.h / 2);
      const bounds = dd ? { x: dd.x, y: dd.y, w: dd.w, h: dd.h } : { x: 0, y: 0, w: lord.screen.width, h: lord.screen.height };
      area.set({ ...a, grab: { what, x: e.x, y: e.y, rect: a.rect, bounds } });
      return;
    }
    if (a.mode === 'record' && shown.length && !displayAt(shown, e.x, e.y)) return;
    if (a.mode === 'area' || a.mode === 'text' || a.mode === 'record') area.set({ ...a, prev: a.rect, start: { x: e.x, y: e.y, display: displayAt((a.shots || []).map((sh) => sh.display).filter(Boolean), e.x, e.y) } });
    else if (a.hover) areaDone(a.hover, a.hover.display);
  };
  const dim = 'rgba(0,0,0,0.35)';
  const g = a.mode === 'window' ? a.hover : a.rect;
  const r = g ? { x: g.x - ox, y: g.y - oy, w: g.w, h: g.h, title: g.title } : null;
  const p = a.pointer ? { x: a.pointer.x - ox, y: a.pointer.y - oy } : null;
  const here = p && (!d || (p.x >= 0 && p.y >= 0 && p.x < d.w && p.y < d.h));
  const guide = 'rgba(255,255,255,0.55)';
  // the waiting selection is on this overlay's display
  const mine = !!r && (!d || (r.x + r.w / 2 >= 0 && r.y + r.h / 2 >= 0 && r.x + r.w / 2 < d.w && r.y + r.h / 2 < d.h));
  const hint = a.mode === 'window' ? 'screenshots.hintWindow' : a.mode === 'color' ? 'screenshots.hintColor' : a.mode === 'text' ? 'screenshots.hintText'
    : a.adjust && a.rect ? (a.mode === 'record' ? 'screenshots.hintAdjustRecord' : 'screenshots.hintAdjust') : a.mode === 'record' ? 'screenshots.hintRecord' : 'screenshots.hintArea';
  return (
    <View style={{ flex: 1, direction: 'ltr', cursor: 'crosshair' }} onMouseDown={down}>
      <MediaView media={shot.handle} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
      {r ? (
        <>
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, height: Math.max(0, r.y), backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: 0, top: r.y + r.h, right: 0, bottom: 0, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: 0, top: r.y, width: Math.max(0, r.x), height: r.h, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: r.x + r.w, top: r.y, right: 0, height: r.h, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: r.x, top: r.y, width: r.w, height: r.h, border: `1px solid ${a.mode === 'window' ? '#4c8dff' : '#fff'}`,
            backgroundColor: a.mode === 'window' ? 'rgba(76,141,255,0.15)' : 'transparent' }} />
          <View style={{ position: 'absolute', left: r.x + r.w + 6, top: r.y + r.h + 6, paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.7)' }}>
            <Text style={{ fontSize: 11, color: '#fff' }}>{a.mode === 'window' ? r.title : `${Math.round(r.w)} × ${Math.round(r.h)}`}</Text>
          </View>
          {/* a selection that waits: its eight handles and the button that takes it (Enter does too) */}
          {a.adjust && !a.start && mine ? (
            <>
              {handlePoints(r).map((hp) => (
                <View key={hp.id} style={{ position: 'absolute', left: hp.x - 5, top: hp.y - 5, width: 10, height: 10, borderRadius: 5, backgroundColor: '#fff',
                  border: '1px solid rgba(0,0,0,0.45)', boxShadow: '0 1px 3px rgba(0,0,0,0.4)', pointerEvents: 'none' }} />
              ))}
              <View onMouseDown={(e) => { e.stopPropagation?.(); }} onClick={confirmArea} accessibilityLabel={tr(a.mode === 'record' ? 'screenshots.record' : 'screenshots.capture')}
                style={{ position: 'absolute', left: r.x + r.w / 2 - 60, top: r.y + r.h + 46 > (d ? d.h : lord.screen.height) ? r.y + r.h - 44 : r.y + r.h + 12, width: 120, height: 32,
                  borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: a.mode === 'record' ? '#ff453a' : '#0a84ff', boxShadow: '0 6px 18px rgba(0,0,0,0.45)' }}>
                <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff' }}>{tr(a.mode === 'record' ? 'screenshots.record' : 'screenshots.capture')}</Text>
              </View>
            </>
          ) : null}
        </>
      ) : <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: dim }} />}
      {/* the crosshair: guides through the pointer across this display and its coordinates on the display */}
      {a.mode !== 'window' && here ? (
        <>
          <View style={{ position: 'absolute', left: 0, right: 0, top: Math.round(p.y), height: 1, backgroundColor: guide, pointerEvents: 'none' }} />
          <View style={{ position: 'absolute', top: 0, bottom: 0, left: Math.round(p.x), width: 1, backgroundColor: guide, pointerEvents: 'none' }} />
          {!a.start && !a.loupe && (
            <View style={{ position: 'absolute', left: p.x + 12, top: p.y + 12, paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.7)', pointerEvents: 'none' }}>
              <Text style={{ fontSize: 11, color: '#fff', fontFamily: 'monospace' }}>{`${Math.round(p.x)}, ${Math.round(p.y)}`}</Text>
            </View>
          )}
        </>
      ) : null}
      {/* the magnifier: the capture's pixels around the pointer, its color and the capture pixel's coordinates; it
          flips to the other side near the display's right or bottom edge */}
      {a.mode !== 'window' && here && a.loupe ? (() => {
        const size = LOUPE_PX * LOUPE_ZOOM, w = d ? d.w : lord.screen.width, h = d ? d.h : lord.screen.height;
        const lx = p.x + 24 + size > w ? p.x - 24 - size : p.x + 24, ly = p.y + 24 + size + 40 > h ? p.y - 24 - size - 40 : p.y + 24;
        return (
          <View style={{ position: 'absolute', left: lx, top: ly, width: size, pointerEvents: 'none', borderRadius: 10, overflow: 'hidden',
            border: '2px solid rgba(255,255,255,0.9)', boxShadow: '0 8px 24px rgba(0,0,0,0.5)', backgroundColor: 'rgba(0,0,0,0.8)' }}>
            <Image pixels={{ width: size, height: size, data: a.loupe.data }} style={{ width: size, height: size }} />
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6, height: 20 }}>
              <View style={{ width: 10, height: 10, borderRadius: 2, backgroundColor: a.loupe.color, border: '1px solid rgba(255,255,255,0.6)' }} />
              <Text style={{ fontSize: 11, color: '#fff', fontFamily: 'monospace' }}>{a.loupe.color}</Text>
            </View>
            <Text style={{ fontSize: 10, color: 'rgba(255,255,255,0.75)', fontFamily: 'monospace', paddingHorizontal: 6, paddingBottom: 4 }}>{`${a.loupe.px}, ${a.loupe.py}`}</Text>
          </View>
        );
      })() : null}
      {(index === 0 || (here && !a.start)) && (
        <View style={{ position: 'absolute', left: 0, right: 0, top: 18, alignItems: 'center', pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 14, height: 30, justifyContent: 'center', borderRadius: 15, backgroundColor: 'rgba(0,0,0,0.7)' }}>
            <Text style={{ fontSize: 12, color: '#fff' }}>{tr(hint)}</Text>
          </View>
        </View>
      )}
    </View>
  );
}

function Thumb({ handle, path, failed, folder, display }) {
  const win = useAppWindow();
  const [hover, setHover] = useState(false);
  useEffect(() => {
    media.view(handle, { fit: 'cover', background: 0xff222222 });
    const tmr = setTimeout(() => { if (!hover) wm.close(win.id, true); }, 8000);
    return () => clearTimeout(tmr);
  }, [handle, hover]);
  useEffect(() => () => { if (!windows.get().some((w) => w.args?.handle === handle && w.id !== win.id)) media.close(handle); }, []);
  return (
    <View style={{ flex: 1 }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      accessibilityLabel={tr('screenshots.thumbOpen')} title={tr('screenshots.thumbOpen')}
      onClick={() => {
        openEditor(handle, path, display);
        setTimeout(() => wm.close(win.id, true), 50);
      }}>
      <MediaView media={handle} style={{ flex: 1 }} />
      <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, paddingHorizontal: 10, paddingVertical: 5, backgroundColor: 'rgba(0,0,0,0.55)' }}>
        <Text style={{ fontSize: 11, color: '#fff' }}>{path ? tr('screenshots.savedTo', { folder: folder || tr('screenshots.folder') }) : failed === 'copy' ? tr('screenshots.notCopied') : failed === 'both' ? tr('screenshots.notKept') : tr('screenshots.copied')}</Text>
      </View>
    </View>
  );
}

function Countdown({ secs, start }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const tmr = setInterval(() => setNow(Date.now()), 200); return () => clearInterval(tmr); }, []);
  const left = Math.max(0, Math.ceil(secs - (now - start) / 1000));
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(20,20,24,0.85)' }}>
      <Text style={{ fontSize: 56, fontWeight: 600, color: '#fff' }}>{left}</Text>
    </View>
  );
}

function RecBadge() {
  const rec = useStore(recording);
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const tmr = setInterval(() => setNow(Date.now()), 500); return () => clearInterval(tmr); }, []);
  const secs = rec ? Math.floor((now - rec.start) / 1000) : 0;
  return (
    <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12, backgroundColor: 'rgba(30,30,34,0.92)' }}>
      <View style={{ width: 9, height: 9, borderRadius: 5, backgroundColor: '#ff3b30' }} />
      <Text style={{ fontSize: 13, fontWeight: 500, color: '#fff', flex: 1, direction: 'ltr' }}>{`${Math.floor(secs / 60)}:${pad(secs % 60)}`}</Text>
      <View onClick={() => capture.record()} style={{ paddingHorizontal: 12, height: 24, borderRadius: 6, justifyContent: 'center', backgroundColor: '#ff3b30' }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{tr('screenshots.stop')}</Text>
      </View>
    </View>
  );
}

// ---- markup editor (editor.jsx) ----

// a capture right after it was taken (handle, and its file if it was saved), or a picture file (path).
// New Screenshot brings the toolbar back for another capture, with no need to close this window first (GitHub #166);
// a capture hides the toolbar but leaves the markup windows open (afterHide).
function MarkupWindow({ handle, path }) {
  const dir = () => saveDir(opts()) || captureDir();
  return <Editor handle={handle} path={path} app={APP} onNewCapture={openToolbar} newCapturePath={() => freeName(dir(), tr('screenshots.fileName', { date: stamp() }), 'png')}
    onSaved={(p) => { if (p.startsWith(`${dir()}/`) || p.startsWith(`${captureDir()}/`)) addRecent({ path: p, time: Date.now() }); }} />;
}

// Options → Save to → Other Location…: the folder, chosen in the system's file dialog; it is kept for the next captures
function FolderWindow() {
  const win = useAppWindow();
  const o = opts();
  return (
    <View style={{ flex: 1 }}>
      <FilePicker mode="folder" title={tr('screenshots.opt.chooseFolder')} start={o.otherDir || userDirs().pictures}
        onClose={() => { wm.close(win.id, true); openToolbar(); }}
        onPick={(x) => {
          const dir = Array.isArray(x) ? x[0] : x;
          if (dir) setOptions({ saveTo: 'other', otherDir: String(dir).replace(/\/+$/, '') });
          wm.close(win.id, true);
          openToolbar();
        }} />
    </View>
  );
}

export default function Screenshots() {
  const win = useAppWindow();
  const inst = String(win.id).split('#')[1] || '';
  const a = win.args || {};
  if (String(inst).startsWith('overlay') && a.index != null) return <Overlay index={a.index} />;
  if (inst.startsWith('thumb') && a.handle) return <Thumb handle={a.handle} path={a.path} failed={a.failed} folder={a.folder} display={a.display} />;
  if (inst === 'count') return <Countdown secs={a.secs} start={a.start} />;
  if (inst === 'rec') return <RecBadge />;
  if (inst === 'folder') return <FolderWindow />;
  if (inst.startsWith('edit') && (a.handle || a.path)) return <MarkupWindow handle={a.handle} path={a.path} />;
  return <SkinnedBar />;
}

// (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → the shared shapes"): the capture bar as a command bar (close,
// the five modes, the color picker, text, and Capture or Record). The capturing, the options menu and the files stay
// Screenshots'.
const MODE_TIP = { full: 'screenshots.tip.full', window: 'screenshots.tip.window', area: 'screenshots.tip.area',
  recordFull: 'screenshots.tip.recordFull', recordArea: 'screenshots.tip.recordArea' };
function SkinnedBar() {
  const skinned = useSkinned(APP);
  useSettings();
  const o = opts();
  const bar = skinned ? commandBar({ commands: [
    { id: 'close', label: tr('screenshots.bar.close'), group: 'close' },
    ...Object.keys(MODE_TIP).map((id) => ({ id, label: tr(MODE_TIP[id]), active: o.mode === id, group: isRecordMode(id) ? 'record' : 'capture' })),
    { id: 'color', label: tr('screenshots.tip.color'), group: 'tools' }, { id: 'text', label: tr('screenshots.tip.text'), group: 'tools' },
    { id: 'primary', label: tr(isRecordMode(o.mode) ? 'screenshots.record' : 'screenshots.capture'), group: 'go' },
  ] }) : null;
  const run = (c) => { if (MODE_TIP[c]) setOptions({ mode: c }); else barActions[c](); };
  const vm = bar ? { app: APP, ...bar, actions: { command: (id) => { const c = pickCommand(bar, id); if (c) run(c); } } } : null;
  return <SkinRegion app={APP} region="toolbar" model={vm}><Toolbar actions={barActions} /></SkinRegion>;
}
