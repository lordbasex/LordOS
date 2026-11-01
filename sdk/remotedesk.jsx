// remotedesk.jsx — the RDP and VNC apps: saved computers, a form, and the remote screen drawn by the engine as a media
// node. There is no client window: lord-rdesk paints a shared picture and takes the input from here. Addresses are in
// settings; passwords only in the keyring. See docs/FILES.md, "Remote desktop".
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, MediaView, addListener } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { useAppWindow } from './components.jsx';
import { settings, useSettings } from './settings.js';
import { lan } from './lan.js';
import { t as tr } from './i18n.js';
import { Button, ScrollView, TextInput, Toggle } from './components.jsx';

// keys with a name in X11's keysyms; printable ones are their code point, Latin-1 or 0x01000000 + code
const SYMS = { Backspace: 0xff08, Tab: 0xff09, Enter: 0xff0d, Escape: 0xff1b, Delete: 0xffff, Home: 0xff50, ArrowLeft: 0xff51, ArrowUp: 0xff52, ArrowRight: 0xff53, ArrowDown: 0xff54,
  PageUp: 0xff55, PageDown: 0xff56, End: 0xff57, Insert: 0xff63, Shift: 0xffe1, Control: 0xffe3, Alt: 0xffe9, Meta: 0xffeb, F1: 0xffbe, F2: 0xffbf, F3: 0xffc0, F4: 0xffc1, F5: 0xffc2, F6: 0xffc3, F7: 0xffc4, F8: 0xffc5, F9: 0xffc6, F10: 0xffc7, F11: 0xffc8, F12: 0xffc9 };
const symOf = (e) => {
  if (SYMS[e.key]) return SYMS[e.key];
  const c = (e.char || e.key || '').codePointAt(0);
  return !c ? 0 : c < 256 ? c : 0x01000000 + c;
};
// RDP's scan codes equal the Linux codes for the main block; these are its extended ones
const EXT = { 102: 0x147, 103: 0x148, 104: 0x149, 105: 0x14b, 106: 0x14d, 107: 0x14f, 108: 0x150, 109: 0x151, 110: 0x152, 111: 0x153, 125: 0x15b, 97: 0x11d, 100: 0x138 };
const scanOf = (e) => EXT[e.code] || e.code || 0;

const bit = (b) => (b === 2 ? 4 : b === 1 ? 2 : 1);

function Screen({ id, st, onEnd, focused }) {
  const box = useRef(null);
  const buttons = useRef(0);
  const down = useRef(new Map());
  useEffect(() => {
    if (!focused) return undefined;
    // a release carries no character: it repeats what the press sent
    const send = (e, on) => {
      const sym = on ? symOf(e) : down.current.get(e.code) || symOf(e);
      if (on) down.current.set(e.code, sym); else down.current.delete(e.code);
      lord.rdesk.key(id, sym, scanOf(e), on ? 1 : 0);
    };
    const a = addListener('keydown', (e) => send(e, true)), b = addListener('keyup', (e) => send(e, false));
    return () => { a(); b(); };
  }, [focused, id]);
  const at = (e) => {
    const [x, y, w, ht] = lord.getLayout(box.current.id);
    const k = Math.min(w / st.w, ht / st.h), ox = x + (w - st.w * k) / 2, oy = y + (ht - st.h * k) / 2;
    return [Math.max(0, Math.min(st.w - 1, Math.round((e.x - ox) / k))), Math.max(0, Math.min(st.h - 1, Math.round((e.y - oy) / k)))];
  };
  const last = useRef([0, 0]);          // a release has no position of its own
  const ptr = (e, extra = 0) => { if (e) last.current = at(e); lord.rdesk.pointer(id, last.current[0], last.current[1], buttons.current | extra); };
  useEffect(() => addListener('pointerup', (e) => { if (buttons.current) { buttons.current &= ~bit(e.button); ptr(null); } }), [id]);
  return h(View, { style: { flex: 1, backgroundColor: '#000' } },
    h(View, { ref: box, style: { flex: 1 },
      onMouseDown: (e) => { buttons.current |= bit(e.button); ptr(e); },
      onMouseMove: (e) => ptr(e),
      onWheel: (e) => { const x = e.deltaY < 0 ? 8 : 16; ptr(e, x); ptr(e); } },
    st.media ? h(MediaView, { media: st.media, style: { flex: 1 } }) : null),
    h(View, { style: { position: 'absolute', right: 8, top: 8 } }, h(Button, { small: true, label: tr('remotedesk.disconnect'), onClick: onEnd })));
}

