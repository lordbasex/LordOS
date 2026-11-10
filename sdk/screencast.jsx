// screencast.jsx — a Linux app asks to share the screen or a window (org.freedesktop.portal.ScreenCast,
// server/src/ui/portal.c, docs/WAYLAND.md "Sharing a screen or a window").
//
// The system draws the chooser, never the app: the displays and the open windows, each with a picture of it now
// (lord.portal.castThumb, only while this chooser is up), and the name of the app asking: only one lord-server verified
// (`who`, its launcher's scope), else "an unknown program" with a warning, never the name a process gives itself, which
// any process can set (portal.c sends none). Nothing is shared until the
// person picks and presses Share; Cancel answers the app "cancelled". What the app may be offered comes from
// lord-server (screens, windows, one or several: sdk/screencastmodel.js). While it shares, the indicator of the
// screen-sharing API (ShareBadge, sdk/share.js) shows it with its Stop, and lord-server sends no frame until it is on
// screen (lord.portal.castShown).
//
//   useStore(castRuns)   → [{ id, who, name }]: the sessions sharing now (for the indicator)
//   castStop(id)         the indicator's Stop
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, MediaView, mainBox, screen } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { ScrollView } from './components.jsx';
import { windows, wm } from './windows.js';
import { castChoices, castToggle, castSources } from './screencastmodel.js';

const api = () => typeof lord !== 'undefined' && lord.portal && typeof lord.portal.cast === 'function';
const castAsk = createStore(null);      // { id, monitor, window, multiple, who }
export const castRuns = createStore([]);

// The names: the app's (from its verified id, `who`; '' when lord-server couldn't tell) and a window's app.
// sdk/hardware.jsx hands them over from sdk/apps.js, which loads the whole app registry this file has no need of.
let names = { app: () => '', window: () => '' };
export function castNames(n) { names = { ...names, ...n }; }
const verified = (p) => (typeof p?.who === 'string' && p.who ? names.app(p) : '');
export const castAppName = (p) => verified(p) || t('shell.cast.unknownApp');

// hw events from portal.c (sdk/hardware.jsx passes them on)
export function castEvent(kind, data) {
  if (kind === 'portal-cast') castAsk.set(data && data.id ? data : null);
  else if (kind === 'portal-cast-gone') { if (castAsk.get()?.id === data?.id) castAsk.set(null); }
  else if (kind === 'portal-casts') castRuns.set(Array.isArray(data) ? data.map((r) => ({ ...r, name: castAppName(r) })) : []);
}

export const castStop = (id) => { if (api()) lord.portal.castStop(id); };
export const castShown = (id) => { if (api()) lord.portal.castShown(id); };

function answer(id, sources) {
  castAsk.set(null);
  if (api()) lord.portal.cast(id, sources);
}

const frameOf = (id) => ({ node: wm.frameNode(id), also: wm.sheetNodes(id) });

function tokens() {
  const th = useTheme();
  const mode = useStore(settings, (s) => s.mode);
  return mode === 'light' ? { ...th, mode: 'light', windowBg: '#f6f6f8', text: '#1d1d1f', textDim: 'rgba(60,60,67,0.62)', separator: 'rgba(0,0,0,0.12)' } : th;
}

