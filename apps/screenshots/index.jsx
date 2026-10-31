// Screenshots: screen captures and screen recording (a floating capture panel,
// with our own design).
//
// Shortcuts (global, also with a Linux app in front):
//   ❖⇧3 / Print Screen  entire screen        ❖⇧4  area (or window, with Space)
//   ❖⇧5                 the Screenshots panel (and stop a recording in progress)
// Protected windows (wm.setProtected) come out black: the server does that, not this app.
// After each capture a floating thumbnail shows up; a click opens the markup editor (arrow, box,
// text, blur). They are saved in ~/Pictures/Screenshots or to the clipboard.
import {
  View, Text, MediaView, Button, Toggle, Segmented, TextInput, Toolbar, ToolbarButton, ToolbarGroup, ToolbarDivider, ToolbarSpacer, GroupBox, FormRow,
  useTheme, useAppWindow, useKeyboard, useStore, useWindows, wm, windows, typeScale,
  t as tr, media, userDirs, fs, addListener, addShortcuts, createStore, windowTitle, settings, useSettings,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

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

async function finish(handle) {
  if (!handle) return;
  const o = opts();
  let path = null;
  if (o.clipboard) await media.copyImage(handle);
  else {
    const r = await media.saveImage(handle, freeName(captureDir(), tr('screenshots.fileName', { date: stamp() }), 'png'));
    if (r.ok) { path = r.path; recent.set((l) => [{ path, time: Date.now() }, ...l].slice(0, 20)); }
  }
  showThumb(handle, path);
}

function showThumb(handle, path) {
  const id = wm.open(APP, { instance: 'thumb', args: { handle, path } });
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

export const capture = {
  full: () => afterHide(() => withTimer(async () => finish(await media.screenshot(APP)))),
  area: (mode = 'area') => afterHide(() => withTimer(async () => {
    const h = await media.screenshot(APP);
    if (!h) return;
    const id = wm.open(APP, { instance: 'overlay', args: { handle: h, mode } });
    wm.setFullscreen(id, true);
  })),
  async record() {
    const cur = recording.get();
    if (cur) { media.stop(cur.id); return; }
    afterHide(() => withTimer(async () => {
      const path = freeName(captureDir(), tr('screenshots.recName', { date: stamp() }), 'mp4');
      const id = await media.screenRecord(APP, path, { mic: opts().mic, appName: tr('apps.screenshots') });
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
const captureFull = () => capture.full();
const captureArea = () => capture.area('area');
const openPanelOrStop = () => { if (recording.get()) capture.record(); else wm.open(APP); };
addShortcuts([
  { id: 'capture.full', keys: ['Meta+Shift+3', 'PrintScreen'], when: hasMedia, run: captureFull, label: 'shell.shortcuts.captureFull', group: 'system' },
  { id: 'capture.area', keys: ['Meta+Shift+4'], when: hasMedia, run: captureArea, label: 'shell.shortcuts.captureArea', group: 'system' },
  { id: 'capture.panel', keys: ['Meta+Shift+5'], when: hasMedia, run: openPanelOrStop, label: 'shell.shortcuts.capturePanel', group: 'system' },
]);

// ---- the app's windows ----

function Overlay({ handle, mode: initial }) {
  const win = useAppWindow();
  const { windows: list } = useWindows();
  const [mode, setMode] = useState(initial);
  const [rect, setRect] = useState(null);
  const [hover, setHover] = useState(null);
  const drag = useRef(null);
  useEffect(() => { media.view(handle, { zoom: 1, cx: 0.5, cy: 0.5, background: 0xff000000 }); }, [handle]);
  const done = async (r) => {
    wm.close(win.id, true);
    if (!r || r.w < 3 || r.h < 3) { media.close(handle); return; }
    const c = media.crop(handle, r.x, r.y, r.w, r.h);
    media.close(handle);
    await finish(c);
  };
  useKeyboard((e) => {
    if (e.key === 'Escape') { wm.close(win.id, true); media.close(handle); }
    if (e.key === ' ') setMode((m) => (m === 'area' ? 'window' : 'area'));
  });
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (drag.current) { const d = drag.current; setRect({ x: Math.min(d.x, e.x), y: Math.min(d.y, e.y), w: Math.abs(e.x - d.x), h: Math.abs(e.y - d.y) }); return; }
      // window under the pointer (the topmost one)
      const vis = list.filter((w) => !w.minimized && w.app !== APP);
      let found = null;
      for (const w of vis) { const f = wm.frame(w); if (e.x >= f.x && e.y >= f.y && e.x < f.x + f.w && e.y < f.y + f.h) found = { ...f, title: windowTitle(w) }; }
      setHover(found);
    });
    const offUp = addListener('pointerup', (e) => {
      if (!drag.current) return;
      const d = drag.current;
      drag.current = null;
      done({ x: Math.min(d.x, e.x), y: Math.min(d.y, e.y), w: Math.abs(e.x - d.x), h: Math.abs(e.y - d.y) });
    });
    return () => { offMove(); offUp(); };
  }, [list, mode]);
  const dim = 'rgba(0,0,0,0.35)';
  const r = mode === 'window' ? hover : rect;
  return (
    <View style={{ flex: 1, direction: 'ltr' }} onMouseDown={(e) => { if (mode === 'area') drag.current = { x: e.x, y: e.y }; else done(hover); }}>
      <MediaView media={handle} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
      {r ? (
        <>
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, height: r.y, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: 0, top: r.y + r.h, right: 0, bottom: 0, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: 0, top: r.y, width: r.x, height: r.h, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: r.x + r.w, top: r.y, right: 0, height: r.h, backgroundColor: dim }} />
          <View style={{ position: 'absolute', left: r.x, top: r.y, width: r.w, height: r.h, border: `1px solid ${mode === 'window' ? '#4c8dff' : '#fff'}`,
            backgroundColor: mode === 'window' ? 'rgba(76,141,255,0.15)' : 'transparent' }} />
          <View style={{ position: 'absolute', left: r.x + r.w + 6, top: r.y + r.h + 6, paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.7)' }}>
            <Text style={{ fontSize: 11, color: '#fff' }}>{mode === 'window' ? r.title : `${Math.round(r.w)} × ${Math.round(r.h)}`}</Text>
          </View>
        </>
      ) : <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: dim }} />}
      <View style={{ position: 'absolute', left: 0, right: 0, top: 18, alignItems: 'center', pointerEvents: 'none' }}>
        <View style={{ paddingHorizontal: 14, height: 30, justifyContent: 'center', borderRadius: 15, backgroundColor: 'rgba(0,0,0,0.7)' }}>
          <Text style={{ fontSize: 12, color: '#fff' }}>{tr(mode === 'window' ? 'screenshots.hintWindow' : 'screenshots.hintArea')}</Text>
        </View>
      </View>
    </View>
  );
}

