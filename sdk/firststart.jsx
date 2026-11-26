// firststart.jsx — the first-start desktop chooser (0.8.9, docs/SETTINGS.md "First start"): on the first session of a
// new account on an installed system, a short wizard over the desktop. startShell starts it (startFirstStart) and
// mounts <FirstStart> in the modal layer for every theme, so it goes on when the theme switches live.
//
//   1  Choose your desktop   a centred panel over the dimmed desktop: the crown, three cards (picture, name, the theme's
//                            own description) in a random order, none chosen; "See it live" waits for a choice
//   2  live                  the chosen theme is applied for real (themes.use: this JS context is discarded and the
//                            new theme's startShell resumes from settings.firstStart); the panel is a small box that
//                            can be dragged anywhere inside the display's work area, with the three names (a click
//                            switches live), Back and Use this
//   3  Change it whenever    where to find it: a drawn replica of Settings → Appearance with Desktop style ringed,
//      you like              Back and Start. Start sets settings.desktopChosen; nothing of it runs afterwards.
//
// Keys: Tab goes round its controls (the modal layer has the keyboard), Space and Enter press the focused one, Enter
// alone the main button, Escape is Back. Who sees it and the steps are firststartmodel.js.
import { useEffect, useRef, useState } from 'react';
import { View, Text, Image, addListener, mainDisplay, displayAt } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, direction } from './i18n.js';
import { settings, saveSettingsNow } from './settings.js';
import { workAreaOf } from './windows.js';
import { shellTokens, DEFAULT_TOKENS } from './theme.js';
import { DesktopStyleThumb } from './stylethumb.jsx';
import { DESKTOPS, FLOAT_W, shouldShow, startState, cleanState, choose, next, back, canNext, themeToApply, clampFloat, floatPlace } from './firststartmodel.js';

// its own look in every theme, as in the approved design: a dark glass panel and the brand's violet
const INK = '#f4f4f6', DIM = 'rgba(244,244,246,0.62)', LINE = 'rgba(255,255,255,0.14)', GLASS = 'rgba(24,22,34,0.86)', VIOLET = '#7C5CFF';
const AMBER = '#f5b43c';
const FLOAT_H = 186;            // the floating box's height, for keeping it inside the work area

// null while it isn't shown, else { st, desks: { id: { name, description } }, current, ctx }
const fsStore = createStore(null);
export const firstStartState = () => fsStore.get()?.st || null;
// whether it is open: a theme's own first-run (Workspaces' activity onboarding) waits until it is done
export const useFirstStartOpen = () => useStore(fsStore, (v) => !!v);

// startShell, once per JS context. ctx: { list() and current() of sdk themes, use(id) switches live, onDone() }.
// Returns whether it is shown (the welcome tour waits for it).
export function startFirstStart(ctx) {
  let info = null, list = [];
  try { info = lord.system.info(); } catch (e) { /* no information: not shown */ }
  try { list = ctx.list() || []; } catch (e) { list = []; }
  const available = DESKTOPS.filter((id) => list.some((x) => x.id === id));
  const s = settings.get();
  if (!shouldShow(s, info, available)) {
    if (s.firstStart) settings.set({ firstStart: null });
    return false;
  }
  let current = null;
  try { current = ctx.current(); } catch (e) { /* none */ }
  let st = cleanState(s.firstStart, available) || startState(available);
  // a step that shows the chosen desktop live, but another one came up (its switch failed): back to choosing
  if (themeToApply(st, current)) st = { ...st, step: 1 };
  const desks = Object.fromEntries(list.filter((x) => available.includes(x.id)).map((x) => [x.id, { name: x.name, description: x.description, sketch: x.sketch || null }]));
  settings.set({ firstStart: st });
  fsStore.set({ st, desks, current, ctx });
  return true;
}

