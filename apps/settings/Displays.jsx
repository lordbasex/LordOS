// Settings → Displays: arrangement of several displays (extend or mirror), resolution, refresh rate,
// rotation, scale (100–200 %, sharp), the main display, brightness, Night Light and video in the
// browser (Video.jsx, docs/VIDEO.md) (docs/SETTINGS.md,
// "Displays", and docs/DISPLAYS.md). Changes apply live with the system's 15-second "Keep these
// display settings?" countdown (sdk/hardware.jsx) and are saved per display (EDID identity).
// Layout: the arrangement on top, then the selected display's
// settings in inset boxes, with the scale as "Larger text … More space" tiles.
import {
  View, Text, useTheme, useLocale, useSettings, Slider, Toggle, admin, useDisplays, applyDisplay, DisplayGlyph, addListener,
  arrangeDisplay, setDisplayMirror, setMainDisplay, setCaptureAllDisplays, typeScale, spacing,
  nightPrefs, nightActive, setNightManual, previewNight, nightTick, sunTimes, formatTime, Button,
  identifyDisplays, IDENTIFY_MS, displayNumbers, snapDisplay, resolveDrop, displayPlacementOk, SNAP_PREVIEW_PX,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Section, SettingRow, PopUp, Note, Stack, TimeField } from './ui.jsx';
import { BrowserVideo } from './Video.jsx';

const hz = (mhz) => {
  const v = mhz / 1000;
  return Math.abs(v - Math.round(v)) < 0.005 ? String(Math.round(v)) : v.toFixed(2);
};
const SCALES = [200, 175, 150, 125, 100];         // tiles: larger text first, more space last

// The width of a box after layout (the page column narrows with the window).
function useWidth(ref, fallback) {
  const [w, setW] = useState(fallback);
  useEffect(() => {
    const measure = () => {
      const n = ref.current;
      if (!n) return;
      const x = Math.round(lord.getLayout(n.id)[2]);
      if (x > 0) setW((old) => (Math.abs(x - old) > 1 ? x : old));
    };
    measure();
    const id = setTimeout(measure, 60);           // the first layout may not be done yet
    return () => clearTimeout(id);
  });
  return w;
}

