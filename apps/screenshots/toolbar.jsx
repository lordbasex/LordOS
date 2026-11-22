// toolbar.jsx — the Screenshots toolbar (❖⇧5): one thin floating bar near the bottom of the screen instead of a window.
//
//   ⠿  ✕ │ ▭ entire screen · ▣ window · ⌜⌟ selection │ ●▭ record screen · ●⌜⌟ record selection │ ◉ color · T text │ Options ⌄ [Capture]
//
// The modes are icons with tooltips, the chosen one lit; Capture (or Record, for a recording mode) runs it. Color and Text
// act at once. Options: Save to, Timer, Microphone, the floating thumbnail, Remember last selection, the pointer in
// recordings, protecting the front window, and the recent captures. It is a bare floating window (wm.setPip { bare }):
// it draws its own translucent shape, takes no keyboard focus from the window in front, and moves by its grip like the
// reading-aloud player (sdk/voiceplayer.jsx); where it was left is kept (settings.screenshots.barPos).
// Keys while it shows (index.jsx): Esc closes it, Enter captures, ← → go through its items and Enter or Space presses the
// one the keyboard is on, ↓ on Options opens it and ↑ ↓ go through its rows.
import {
  View, Text, Tooltip, Popover, DisclosureGlyph, useTheme, useStore, useSettings, useSound, microphones, useAppWindow,
  wm, windows, addListener, screenDisplays, displayAt, workAreaOf, placeFloatingBar, rememberFloatingBar, direction, ScrollView, t as tr,
} from '@lordos/sdk';
import { useEffect, useRef } from 'react';
import { APP, bar, recent, setOptions } from './state.js';
import { normalizeOptions, isRecordMode, SAVE_TO, TIMERS } from './toolbarmodel.js';

const H = 50, PAD = 7, GAP = 2, BTN = 34, SMALL = 30, SEP = 13, GRIP = 16;
const FG = '#f2f2f5', DIM = 'rgba(242,242,245,0.62)', LIT = 'rgba(255,255,255,0.17)', RING = '#7fb2ff';

const measure = (s, size, weight) => {
  try { const w = lord.measureText(String(s), size, weight); if (Number.isFinite(w)) return w; } catch (e) { /* no engine (tests) */ }
  return String(s).length * size * 0.6;
};

// The bar's size in this language: its widest primary button (Capture or Record), so it never jumps when the mode changes
export function barSize() {
  const opt = measure(tr('screenshots.options'), 13, 500) + 34;
  const prim = Math.max(measure(tr('screenshots.capture'), 13, 600), measure(tr('screenshots.record'), 13, 600)) + 32;
  const parts = [GRIP, SMALL, SEP, BTN, BTN, BTN, SEP, BTN, BTN, SEP, SMALL, SMALL, SEP, opt, prim];
  return { w: Math.ceil(parts.reduce((a, b) => a + b, 0) + GAP * (parts.length - 1) + PAD * 2 + 2), h: H };
}

// Where the bar opens: where it was left, else at the bottom center of the main display, above the Dock
export function barFrame(saved) {
  const size = barSize();
  return { ...placeFloatingBar(screenDisplays(), saved || null, null, size), ...size, bare: true };
}

// ---- icons, drawn with views (a font may draw these symbols as color emoji, or not at all) ----

const Box = ({ w = 18, h = 13, fill = false, children }) => (
  <View style={{ width: w, height: h, borderRadius: 2.5, border: `1.5px solid ${FG}`, backgroundColor: fill ? 'rgba(242,242,245,0.22)' : 'transparent',
    alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>{children}</View>
);
const Corners = ({ s = 18, children }) => {
  const c = (l, tp, w, h) => <View style={{ position: 'absolute', left: l, top: tp, width: w, height: h, backgroundColor: FG, borderRadius: 1 }} />;
  const k = 5, b = 1.6;
  return (
    <View style={{ width: s, height: s - 3, alignItems: 'center', justifyContent: 'center' }}>
      {c(0, 0, k, b)}{c(0, 0, b, k)}{c(s - k, 0, k, b)}{c(s - b, 0, b, k)}
      {c(0, s - 3 - b, k, b)}{c(0, s - 3 - k, b, k)}{c(s - k, s - 3 - b, k, b)}{c(s - b, s - 3 - k, b, k)}
      {children}
    </View>
  );
};
const Dot = ({ size = 6, color = '#ff453a' }) => <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: color }} />;

