// remotedesk.jsx — the RDP and VNC apps: saved computers, a form, and the remote screen drawn by the engine as a media
// node. There is no client window: lord-rdesk paints a shared picture and takes the input from here. Addresses are in
// settings; passwords only in the keyring. See docs/FILES.md, "Remote desktop".
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, MediaView, addListener } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { useAppWindow, useWindowState, Segmented } from './components.jsx';
import { useStore } from './store.js';
import { windows, wm } from './windows.js';
import { home as fsHome, stat as fsStat } from './fs.js';
import { settings, useSettings } from './settings.js';
import { lan } from './lan.js';
import { certCheck } from './remotecert.js';
import { t as tr } from './i18n.js';
import { Button, ScrollView, TextInput, Toggle } from './components.jsx';
import { MenuButton, ToolbarButton } from './controls.jsx';

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

function Screen({ id, st, onEnd, focused, name, winId, folders }) {
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
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const inset = full ? 0 : 36;
  return h(View, { style: { flex: 1, backgroundColor: '#000' } },
    h(View, { ref: box, style: { flex: 1, marginTop: inset },
      onMouseDown: (e) => { buttons.current |= bit(e.button); ptr(e); },
      onMouseMove: (e) => ptr(e),
      onWheel: (e) => { const x = e.deltaY < 0 ? 8 : 16; ptr(e, x); ptr(e); } },
    st.media ? h(MediaView, { media: st.media, style: { flex: 1 } }) : null),
    h(SessionBar, { id, name, winId, folders, onEnd }));
}

// The strip above a session: always there in a window, and at the top edge when the session fills the screen.
function SessionBar({ id, name, winId, folders, onEnd }) {
  const t = useTheme();
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const [near, setNear] = useState(false);
  const cad = () => {
    const keys = [[0xffe3, 29], [0xffe9, 56], [0xffff, 0x153]];
    keys.forEach(([sym, scan]) => lord.rdesk.key(id, sym, scan, 1));
    keys.reverse().forEach(([sym, scan]) => lord.rdesk.key(id, sym, scan, 0));
  };
  const shared = folders.length ? folders.map((x) => ({ label: `${x.name}${x.ro ? ` · ${tr('remotedesk.readOnly')}` : ''}`, disabled: true })) : [{ label: tr('remotedesk.foldersNone'), disabled: true }];
  const bar = h(View, { onMouseLeave: () => setNear(false), style: { height: 36, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 10, backgroundColor: t.windowBg } },
    h(Text, { style: { flex: 1, fontSize: 13, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, name),
    h(ToolbarButton, { label: '⌃⌥⌦', title: tr('remotedesk.sendCad'), onClick: cad }),
    h(MenuButton, { icon: '▤', title: tr('remotedesk.tabFolders'), items: shared }),
    h(ToolbarButton, { icon: '⛶', title: tr(full ? 'remotedesk.exitFullscreen' : 'remotedesk.fullscreen'), active: full, onClick: () => wm.setFullscreen(winId, !full) }),
    h(ToolbarButton, { icon: '✕', title: tr('remotedesk.disconnect'), onClick: onEnd }));
  if (!full) return h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0 } }, bar);
  return h(View, { onMouseEnter: () => setNear(true), style: { position: 'absolute', left: 0, right: 0, top: 0, height: near ? 36 : 3 } }, near ? bar : null);
}