// The arrangement (docs/DISPLAYS.md, "Numbers, identify and snapping"): every active display at its
// place in the desktop, proportional to its logical size, drawn as a small screen with its number
// (with several displays), name and resolution. Dragging is magnetic (sdk/displaylayout.js): edges
// stick edge to edge and tops/bottoms, lefts/rights and centers align within 12 preview px; on drop
// the display always touches another and never overlaps one, and lord-server keeps that position.
// A click selects a display and shows its number on the real screen (identifyDisplays). The white bar
// marks the main display (menu bar, Dock).
function Arrangement({ outs, sel, onSelect, mirror, nums, t }) {
  const act = outs.filter((o) => o.active && o.w);
  const box = useRef(null);
  const W = useWidth(box, 520), H = 180;
  const [drag, setDrag] = useState(null);       // { id, x, y, view } in logical px while dragging
  const [flash, setFlash] = useState(null);     // the tile just identified
  const st = useRef(null);
  const actRef = useRef(act);
  actRef.current = act;
  const several = act.length > 1 && !mirror;
  const identify = (id) => {
    if (!identifyDisplays([id])) return;
    setFlash(id);
    setTimeout(() => setFlash((f) => (f === id ? null : f)), IDENTIFY_MS);
  };
  useEffect(() => {
    // where the dragged display wants to be, from the pointer, in logical px
    const want = (d, e) => ({ x: d.ox + (e.x - d.px) / d.view.k, y: d.oy + (e.y - d.py) / d.view.k });
    const rectsOf = (d) => {
      const list = actRef.current;
      return { rects: list.map((o) => ({ x: o.x, y: o.y, w: o.w, h: o.h })), i: list.findIndex((o) => o.id === d.id) };
    };
    const offMove = addListener('pointermove', (e) => {
      const d = st.current;
      if (!d) return;
      if (!d.moved && Math.hypot(e.x - d.px, e.y - d.py) < 4) return;
      d.moved = true;
      const { rects, i } = rectsOf(d);
      if (i < 0) return;
      const w = want(d, e);
      const p = snapDisplay(rects, i, w.x, w.y, SNAP_PREVIEW_PX / d.view.k);
      setDrag({ id: d.id, x: p.x, y: p.y, ok: displayPlacementOk(rects, i, p.x, p.y), view: d.view });
    });
    const offUp = addListener('pointerup', (e) => {
      const d = st.current;
      st.current = null;
      if (!d) return;
      if (!d.moved) { if (d.identify) d.identify(d.id); return; }
      const { rects, i } = rectsOf(d);
      if (i < 0) { setDrag(null); return; }
      const w = want(d, e);
      const p = resolveDrop(rects, i, w.x, w.y, SNAP_PREVIEW_PX / d.view.k);
      setDrag({ id: d.id, x: p.x, y: p.y, ok: true, view: d.view });
      arrangeDisplay(d.id, p.x, p.y, true);
      setTimeout(() => setDrag(null), 400);   // until the new list arrives
    });
    return () => { offMove(); offUp(); };
  }, []);
  const dark = t.mode === 'dark';
  const frame = { height: H, borderRadius: 10, direction: 'ltr', overflow: 'hidden',
    backgroundColor: dark ? 'rgba(0,0,0,0.22)' : 'rgba(0,0,0,0.045)', border: `1px solid ${t.separator}` };
  if (!act.length) return <View ref={box} style={frame} />;
  // mirror: the displays are drawn one behind the other; extend: at their place
  const pos = (o, i) => (mirror ? { x: i * 18, y: i * 14 } : drag && drag.id === o.id ? drag : { x: o.x, y: o.y });
  const boxes = act.map((o, i) => ({ o, ...pos(o, i) }));
  // the preview's scale and offset: frozen while dragging, so the tile follows the pointer
  let view = drag && drag.view;
  if (!view) {
    const x0 = Math.min(...boxes.map((b) => b.x)), y0 = Math.min(...boxes.map((b) => b.y));
    const x1 = Math.max(...boxes.map((b) => b.x + b.o.w)), y1 = Math.max(...boxes.map((b) => b.y + b.o.h));
    const k = Math.min((W - 56) / Math.max(1, x1 - x0), (H - 40) / Math.max(1, y1 - y0));
    view = { k, x0, y0, offX: (W - (x1 - x0) * k) / 2, offY: (H - (y1 - y0) * k) / 2 };
  }
  const { k, x0, y0, offX, offY } = view;
  const movable = several;
  return (
    <View ref={box} style={frame}>
      {boxes.map(({ o, x, y }) => {
        const on = o.id === sel, lit = o.id === flash, moving = drag && drag.id === o.id;
        const w = Math.max(28, o.w * k - 3), h = Math.max(18, o.h * k - 3);
        const n = several ? nums[o.id] : 0;
        return (
          <View key={o.id}
            onMouseDown={(e) => { onSelect(o.id); if (several) st.current = { id: o.id, px: e.x, py: e.y, ox: o.x, oy: o.y, view, moved: false, identify: movable ? identify : null }; }}
            style={{ position: 'absolute', left: offX + (x - x0) * k - 3, top: offY + (y - y0) * k - 3, width: w + 6, height: h + 6, padding: 1,
              borderRadius: 8, cursor: movable ? 'grab' : 'pointer', zIndex: moving ? 2 : 1, opacity: moving && !drag.ok ? 0.7 : 1,
              border: `2px solid ${on || lit ? t.accent : 'transparent'}` }}>
            <View style={{ flex: 1, borderRadius: 5, overflow: 'hidden', alignItems: 'center', justifyContent: 'center', gap: 1,
              background: lit ? 'linear-gradient(160deg, #6aa4ff, #a57cf0)' : on ? 'linear-gradient(160deg, #3d7be0, #7b46cf)' : 'linear-gradient(160deg, #4f6fae, #5d4790)',
              boxShadow: lit ? '0 0 0 3px rgba(255,255,255,0.55), 0 3px 10px rgba(0,0,0,0.3)' : '0 3px 10px rgba(0,0,0,0.3)' }}>
              {o.main ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 5, backgroundColor: 'rgba(255,255,255,0.85)' }} /> : null}
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4, maxWidth: w - 6 }}>
                {n ? <NumberBadge n={n} size={16} /> : null}
                {w > 54 || !n ? <Text style={{ fontSize: 11, fontWeight: 600, color: '#fff', textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden' }}>{o.name}</Text> : null}
              </View>
              {h > 44 ? <Text style={{ fontSize: 10, color: 'rgba(255,255,255,0.82)', textAlign: 'center', whiteSpace: 'nowrap' }}>{`${o.mode.w} × ${o.mode.h}`}</Text> : null}
            </View>
          </View>
        );
      })}
    </View>
  );
}