const ICONS = {
  full: () => <Box fill />,
  window: () => (
    <Box>
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 3.5, backgroundColor: FG }} />
      <View style={{ position: 'absolute', left: 2, right: 2, top: 5.5, bottom: 2, backgroundColor: 'rgba(242,242,245,0.22)', borderRadius: 1 }} />
    </Box>
  ),
  area: () => <Corners />,
  recordFull: () => <Box><Dot /></Box>,
  recordArea: () => <Corners><Dot /></Corners>,
  color: () => (
    <View style={{ width: 16, height: 16, borderRadius: 8, border: `1.5px solid ${FG}`, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, background: 'linear-gradient(135deg, #ff9f0a, #ff375f 50%, #5e5ce6)' }} />
    </View>
  ),
  text: () => <Corners s={17}><Text style={{ fontSize: 10, fontWeight: 700, color: FG, lineHeight: 11 }}>T</Text></Corners>,
};

const TIP = {
  full: 'screenshots.tip.full', window: 'screenshots.tip.window', area: 'screenshots.tip.area',
  recordFull: 'screenshots.tip.recordFull', recordArea: 'screenshots.tip.recordArea', color: 'screenshots.tip.color', text: 'screenshots.tip.text',
};

function Sep() {
  return <View style={{ width: 1, height: 24, marginHorizontal: (SEP - 1) / 2, backgroundColor: 'rgba(255,255,255,0.15)' }} />;
}

// An icon button: lit when its mode is the chosen one, ringed when the keyboard is on it
function IconButton({ id, label, lit, ring, size = BTN, onClick, children }) {
  return (
    <Tooltip text={label}>
      <View onClick={onClick} accessibilityLabel={label} title={label}
        style={{ width: size, height: size, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: lit ? LIT : 'transparent',
          border: `2px solid ${ring ? RING : 'transparent'}` }}>
        {children || ICONS[id]?.()}
      </View>
    </Tooltip>
  );
}

function Grip({ onMouseDown, label }) {
  const dot = (k) => <View key={k} style={{ width: 3, height: 3, borderRadius: 2, backgroundColor: 'rgba(242,242,245,0.45)' }} />;
  return (
    <View onMouseDown={onMouseDown} accessibilityLabel={label} title={label} style={{ width: GRIP, height: 36, alignItems: 'center', justifyContent: 'center', cursor: 'grab' }}>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', width: 9, gap: 3 }}>{[0, 1, 2, 3, 4, 5].map(dot)}</View>
    </View>
  );
}

// ---- the Options menu ----

// The rows of a page: { key, label, checked, run } choose something, { header } and { note } are not selectable
export function menuRows(o, page, { mics, recents, front, protectedCount, actions }) {
  if (page === 'recent') {
    const rows = [{ key: 'back', label: tr('screenshots.opt.back'), back: true, run: () => bar.set({ page: 'main', lit: 0 }), keep: true }];
    if (!recents.length) rows.push({ note: tr('screenshots.opt.noRecent') });
    for (const r of recents.slice(0, 8)) rows.push({ key: r.path, label: r.path.split('/').pop(), video: !!r.video, run: () => actions.openRecent(r) });
    return rows;
  }
  // as macOS's ⇧⌘5 Options menu: dimmed section headers, a separator between sections, a check on each choice
  const rows = [{ header: tr('screenshots.opt.saveTo') }];
  for (const s of SAVE_TO) {
    if (s === 'mail' && !actions.hasMail) continue;
    if (s === 'other') {
      const name = o.otherDir ? o.otherDir.split('/').pop() : '';
      if (name) rows.push({ key: 'save.otherDir', label: name, checked: o.saveTo === 'other', run: () => setOptions({ saveTo: 'other' }), keep: true });
      rows.push({ key: 'save.other', label: tr('screenshots.opt.other'), run: () => actions.chooseFolder() });
      continue;
    }
    rows.push({ key: `save.${s}`, label: s === 'mail' ? tr('apps.mail') : tr(`screenshots.opt.${s}`), checked: o.saveTo === s, run: () => setOptions({ saveTo: s }), keep: true });
  }
  rows.push({ sep: true });
  rows.push({ header: tr('screenshots.timer') });
  for (const n of TIMERS) {
    rows.push({ key: `timer.${n}`, label: tr(n === 5 ? 'screenshots.opt.timer5' : n === 10 ? 'screenshots.opt.timer10' : 'screenshots.timerNone'), checked: o.timer === n, run: () => setOptions({ timer: n }), keep: true });
  }
  rows.push({ sep: true });
  rows.push({ header: tr('screenshots.opt.mic') });
  rows.push({ key: 'mic.none', label: tr('screenshots.timerNone'), checked: !o.mic, run: () => setOptions({ mic: false }), keep: true });
  const chosen = o.micSource && mics.some((m) => m.name === o.micSource) ? o.micSource : (mics.find((m) => m.default) || mics[0])?.name;
  for (const m of mics) {
    rows.push({ key: `mic.${m.name}`, label: m.description || m.name, checked: o.mic && m.name === chosen, run: () => setOptions({ mic: true, micSource: m.name }), keep: true });
  }
  rows.push({ sep: true });
  rows.push({ header: tr('screenshots.options') });
  rows.push({ key: 'thumbnail', label: tr('screenshots.opt.thumbnail'), checked: o.thumbnail, run: () => setOptions({ thumbnail: !o.thumbnail }), keep: true });
  rows.push({ key: 'remember', label: tr('screenshots.opt.remember'), checked: o.remember, run: () => setOptions({ remember: !o.remember, ...(o.remember ? { lastRect: null } : null) }), keep: true });
  rows.push({ key: 'pointer', label: tr('screenshots.opt.pointer'), checked: o.pointer, run: () => setOptions({ pointer: !o.pointer }), keep: true });
  rows.push({ sep: true });
  rows.push({ key: 'protect', label: tr('screenshots.menu.protect'), checked: !!front?.protected, disabled: !front, run: () => actions.protectFront(), keep: true });
  if (protectedCount > 0) rows.push({ note: tr('screenshots.protectedNote', { count: protectedCount }) });
  rows.push({ sep: true });
  rows.push({ key: 'recent', label: tr('screenshots.recent'), more: true, run: () => bar.set({ page: 'recent', lit: 0 }), keep: true });
  return rows;
}
export const selectable = (r) => !!r && !!r.run && !r.disabled;

