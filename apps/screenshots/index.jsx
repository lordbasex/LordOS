// Screenshots: screen captures and screen recording (a floating capture panel,
// with our own design).
//
// Shortcuts (global, also with a Linux app in front):
//   ❖⇧3 / Print Screen  entire screen        ❖⇧4  area (or window, with Space)
//   ❖⇧5                 the Screenshots panel (and stop a recording in progress)
// Protected windows (wm.setProtected) come out black: the server does that, not this app.
// After each capture a floating thumbnail shows up; a click opens the markup editor (editor.jsx: arrow,
// box, line, text, blur, pixelate and solid-fill redaction, crop, resize, turns). Photos and Files'
// Quick Preview open the same editor on any picture (media.js openMarkup). Captures are saved in
// ~/Pictures/Screenshots or to the clipboard.
import {
  View, Text, Image, MediaView, Toggle, Segmented, GroupBox, FormRow, clipboard,
  useTheme, useAppWindow, useStore, useWindows, wm, windows, typeScale,
  t as tr, media, userDirs, fs, addListener, addShortcuts, createStore, windowTitle, settings, useSettings, notify, toast, errorText, dndActive, appNotifyPrefs,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { captureRect, displaysToCapture, displayAt, dragRect, capturePixel, magnify, middleColor, LOUPE_PX, LOUPE_ZOOM } from './geometry.js';
import { ocrLangs, cleanText } from './ocr.js';
import { Editor } from './editor.jsx';

const APP = 'screenshots';
const recording = createStore(null);   // { id, path, start }
const recent = createStore([]);       // [{ path, time }]

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
function freeName(dir, base, ext) {
  for (let n = 1; n < 1000; n++) {
    const p = `${dir}/${base}${n > 1 ? ` (${n})` : ''}.${ext}`;
    try { fs.stat(p); } catch (e) { return p; }
  }
  return `${dir}/${base} ${Date.now()}.${ext}`;
}
const opts = () => ({ timer: 0, clipboard: false, mic: false, ...(settings.get().screenshots || {}) });

// ---- capture ----

// Saved to ~/Pictures/Screenshots and on the clipboard as well (pasted into a Terminal it goes in as its path,
// which Claude Code attaches as an image); with "only copy" on, the clipboard alone. copy: false for the other
// displays of a ❖⇧3 (the clipboard keeps the first one's).
async function finish(handle, { copy = true } = {}) {
  if (!handle) return;
  const o = opts();
  let path = null, failed = null;
  if (copy || o.clipboard) { const c = await media.copyImage(handle); if (!c.ok) failed = { what: 'copy', error: c.error }; }
  if (!o.clipboard) {
    const r = await media.saveImage(handle, freeName(captureDir(), tr('screenshots.fileName', { date: stamp() }), 'png'));
    if (r.ok) { path = r.path; recent.set((l) => [{ path, time: Date.now() }, ...l].slice(0, 20)); }
    else failed = { what: failed ? 'both' : 'save', error: r.error };
  }
  if (failed) notSaved(failed);
  showThumb(handle, path, failed ? failed.what : null);
}

// The capture was taken but couldn't be saved or copied (lord-media missing, a full disk…): said, never silent
function notSaved({ what, error }) {
  const title = tr(what === 'copy' ? 'screenshots.notCopied' : what === 'both' ? 'screenshots.notKept' : 'screenshots.notSaved');
  const body = errorText(error || 'media.noHelper');
  const banner = notify({ app: APP, label: tr('apps.screenshots'), title, body });
  if (!banner || dndActive() || appNotifyPrefs(APP).style === 'none') toast.set(`${title}: ${body}`);
}

function showThumb(handle, path, failed = null) {
  const id = wm.open(APP, { instance: 'thumb', args: { handle, path, failed } });
  const w = 240, h = 150;
  wm.setPip(id, { x: lord.screen.width - w - 20, y: lord.screen.height - h - 90, w, h });
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
  wm.setPip(id, { x: Math.round(lord.screen.width / 2 - 60), y: Math.round(lord.screen.height / 2 - 60), w: 120, h: 120 });
  setTimeout(() => { wm.close(id, true); setTimeout(fn, 180); }, secs * 1000);
}

// The screen permission is denied (no answer here means that) or the app is restricted (an exception):
// nothing is captured, and the person who pressed the key is told why. Do Not Disturb or this app's
// notification settings would hide a banner, so then it is a toast.
function refused(err) {
  const body = errorText(err || { code: 'media.denied.screen', params: { app: tr('apps.screenshots') } });
  const banner = notify({ app: APP, label: tr('apps.screenshots'), title: tr('screenshots.refused'), body,
    actions: [{ id: 'privacy', label: tr('screenshots.refused.open'), run: () => wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } }) }] });
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
// (other windows, a menu, the Dock) is left out and the window is whole. null when lord-server can't (a window
// the theme draws without a frame node, or one gone meanwhile): the still is cropped as before.
async function windowAlone(r, shot) {
  const node = r && r.win ? wm.frameNode(r.win) : 0;
  if (!node) return null;
  try { return await media.screenshot(APP, { ...(shot.display ? { display: shot.display.id } : null), until: node }); } catch { return null; }
}
async function areaDone(r, at) {
  const a = area.get();
  if (!a) return;
  let shot = a.shots.find((sh) => !sh.display || (at && sh.display.id === at.id)) || a.shots[0];
  endArea(shot.handle);
  if (!r || r.w < 3 || r.h < 3) { media.close(shot.handle); return; }
  if (a.mode === 'window') {
    const alone = await windowAlone(r, shot);
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
  await finish(c);
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
    for (const d of displaysToCapture(lord.screen)) { const h = await grab(d); if (!h) return; await finish(h, { copy: first }); first = false; }
  })),
  area: (mode = 'area') => afterHide(() => withTimer(async () => {
    endArea();
    const shots = [];
    for (const d of displaysToCapture(lord.screen)) {
      const h = await grab(d);
      if (!h) { shots.forEach((sh) => media.close(sh.handle)); return; }
      shots.push({ handle: h, display: d });
    }
    area.set({ shots, mode, start: null, rect: null, hover: null, pointer: null });
    // in the capture layer, above every other: a sheet, menu, prompt or Live Speech that was open is part of the
    // still, and the selection is drawn over it and gets the pointer there as well
    shots.forEach((sh, i) => {
      const d = sh.display;
      const id = wm.open(APP, { instance: `overlay${i}`, args: { index: i }, ...(d ? { frame: { x: d.x, y: d.y, w: d.w, h: d.h } } : {}) });
      wm.setFullscreen(id, true, { capture: true });
    });
  })),
  async record() {
    const cur = recording.get();
    if (cur) { media.stop(cur.id); return; }
    afterHide(() => withTimer(async () => {
      const path = freeName(captureDir(), tr('screenshots.recName', { date: stamp() }), 'mp4');
      const id = await allowed(() => media.screenRecord(APP, path, { mic: opts().mic, appName: tr('apps.screenshots') }));
      if (!id) return;
      recording.set({ id, path, start: Date.now() });
      const off = media.on(id, (type, d) => {
        if (type === 'done' || type === 'closed') {
          off();
          recording.set(null);
          wm.close(`${APP}#rec`, true);
          if (type === 'done' && d.ok) recent.set((l) => [{ path: d.path, time: Date.now(), video: true }, ...l].slice(0, 20));
          media.close(id);
        }
      });
      const rid = wm.open(APP, { instance: 'rec' });
      wm.setPip(rid, { x: lord.screen.width - 210, y: 40, w: 190, h: 44 });
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
const openPanelOrStop = () => { if (recording.get()) capture.record(); else wm.open(APP); };
addShortcuts([
  { id: 'capture.full', keys: ['Meta+Shift+3', 'PrintScreen'], when: hasMedia, run: captureFull, label: 'shell.shortcuts.captureFull', group: 'system' },
  { id: 'capture.area', keys: ['Meta+Shift+4'], when: hasMedia, run: captureArea, label: 'shell.shortcuts.captureArea', group: 'system' },
  { id: 'capture.panel', keys: ['Meta+Shift+5'], when: hasMedia, run: openPanelOrStop, label: 'shell.shortcuts.capturePanel', group: 'system' },
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
      if (cur.start) { area.set({ ...cur, pointer: { x: e.x, y: e.y }, loupe, rect: dragRect(cur.start, e.x, e.y, cur.start.display) }); return; }
      // the window under the pointer (the topmost one), for the window mode
      const vis = list.filter((w) => !w.minimized && w.app !== APP && (w.pip || w.desktop == null || w.desktop === wm.desktop()));
      let found = null;
      for (const w of vis) { const f = wm.frame(w); if (e.x >= f.x && e.y >= f.y && e.x < f.x + f.w && e.y < f.y + f.h) found = { x: f.x, y: f.y, w: f.w, h: f.h, win: w.fullscreen ? null : w.id, radius: w.fullscreen ? 0 : wm.cornerRadius(w.id), title: windowTitle(w), display: displayAt(displays(), f.x + f.w / 2, f.y + f.h / 2) }; }
      area.set({ ...cur, pointer: { x: e.x, y: e.y }, hover: found, loupe });
    });
    const offUp = addListener('pointerup', (e) => {
      const cur = area.get();
      if (!cur?.start) return;
      areaDone(dragRect(cur.start, e.x, e.y, cur.start.display), cur.start.display);
    });
    return () => { offMove(); offUp(); };
  }, [list, index]);
  if (!a || !shot) return null;
  const down = (e) => {
    if (a.mode === 'color') { if (a.loupe) copyColor(a.loupe.color); endArea(); return; }
    if (a.mode === 'area' || a.mode === 'text') area.set({ ...a, start: { x: e.x, y: e.y, display: displayAt((a.shots || []).map((sh) => sh.display).filter(Boolean), e.x, e.y) } });
    else if (a.hover) areaDone(a.hover, a.hover.display);
  };
  const dim = 'rgba(0,0,0,0.35)';
  const g = a.mode === 'window' ? a.hover : a.rect;
  const r = g ? { x: g.x - ox, y: g.y - oy, w: g.w, h: g.h, title: g.title } : null;
  const p = a.pointer ? { x: a.pointer.x - ox, y: a.pointer.y - oy } : null;
  const here = p && (!d || (p.x >= 0 && p.y >= 0 && p.x < d.w && p.y < d.h));
  const guide = 'rgba(255,255,255,0.55)';
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
            <Text style={{ fontSize: 12, color: '#fff' }}>{tr(a.mode === 'window' ? 'screenshots.hintWindow' : a.mode === 'color' ? 'screenshots.hintColor' : a.mode === 'text' ? 'screenshots.hintText' : 'screenshots.hintArea')}</Text>
          </View>
        </View>
      )}
    </View>
  );
}