// Moves to state st2, switching the theme when the step shows a desktop that isn't the current one. The settings
// carry the state across the switch (themes.use saves them before the new theme loads).
function go(st2) {
  const cur = fsStore.get();
  if (!cur) return;
  settings.set({ firstStart: st2 });
  const target = themeToApply(st2, cur.current);
  if (target) {
    let ok = false;
    try { ok = !!cur.ctx.use(target); } catch (e) { ok = false; }
    if (ok) return;                       // this JS context ends; the new theme resumes on st2
    // not switched: the theme stays as it was and the person chooses again
    st2 = { ...st2, step: 1, pick: cur.current && st2.order.includes(cur.current) ? cur.current : null };
    settings.set({ theme: cur.current, themeReopen: null, firstStart: st2 });
  }
  fsStore.set({ ...cur, st: st2 });
}

function finish() {
  const cur = fsStore.get();
  if (!cur) return;
  settings.set({ desktopChosen: true, firstStart: null });
  try { saveSettingsNow(); } catch (e) { /* saved in 300 ms anyway */ }
  fsStore.set(null);
  try { cur.ctx.onDone?.(); } catch (e) { /* the tour is optional */ }
}

const primary = () => {
  const cur = fsStore.get();
  if (!cur || !canNext(cur.st)) return;
  if (cur.st.step === 3) finish();
  else go(next(cur.st));
};
const goBack = () => { const cur = fsStore.get(); if (cur && cur.st.step > 1) go(back(cur.st)); };
const pick = (id) => { const cur = fsStore.get(); if (cur) go(choose(cur.st, id)); };

// ---- small parts ----

function Btn({ label, onClick, main, disabled, grow }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} focusable={!disabled} accessibilityLabel={label}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 34, paddingHorizontal: 18, borderRadius: 10, alignItems: 'center', justifyContent: 'center', ...(grow ? { flex: 1 } : null),
        backgroundColor: main ? VIOLET : hover ? 'rgba(255,255,255,0.16)' : 'rgba(255,255,255,0.10)', opacity: disabled ? 0.35 : hover && main ? 0.88 : 1 }}>
      <Text style={{ fontSize: 14, fontWeight: 600, color: main ? '#fff' : INK, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

function Steps({ step }) {
  return (
    <View accessibilityLabel={t('shell.firstStart.step', { n: step, total: 3 })}
      style={{ flexDirection: 'row', justifyContent: 'center', gap: 8, marginBottom: 14 }}>
      {[1, 2, 3].map((i) => <View key={i} style={{ width: 28, height: 5, borderRadius: 3, backgroundColor: i <= step ? VIOLET : 'rgba(255,255,255,0.18)' }} />)}
    </View>
  );
}

const Crown = () => <Image src="asset:icons/lordos-crown-512.png" style={{ width: 52, height: 52, alignSelf: 'center', marginBottom: 10 }} />;
const arrow = () => (direction() === 'rtl' ? '←' : '→');
const settingsPath = (deep) => [t('apps.settings'), t('settings.section.appearance'), ...(deep ? [t('settings.style.title')] : [])];

function Card({ id, desk, on, width }) {
  const [hover, setHover] = useState(false);
  const thumbW = width - 24, thumbH = Math.round(thumbW * 10 / 16);
  return (
    <View onClick={() => pick(id)} focusable accessibilityLabel={desk.name} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width, padding: 10, paddingBottom: 14, borderRadius: 16, border: `2px solid ${on ? VIOLET : 'transparent'}`,
        backgroundColor: on ? 'rgba(124,92,255,0.14)' : hover ? 'rgba(255,255,255,0.09)' : 'rgba(255,255,255,0.05)' }}>
      <View style={{ width: thumbW, height: thumbH, borderRadius: 10, overflow: 'hidden', border: `1px solid ${LINE}` }}>
        <Image src={`asset:desktops/${id}.jpg`} style={{ width: thumbW, height: thumbH }} />
      </View>
      <Text style={{ fontSize: 16, fontWeight: 700, color: INK, marginTop: 12, marginHorizontal: 4, textAlign: 'start' }}>{desk.name}</Text>
      <Text style={{ fontSize: 13, color: DIM, marginTop: 4, marginHorizontal: 4, lineHeight: 1.4, textAlign: 'start' }}>{desk.description}</Text>
    </View>
  );
}