function MenuRow({ row, lit, onPick }) {
  const th = useTheme();
  if (row.header) return <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, paddingHorizontal: 10, paddingTop: 7, paddingBottom: 3 }}>{row.header}</Text>;
  if (row.note) return <Text style={{ fontSize: 11, color: th.textDim, paddingHorizontal: 26, paddingVertical: 4, lineHeight: 1.35 }}>{row.note}</Text>;
  if (row.sep) return <View style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: th.separator }} />;
  const on = lit && !row.disabled;
  const color = on ? '#fff' : th.text;
  return (
    <View onClick={() => { if (!row.disabled) onPick(row); }} accessibilityLabel={row.label}
      style={{ height: 26, paddingHorizontal: 8, borderRadius: th.menuItemRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: on ? th.accent : 'transparent', opacity: row.disabled ? 0.4 : 1 }}>
      <Text style={{ width: 12, fontSize: 11, color }}>{row.checked ? '✓' : row.back ? '‹' : row.video ? '▶' : ''}</Text>
      <View style={{ flex: 1, minWidth: 0, overflow: 'hidden' }}>
        <Text style={{ fontSize: 13, color, whiteSpace: 'nowrap' }}>{row.label}</Text>
      </View>
      {row.more ? <Text style={{ fontSize: 13, color: on ? '#fff' : th.textDim }}>{direction() === 'rtl' ? '‹' : '›'}</Text> : null}
    </View>
  );
}

// The menu stays inside the work area of the display the bar is on (the bar's own frame is only as tall as the bar):
// above the bar when there is more room there, and scrolling when even that side is too short.
function OptionsMenu({ anchor, rows, frame }) {
  const b = useStore(bar);
  const pick = (row) => { row.run(); if (!row.keep) bar.set({ menu: false, page: 'main', lit: -1 }); };
  const area = frame ? workAreaOf(displayAt(frame.x + frame.w / 2, frame.y + frame.h / 2)) : null;
  return (
    <Popover anchor={anchor} width={280} padding={5} area={area} onClose={() => bar.set({ menu: false, page: 'main', lit: -1 })}>
      <ScrollView style={{ flexShrink: 1, minHeight: 0 }} resetKey={b.page}>
        <View style={{ gap: 0 }} onMouseMove={() => { if (b.lit >= 0) bar.set({ lit: -1 }); }}>
          {rows.map((row, i) => <MenuRow key={row.key || `${i}`} row={row} lit={b.lit === i} onPick={pick} />)}
        </View>
      </ScrollView>
    </Popover>
  );
}

// ---- the bar ----