function Thumb({ handle, path, failed }) {
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
      onClick={() => {
        const w = Math.min(1000, lord.screen.width - 80), h = Math.min(680, lord.screen.height - 140);
        wm.open(APP, { instance: `edit${handle}`, args: { handle, path }, frame: { x: Math.round((lord.screen.width - w) / 2), y: 60, w, h } });
        setTimeout(() => wm.close(win.id, true), 50);
      }}>
      <MediaView media={handle} style={{ flex: 1 }} />
      <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, paddingHorizontal: 10, paddingVertical: 5, backgroundColor: 'rgba(0,0,0,0.55)' }}>
        <Text style={{ fontSize: 11, color: '#fff' }}>{path ? tr('screenshots.savedTo', { folder: tr('screenshots.folder') }) : failed === 'copy' ? tr('screenshots.notCopied') : failed === 'both' ? tr('screenshots.notKept') : tr('screenshots.copied')}</Text>
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

// a capture right after it was taken (handle, and its file if it was saved), or a picture file (path)
function MarkupWindow({ handle, path }) {
  return <Editor handle={handle} path={path} app={APP} newCapturePath={() => freeName(captureDir(), tr('screenshots.fileName', { date: stamp() }), 'png')}
    onSaved={(p) => { if (p.startsWith(`${captureDir()}/`)) recent.set((l) => [{ path: p, time: Date.now() }, ...l.filter((x) => x.path !== p)].slice(0, 20)); }} />;
}