// Settings → Appearance in miniature, in the person's language, with the chosen desktop selected and the Desktop style
// group ringed: drawn with the SDK's own parts, not a screenshot. Its styles are in Settings' order (Flow, Lord
// Classic, then the others), not the chooser's random one.
function SettingsReplica({ cur, width }) {
  const tokens = useStore(shellTokens) || DEFAULT_TOKENS;
  const accent = tokens.accent || DEFAULT_TOKENS.accent;
  const side = [['general', '#8e8e93'], ['appearance', '#5e5ce6'], ['wallpaper', '#30b0c7'], ['dock', '#636366'], ['displays', '#0a84ff'],
    ['network', '#0a84ff'], ['bluetooth', '#0a84ff'], ['sound', '#ff375f']];
  const label = (s, extra) => <Text style={{ fontSize: 11, color: INK, whiteSpace: 'nowrap', textAlign: 'start', ...extra }}>{s}</Text>;
  return (
    <View style={{ width, alignSelf: 'center', borderRadius: 14, overflow: 'hidden', border: `1px solid ${LINE}`, backgroundColor: '#1d1c22',
      boxShadow: '0 14px 34px rgba(0,0,0,0.45)' }}>
      <View style={{ height: 26, alignItems: 'center', justifyContent: 'center', borderBottom: `1px solid ${LINE}` }}>
        <View style={{ position: 'absolute', start: 10, top: 9, flexDirection: 'row', gap: 5, direction: 'ltr' }}>
          {['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: c }} />)}
        </View>
        {label(t('apps.settings'), { color: DIM })}
      </View>
      <View style={{ flexDirection: 'row', height: 268 }}>
        <View style={{ width: 150, padding: 8, gap: 2, backgroundColor: '#24232a' }}>
          <View style={{ height: 20, borderRadius: 5, backgroundColor: 'rgba(255,255,255,0.07)', justifyContent: 'center', paddingHorizontal: 6, marginBottom: 6 }}>
            {label(t('settings.search'), { color: DIM, fontSize: 10 })}
          </View>
          {side.map(([k, c]) => (
            <View key={k} style={{ height: 20, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6, borderRadius: 5,
              backgroundColor: k === 'appearance' ? accent : 'transparent' }}>
              <View style={{ width: 12, height: 12, borderRadius: 3, backgroundColor: k === 'appearance' ? 'rgba(255,255,255,0.35)' : c }} />
              {label(t(`settings.section.${k}`), { flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', color: k === 'appearance' ? '#fff' : INK })}
            </View>
          ))}
        </View>
        <View style={{ flex: 1, minWidth: 0, paddingHorizontal: 14, paddingTop: 10, gap: 8 }}>
          {label(t('settings.section.appearance'), { fontSize: 13, fontWeight: 600 })}
          <View style={{ borderRadius: 12, border: `3px solid ${AMBER}`, padding: 6, gap: 6, boxShadow: '0 0 0 5px rgba(245,180,60,0.22)' }}>
            {label(t('settings.style.title'), { fontWeight: 600 })}
            <View style={{ flexDirection: 'row', justifyContent: 'center', gap: 10 }}>
              {[...cur.st.order].sort((a, b) => DESKTOPS.indexOf(a) - DESKTOPS.indexOf(b)).map((id) => {
                const on = id === cur.st.pick;
                return (
                  <View key={id} style={{ alignItems: 'center', gap: 4 }}>
                    <View style={{ padding: 2, borderRadius: 10, border: `2px solid ${on ? accent : 'transparent'}` }}>
                      <DesktopStyleThumb id={id} accent={accent} dark sketch={cur.desks[id]?.sketch || null} />
                    </View>
                    {label(cur.desks[id]?.name || id, { fontWeight: on ? 700 : 400, fontSize: 10 })}
                  </View>
                );
              })}
            </View>
          </View>
          <Text style={{ fontSize: 9, color: DIM, lineHeight: 1.35, textAlign: 'start' }}>{t('settings.style.hint')}</Text>
          {label(t('settings.mode'), { fontWeight: 600, marginTop: 2 })}
          <View style={{ flexDirection: 'row', gap: 16, justifyContent: 'center' }}>
            {[['dark', '#2c2c30'], ['light', '#f2f2f7']].map(([k, c]) => (
              <View key={k} style={{ alignItems: 'center', gap: 3 }}>
                <View style={{ width: 54, height: 34, borderRadius: 6, backgroundColor: c, border: `2px solid ${k === 'dark' ? accent : 'transparent'}` }} />
                {label(t(`settings.${k}`), { fontSize: 10, color: DIM })}
              </View>
            ))}
          </View>
        </View>
      </View>
    </View>
  );
}

function Chips() {
  const parts = settingsPath(true);
  return (
    <View style={{ flexDirection: 'row', justifyContent: 'center', alignItems: 'center', gap: 8, marginTop: 14, flexWrap: 'wrap' }}>
      {parts.flatMap((p, i) => [
        ...(i ? [<Text key={`a${i}`} style={{ fontSize: 14, color: DIM }}>{arrow()}</Text>] : []),
        <View key={`c${i}`} style={{ paddingHorizontal: 10, paddingVertical: 4, borderRadius: 8, backgroundColor: 'rgba(255,255,255,0.08)' }}>
          <Text style={{ fontSize: 14, fontWeight: 600, color: INK, whiteSpace: 'nowrap' }}>{p}</Text>
        </View>,
      ])}
    </View>
  );
}

// ---- steps 1 and 3: the centred panel over the dimmed desktop ----

function Panel({ cur }) {
  const d = mainDisplay();
  const { st } = cur;
  const effects = settings.get().effects !== false;
  const w = Math.round(Math.min(d.w * 0.92, 1000));
  const inner = w - 56;
  const cardW = Math.floor((inner - 32) / 3);
  const name = cur.desks[st.pick]?.name || '';
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: effects ? 'rgba(8,6,14,0.45)' : 'rgba(8,6,14,0.72)',
      ...(effects ? { backdropFilter: 'blur(18px)' } : null) }}>
      <View style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: w, paddingHorizontal: 28, paddingTop: 24, paddingBottom: 20, borderRadius: 24, backgroundColor: GLASS, border: `1px solid ${LINE}`,
          boxShadow: '0 24px 60px rgba(0,0,0,0.5)', direction: direction(), ...(effects ? { backdropFilter: 'blur(26px)' } : null) }}>
          <Steps step={st.step} />
          <Crown />
          <Text style={{ fontSize: 24, fontWeight: 700, color: INK, textAlign: 'center' }}>{t(st.step === 1 ? 'shell.firstStart.title' : 'shell.firstStart.laterTitle')}</Text>
          <Text style={{ fontSize: 15, color: DIM, textAlign: 'center', lineHeight: 1.4, marginTop: 6, marginBottom: 18, maxWidth: 660, alignSelf: 'center' }}>
            {st.step === 1 ? t('shell.firstStart.lead') : t('shell.firstStart.laterLead', { name })}
          </Text>
          {st.step === 1 ? (
            <View style={{ flexDirection: 'row', gap: 16, justifyContent: 'center' }}>
              {st.order.map((id) => <Card key={id} id={id} desk={cur.desks[id] || { name: id, description: '' }} on={id === st.pick} width={cardW} />)}
            </View>
          ) : (
            <View>
              <SettingsReplica cur={cur} width={Math.min(640, inner)} />
              <Chips />
            </View>
          )}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, marginTop: 18 }}>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 12.5, color: DIM, textAlign: 'start' }}>
              {st.step === 1 ? t('shell.firstStart.note', { path: settingsPath(false).join(` ${arrow()} `) }) : ''}
            </Text>
            {st.step > 1 ? <Btn label={t('shell.firstStart.back')} onClick={goBack} /> : null}
            <Btn main label={t(st.step === 1 ? 'shell.firstStart.seeLive' : 'shell.firstStart.start')} onClick={primary} disabled={!canNext(st)} />
          </View>
        </View>
      </View>
    </View>
  );
}