function Card({ thumb, label, sub, picked, onClick, th }) {
  return h(View, { onClick, style: { width: 164, gap: 6, padding: 6, borderRadius: 10, alignItems: 'center',
    border: `2px solid ${picked ? th.accent : 'transparent'}`, backgroundColor: picked ? `${th.accent}22` : 'transparent' } },
  h(View, { style: { width: 148, height: 92, borderRadius: 6, overflow: 'hidden', alignItems: 'center', justifyContent: 'center',
    backgroundColor: th.mode === 'light' ? 'rgba(0,0,0,0.06)' : 'rgba(0,0,0,0.35)' } },
  thumb ? h(MediaView, { media: thumb, style: { width: 148, height: 92 } }) : null),
  h(Text, { style: { alignSelf: 'stretch', fontSize: 12, fontWeight: 600, color: th.text, textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' } }, label),
  sub ? h(Text, { style: { alignSelf: 'stretch', fontSize: 11, color: th.textDim, textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' } }, sub) : null);
}

function Button({ label, primary, disabled, onClick, th }) {
  return h(View, { onClick: disabled ? undefined : onClick, style: { flex: 1, height: 28, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
    backgroundColor: primary ? th.accent : th.mode === 'light' ? '#ffffff' : 'rgba(255,255,255,0.14)', opacity: disabled ? 0.45 : 1,
    boxShadow: primary ? 'none' : '0 0.5px 1.5px rgba(0,0,0,0.25)' } },
  h(Text, { style: { fontSize: 13, fontWeight: 500, color: primary ? '#fff' : th.text } }, label));
}

export function ScreenCastPrompt() {
  const req = useStore(castAsk);
  const list = useStore(windows);
  const th = tokens();
  const [picked, setPicked] = useState([]);
  const [thumbs, setThumbs] = useState({});
  const id = req ? req.id : 0;
  // a window the theme draws with no frame node can't be painted alone, so it isn't offered
  const choices = castChoices(req, screen.displays, list.filter((w) => wm.frameNode(w.id) > 0));
  // the pictures, once per chooser, closed with it
  useEffect(() => {
    setPicked([]);
    if (!id || !api()) { setThumbs({}); return undefined; }
    const got = {};
    for (const s of choices.screens) got[s.key] = lord.portal.castThumb(id, { display: s.display }) || 0;
    for (const w of choices.windows) {
      const f = frameOf(w.id);
      got[w.key] = f.node ? lord.portal.castThumb(id, { node: f.node, also: f.also }) || 0 : 0;
    }
    setThumbs(got);
    // lord.media's ids, closed here (sdk/media.js would bring the players with it)
    return () => { for (const k of Object.keys(got)) if (got[k] && lord.media) lord.media.close(got[k]); };
  }, [id]);
  if (!req) return null;
  const app = verified(req);
  const pick = (key) => setPicked((p) => castToggle(p, key, !!req.multiple));
  const share = () => {
    const sources = castSources(picked, choices, frameOf);
    if (sources.length) answer(id, sources);
  };
  const section = (title, items) => [
    h(Text, { key: `${title}-t`, style: { alignSelf: 'stretch', fontSize: 11, fontWeight: 700, color: th.textDim, textAlign: 'start', marginTop: 6 } }, title),
    h(View, { key: `${title}-l`, style: { alignSelf: 'stretch', flexDirection: 'row', flexWrap: 'wrap', gap: 4 } }, items),
  ];
  const screens = choices.screens.map((s) => h(Card, { key: s.key, th, thumb: thumbs[s.key], picked: picked.includes(s.key), onClick: () => pick(s.key),
    label: s.name || t('shell.cast.screenN', { n: s.index + 1 }) }));
  const wins = choices.windows.map((w) => h(Card, { key: w.key, th, thumb: thumbs[w.key], picked: picked.includes(w.key), onClick: () => pick(w.key),
    label: w.title || names.window(w), sub: w.title ? names.window(w) : '' }));
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start', backgroundColor: 'rgba(0,0,0,0.3)', zIndex: 100000 } },
    h(View, { style: { marginTop: 70, width: 560, maxHeight: Math.max(320, mainBox().height - 140), borderRadius: 14, padding: 20, gap: 8, alignItems: 'center',
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 20px 60px rgba(0,0,0,0.45)' } },
    h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text, textAlign: 'center' } },
      app ? t('shell.cast.title', { app }) : t('shell.cast.titleUnknown')),
    app ? null : h(Text, { style: { alignSelf: 'stretch', fontSize: 12, fontWeight: 600, color: th.mode === 'light' ? '#b25000' : '#ff9f0a', textAlign: 'center', lineHeight: 1.4 } },
      t('shell.cast.unverified')),
    h(Text, { style: { fontSize: 12, color: th.textDim, textAlign: 'center', lineHeight: 1.4 } },
      t(req.multiple ? 'shell.cast.bodyMany' : 'shell.cast.body')),
    h(ScrollView, { style: { alignSelf: 'stretch', flexShrink: 1, maxHeight: Math.max(200, mainBox().height - 300) }, contentStyle: { gap: 4 } },
      req.monitor ? section(t('shell.cast.screens'), screens) : null,
      req.window ? section(t('shell.cast.windows'), wins.length ? wins
        : [h(Text, { key: 'none', style: { fontSize: 12, color: th.textDim, textAlign: 'start', paddingVertical: 8 } }, t('shell.cast.noWindows'))]) : null),
    h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 8, alignSelf: 'stretch' } },
      h(Button, { th, label: t('shell.cast.cancel'), onClick: () => answer(id, null) }),
      h(Button, { th, primary: true, disabled: !picked.length, label: t('shell.cast.share'), onClick: share }))));
}