// A display's number in a small white circle (the arrangement's tiles and the display's header).
function NumberBadge({ n, size = 16, color = '#fff', textColor = '#2c3e70' }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: color, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: Math.round(size * 0.66), fontWeight: 700, color: textColor, textAlign: 'center' }}>{String(n)}</Text>
    </View>
  );
}

// One scale as a picture of the screen with a window: the larger the scale, the bigger the window
// ("Larger text … More space"). A 2 px accent ring marks the current one.
function ScaleTile({ n, on, rec, label, onPick, t, tr }) {
  const [hover, setHover] = useState(false);
  const ww = Math.round(20 + (n - 100) * 0.26), wh = Math.round(ww * 0.64);
  return (
    <View onClick={onPick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flex: 1, minWidth: 0, alignItems: 'center', gap: 5 }}>
      <View style={{ width: 70, height: 48, padding: 2, borderRadius: 8, border: `2px solid ${on ? t.accent : 'transparent'}` }}>
        <View style={{ flex: 1, borderRadius: 4, overflow: 'hidden', alignItems: 'center', justifyContent: 'center', direction: 'ltr',
          background: 'linear-gradient(160deg, #3d7be0, #7b46cf)', opacity: hover && !on ? 0.85 : 1 }}>
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: Math.max(2, Math.round(n / 50)), backgroundColor: 'rgba(255,255,255,0.8)' }} />
          <View style={{ width: ww, height: wh, marginTop: 3, borderRadius: 2, backgroundColor: 'rgba(246,246,249,0.95)',
            boxShadow: '0 1px 3px rgba(0,0,0,0.3)' }}>
            <View style={{ height: Math.max(2, Math.round(wh / 6)), backgroundColor: 'rgba(0,0,0,0.12)', borderRadius: 2 }} />
          </View>
        </View>
      </View>
      <Text style={{ fontSize: typeScale.caption, color: on ? t.text : t.textDim, fontWeight: on ? 600 : 400, textAlign: 'center', whiteSpace: 'nowrap' }}>{label}</Text>
      <Text style={{ fontSize: 10, color: rec ? t.textDim : 'transparent', textAlign: 'center', whiteSpace: 'nowrap', marginTop: -4 }}>{tr('prefs.display.recommended')}</Text>
    </View>
  );
}

function ScaleTiles({ value, rec, onPick, t, tr }) {
  return (
    <View style={{ paddingTop: spacing.m, paddingBottom: spacing.s, gap: spacing.xs }}>
      <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: spacing.xs }}>
        {SCALES.map((n) => (
          <ScaleTile key={n} n={n} on={n === value} rec={n === rec} label={tr('prefs.display.scaleN', { n })} onPick={() => n !== value && onPick(n)} t={t} tr={tr} />
        ))}
      </View>
      <View style={{ flexDirection: 'row', paddingHorizontal: spacing.xs }}>
        <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('prefs.display.largerText')}</Text>
        <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{tr('prefs.display.moreSpace')}</Text>
      </View>
    </View>
  );
}