// ---- the panel (main window) ----

function Panel() {
  const th = useTheme();
  const win = useAppWindow();
  const [s, update] = useSettings();
  const o = { ...opts(), ...(s.screenshots || {}) };
  const set = (p) => update({ screenshots: { ...o, ...p } });
  const rec = useStore(recording);
  const list = useStore(recent);
  useEffect(() => {
    wm.setMenus(win.id, [{ label: tr('screenshots.menu.window'), items: [
      { label: tr('screenshots.menu.protect'), onClick: () => {
        const others = windows.get().filter((w) => w.app !== APP && !w.minimized && w.desktop === wm.desktop());
        const front = others[others.length - 1];
        if (front) wm.setProtected(front.id, !front.protected);
      } },
    ] }]);
  }, []);
  const Mode = ({ glyph, label, onClick, danger }) => (
    <View onClick={onClick} style={{ flex: 1, minWidth: 0, height: 82, borderRadius: 10, alignItems: 'center', gap: 6, paddingHorizontal: 4, paddingTop: 16,
      backgroundColor: danger ? 'rgba(255,59,48,0.14)' : th.mode === 'dark' ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.04)', border: `1px solid ${th.separator}` }}>
      <Text style={{ fontSize: 22, color: danger ? '#ff3b30' : th.text }}>{glyph}</Text>
      <Text style={{ fontSize: typeScale.caption, fontWeight: 500, color: th.text, textAlign: 'center', lineHeight: 1.2 }}>{label}</Text>
    </View>
  );
  const prot = windows.get().filter((w) => w.protected).length;
  return (
    <View style={{ flex: 1, padding: 20, gap: 16 }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Mode glyph="▣" label={tr('screenshots.mode.full')} onClick={() => capture.full()} />
        <Mode glyph="⧈" label={tr('screenshots.mode.window')} onClick={() => capture.area('window')} />
        <Mode glyph="⬚" label={tr('screenshots.mode.area')} onClick={() => capture.area('area')} />
        <Mode glyph="◉" label={tr('screenshots.mode.color')} onClick={() => capture.area('color')} />
        <Mode glyph="T" label={tr('screenshots.mode.text')} onClick={() => capture.area('text')} />
        <Mode glyph={rec ? '■' : '●'} danger label={tr(rec ? 'screenshots.mode.stop' : 'screenshots.mode.record')} onClick={() => capture.record()} />
      </View>
      <GroupBox footer={null}>
        <FormRow label={tr('screenshots.timer')}>
          <Segmented small value={o.timer} onChange={(v) => set({ timer: v })} options={[[0, tr('screenshots.timerNone')], [5, '5 s'], [10, '10 s']]} />
        </FormRow>
        <FormRow label={tr('screenshots.toClipboard')}><Toggle on={!!o.clipboard} onChange={(v) => set({ clipboard: v })} /></FormRow>
        <FormRow label={tr('screenshots.mic')}><Toggle on={!!o.mic} onChange={(v) => set({ mic: v })} /></FormRow>
      </GroupBox>
      <Text style={{ fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.45, paddingHorizontal: 8 }}>{tr('screenshots.shortcuts')}</Text>
      {prot > 0 && <Text style={{ fontSize: typeScale.caption, color: th.textDim, paddingHorizontal: 8 }}>{tr('screenshots.protectedNote', { count: prot })}</Text>}
      {list.length > 0 && (
        <GroupBox title={tr('screenshots.recent')}>
          {list.slice(0, 5).map((r) => (
            <View key={r.path} onClick={() => wm.open(r.video ? 'player' : 'photos', { new: true, args: { path: r.path } })}
              style={{ minHeight: 30, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Text style={{ width: 16, fontSize: 12, color: th.accent, textAlign: 'center' }}>{r.video ? '▶' : '◫'}</Text>
              <View style={{ flex: 1, minWidth: 0, overflow: 'hidden' }}>
                <Text style={{ fontSize: typeScale.callout, color: th.text, whiteSpace: 'nowrap' }}>{r.path.split('/').pop()}</Text>
              </View>
            </View>
          ))}
        </GroupBox>
      )}
    </View>
  );
}

export default function Screenshots() {
  const win = useAppWindow();
  const inst = String(win.id).split('#')[1] || '';
  const a = win.args || {};
  if (String(inst).startsWith('overlay') && a.index != null) return <Overlay index={a.index} />;
  if (inst === 'thumb' && a.handle) return <Thumb handle={a.handle} path={a.path} failed={a.failed} />;
  if (inst === 'count') return <Countdown secs={a.secs} start={a.start} />;
  if (inst === 'rec') return <RecBadge />;
  if (inst.startsWith('edit') && (a.handle || a.path)) return <MarkupWindow handle={a.handle} path={a.path} />;
  return <Panel />;
}