const RESOLUTIONS = [['auto', 0, 0], ['1280x800', 1280, 800], ['1600x900', 1600, 900], ['1920x1080', 1920, 1080], ['2560x1440', 2560, 1440]];
const BLANK = { proto: 'rdp', host: '', port: 0, user: '', domain: '', name: '', res: 'auto', full: false, folders: [] };
const keyOf = (d) => `${d.host}:${Number(d.port) || 0}`;
const idOf = (d) => `${d.proto}:${keyOf(d)}`;
const titleOf = (d) => d.name || (d.port ? `${d.host}:${d.port}` : d.host);
// what older versions kept, one list per protocol, becomes one list of computers
function devicesOf(s) {
  const r = s.remoteDesks || {};
  if (r.devices) return r.devices;
  return ['rdp', 'vnc'].flatMap((proto) => (r[proto] || []).map((x) => ({ ...BLANK, ...x, proto })));
}
const putDevices = (list) => settings.set({ remoteDesks: { ...(settings.get().remoteDesks || {}), devices: list } });
const CARD = { rdp: 'linear-gradient(135deg, #5468d8, #2a2f6e)', vnc: 'linear-gradient(135deg, #1f9aa8, #0f4a58)' };

function Card({ d, onOpen, onEdit, onDelete }) {
  const [hover, setHover] = useState(false);
  return h(View, { onClick: onOpen, onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false), style: { width: 240, height: 136, borderRadius: 12, padding: 12, justifyContent: 'space-between', background: CARD[d.proto], cursor: 'pointer' } },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 24 } },
      h(Text, { style: { flex: 1, fontSize: 11, fontWeight: 600, color: '#fff' } }, d.proto.toUpperCase()),
      hover ? h(Button, { small: true, label: tr('remotedesk.edit'), onClick: (e) => { e?.stopPropagation?.(); onEdit(); } }) : null,
      hover ? h(Button, { small: true, label: tr('remotedesk.delete'), onClick: (e) => { e?.stopPropagation?.(); onDelete(); } }) : null),
    h(View, null,
      h(Text, { style: { fontSize: 17, fontWeight: 700, color: '#fff', whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, titleOf(d)),
      d.user ? h(Text, { style: { fontSize: 12, color: 'rgba(255,255,255,0.8)', whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, d.user) : null));
}

function Folders({ draft, set }) {
  const t = useTheme();
  const [path, setPath] = useState('');
  const [bad, setBad] = useState(false);
  const home = fsHome();
  const add = (raw) => {
    const p = raw.trim().replace(/^~(?=\/)/, home).replace(/\/+$/, '');
    let ok = p.startsWith(`${home}/`) && !p.slice(home.length).includes('/.');
    try { ok = ok && fsStat(p).dir; } catch (e) { ok = false; }
    setBad(!ok);
    if (!ok || draft.folders.some((x) => x.path === p) || draft.folders.length >= 8) return;
    const name = (p.split('/').pop().replace(/[^0-9A-Za-z _-]/g, '_').slice(0, 31)) || 'folder';
    set({ folders: [...draft.folders, { name, path: p, ro: true }] });
    setPath('');
  };
  return h(View, { style: { gap: 8 } },
    h(Text, { style: { fontSize: 13, color: t.textDim, lineHeight: 1.4, textAlign: 'start' } }, tr('remotedesk.foldersIntro')),
    ...draft.folders.map((x) => h(View, { key: x.path, style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
      h(View, { style: { flex: 1, minWidth: 0 } },
        h(Text, { style: { fontSize: 13, color: t.text, textAlign: 'start' } }, x.name),
        h(Text, { style: { fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, x.path)),
      h(Text, { style: { fontSize: 12, color: t.textDim } }, tr('remotedesk.readOnly')),
      h(Toggle, { small: true, on: x.ro, onChange: (v) => set({ folders: draft.folders.map((y) => (y.path === x.path ? { ...y, ro: v } : y)) }) }),
      h(Button, { small: true, label: '−', onClick: () => set({ folders: draft.folders.filter((y) => y.path !== x.path) }) }))),
    h(View, { style: { flexDirection: 'row', gap: 8 } },
      h(View, { style: { flex: 1 } }, h(TextInput, { value: path, onChange: setPath, placeholder: tr('remotedesk.folderPath'), onSubmit: () => add(path) })),
      h(Button, { label: tr('remotedesk.folderAdd'), disabled: !path.trim(), onClick: () => add(path) })),
    bad ? h(Text, { style: { fontSize: 12, color: '#ff453a', textAlign: 'start' } }, tr('remotedesk.folderBad')) : null);
}

function Configure({ start, password: pw0, remember: rem0, note, onCancel, onSave }) {
  const t = useTheme();
  const [draft, setDraft] = useState(start);
  const [tab, setTab] = useState('general');
  const [password, setPassword] = useState(pw0 || '');
  const [remember, setRemember] = useState(!!rem0);
  const set = (p) => setDraft((d) => ({ ...d, ...p }));
  const rdp = draft.proto === 'rdp';
  const label = (k) => h(Text, { style: { fontSize: 12, color: t.textDim, textAlign: 'start' } }, tr(k));
  const tabs = [['general', tr('remotedesk.tabGeneral')], ['display', tr('remotedesk.tabDisplay')], ...(rdp ? [['folders', tr('remotedesk.tabFolders')]] : [])];
  const save = (connect) => onSave({ ...draft, host: draft.host.trim(), user: draft.user.trim(), domain: draft.domain.trim(), name: draft.name.trim(), folders: rdp ? draft.folders : [] }, password, remember, connect);
  return h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' } },
    h(View, { style: { width: 480, maxHeight: '94%', padding: 20, gap: 10, borderRadius: 12, backgroundColor: t.windowBg } },
      h(Text, { style: { fontSize: 15, fontWeight: 600, color: t.text, textAlign: 'start' } }, tr('remotedesk.configure')),
      note ? h(Text, { style: { fontSize: 13, color: t.text, textAlign: 'start' } }, note) : null,
      h(Segmented, { small: true, fill: true, options: tabs, value: tabs.some((x) => x[0] === tab) ? tab : 'general', onChange: setTab }),
      h(ScrollView, { style: { height: 330 }, contentStyle: { gap: 8, paddingVertical: 4 } },
        tab === 'general' ? [
          label('remotedesk.protocol'), h(Segmented, { key: 'p', small: true, options: [['rdp', 'RDP'], ['vnc', 'VNC']], value: draft.proto, onChange: (v) => set({ proto: v }) }),
          label('remotedesk.host'), h(TextInput, { key: 'h', value: draft.host, onChange: (v) => set({ host: v }), autoFocus: true }),
          label('remotedesk.port'), h(TextInput, { key: 'o', value: draft.port ? String(draft.port) : '', onChange: (v) => set({ port: Number(v.replace(/\D/g, '').slice(0, 5)) || 0 }), placeholder: rdp ? '3389' : '5900' }),
          rdp ? label('remotedesk.user') : null, rdp ? h(TextInput, { key: 'u', value: draft.user, onChange: (v) => set({ user: v }) }) : null,
          rdp ? label('remotedesk.domain') : null, rdp ? h(TextInput, { key: 'd', value: draft.domain, onChange: (v) => set({ domain: v }) }) : null,
          label('remotedesk.password'), h(TextInput, { key: 'w', secure: true, value: password, onChange: setPassword }),
          h(View, { key: 'r', style: { flexDirection: 'row', gap: 8, alignItems: 'center' } }, h(Toggle, { small: true, on: remember, onChange: setRemember }), h(Text, { style: { fontSize: 13, color: t.text } }, tr('remotedesk.remember'))),
          label('remotedesk.name'), h(TextInput, { key: 'n', value: draft.name, onChange: (v) => set({ name: v }), placeholder: tr('remotedesk.nameHint') }),
          !rdp ? h(Text, { key: 'v', style: { fontSize: 12, color: t.textDim, textAlign: 'start' } }, tr('remotedesk.vncPlain')) : null] : null,
        tab === 'display' ? [
          ...(rdp ? [label('remotedesk.resolution'), h(View, { key: 'g', style: { gap: 4 } }, ...RESOLUTIONS.map(([k]) => h(Button, { key: k, small: true, primary: draft.res === k, label: k === 'auto' ? tr('remotedesk.resAuto') : k.replace('x', ' × '), onClick: () => set({ res: k }) })))] : []),
          h(View, { key: 'f', style: { flexDirection: 'row', gap: 8, alignItems: 'center' } }, h(Toggle, { small: true, on: draft.full, onChange: (v) => set({ full: v }) }), h(Text, { style: { fontSize: 13, color: t.text } }, tr('remotedesk.startFull')))] : null,
        tab === 'folders' ? h(Folders, { draft, set }) : null),
      h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end' } },
        h(Button, { label: tr('remotedesk.cancel'), onClick: onCancel }),
        h(Button, { label: tr('remotedesk.save'), disabled: !draft.host.trim(), onClick: () => save(false) }),
        h(Button, { primary: true, label: tr('remotedesk.connect'), disabled: !draft.host.trim(), onClick: () => save(true) }))));
}

export function RemoteDeskApp() {
  const t = useTheme();
  const { id: winId, args, focused } = useAppWindow();
  const [s] = useSettings();
  const devices = devicesOf(s);
  const trusted = s.remoteDesks?.certs || {};
  const root = useRef(null);
  const [query, setQuery] = useState('');
  const [sheet, setSheet] = useState(args?.host ? { start: { ...BLANK, proto: args.proto === 'vnc' ? 'vnc' : 'rdp', host: args.host, port: args.port || 0, user: args.user || '' } } : null);
  // going to full screen moves the app in the tree and remounts it: the session belongs to the window
  const [active, setActive] = useWindowState('remote.active', null);
  const [error, setError] = useState('');
  const [id, setId] = useWindowState('remote.id', 0);
  const [st, setSt] = useWindowState('remote.state', null);
  const [entered, setEntered] = useWindowState('remote.entered', false);
  const end = () => { if (id) lord.rdesk.close(id); setId(0); setSt(null); setActive(null); setEntered(false); wm.setFullscreen(winId, false); };
  useEffect(() => () => { if (id && !windows.get().some((w) => w.id === winId)) lord.rdesk.close(id); }, [id]);
  useEffect(() => {
    if (!id) return undefined;
    const timer = setInterval(() => {
      const p = lord.rdesk.poll(id);
      if (!p) return;
      if (p.cert && certCheck(trusted, keyOf(active), p.cert).kind === 'known') lord.rdesk.cert(id, 1);
      else setSt(p);
      if (p.state === 'connected') {
        setEntered((was) => {
          if (was) return was;
          const list = devicesOf(settings.get());
          if (!list.some((x) => idOf(x) === idOf(active))) putDevices([active, ...list].slice(0, 50));
          if (active.full) wm.setFullscreen(winId, true);
          return true;
        });
      }
      if (['denied', 'failed', 'ended', 'unreachable'].includes(p.state)) { setError(p.state === 'ended' ? '' : tr(`remotedesk.err.${p.state}`)); end(); }
    }, 33);
    return () => clearInterval(timer);
  }, [id]);
  const start = (d, password) => {
    setError('');
    let [w, ht] = (RESOLUTIONS.find((r) => r[0] === d.res) || RESOLUTIONS[0]).slice(1);
    if (!w && root.current) { const l = lord.getLayout(root.current.id); w = Math.round(l[2]) & ~1; ht = Math.round(l[3]) & ~1; }
    const n = lord.rdesk.open(d.proto, d.host, Number(d.port) || 0, d.user, d.domain, password, Math.max(w, 640), Math.max(ht, 480), (d.folders || []).map((x) => [x.name, x.path, !!x.ro]));
    if (!n) { setError(tr('remotedesk.err.failed')); return; }
    setActive(d);
    setId(n);
  };
  const open = (d) => {
    if (id) return;
    lan.saved(d.proto, keyOf(d), '').then((c) => {
      if (c) start(d, c.password);
      else setSheet({ start: d, old: idOf(d), note: tr('remotedesk.needPassword') });
    });
  };
  const edit = (d) => { lan.saved(d.proto, keyOf(d), '').then((c) => setSheet({ start: d, old: idOf(d), password: c?.password, remember: !!c })); };
  const save = (d, password, remember, connect) => {
    const list = devicesOf(settings.get()).filter((x) => idOf(x) !== (sheet.old || idOf(d)) && idOf(x) !== idOf(d));
    putDevices([d, ...list].slice(0, 50));
    if (remember && password) lan.remember(d.proto, keyOf(d), '', d.user || '-', password, tr('remotedesk.keyring', { name: d.host })).catch(() => {});
    setSheet(null);
    if (connect) start(d, password);
  };
  const acceptCert = (ok) => {
    if (ok) settings.set({ remoteDesks: { ...(settings.get().remoteDesks || {}), certs: { ...trusted, [keyOf(active)]: st.cert } } });
    lord.rdesk.cert(id, ok ? 1 : 0);
    setSt({ ...st, cert: '' });
  };
  if (st?.state === 'connected' && st.media) return h(Screen, { id, st, focused, onEnd: end, name: titleOf(active), winId, folders: active.folders || [] });
  const q = query.trim().toLowerCase();
  const shown = devices.filter((d) => !q || `${titleOf(d)} ${d.host} ${d.user}`.toLowerCase().includes(q));
  return h(View, { ref: root, style: { flex: 1, backgroundColor: t.windowBg } },
    h(View, { style: { height: 48, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12 } },
      h(Text, { style: { flex: 1, fontSize: 15, fontWeight: 600, color: t.text, textAlign: 'start' } }, tr('remotedesk.saved')),
      h(View, { style: { width: 200 } }, h(TextInput, { value: query, onChange: setQuery, placeholder: tr('remotedesk.search') })),
      h(Button, { small: true, label: tr('remotedesk.add'), onClick: () => setSheet({ start: { ...BLANK } }) })),
    error ? h(Text, { style: { fontSize: 13, color: '#ff453a', paddingHorizontal: 12, textAlign: 'start' } }, error) : null,
    !devices.length ? h(View, { style: { flex: 1, alignItems: 'center', justifyContent: 'center' } }, h(Text, { style: { fontSize: 13, color: t.textDim } }, tr('remotedesk.empty'))) : null,
    devices.length ? h(ScrollView, { style: { flex: 1 }, contentStyle: { flexDirection: 'row', flexWrap: 'wrap', gap: 12, padding: 12 } },
      ...shown.map((d) => h(Card, { key: idOf(d), d, onOpen: () => open(d), onEdit: () => edit(d), onDelete: () => putDevices(devicesOf(settings.get()).filter((x) => idOf(x) !== idOf(d))) }))) : null,
    id ? h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' } },
      h(View, { style: { width: 420, padding: 20, gap: 10, borderRadius: 12, backgroundColor: t.windowBg } },
        h(Text, { style: { fontSize: 15, fontWeight: 600, color: t.text, textAlign: 'start' } }, tr('remotedesk.connectingTo', { name: active ? titleOf(active) : '' })),
        st?.cert ? h(Text, { style: { fontSize: 13, color: t.text, textAlign: 'start' } }, certCheck(trusted, keyOf(active), st.cert).kind === 'changed' ? tr('remotedesk.certChanged', { old: trusted[keyOf(active)], print: st.cert }) : tr('remotedesk.cert', { print: st.cert })) : null,
        h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end' } },
          st?.cert ? h(Button, { label: tr('remotedesk.certNo'), onClick: () => acceptCert(false) }) : h(Button, { label: tr('remotedesk.cancel'), onClick: end }),
          st?.cert ? h(Button, { primary: true, label: tr('remotedesk.certYes'), onClick: () => acceptCert(true) }) : null))) : null,
    sheet ? h(Configure, { start: sheet.start, password: sheet.password, remember: sheet.remember, note: sheet.note, onCancel: () => setSheet(null), onSave: save }) : null);
}