// The selected display: its picture, name and size line (first row of its box).
function DisplayHeader({ cur, sizeLine, several, n, t, tr }) {
  return (
    <View style={{ minHeight: 56, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s }}>
      <DisplayGlyph size={36} color={t.accent} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap' }}>{cur.name}</Text>
          {n ? (
            <View style={{ height: 16, paddingHorizontal: 6, borderRadius: 8, justifyContent: 'center', backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.07)' }}>
              <Text style={{ fontSize: 10, fontWeight: 500, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('prefs.display.number', { n })}</Text>
            </View>
          ) : null}
          {cur.main && several ? (
            <View style={{ height: 16, paddingHorizontal: 6, borderRadius: 8, justifyContent: 'center', backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.07)' }}>
              <Text style={{ fontSize: 10, fontWeight: 500, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('prefs.display.mainLabel')}</Text>
            </View>
          ) : null}
        </View>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{sizeLine}</Text>
      </View>
    </View>
  );
}

function Brightness({ list }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [vals, setVals] = useState({});
  const timer = useRef(0);
  const [err, setErr] = useState('');
  if (!list.length) return [<SettingRow key="none" label={tr('prefs.display.brightness')} hint={tr('prefs.display.noBacklight')} disabled />];
  return list.map((b) => {
    const v = vals[b.name] ?? b.value;
    const set = (x) => {
      const n = Math.max(1, Math.round(x));          // 0 turns some panels fully off
      setVals((o) => ({ ...o, [b.name]: n }));
      clearTimeout(timer.current);
      timer.current = setTimeout(() => admin.run('display-brightness', `${b.name} ${n}`, undefined, { quiet: true }).then(() => setErr(''), (e) => setErr(e.message)), 120);
    };
    return (
      <SettingRow key={b.name} label={tr('prefs.display.brightness')} hint={err || (list.length > 1 ? b.name : '')}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, direction: 'ltr' }}>
          <Text style={{ fontSize: 12, color: t.textDim }}>☼</Text>
          <Slider value={v} min={0} max={b.max} width={180} onChange={set} />
          <Text style={{ fontSize: 15, color: t.textDim }}>☀</Text>
        </View>
      </SettingRow>
    );
  });
}

function NightShift() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const p = nightPrefs(s);
  const [preview, setPreview] = useState(null);
  const tm = useRef(0);
  const patch = (x) => set({ night: { ...p, ...x } });
  const sun = p.schedule === 'sun' ? sunTimes() : null;
  const active = nightActive();
  useEffect(() => { nightTick(); }, [s.night]);
  const fmt = (m) => { const d = new Date(); d.setHours(Math.floor(m / 60), m % 60, 0, 0); return formatTime(d); };
  const scheduleHint = p.schedule === 'sun'
    ? (sun ? tr('prefs.night.sunHint', { set: fmt(sun.sunset), rise: fmt(sun.sunrise) }) : tr('prefs.night.noSun'))
    : p.schedule === 'custom' ? tr('prefs.night.customHint') : tr('prefs.night.offHint');
  return (
    <Section title={tr('prefs.night.title')} footer={tr('prefs.night.footer')}>
      <SettingRow label={tr('prefs.night.schedule')} hint={scheduleHint}>
        <PopUp value={p.schedule} onChange={(schedule) => patch({ schedule, manual: null })}
          options={[['off', tr('prefs.night.off')], ['sun', tr('prefs.night.sun')], ['custom', tr('prefs.night.custom')]]} />
      </SettingRow>
      {p.schedule === 'custom' && (
        <SettingRow label={tr('prefs.night.from')}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
            <TimeField value={p.from} onChange={(from) => patch({ from })} format={fmt} />
            <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('prefs.night.to')}</Text>
            <TimeField value={p.to} onChange={(to) => patch({ to })} format={fmt} />
          </View>
        </SettingRow>
      )}
      <SettingRow label={tr('prefs.night.now')}
        hint={p.manual && Date.now() < p.manual.until ? tr(p.manual.on ? 'prefs.night.manualOn' : 'prefs.night.manualOff', { time: formatTime(new Date(p.manual.until)) }) : ''}>
        <Toggle small on={active} onChange={(on) => setNightManual(on)} />
      </SettingRow>
      <SettingRow label={tr('prefs.night.temp')}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, direction: 'ltr' }}>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('prefs.night.lessWarm')}</Text>
          {/* the slider goes from less warm (left) to more warm (right) */}
          <Slider value={6500 - (preview ?? p.temp)} min={6500 - 6000} max={6500 - 1900} width={150}
            onChange={(v) => { const k = Math.round(6500 - v); setPreview(k); previewNight(k); clearTimeout(tm.current);
              tm.current = setTimeout(() => { setPreview(null); patch({ temp: k }); }, 400); }} />
          <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('prefs.night.moreWarm')}</Text>
        </View>
      </SettingRow>
    </Section>
  );
}