export function RemoteDeskApp({ proto }) {
  const t = useTheme();
  const { args, focused } = useAppWindow();
  const [s] = useSettings();
  const saved = s.remoteDesks?.[proto] || [];
  const trusted = s.remoteDesks?.certs || {};
  const [host, setHost] = useState(args?.host || '');
  const [port, setPort] = useState(args?.port ? String(args.port) : '');
  const [user, setUser] = useState(args?.user || '');
  const [domain, setDomain] = useState('');
  const [password, setPassword] = useState('');
  const [remember, setRemember] = useState(false);
  const [error, setError] = useState('');
  const [id, setId] = useState(0);
  const [st, setSt] = useState(null);
  const rdp = proto === 'rdp';
  const key = () => `${host.trim()}:${Number(port) || 0}`;
  const end = () => { if (id) lord.rdesk.close(id); setId(0); setSt(null); };
  useEffect(() => {
    if (!host.trim()) return;
    lan.saved(proto, key(), '').then((c) => { if (c) { setUser(c.user === '-' ? '' : c.user); setPassword(c.password); setRemember(true); } });
  }, [host, port]);
  useEffect(() => () => { if (id) lord.rdesk.close(id); }, [id]);
  useEffect(() => {
    if (!id) return undefined;
    const timer = setInterval(() => {
      const p = lord.rdesk.poll(id);
      if (!p) return;
      if (p.cert && trusted[key()] === p.cert) lord.rdesk.cert(id, 1);
      else setSt(p);
      if (p.state === 'connected') {
        const item = { host: host.trim(), port: Number(port) || 0, user: user.trim() };
        if (!saved.some((x) => x.host === item.host && x.port === item.port)) settings.set({ remoteDesks: { ...(settings.get().remoteDesks || {}), [proto]: [item, ...saved].slice(0, 20) } });
      }
      if (['denied', 'failed', 'ended', 'unreachable'].includes(p.state)) { setError(p.state === 'ended' ? '' : tr(`remotedesk.err.${p.state}`)); end(); }
    }, 33);
    return () => clearInterval(timer);
  }, [id]);
  const go = () => {
    if (id || !host.trim()) return;
    setError('');
    const n = lord.rdesk.open(proto, host.trim(), Number(port) || 0, user.trim(), domain.trim(), password);
    if (!n) { setError(tr('remotedesk.err.failed')); return; }
    if (remember) lan.remember(proto, key(), '', user.trim() || '-', password, tr('remotedesk.keyring', { name: host.trim() })).catch(() => {});
    setId(n);
  };
  const acceptCert = (ok) => {
    if (ok) settings.set({ remoteDesks: { ...(settings.get().remoteDesks || {}), certs: { ...trusted, [key()]: st.cert } } });
    lord.rdesk.cert(id, ok ? 1 : 0);
    setSt({ ...st, cert: '' });
  };
  if (st?.state === 'connected' && st.media) return h(Screen, { id, st, focused, onEnd: end });
  const label = (k) => h(Text, { style: { fontSize: 12, color: t.textDim, textAlign: 'start' } }, tr(k));
  return h(View, { style: { flex: 1, flexDirection: 'row', backgroundColor: t.windowBg } },
    h(View, { style: { width: 200, padding: 10, gap: 2, backgroundColor: t.sidebarBg } },
      label('remotedesk.saved'),
      ...saved.map((x) => h(View, { key: `${x.host}:${x.port}`, onClick: () => { setHost(x.host); setPort(x.port ? String(x.port) : ''); setUser(x.user); }, style: { height: 28, justifyContent: 'center', paddingHorizontal: 6, borderRadius: 6 } },
        h(Text, { style: { fontSize: 13, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, x.port ? `${x.host}:${x.port}` : x.host)))),
    h(ScrollView, { style: { flex: 1 }, contentStyle: { padding: 24, gap: 8 } },
      h(Text, { style: { fontSize: 20, fontWeight: 600, color: t.text, textAlign: 'start' } }, tr(rdp ? 'remotedesk.titleRdp' : 'remotedesk.titleVnc')),
      label('remotedesk.host'), h(TextInput, { value: host, onChange: setHost, autoFocus: true, onSubmit: go }),
      label('remotedesk.port'), h(TextInput, { value: port, onChange: (v) => setPort(v.replace(/\D/g, '').slice(0, 5)), placeholder: rdp ? '3389' : '5900', onSubmit: go }),
      rdp ? label('remotedesk.user') : null, rdp ? h(TextInput, { value: user, onChange: setUser, onSubmit: go }) : null,
      rdp ? label('remotedesk.domain') : null, rdp ? h(TextInput, { value: domain, onChange: setDomain, onSubmit: go }) : null,
      label('remotedesk.password'), h(TextInput, { secure: true, value: password, onChange: setPassword, onSubmit: go }),
      h(View, { style: { flexDirection: 'row', gap: 8, alignItems: 'center' } }, h(Toggle, { small: true, on: remember, onChange: setRemember }), h(Text, { style: { fontSize: 13, color: t.text } }, tr('remotedesk.remember'))),
      st?.cert ? h(View, { style: { gap: 6 } }, h(Text, { style: { fontSize: 13, color: t.text, textAlign: 'start' } }, tr('remotedesk.cert', { print: st.cert })),
        h(View, { style: { flexDirection: 'row', gap: 8 } }, h(Button, { label: tr('remotedesk.certNo'), onClick: () => acceptCert(false) }), h(Button, { primary: true, label: tr('remotedesk.certYes'), onClick: () => acceptCert(true) }))) : null,
      error ? h(Text, { style: { fontSize: 13, color: '#ff453a', textAlign: 'start' } }, error) : null,
      h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end' } },
        h(Button, { primary: true, label: id ? tr('remotedesk.connecting') : tr('remotedesk.connect'), onClick: go, disabled: !!id || !host.trim() }))));
}
