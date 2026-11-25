// slideshow.jsx — the wallpaper slideshow (0.8.9; docs/WALLPAPERS.md is the collection, docs/WORKSPACES.md "Wallpaper
// slideshow" the feature). The decisions are in slideshowmodel.js, tested with node; a theme that declares
// "wallpaper": { "slideshow": true } in its theme.json draws <SlideshowWallpapers> instead of its still pictures while
// settings.slideshow.on is true (Lord Workspaces only, today).
//
// Cost: nothing at rest. One timer per change (no interval): the next picture is decoded off the interface's thread a
// few seconds before (a "thumb:" image, server/src/ui/image.c, at the display's size), drawn with opacity 0 until its
// change, and then one shared frame loop sets the arriving picture's opacity and translation with lord.setStyle for the
// transition's 2.4 s (lord-server blits the picture at its new place: React doesn't render per frame). After it the picture
// that left is unmounted (its pixels go to the image cache's idle budget, which drops them first-in first-out).
// Nothing changes while nobody sees the screen, on Power Saver or a low battery, or on a display hidden by a window.
import { useEffect, useMemo, useRef } from 'react';
import { Image, View } from '@lordos/runtime';
import { settings, useSettings } from './settings.js';
import { createStore, useStore } from './store.js';
import { windows, activeDesktop, overviewShowing } from './windows.js';
import { windowHidden } from './desktopmodel.js';
import { battery, powerProfiles } from './hardware.jsx';
import { displayCovered } from './motionmodel.js';
import { displayWallpaperKey } from './wallpapermodel.js';
import { downloadedStills, stillStore, withTwinFile } from './wallpapers.js';
import {
  slideshowPrefs, slideshowIds, drawNext, staggerMs, nextDue, preloadAt, transitionFor, transitionFrame, slideshowPause, MARGIN,
} from './slideshowmodel.js';

export { slideshowPrefs, slideshowPatch, slideshowStop, slideshowCategories, SLIDESHOW_EVERY } from './slideshowmodel.js';

// groupKey → { cur, next, run }: what each group of displays shows (one group for all, or one per display)
const shown = createStore({});
let ctl = null;

const seen = () => { try { return typeof lord === 'undefined' || !lord.system?.seen || lord.system.seen(); } catch (e) { return true; } };
const nextFrame = (fn) => (typeof requestAnimationFrame === 'function' ? requestAnimationFrame(fn) : setTimeout(fn, 16));
const setStyle = (node, k, v) => { try { lord.setStyle(node, k, v); } catch (e) { /* the node is gone */ } };

// the displays of a group, hidden by a window of the activity in view (the Overview shows the desk)
function covered(ds) {
  if (overviewShowing.get()) return false;
  const act = activeDesktop.get();
  const rects = windows.get().filter((w) => !windowHidden(w, act)).map((w) => ({ x: w.x, y: w.y, w: w.w, h: w.h }));
  return ds.length > 0 && ds.every((d) => displayCovered(d, rects));
}
const pauseNow = (ds) => slideshowPause({ seen: seen(), powerProfile: powerProfiles.get()?.active || null, battery: battery.get(), covered: covered(ds) });

function publish() {
  if (!ctl) return;
  const out = {};
  for (const [k, g] of ctl.groups) out[k] = { cur: g.cur, next: g.next, run: !!g.tr };
  shown.set(out);
}

// ---- the controller: one timer to the next preload or change ----

function plan() {
  if (!ctl) return;
  clearTimeout(ctl.timer);
  const now = Date.now();
  let soonest = Infinity;
  for (const g of ctl.groups.values()) {
    if (g.tr) continue;                                  // in a transition: its end plans again
    if (!g.next && now >= preloadAt(g.due, ctl.everyMs)) preload(g, now);
    if (g.next && now >= g.due - 5) { change(g); continue; }
    soonest = Math.min(soonest, g.next ? g.due : preloadAt(g.due, ctl.everyMs));
  }
  if (soonest < Infinity) ctl.timer = setTimeout(plan, Math.max(50, soonest - Date.now()));
}