export function DisplaysSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const d = useDisplays();
  const [s, set] = useSettings();
  const [msg, setMsg] = useState('');
  const [selId, setSel] = useState(null);
  const [list, setList] = useState(false);       // the scale as a list instead of tiles
  if (!d) return <Note>{tr('prefs.display.unavailable')}</Note>;
  const outs = d.outputs || [];
  const act = outs.filter((o) => o.active);
  const cur = outs.find((o) => o.id === selId && o.active) || outs.find((o) => o.main) || act[0] || outs[0];
  if (!d.onMonitor || !cur) {
    return (
      <Stack>
        <Note>{tr('prefs.display.notOnMonitor')}</Note>
        <NightShift />
        <BrowserVideo />
      </Stack>
    );
  }
  const mode = cur.mode || { w: cur.preferred.w, h: cur.preferred.h, refresh: 0 };
  const res = cur.resolutions || [];
  const curRes = res.find((r) => r.w === mode.w && r.h === mode.h) || res[0];
  const go = (o, x) => { if (!applyDisplay(o, x)) setMsg(tr('prefs.display.failed')); else setMsg(''); };
  const native = (r) => r.w === cur.preferred.w && r.h === cur.preferred.h;
  const scale = cur.scale || 100, rot = cur.rotation || 0, rec = cur.recommended || 100;
  const setScale = (sc) => go(cur, { w: mode.w, h: mode.h, refresh: mode.refresh, rotation: rot, scale: sc });
  const looks = scale !== 100 && cur.w ? tr('prefs.display.looksLike', { w: cur.w, h: cur.h }) : '';
  const sizeLine = [cur.connector, cur.diagonal ? tr('prefs.display.inches', { n: (cur.diagonal / 10).toFixed(1) }) : '', `${mode.w} × ${mode.h}`, looks].filter(Boolean).join(' · ');
  const scales = [100, 125, 150, 175, 200].map((n) => [n, n === rec ? tr('prefs.display.scaleRecommended', { n }) : tr('prefs.display.scaleN', { n })]);
  const several = act.length > 1 || outs.length > 1;
  const nums = displayNumbers(outs);
  return (
    <Stack>
      <ArrangementBlock outs={outs} sel={cur.id} onSelect={setSel} mirror={d.mirror} nums={nums} t={t} tr={tr}
        hint={act.length > 1 && !d.mirror ? tr('prefs.display.arrangeHint') : ''} canIdentify={act.length > 1 && !d.mirror} />

      <Section>
        <DisplayHeader cur={cur} sizeLine={sizeLine} several={several} n={act.length > 1 && !d.mirror ? nums[cur.id] : 0} t={t} tr={tr} />
        {several ? (
          <SettingRow label={tr('prefs.display.useAsMain')} hint={cur.main ? tr('prefs.display.mainHint') : ''} disabled={!cur.active || d.mirror}>
            <Toggle small on={!!cur.main} onChange={(on) => { if (on && !cur.main) setMainDisplay(cur.id); }} />
          </SettingRow>
        ) : null}
        <Brightness list={d.backlight || []} />
      </Section>

      <Section title={tr('prefs.display.scale')} footer={tr('prefs.display.scaleHint')}
        accessory={(
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('prefs.display.showList')}</Text>
            <Toggle small on={list} onChange={setList} />
          </View>
        )}>
        {list ? (
          <SettingRow label={tr('prefs.display.scale')} hint={looks}>
            <PopUp width={210} value={scale} onChange={setScale} options={scales} />
          </SettingRow>
        ) : <ScaleTiles value={scale} rec={rec} onPick={setScale} t={t} tr={tr} />}
        <SettingRow label={tr('prefs.display.resolution')}>
          <PopUp width={210} value={`${curRes?.w}x${curRes?.h}`} onChange={(v) => { const [w, h] = v.split('x').map(Number); go(cur, { w, h, refresh: 0, rotation: rot, scale }); }}
            options={res.map((r) => [`${r.w}x${r.h}`, native(r) ? tr('prefs.display.native', { w: r.w, h: r.h }) : `${r.w} × ${r.h}`])} />
        </SettingRow>
        <SettingRow label={tr('prefs.display.refresh')}>
          <PopUp width={210} value={mode.refresh} onChange={(refresh) => go(cur, { w: mode.w, h: mode.h, refresh, rotation: rot, scale })}
            options={(curRes?.rates || []).map((r) => [r, tr('prefs.display.hz', { hz: hz(r) })])} disabled={(curRes?.rates || []).length < 2} />
        </SettingRow>
        <SettingRow label={tr('prefs.display.rotation')} hint={rot ? tr('prefs.display.rotationHint') : ''}>
          <PopUp width={210} value={rot} onChange={(rotation) => go(cur, { w: mode.w, h: mode.h, refresh: mode.refresh, rotation, scale })}
            options={[[0, tr('prefs.display.rotStandard')], [1, '90°'], [2, '180°'], [3, '270°']]} />
        </SettingRow>
      </Section>
      {msg ? <Note tone="warn" style={{ marginTop: -12 }}>{msg}</Note> : null}

      {several ? (
        <Section title={tr('prefs.display.multiple')} footer={tr('prefs.display.multipleHint')}>
          <SettingRow label={tr('prefs.display.mirrorDisplays')} disabled={act.length < 2 && !d.mirror}>
            <Toggle small on={!!d.mirror} onChange={(on) => setDisplayMirror(on)} />
          </SettingRow>
          <SettingRow label={tr('prefs.display.menuBarAll')} disabled={d.mirror}>
            <Toggle small on={!!s.menuBarAllDisplays} onChange={(on) => set({ menuBarAllDisplays: on })} />
          </SettingRow>
          <SettingRow label={tr('prefs.display.capture')} hint={tr('prefs.display.captureHint')}>
            <PopUp width={170} value={d.captureAll ? 'all' : 'main'} onChange={(v) => setCaptureAllDisplays(v === 'all')}
              options={[['main', tr('prefs.display.captureMain')], ['all', tr('prefs.display.captureAll')]]} />
          </SettingRow>
          {outs.filter((o) => !o.active).map((o) => (
            <SettingRow key={o.id} label={o.name} hint={tr('prefs.display.noCrtc', { connector: o.connector })} disabled />
          ))}
        </Section>
      ) : null}

      <NightShift />
      {d.softTint ? <Note style={{ marginTop: -12 }}>{tr('prefs.night.software')}</Note> : null}
      <BrowserVideo />
    </Stack>
  );
}

// The arrangement with its hint and the Identify button under it (like a section footer).
function ArrangementBlock({ hint, canIdentify, tr, ...p }) {
  return (
    <View style={{ gap: 6 }}>
      <Arrangement {...p} />
      {hint || canIdentify ? (
        <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: spacing.s }}>
          <View style={{ flex: 1, minWidth: 0 }}>{hint ? <Note>{hint}</Note> : null}</View>
          {canIdentify ? <Button small label={tr('prefs.display.identify')} onClick={() => identifyDisplays()} /> : null}
        </View>
      ) : null}
    </View>
  );
}