// ---- step 2: the small floating box over the chosen desktop ----

const SIZE = { w: FLOAT_W, h: FLOAT_H };
const areaAt = (x, y) => workAreaOf(displayAt(x, y));

function Floating({ cur }) {
  const { st } = cur;
  const [pos, setPos] = useState(() => floatPlace(st.pos, workAreaOf(mainDisplay()), SIZE));
  const drag = useRef(null);
  const live = useRef(pos);
  live.current = pos;
  // the listeners exist only while the grip is held
  const onGrip = (e) => {
    e.stopPropagation?.();
    if (drag.current) return;
    const start = { x: e.x, y: e.y, ox: live.current.x, oy: live.current.y };
    const at = (ev) => {
      const p = { x: start.ox + ev.x - start.x, y: start.oy + ev.y - start.y };
      return clampFloat(p, areaAt(p.x + SIZE.w / 2, p.y + SIZE.h / 2), SIZE);
    };
    const offMove = addListener('pointermove', (ev) => setPos(at(ev)));
    const offUp = addListener('pointerup', (ev) => {
      offMove(); offUp();
      drag.current = null;
      const p = at(ev);
      setPos(p);
      const now = fsStore.get();
      if (now) { const st2 = { ...now.st, pos: p }; settings.set({ firstStart: st2 }); fsStore.set({ ...now, st: st2 }); }
    });
    drag.current = () => { offMove(); offUp(); };
  };
  useEffect(() => () => { drag.current?.(); drag.current = null; }, []);
  const dots = [0, 1, 2, 3, 4, 5].map((k) => <View key={k} style={{ width: 4, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.45)' }} />);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none', direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: pos.x, top: pos.y, width: SIZE.w, paddingHorizontal: 10, paddingTop: 8, paddingBottom: 10, borderRadius: 16,
        backgroundColor: GLASS, border: `1px solid ${LINE}`, boxShadow: '0 18px 44px rgba(0,0,0,0.5)', backdropFilter: 'blur(22px)' }}>
        <View onMouseDown={onGrip} accessibilityLabel={t('shell.firstStart.move')} title={t('shell.firstStart.move')}
          style={{ height: 16, marginBottom: 6, borderRadius: 8, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 3, cursor: 'grab' }}>
          {dots}
        </View>
        <View style={{ direction: direction(), gap: 4 }}>
          {st.order.map((id) => {
            const on = id === st.pick;
            return (
              <View key={id} onClick={() => { if (!on) pick(id); }} focusable accessibilityLabel={cur.desks[id]?.name || id}
                style={{ height: 30, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 10, borderRadius: 8,
                  border: `1.5px solid ${on ? VIOLET : 'transparent'}`, backgroundColor: on ? 'rgba(124,92,255,0.16)' : 'rgba(255,255,255,0.05)' }}>
                <Text style={{ flex: 1, minWidth: 0, fontSize: 13, fontWeight: 600, color: INK, textAlign: 'start', whiteSpace: 'nowrap' }}>{cur.desks[id]?.name || id}</Text>
                {on ? <Text style={{ fontSize: 13, color: '#b9a8ff' }}>✓</Text> : null}
              </View>
            );
          })}
          <View style={{ flexDirection: 'row', gap: 6, marginTop: 8 }}>
            <Btn label={t('shell.firstStart.back')} onClick={goBack} />
            <Btn main grow label={t('shell.firstStart.useThis')} onClick={primary} />
          </View>
        </View>
      </View>
    </View>
  );
}

// Mounted by startShell in the modal layer (sdk/index.js LocaleRoot): nothing at all once it is done.
export function FirstStart() {
  const cur = useStore(fsStore);
  const shown = !!cur;
  useEffect(() => {
    if (!shown) return undefined;
    return addListener('keydown', (e) => {
      if (e.ctrl || e.meta || e.alt) return;
      if (e.key === 'Escape') goBack();
      else if (e.key === 'Enter') primary();
    });
  }, [shown]);
  if (!cur) return null;
  return cur.st.step === 2 ? <Floating key="float" cur={cur} /> : <Panel cur={cur} />;
}