function busyFor(g) {
  const out = [];
  for (const o of ctl.groups.values()) if (o !== g) { if (o.cur) out.push(o.cur); if (o.next) out.push(o.next); }
  return out;
}

function preload(g, now) {
  if (pauseNow(g.displays)) { g.due = nextDue(now + 1, ctl.start, ctl.everyMs, g.offset); return; }
  const r = drawNext(g.deck, ctl.ids, busyFor(g), Math.random, g.cur);
  g.deck = r.deck;
  g.next = r.id;
  publish();
}

function change(g) {
  const now = Date.now();
  g.due = nextDue(now + 1, ctl.start, ctl.everyMs, g.offset);
  if (pauseNow(g.displays) || !g.next || g.next === g.cur) { g.next = null; publish(); return; }
  g.tr = transitionFor({ reduceMotion: !!settings.get().reduceMotion });
  g.t0 = 0;
  publish();
  if (!ctl.ticking) { ctl.ticking = true; nextFrame(frame); }
}

function frame() {
  const c = ctl;
  if (!c) return;
  const now = Date.now();
  const visible = seen();
  let more = false;
  for (const g of c.groups.values()) {
    if (!g.tr) continue;
    if (!g.t0) g.t0 = now;
    // nobody sees it: straight to the end
    const p = visible ? (now - g.t0) / g.tr.ms : 1;
    const f = transitionFrame(g.tr, p);
    for (const n of g.nodes.values()) {
      // whole pixels: the picture stays a 1:1 copy of its decoded pixels
      if (g.tr.kind === 'drift') {
        setStyle(n.id, 'translateX', Math.round(f.x * n.w));
        setStyle(n.id, 'translateY', Math.round(f.y * n.h));
      }
      setStyle(n.id, 'opacity', f.opacity);
    }
    if (p >= 1) { g.cur = g.next; g.next = null; g.tr = null; g.nodes.clear(); publish(); } else more = true;
  }
  if (more && ctl === c) nextFrame(frame);
  else { c.ticking = false; if (ctl === c) plan(); }
}

function start({ list, displays, prefs }) {
  const ids = slideshowIds(list, prefs.category);
  const per = prefs.perDisplay && displays.length > 1;
  const keys = per ? displays.map(displayWallpaperKey) : ['*'];
  const sig = `${ids.join(',')}|${prefs.every}|${keys.join(',')}`;
  if (ctl && ctl.sig === sig) {
    for (const g of ctl.groups.values()) g.displays = per ? displays.filter((d) => displayWallpaperKey(d) === g.key) : displays;
    return;
  }
  const old = ctl;
  stop();
  const now = Date.now();
  const everyMs = prefs.every * 1000;
  ctl = { sig, ids, everyMs, start: now, groups: new Map(), timer: 0, ticking: false };
  keys.forEach((key, i) => {
    const offset = staggerMs(i, keys.length, everyMs);
    const was = old && (old.groups.get(key) || (key !== '*' && old.groups.get('*')));
    const g = { key, offset, due: nextDue(now + 1, now, everyMs, offset || everyMs), deck: was ? was.deck : null, cur: null, next: null,
      tr: null, nodes: new Map(), displays: per ? displays.filter((d) => displayWallpaperKey(d) === key) : displays };
    ctl.groups.set(key, g);
    // the picture it had stays, if it is in the category and not on another display already
    const keep = was && ids.includes(was.cur) && ![...ctl.groups.values()].some((o) => o !== g && o.cur === was.cur) ? was.cur : null;
    if (keep) g.cur = keep;
    else { const r = drawNext(g.deck, ids, busyFor(g)); g.deck = r.deck; g.cur = r.id; }
  });
  publish();
  plan();
}

function stop() {
  if (!ctl) return;
  clearTimeout(ctl.timer);
  ctl = null;
  shown.set({});
}

// ---- drawing ----