function Thumb({ handle, path }) {
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
        <Text style={{ fontSize: 11, color: '#fff' }}>{path ? tr('screenshots.savedTo', { folder: tr('screenshots.folder') }) : tr('screenshots.copied')}</Text>
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

// ---- markup editor ----

const COLORS = ['#ff3b30', '#ffcc00', '#34c759', '#0a84ff', '#ffffff', '#1c1c1e'];

function Editor({ handle, path }) {
  const th = useTheme();
  const win = useAppWindow();
  const [tool, setTool] = useState('arrow');
  const [color, setColor] = useState(COLORS[0]);
  const [ops, setOps] = useState([]);
  const [draft, setDraft] = useState(null);
  const [text, setText] = useState(null);        // { x, y, value } (image coordinates)
  const [preview, setPreview] = useState(0);
  const [msg, setMsg] = useState(null);
  const box = useRef(null);
  const drag = useRef(null);
  const info = media.info(handle) || { width: 1, height: 1 };
  useEffect(() => () => { media.close(handle); }, []);
  useEffect(() => () => { if (preview) media.close(preview); }, [preview]);
  useEffect(() => {
    const all = draft ? [...ops, draft] : ops;
    const id = media.annotate(handle, all, preview);
    if (id && id !== preview) setPreview(id);
    if (id) media.view(id, { fit: 'contain', background: 0 });
  }, [ops, draft, handle]);
  const map = (x, y) => {
    const [lx, ly, bw, bh] = box.current ? lord.getLayout(box.current.id) : [0, 0, 1, 1];
    const s = Math.min(bw / info.width, bh / info.height);
    const ox = lx + (bw - info.width * s) / 2, oy = ly + (bh - info.height * s) / 2;
    return { x: (x - ox) / s, y: (y - oy) / s, s };
  };
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (!drag.current) return;
      const p = map(e.x, e.y);
      setDraft((d) => d && { ...d, x2: p.x, y2: p.y });
    });
    const offUp = addListener('pointerup', () => {
      if (!drag.current) return;
      drag.current = null;
      setDraft((d) => { if (d && Math.hypot(d.x2 - d.x1, d.y2 - d.y1) > 3) setOps((o) => [...o, d]); return null; });
    });
    return () => { offMove(); offUp(); };
  });
  const width = Math.max(3, Math.round(Math.min(info.width, info.height) / 220));
  const down = (e) => {
    const p = map(e.x, e.y);
    if (tool === 'text') { setText({ x: p.x, y: p.y, value: '' }); return; }
    drag.current = true;
    setDraft({ type: tool, x1: p.x, y1: p.y, x2: p.x, y2: p.y, color, width: tool === 'blur' ? 6 : width });
  };
  const addText = () => {
    if (text && text.value.trim()) setOps((o) => [...o, { type: 'text', x1: text.x, y1: text.y, text: text.value, color, size: Math.max(18, width * 7) }]);
    setText(null);
  };
  useKeyboard((e) => { if ((e.ctrl || e.meta) && String(e.base).toLowerCase() === 'z') setOps((o) => o.slice(0, -1)); });
  const save = async () => {
    const base = path ? path.split('/').pop().replace(/\.png$/i, '') : tr('screenshots.fileName', { date: stamp() });
    const r = await media.saveImage(preview || handle, freeName(captureDir(), `${base} (${tr('screenshots.annotated')})`, 'png'));
    setMsg(r.ok ? tr('screenshots.savedTo', { folder: tr('screenshots.folder') }) : tr('screenshots.saveError'));
    setTimeout(() => setMsg(null), 2500);
  };
  const copy = async () => { const r = await media.copyImage(preview || handle); setMsg(r.ok ? tr('screenshots.copied') : tr('screenshots.saveError')); setTimeout(() => setMsg(null), 2500); };
  const tools = [['arrow', '↗', 'screenshots.tool.arrow'], ['box', '▢', 'screenshots.tool.box'], ['text', 'T', 'screenshots.tool.text'],
    ['blur', '▒', 'screenshots.tool.blur'], ['line', '╱', 'screenshots.tool.line']];
  const tp = text ? (() => { const [lx, ly, bw, bh] = box.current ? lord.getLayout(box.current.id) : [0, 0, 1, 1]; const s = Math.min(bw / info.width, bh / info.height);
    return { left: (bw - info.width * s) / 2 + text.x * s, top: (bh - info.height * s) / 2 + text.y * s }; })() : null;
  return (
    <View style={{ flex: 1 }}>
      <Toolbar>
        <ToolbarGroup>
          {tools.map(([k, g, key]) => <ToolbarButton key={k} icon={g} title={tr(key)} active={tool === k} onClick={() => setTool(k)} />)}
        </ToolbarGroup>
        <ToolbarDivider />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          {COLORS.map((c) => (
            <View key={c} onClick={() => setColor(c)} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
              border: `2px solid ${color === c ? th.accent : 'transparent'}` }}>
              <View style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: c, border: `1px solid ${th.separator}` }} />
            </View>
          ))}
        </View>
        <ToolbarSpacer />
        <ToolbarButton icon="⟲" title={tr('screenshots.undo')} onClick={() => setOps((o) => o.slice(0, -1))} />
        <ToolbarButton icon="⧉" title={tr('screenshots.copy')} onClick={copy} />
        <Button label={tr('screenshots.done')} onClick={() => wm.close(win.id)} />
        <Button primary label={tr('screenshots.save')} onClick={save} />
      </Toolbar>
      <View ref={box} style={{ flex: 1, backgroundColor: th.mode === 'dark' ? '#111' : '#ddd', overflow: 'hidden', direction: 'ltr' }} onMouseDown={down}>
        <MediaView media={preview || handle} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
        {text && tp && (
          <View style={{ position: 'absolute', left: tp.left, top: tp.top, width: 260 }}>
            <TextInput value={text.value} autoFocus onChange={(v) => setText({ ...text, value: v })} onSubmit={addText} onCancel={() => setText(null)}
              placeholder={tr('screenshots.textHint')} />
          </View>
        )}
        {msg && <View style={{ position: 'absolute', left: 0, right: 0, bottom: 20, alignItems: 'center' }}>
          <View style={{ paddingHorizontal: 14, height: 30, justifyContent: 'center', borderRadius: 15, backgroundColor: 'rgba(0,0,0,0.75)' }}><Text style={{ fontSize: 12, color: '#fff' }}>{msg}</Text></View>
        </View>}
      </View>
    </View>
  );
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
        const others = windows.get().filter((w) => w.app !== APP && !w.minimized);
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
  if (inst === 'overlay' && a.handle) return <Overlay handle={a.handle} mode={a.mode} />;
  if (inst === 'thumb' && a.handle) return <Thumb handle={a.handle} path={a.path} />;
  if (inst === 'count') return <Countdown secs={a.secs} start={a.start} />;
  if (inst === 'rec') return <RecBadge />;
  if (inst.startsWith('edit') && a.handle) return <Editor handle={a.handle} path={a.path} />;
  return <Panel />;
}