export function Toolbar({ actions }) {
  const win = useAppWindow();
  const th = useTheme();
  const [s] = useSettings();
  const o = normalizeOptions(s.screenshots);
  const b = useStore(bar);
  const list = useStore(recent);
  const all = useStore(windows);
  const snd = useSound();
  const optRef = useRef(null);
  const drag = useRef(null);
  const me = all.find((w) => w.id === win.id);

  // opened as a window (the launcher, an older wm.open): it becomes the floating bar
  useEffect(() => { if (me && !me.pip) wm.setPip(win.id, barFrame(o.barPos)); }, [!!me?.pip]);
  // the language changed: its size too
  useEffect(() => {
    if (!me?.pip) return;
    const size = barSize();
    if (size.w !== me.pip.w || size.h !== me.pip.h) wm.setPip(win.id, { ...me.pip, ...size });
  }, [s.language, !!me?.pip]);
  // moved by its grip, also to another display; it stays where it was left
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (d) wm.setPip(win.id, { ...d.frame, x: Math.round(d.frame.x + e.x - d.x), y: Math.round(d.frame.y + e.y - d.y) });
    });
    const offUp = addListener('pointerup', (e) => {
      const d = drag.current;
      drag.current = null;
      if (!d) return;
      const place = rememberFloatingBar(screenDisplays(), d.frame.x + e.x - d.x, d.frame.y + e.y - d.y, d.frame);
      setOptions({ barPos: place });
      wm.setPip(win.id, barFrame(place));
    });
    return () => { offMove(); offUp(); };
  }, []);

  const front = all.filter((w) => w.app !== APP && !w.minimized && !w.pip && w.desktop === wm.desktop()).pop() || null;
  const rows = menuRows(o, b.page, { mics: microphones(snd), recents: list, front, protectedCount: all.filter((w) => w.protected).length, actions });
  const rec = isRecordMode(o.mode);
  const choose = (mode) => { setOptions({ mode }); bar.set({ ring: b.ring ? mode : null }); };
  const effects = !!th.mat?.blur && s.effects !== false;
  const primLabel = tr(rec ? 'screenshots.record' : 'screenshots.capture');
  const ring = (id) => b.ring === id;
  const mode = (id) => <IconButton key={id} id={id} label={tr(TIP[id])} lit={o.mode === id} ring={ring(id)} onClick={() => choose(id)} />;
  return (
    <View accessibilityLabel={tr('screenshots.bar.label')}
      style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: GAP, paddingHorizontal: PAD, borderRadius: 14,
        backgroundColor: effects ? 'rgba(28,28,32,0.72)' : 'rgba(28,28,32,0.96)', ...(effects ? { backdropFilter: 'blur(24px)' } : null),
        border: '1px solid rgba(255,255,255,0.13)', boxShadow: '0 14px 40px rgba(0,0,0,0.45), 0 2px 8px rgba(0,0,0,0.25)' }}>
      <Grip label={tr('screenshots.bar.move')} onMouseDown={(e) => { e.stopPropagation?.(); if (me?.pip) drag.current = { x: e.x, y: e.y, frame: me.pip }; }} />
      <IconButton id="close" size={SMALL} label={tr('screenshots.bar.close')} ring={ring('close')} onClick={actions.close}>
        <View style={{ width: 18, height: 18, borderRadius: 9, backgroundColor: 'rgba(255,255,255,0.14)', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 10, color: FG, lineHeight: 12 }}>✕</Text>
        </View>
      </IconButton>
      <Sep />
      {mode('full')}{mode('window')}{mode('area')}
      <Sep />
      {mode('recordFull')}{mode('recordArea')}
      <Sep />
      <IconButton id="color" size={SMALL} label={tr(TIP.color)} ring={ring('color')} onClick={actions.color} />
      <IconButton id="text" size={SMALL} label={tr(TIP.text)} ring={ring('text')} onClick={actions.text} />
      <Sep />
      <View ref={optRef}>
        <View onClick={() => bar.set({ menu: !b.menu, page: 'main', lit: -1 })} accessibilityLabel={tr('screenshots.options')} title={tr('screenshots.options')}
          style={{ height: BTN, paddingHorizontal: 9, borderRadius: 9, flexDirection: 'row', alignItems: 'center', gap: 4,
            backgroundColor: b.menu ? LIT : 'transparent', border: `2px solid ${ring('options') ? RING : 'transparent'}` }}>
          <Text style={{ fontSize: 13, fontWeight: 500, color: FG, whiteSpace: 'nowrap' }}>{tr('screenshots.options')}</Text>
          <DisclosureGlyph open color={DIM} />
        </View>
      </View>
      <View onClick={actions.primary} accessibilityLabel={primLabel} title={primLabel}
        style={{ marginStart: 4, height: 32, paddingHorizontal: 14, borderRadius: 8, alignItems: 'center', justifyContent: 'center',
          backgroundColor: rec ? '#ff453a' : th.accent || '#0a84ff', border: `2px solid ${ring('primary') ? '#fff' : 'transparent'}` }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap' }}>{primLabel}</Text>
      </View>
      {b.menu ? <OptionsMenu anchor={optRef} rows={rows} frame={me?.pip} /> : null}
    </View>
  );
}