// The picture for a display of w×h device pixels: the smallest variant that covers it (sdk/wallpapers.js), decoded
// off the interface's thread straight to the size it is drawn at ("thumb:<px>:<src>", image.c).
export function slideSrc(w, width, height) {
  if (!w) return null;
  const list = w.variants || [];
  let v = list.find((x) => Math.max(width / x.w, height / x.h) <= 1) || list[list.length - 1] || null;
  const src = v ? v.src : w.src;
  if (!src || !(src.startsWith('asset:') || src.startsWith('/'))) return src;
  const iw = v ? v.w : width, ih = v ? v.h : height;
  const s = Math.min(1, Math.max(width / iw, height / ih));
  const px = Math.min(4096, Math.max(16, Math.ceil(Math.max(iw, ih) * s)));
  return `thumb:${px}:${src}`;
}


function Slide({ d, group, list, entry, onContextMenu }) {
  const px = (d.scale || 100) / 100;
  const find = (id) => list.find((w) => w.id === id) || null;
  const key = displayWallpaperKey(d);
  const ref = useRef(null);
  // the arriving picture's node, for the frame loop
  useEffect(() => {
    const g = ctl && ctl.groups.get(group);
    if (!g) return undefined;
    if (entry.next && ref.current) g.nodes.set(key, { id: ref.current.id, w: d.w, h: d.h });
    return () => { g.nodes.delete(key); };
  }, [entry.next, group, key, d.w, d.h]);
  // every picture MARGIN larger than the display on each side, for the drift (slideshowmodel.js transitionFor), and
  // decoded at that size (device pixels), so it is drawn 1:1
  const mx = Math.round(d.w * MARGIN), my = Math.round(d.h * MARGIN);
  const fill = { position: 'absolute', left: -mx, top: -my, width: d.w + 2 * mx, height: d.h + 2 * my, objectFit: 'cover' };
  const src = (id) => slideSrc(find(id), (d.w + 2 * mx) * px, (d.h + 2 * my) * px);
  return (
    <View onContextMenu={onContextMenu} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, overflow: 'hidden' }}>
      {entry.cur ? <Image key={`c:${entry.cur}`} src={src(entry.cur)} style={fill} /> : null}
      {entry.next ? <Image key={`n:${entry.next}`} ref={ref} src={src(entry.next)} style={{ ...fill, opacity: 0 }} /> : null}
    </View>
  );
}

// The wallpapers a slideshow can go through: the theme's, and the catalog's still pictures on this computer (their
// collections, Abstract, Epic and Anime, are categories too: Settings → Wallpaper → Use as slideshow).
// A built-in with a catalog twin brings its 4K file once it is on disk (sdk/wallpapers.js withTwinFile).
export const withDownloadedStills = (list) => [...(list || []).map(withTwinFile), ...downloadedStills()];

// Every display's wallpaper while the slideshow is on: list is the theme's wallpapers, displays screenDisplays().
export function SlideshowWallpapers({ list: themeList, displays, onContextMenu }) {
  const [s] = useSettings();
  const prefs = slideshowPrefs(s);
  const files = useStore(stillStore);
  const list = useMemo(() => withDownloadedStills(themeList), [themeList, files, s.wallpaperQuality]);
  const state = useStore(shown);
  const dkeys = displays.map((d) => `${displayWallpaperKey(d)}@${d.x},${d.y},${d.w}x${d.h}`).join(' ');
  useEffect(() => { start({ list, displays, prefs }); }, [list, dkeys, prefs.category, prefs.every, prefs.perDisplay]);
  useEffect(() => stop, []);
  const per = prefs.perDisplay && displays.length > 1;
  return displays.map((d) => {
    const group = per ? displayWallpaperKey(d) : '*';
    return <Slide key={d.id + ':' + d.x} d={d} group={group} list={list} entry={state[group] || {}} onContextMenu={onContextMenu} />;
  });
}

// for tests: the controller's groups
export const slideshowState = () => shown.get();
