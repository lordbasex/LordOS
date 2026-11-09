// remotedesk.jsx — Remote Desk, the RDP and VNC app (docs/FILES.md, "Remote desktop"). Three kinds of window:
//   the list (no args): a sidebar (Favorites, Devices), a toolbar (add, pictures or list, sort, show, search) and the saved
//     computers as cards with the picture of their last session, or as rows;
//   a session (args.connect: a device id): the remote screen drawn by the engine as a media node; lord-rdesk paints a
//     shared picture and takes the input from here. There is no client window;
//   the settings (args.prefs): General, Credentials and Groups.
// Addresses are in settings, passwords only in the keyring (sdk/lan.js, service rdesk://<credential id>/). The data and
// its migration from older versions are in sdk/remotedeskmodel.js. The password fields have the 🔑 of Passwords (entries
// for exactly the computer's address, sdk/loginmodel.js); a password typed in the sign-in that the computer accepted gets
// "Save to Passwords?" over the session (sdk/vault.js SaveLoginOffer).
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image, MediaView, addListener } from '@lordos/runtime';
import { useTheme, popupBg } from './theme.js';
import { useAppWindow, useWindowState, Segmented, SidebarItem, useKeyboard, hasTextFocus } from './components.jsx';
import { useStore } from './store.js';
import { windows, wm } from './windows.js';
import { home as fsHome, stat as fsStat, read as fsRead, write as fsWrite } from './fs.js';
import { FilePicker } from './filepicker.jsx';
import { settings, useSettings } from './settings.js';
import { lan } from './lan.js';
import { AutofillButton, SaveLoginOffer } from './vault.js';
import { loginHosts, typedSecret } from './loginmodel.js';
import { certCheck } from './remotecert.js';
import { t as tr } from './i18n.js';
import { Button, ScrollView, TextInput, Toggle } from './components.jsx';
import { MenuButton, ToolbarButton, Toolbar, ToolbarGroup, ToolbarSpacer, SearchField, EmptyState, Hairline, ConfirmDialog } from './controls.jsx';
import {
  DEVICE, RESOLUTIONS, SCALES, SOUNDS, DEPTHS, MAX_DEVICES, MAX_CREDS, migrate, parseAddress, addressOf, defaultPort, splitUser, userOk, domainOk, credText, credName,
  titleOf, keyOf, newId, sortDevices, filterDevices, groupDevices, duplicate, swapButtons, thumbPath, decodeRdp, parseRdp, exportRdp, fileNameOf, resSize, resOk,
} from './remotedeskmodel.js';

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
const RED = '#ff453a';

// ---- settings ----
const desksOf = (s) => migrate(s.remoteDesks).value;
const putDesks = (patch) => settings.set({ remoteDesks: { ...desksOf(settings.get()), ...(typeof patch === 'function' ? patch(desksOf(settings.get())) : patch) } });
const putDevice = (id, patch) => putDesks((r) => ({ devices: r.devices.map((d) => (d.id === id ? { ...d, ...patch } : d)) }));
const groupName = (g) => g || tr('remotedesk.groupDefault');
const userLine = (d, creds) => credText(creds.find((c) => c.id === d.cred)) || '';

// the password of a device: its credential's, or (a device saved before 0.8.8.4) what was remembered for its old address,
// which then moves to the credential. { user, domain, password } or null.
async function secretOf(d, cred) {
  if (cred) {
    const c = await lan.saved('rdesk', cred.id, '');
    if (c) return { user: cred.user, domain: cred.domain, password: c.password };
  }
  if (d.legacy) {
    const old = await lan.saved(d.proto, d.legacy, '');
    if (old) {
      if (cred) lan.remember('rdesk', cred.id, '', cred.user || '-', old.password, tr('remotedesk.keyring', { name: credText(cred) || titleOf(d) })).catch(() => {});
      return { user: cred ? cred.user : old.user === '-' ? '' : old.user, domain: cred?.domain || '', password: old.password };
    }
  }
  return null;
}
const rememberCred = (c, password) => (password ? lan.remember('rdesk', c.id, '', c.user || '-', password, tr('remotedesk.keyring', { name: credText(c) || '—' })).catch(() => {}) : Promise.resolve());
const forgetCred = (c) => lan.forget('rdesk', c.id, '', c.user || '-');

// ---- small drawings ----
function Monitor({ size = 16, color }) {
  const k = size / 16;
  return h(View, { style: { width: size, height: size, alignItems: 'center', justifyContent: 'center' } },
    h(View, { style: { width: 14 * k, height: 10 * k, borderRadius: 2 * k, border: `${Math.max(1, 1.5 * k)}px solid ${color}` } }),
    h(View, { style: { width: 2 * k, height: 2 * k, backgroundColor: color } }),
    h(View, { style: { width: 8 * k, height: 1.5 * k, borderRadius: k, backgroundColor: color } }));
}

// what a card shows until the computer has a picture: the accent in a soft wash, and a screen
function Placeholder({ w, ht, radius }) {
  const t = useTheme();
  const dark = t.mode === 'dark';
  return h(View, { style: { position: 'absolute', left: 0, top: 0, width: w, height: ht, borderRadius: radius, alignItems: 'center', justifyContent: 'center',
    background: `linear-gradient(150deg, ${t.accent}, ${dark ? '#17171c' : '#3a3f58'})` } },
  h(View, { style: { width: w * 0.3, height: w * 0.19, borderRadius: Math.max(3, w * 0.02), border: `${Math.max(1.5, w * 0.008)}px solid rgba(255,255,255,0.55)`, backgroundColor: 'rgba(255,255,255,0.08)' } }),
  h(View, { style: { width: w * 0.03, height: w * 0.025, backgroundColor: 'rgba(255,255,255,0.55)' } }),
  h(View, { style: { width: w * 0.14, height: Math.max(2, w * 0.01), borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.55)' } }));
}

function Pill({ text }) {
  return h(View, { style: { height: 20, paddingHorizontal: 8, borderRadius: 10, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.5)' } },
    h(Text, { style: { fontSize: 10, fontWeight: 700, color: '#fff', letterSpacing: 0.5 } }, text));
}

// ---- the session ----
function Screen({ id, st, onEnd, focused, name, winId, folders, swap, extra }) {
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
  const ptr = (e, extra = 0) => { if (e) last.current = at(e); lord.rdesk.pointer(id, last.current[0], last.current[1], swapButtons(buttons.current, swap) | extra); };
  useEffect(() => addListener('pointerup', (e) => { if (buttons.current) { buttons.current &= ~bit(e.button); ptr(null); } }), [id, swap]);
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const inset = full ? 0 : 36;
  return h(View, { style: { flex: 1, backgroundColor: '#000' } },
    h(View, { ref: box, style: { flex: 1, marginTop: inset },
      onMouseDown: (e) => { buttons.current |= bit(e.button); ptr(e); },
      onMouseMove: (e) => ptr(e),
      onWheel: (e) => { const x = e.deltaY < 0 ? 8 : 16; ptr(e, x); ptr(e); } },
    st.media ? h(MediaView, { media: st.media, style: { flex: 1 } }) : null),
    h(SessionBar, { id, name, winId, folders, onEnd }),
    extra || null);
}

function sendCad(id) {
  const keys = [[0xffe3, 29], [0xffe9, 56], [0xffff, 0x153]];
  keys.forEach(([sym, scan]) => lord.rdesk.key(id, sym, scan, 1));
  keys.reverse().forEach(([sym, scan]) => lord.rdesk.key(id, sym, scan, 0));
}

// The strip above a session: always there in a window, and at the top edge when the session fills the screen.
function SessionBar({ id, name, winId, folders, onEnd }) {
  const t = useTheme();
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  const [near, setNear] = useState(false);
  const cad = () => sendCad(id);
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

// A dimmed window with a panel in the middle: the sheets and the session's prompts.
function Sheet({ width = 480, children }) {
  const t = useTheme();
  return h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 20 } },
    h(View, { style: { width, maxWidth: '96%', maxHeight: '96%', padding: 20, gap: 10, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg,
      border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' } }, children));
}
const Title = ({ children }) => { const t = useTheme(); return h(Text, { style: { fontSize: 15, fontWeight: 600, color: t.text, textAlign: 'start' } }, children); };
const Note = ({ children, color }) => { const t = useTheme(); return h(Text, { style: { fontSize: 12, color: color || t.textDim, lineHeight: 1.4, textAlign: 'start' } }, children); };
const Buttons = ({ children }) => h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 } }, children);

// a label column and a field, as in a classic form
function Row({ label, children, top }) {
  const t = useTheme();
  return h(View, { style: { flexDirection: 'row', alignItems: top ? 'flex-start' : 'center', gap: 10, minHeight: 28 } },
    h(Text, { style: { width: 120, fontSize: 13, color: t.textDim, textAlign: 'end', marginTop: top ? 6 : 0 } }, label),
    h(View, { style: { flex: 1, minWidth: 0 } }, children));
}
function Check({ on, onChange, label }) {
  const t = useTheme();
  return h(View, { style: { flexDirection: 'row', gap: 8, alignItems: 'center', minHeight: 26 } },
    h(Toggle, { small: true, on, onChange }), h(Text, { style: { flex: 1, fontSize: 13, color: t.text, textAlign: 'start' } }, label));
}

// A pop-up field: the current choice, and the list of choices (plus extra items, such as "Add Credentials…").
function Popup({ value, options, onChange, extra = [], width }) {
  const t = useTheme();
  const cur = options.find((o) => o[0] === value);
  const items = [...options.map(([v, label]) => ({ label, checked: v === value, onClick: () => onChange(v) })), ...(extra.length ? ['-', ...extra] : [])];
  return h(MenuButton, { items, minWidth: width || 220, trigger: (open) => h(View, { style: { width, height: 28, paddingHorizontal: 10, borderRadius: t.controlRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
    backgroundColor: t.control, border: `1px solid ${open ? t.accent : 'transparent'}` } },
  h(Text, { style: { flex: 1, fontSize: 13, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, cur ? cur[1] : ''),
  h(Text, { style: { fontSize: 9, color: t.textDim } }, '▼')) });
}

// ---- credentials ----
// cred: the one to edit, or null for a new one. onSave(cred) after it is in settings and its password in the keyring.
// hosts: the computers it is for (the 🔑 offers Passwords entries for exactly those addresses).
function CredentialSheet({ cred, hosts, onCancel, onSave }) {
  const t = useTheme();
  const [user, setUser] = useState(cred ? credText({ user: cred.user, domain: '' }) : '');
  const [domain, setDomain] = useState(cred?.domain || '');
  const [password, setPassword] = useState('');
  const [label, setLabel] = useState(cred?.label || '');
  const split = splitUser(user, domain);
  const bad = !split.user || !userOk(split.user) || !domainOk(split.domain);
  const save = async () => {
    if (bad) return;
    const c = { id: cred?.id || newId('c'), user: split.user, domain: split.domain, ...(label.trim() ? { label: label.trim().slice(0, 60) } : null) };
    putDesks((r) => ({ creds: cred ? r.creds.map((x) => (x.id === c.id ? c : x)) : [...r.creds, c].slice(-MAX_CREDS) }));
    if (cred && cred.user !== c.user && !password) {      // the keyring keeps it under the user name: move it
      const old = await lan.saved('rdesk', c.id, '');
      if (old) { await forgetCred(cred); await rememberCred(c, old.password); }
    } else if (password) { if (cred) await forgetCred(cred); await rememberCred(c, password); }
    onSave(c);
  };
  return h(Sheet, { width: 440 },
    h(Title, null, tr(cred ? 'remotedesk.credEdit' : 'remotedesk.credAddTitle')),
    h(Row, { label: tr('remotedesk.user') }, h(TextInput, { value: user, onChange: setUser, placeholder: tr('remotedesk.userHint'), autoFocus: true, onSubmit: save })),
    h(Row, { label: tr('remotedesk.password') }, h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
      h(View, { style: { flex: 1, minWidth: 0 } }, h(TextInput, { value: password, onChange: setPassword, secure: true, placeholder: cred ? tr('remotedesk.passwordKeep') : '', onSubmit: save })),
      h(AutofillButton, { app: 'remote-desk', hosts: loginHosts(hosts || []), onFill: ({ username, password: pw }) => { if (username && !user) setUser(username); setPassword(pw || ''); } }))),
    h(Row, { label: tr('remotedesk.domain'), top: true }, h(View, { style: { gap: 4 } },
      h(TextInput, { value: user.includes('\\') ? split.domain : domain, onChange: setDomain, placeholder: tr('remotedesk.domainHint') }),
      h(Note, null, tr('remotedesk.domainWhy')))),
    h(Row, { label: tr('remotedesk.credLabel') }, h(TextInput, { value: label, onChange: setLabel, placeholder: tr('remotedesk.nameHint') })),
    user && bad ? h(Note, { color: RED }, tr('remotedesk.userBad')) : null,
    h(Note, null, `${tr('remotedesk.credsSafe')} ${tr('remotedesk.credVnc')}`),
    h(Buttons, null,
      h(Button, { label: tr('remotedesk.cancel'), onClick: onCancel }),
      h(Button, { primary: true, label: tr('remotedesk.save'), disabled: bad, onClick: save })));
}

// ---- the computer's sheet ----
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
    h(Note, null, tr('remotedesk.foldersIntro')),
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
    bad ? h(Note, { color: RED }, tr('remotedesk.folderBad')) : null);
}

// what RDP brings over from this computer, and what lord-rdesk can't do yet
function DevicesTab({ draft, set }) {
  const t = useTheme();
  const rows = [['devKeyboard', true], ['devFolders', true, draft.folders.length ? tr('remotedesk.devFoldersOn', { count: draft.folders.length }) : tr('remotedesk.devFoldersOff')],
    ['devClipboard', false], ['devMic', false], ['devPrinters', false], ['devCameras', false], ['devCards', false], ['devUsb', false]];
  return h(View, { style: { gap: 2 } },
    h(Row, { label: tr('remotedesk.sound') }, h(Popup, { value: SOUNDS.includes(draft.sound) ? draft.sound : 'here', onChange: (v) => set({ sound: v }),
      options: SOUNDS.map((k) => [k, tr(`remotedesk.sound.${k}`)]) })),
    h(View, { style: { height: 1, backgroundColor: t.separator, marginVertical: 6 } }),
    h(Note, null, tr('remotedesk.devicesIntro')),
    ...rows.map(([k, ok, detail]) => h(View, { key: k, style: { flexDirection: 'row', alignItems: 'center', minHeight: 30, gap: 8 } },
      h(Text, { style: { width: 16, fontSize: 12, color: ok ? t.accent : t.textDim, textAlign: 'center' } }, ok ? '✓' : '–'),
      h(Text, { style: { flex: 1, fontSize: 13, color: ok ? t.text : t.textDim, textAlign: 'start' } }, tr(`remotedesk.${k}`)),
      h(Text, { style: { fontSize: 12, color: t.textDim } }, detail || tr(ok ? 'remotedesk.devYes' : 'remotedesk.devNotYet')))));
}

// The sheet to add or edit a computer. start: a device (id '' for a new one).
function DeviceSheet({ start, onCancel, onSaved }) {
  const t = useTheme();
  const [s] = useSettings();
  const r = desksOf(s);
  const [draft, setDraft] = useState(start);
  const [addr, setAddr] = useState(start.host ? addressOf(start) : '');
  const [tab, setTab] = useState('general');
  const [credSheet, setCredSheet] = useState(false);
  const set = (p) => setDraft((d) => ({ ...d, ...p }));
  const rdp = draft.proto === 'rdp';
  const parsed = parseAddress(addr);
  const tabs = [['general', tr('remotedesk.tabGeneral')], ['display', tr('remotedesk.tabDisplay')], ...(rdp ? [['devices', tr('remotedesk.tabDevices')], ['folders', tr('remotedesk.tabFolders')]] : [])];
  const cur = tabs.some((x) => x[0] === tab) ? tab : 'general';
  const credOptions = [['', tr('remotedesk.credAsk')], ...r.creds.map((c) => [c.id, credName(c)])];
  const groups = [['', groupName('')], ...(r.groups || []).map((g) => [g, g])];
  const save = () => {
    if (!parsed) return;
    const d = { ...draft, id: draft.id || newId(), host: parsed.host, port: parsed.port, name: draft.name.trim(), folders: rdp ? draft.folders : [] };
    // a computer whose address changed isn't the one in its old picture or trusted certificate
    const moved = start.id && (start.host !== d.host || start.port !== d.port || start.proto !== d.proto);
    if (moved && start.shot) { lord.rdesk?.forgetSnapshot?.(d.id); d.shot = 0; }
    putDesks((x) => ({ devices: start.id ? x.devices.map((y) => (y.id === d.id ? d : y)) : [d, ...x.devices].slice(0, MAX_DEVICES) }));
    onSaved(d);
  };
  return h(Sheet, { width: 560 },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
      h(View, { style: { flex: 1 } }, h(Title, null, tr(start.id ? 'remotedesk.editTitle' : 'remotedesk.addTitle'))),
      h(Segmented, { small: true, options: [['rdp', 'RDP'], ['vnc', 'VNC']], value: draft.proto, onChange: (v) => set({ proto: v }) })),
    h(Row, { label: tr('remotedesk.host'), top: true }, h(View, { style: { gap: 4 } },
      h(TextInput, { value: addr, onChange: setAddr, autoFocus: true, placeholder: tr('remotedesk.hostHint'), onSubmit: save }),
      addr.trim() && !parsed ? h(Note, { color: RED }, tr('remotedesk.hostBad')) : h(Note, null, `${tr('remotedesk.hostHint')}. ${tr('remotedesk.hostPort', { port: defaultPort(draft.proto) })}`))),
    h(Row, { label: tr('remotedesk.credentials') }, h(Popup, { value: r.creds.some((c) => c.id === draft.cred) ? draft.cred : '', options: credOptions, onChange: (v) => set({ cred: v }),
      extra: [{ label: tr('remotedesk.credAdd'), onClick: () => setCredSheet(true) }] })),
    h(Segmented, { small: true, fill: true, options: tabs, value: cur, onChange: setTab, style: { marginTop: 6 } }),
    h(ScrollView, { style: { height: 250 }, contentStyle: { gap: 8, paddingVertical: 8 } },
      cur === 'general' ? [
        h(Row, { key: 'n', label: tr('remotedesk.name') }, h(TextInput, { value: draft.name, onChange: (v) => set({ name: v }), placeholder: tr('remotedesk.nameHint') })),
        h(Row, { key: 'g', label: tr('remotedesk.group') }, h(Popup, { value: (r.groups || []).includes(draft.group) ? draft.group : '', options: groups, onChange: (v) => set({ group: v }) })),
        h(View, { key: 's', style: { height: 1, backgroundColor: t.separator, marginVertical: 4 } }),
        rdp ? h(Check, { key: 'r', on: draft.reconnect, onChange: (v) => set({ reconnect: v }), label: tr('remotedesk.reconnect') }) : null,
        rdp ? h(Check, { key: 'a', on: draft.admin, onChange: (v) => set({ admin: v }), label: tr('remotedesk.admin') }) : null,
        h(Check, { key: 'w', on: draft.swap, onChange: (v) => set({ swap: v }), label: tr('remotedesk.swap') }),
        !rdp ? h(Note, { key: 'v' }, `${tr('remotedesk.vncPlain')} ${tr('remotedesk.vncNoSound')}`) : null] : null,
      cur === 'display' ? [
        rdp ? h(Row, { key: 'r', label: tr('remotedesk.resolution') }, h(Popup, { value: draft.res, onChange: (v) => set({ res: v }),
          options: resOptions(r.resolutions, draft.res) })) : null,
        rdp ? h(Row, { key: 's', label: tr('remotedesk.scale'), top: true }, h(View, { style: { gap: 4 } },
          h(Popup, { value: SCALES.includes(draft.scale) ? draft.scale : 100, onChange: (v) => set({ scale: v }), options: SCALES.map((k) => [k, `${k} %`]) }),
          h(Note, null, tr('remotedesk.scaleHint')))) : null,
        rdp ? h(Row, { key: 'd', label: tr('remotedesk.depth') }, h(Popup, { value: DEPTHS.includes(draft.depth) ? draft.depth : 32, onChange: (v) => set({ depth: v }),
          options: DEPTHS.map((k) => [k, tr(`remotedesk.depth${k}`)]) })) : null,
        h(Check, { key: 'f', on: draft.full, onChange: (v) => set({ full: v }), label: tr('remotedesk.startFull') }),
        !rdp ? h(Note, { key: 'v' }, tr('remotedesk.vncSize')) : null] : null,
      cur === 'devices' ? h(DevicesTab, { draft, set }) : null,
      cur === 'folders' ? h(Folders, { draft, set }) : null),
    h(Buttons, null,
      h(Button, { label: tr('remotedesk.cancel'), onClick: onCancel }),
      h(Button, { primary: true, label: tr('remotedesk.save'), disabled: !parsed, onClick: save })),
    credSheet ? h(CredentialSheet, { cred: null, hosts: parsed ? [parsed.host] : [], onCancel: () => setCredSheet(false), onSave: (c) => { set({ cred: c.id }); setCredSheet(false); } }) : null);
}

// the resolutions to choose from: automatic, the presets, the custom ones (Settings → Resolutions) and the device's own
function resOptions(custom, cur) {
  const list = [...RESOLUTIONS.map(([k]) => k), ...(custom || []).filter(resOk), ...(resOk(cur) ? [cur] : [])];
  return [...new Set(list)].map((k) => [k, k === 'auto' ? tr('remotedesk.resAuto') : k.replace('x', ' × ')]);
}

// ---- the list ----
function deviceMenu(d, act) {
  return [
    { label: tr('remotedesk.connect'), onClick: () => act.connect(d) },
    { label: tr('remotedesk.edit'), onClick: () => act.edit(d) },
    { label: tr('remotedesk.duplicate'), onClick: () => act.duplicate(d) },
    ...(d.proto === 'rdp' ? [{ label: tr('remotedesk.export'), onClick: () => act.exportOne(d) }] : []),
    { label: tr(d.fav ? 'remotedesk.favRemove' : 'remotedesk.favAdd'), onClick: () => act.fav(d) },
    '-',
    { label: tr('remotedesk.remove'), destructive: true, onClick: () => act.remove(d) },
  ];
}

function RoundButton({ children }) {
  const t = useTheme();
  return h(View, { style: { width: 30, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center', backgroundColor: popupBg(t), boxShadow: '0 2px 6px rgba(0,0,0,0.3)' } }, children);
}
function Star({ d, act, small }) {
  const t = useTheme();
  return h(ToolbarButton, { small, icon: d.fav ? '★' : '☆', color: d.fav ? t.accent : t.text, title: tr(d.fav ? 'remotedesk.favRemove' : 'remotedesk.favorite'), onClick: () => act.fav(d) });
}

function Card({ d, w, ht, creds, home, selected, act }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const [menu, setMenu] = useState(false);
  const shot = thumbPath(home, d);
  const user = userLine(d, creds);
  const lit = hover || menu;
  return h(View, { onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false), onClick: () => act.select(d), onDoubleClick: () => act.connect(d),
    style: { width: w, height: ht, borderRadius: 14, padding: 2, border: `2px solid ${lit || selected ? t.accent : 'transparent'}` } },
  h(View, { style: { flex: 1, borderRadius: 11, overflow: 'hidden' } },
    h(Placeholder, { w: w - 8, ht: ht - 8, radius: 11 }),
    shot ? h(Image, { src: shot, style: { position: 'absolute', left: 0, top: 0, width: w - 8, height: ht - 8, borderRadius: 11, objectFit: 'cover' } }) : null,
    h(View, { style: { position: 'absolute', left: 0, right: 0, bottom: 0, height: 74, borderBottomLeftRadius: 11, borderBottomRightRadius: 11, background: 'linear-gradient(180deg, rgba(0,0,0,0), rgba(0,0,0,0.62))' } }),
    h(View, { style: { position: 'absolute', left: 12, top: 12 } }, h(Pill, { text: d.proto.toUpperCase() })),
    lit ? h(View, { style: { position: 'absolute', right: 10, top: 10, flexDirection: 'row', gap: 8 } },
      h(RoundButton, null, h(MenuButton, { icon: '⋯', small: true, title: tr('remotedesk.more'), items: deviceMenu(d, act), onOpenChange: setMenu })),
      h(RoundButton, null, h(Star, { d, act, small: true })))
      : d.fav ? h(View, { style: { position: 'absolute', right: 14, top: 12 } }, h(Text, { style: { fontSize: 15, color: '#fff' } }, '★')) : null,
    h(View, { style: { position: 'absolute', left: 14, right: 14, bottom: 12 } },
      h(Text, { style: { fontSize: 15, fontWeight: 700, color: '#fff', whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, titleOf(d)),
      h(Text, { style: { fontSize: 12, color: 'rgba(255,255,255,0.82)', whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, user || tr(d.cred ? 'remotedesk.noUser' : 'remotedesk.credAsk')))));
}

function ListRow({ d, creds, home, selected, act }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const [menu, setMenu] = useState(false);
  const shot = thumbPath(home, d);
  const lit = hover || menu;
  const sel = t.mode === 'dark' ? 'rgba(255,255,255,0.09)' : 'rgba(0,0,0,0.06)';
  return h(View, { onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false), onClick: () => act.select(d), onDoubleClick: () => act.connect(d),
    style: { height: 76, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: t.controlRadius,
      backgroundColor: selected ? sel : hover ? t.hover : 'transparent', border: `1px solid ${selected ? t.accent : 'transparent'}` } },
  h(View, { style: { width: 100, height: 62, borderRadius: 7, overflow: 'hidden' } },
    h(Placeholder, { w: 100, ht: 62, radius: 7 }),
    shot ? h(Image, { src: shot, style: { position: 'absolute', left: 0, top: 0, width: 100, height: 62, borderRadius: 7, objectFit: 'cover' } }) : null),
  h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
    h(Text, { style: { fontSize: 13, fontWeight: 700, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, titleOf(d)),
    h(Text, { style: { fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, userLine(d, creds) || tr(d.cred ? 'remotedesk.noUser' : 'remotedesk.credAsk'))),
  h(Text, { style: { fontSize: 11, fontWeight: 600, color: t.textDim } }, d.proto.toUpperCase()),
  lit || d.fav ? h(Star, { d, act, small: true }) : h(View, { style: { width: 22 } }),
  lit ? h(MenuButton, { icon: '⋯', small: true, title: tr('remotedesk.more'), items: deviceMenu(d, act), onOpenChange: setMenu }) : h(View, { style: { width: 22 } }));
}

function Section({ title, open, onToggle, children }) {
  const t = useTheme();
  return h(View, { style: { gap: 10 } },
    h(View, { onClick: onToggle, style: { flexDirection: 'row', alignItems: 'center', gap: 6, height: 26, alignSelf: 'flex-start', paddingRight: 8 } },
      h(Text, { style: { fontSize: 15, fontWeight: 700, color: t.text } }, title),
      h(Text, { style: { fontSize: 11, color: t.textDim } }, open ? '⌄' : '›')),
    open ? children : null);
}

function ListWindow({ winId, args }) {
  const t = useTheme();
  const [s] = useSettings();
  const r = desksOf(s);
  const home = fsHome();
  const [query, setQuery] = useState('');
  const [sheet, setSheet] = useState(args?.host ? { start: { ...DEVICE, proto: args.proto === 'vnc' ? 'vnc' : 'rdp', host: args.host, port: args.port || 0 } } : null);
  const [ask, setAsk] = useState(null);
  const [sel, setSel] = useState('');
  const frameW = useStore(windows, (ws) => ws.find((w) => w.id === winId)?.w || 980);
  const view = r.view === 'list' ? 'list' : 'grid';
  const sort = r.sort === 'last' ? 'last' : 'name';
  const kind = ['rdp', 'vnc'].includes(r.kind) ? r.kind : 'all';
  const side = r.sidebar !== false;
  const grouped = r.sections !== false;
  const section = r.section === 'favorites' ? 'favorites' : 'devices';
  const collapsed = r.collapsed || {};
  const add = (proto) => setSheet({ start: { ...DEVICE, proto, reconnect: r.reconnectDefault !== false } });
  const [picker, setPicker] = useState(null);        // { mode: 'open' } | { mode: 'save', d }
  const [note, setNote] = useState('');
  const importFile = (path) => {
    setPicker(null);
    let got = null;
    try { got = parseRdp(decodeRdp(new Uint8Array(fsRead(path, { binary: true })))); } catch (e) { got = null; }
    if (!got) { setNote(tr('remotedesk.importBad')); return; }
    const name = String(path).split('/').pop().replace(/\.rdp$/i, '');
    putDesks((x) => {
      let creds = x.creds, cred = '';
      if (got.user) {
        const same = creds.find((c) => c.user === got.user && (c.domain || '') === got.domain);
        if (same) cred = same.id;
        else { const c = { id: newId('c'), user: got.user, domain: got.domain }; creds = [...creds, c].slice(-MAX_CREDS); cred = c.id; }
      }
      const d = { ...got.device, id: newId(), name, cred };
      setSel(d.id);
      return { creds, devices: [d, ...x.devices].slice(0, MAX_DEVICES) };
    });
    setNote(tr('remotedesk.imported', { name }));
  };
  const exportFile = (path, d) => {
    setPicker(null);
    try { fsWrite(path, exportRdp(d, r.creds.find((c) => c.id === d.cred))); setNote(tr('remotedesk.exported', { name: String(path).split('/').pop() })); }
    catch (e) { setNote(e.message); }
  };
  const act = {
    select: (d) => setSel(d.id),
    connect: (d) => wm.open('remote-desk', { instance: `s-${d.id}`, args: { connect: d.id } }),
    edit: (d) => setSheet({ start: { ...DEVICE, ...d } }),
    duplicate: (d) => putDesks((x) => ({ devices: [duplicate(d, x.devices, (n) => tr('remotedesk.copyName', { name: n })), ...x.devices].slice(0, MAX_DEVICES) })),
    fav: (d) => putDevice(d.id, { fav: !d.fav }),
    remove: (d) => setAsk(d),
    exportOne: (d) => setPicker({ mode: 'save', d }),
  };
  const selected = r.devices.find((x) => x.id === sel);
  useEffect(() => {
    if (!winId) return;
    wm.setMenus(winId, [
      { label: tr('remotedesk.menuFile'), items: [
        { label: tr('remotedesk.addRdp'), hint: '❖N', onClick: () => add('rdp') },
        { label: tr('remotedesk.addVnc'), onClick: () => add('vnc') }, '-',
        { label: tr('remotedesk.import'), onClick: () => setPicker({ mode: 'open' }) },
        { label: tr('remotedesk.export'), disabled: !selected || selected.proto !== 'rdp', onClick: () => selected && setPicker({ mode: 'save', d: selected }) }, '-',
        { label: tr('remotedesk.newWindow'), onClick: () => wm.open('remote-desk', { new: true }) },
        { label: tr('remotedesk.settingsMenu'), hint: '❖,', onClick: openPrefs },
      ] },
      { label: tr('remotedesk.menuView'), items: [
        { label: tr('remotedesk.viewGrid'), checked: view === 'grid', onClick: () => putDesks({ view: 'grid' }) },
        { label: tr('remotedesk.viewList'), checked: view === 'list', onClick: () => putDesks({ view: 'list' }) }, '-',
        { label: tr('remotedesk.showSidebar'), checked: side, onClick: () => putDesks({ sidebar: !side }) },
        { label: tr('remotedesk.showGroups'), checked: grouped, onClick: () => putDesks({ sections: !grouped }) }, '-',
        { label: tr('remotedesk.sortName'), checked: sort === 'name', onClick: () => putDesks({ sort: 'name' }) },
        { label: tr('remotedesk.sortLast'), checked: sort === 'last', onClick: () => putDesks({ sort: 'last' }) },
      ] },
      { label: tr('remotedesk.menuConnections'), items: [
        { label: tr('remotedesk.connect'), disabled: !selected, onClick: () => selected && act.connect(selected) },
        { label: tr('remotedesk.edit'), disabled: !selected, onClick: () => selected && act.edit(selected) },
        { label: tr(selected?.fav ? 'remotedesk.favRemove' : 'remotedesk.favAdd'), disabled: !selected, onClick: () => selected && act.fav(selected) },
      ] },
    ]);
  }, [winId, sel, selected?.fav, selected?.proto, view, side, sort, r.sections]);
  const remove = (d) => {
    lord.rdesk?.forgetSnapshot?.(d.id);
    putDesks((x) => ({ devices: x.devices.filter((y) => y.id !== d.id) }));
    setAsk(null);
  };
  useKeyboard((e) => {
    if (sheet || ask || picker || hasTextFocus()) return;
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === 'n') { add('rdp'); return; }
    const d = r.devices.find((x) => x.id === sel);
    if (!d) return;
    if (e.key === 'Enter') act.connect(d);
    else if ((e.key === 'Delete' || (e.key === 'Backspace' && e.meta))) setAsk(d);
  });
  const shown = sortDevices(filterDevices(r.devices, { query, kind, favorites: section === 'favorites', creds: r.creds }), sort);
  const sections = section === 'favorites' || !grouped ? [['', shown]] : groupDevices(shown, query || kind !== 'all' ? [] : r.groups || []);
  // the cards share the row: as many as fit at 300 px or more, up to 380
  const cw = frameW - (side ? 200 : 0) - 40;
  const cols = Math.max(1, Math.floor((cw + 16) / (300 + 16)));
  const cardW = Math.max(220, Math.min(380, Math.floor((cw - 16 * (cols - 1)) / cols)));
  const cardH = Math.round(cardW * 215 / 350);
  const body = (list) => (view === 'grid'
    ? h(View, { style: { width: cw, flexDirection: 'row', flexWrap: 'wrap', gap: 16 } }, ...list.map((d) => h(Card, { key: d.id, d, w: cardW, ht: cardH, creds: r.creds, home, selected: sel === d.id, act })))
    : h(View, { style: { gap: 2 } }, ...list.map((d) => h(ListRow, { key: d.id, d, creds: r.creds, home, selected: sel === d.id, act }))));
  const addItems = [
    { label: tr('remotedesk.addRdp'), detail: tr('remotedesk.addRdpHint'), onClick: () => add('rdp') },
    { label: tr('remotedesk.addVnc'), detail: tr('remotedesk.addVncHint'), onClick: () => add('vnc') },
  ];
  const empty = !r.devices.length
    ? h(EmptyState, { icon: h(Monitor, { size: 48, color: t.textDim }), title: tr('remotedesk.emptyTitle'), hint: tr('remotedesk.empty'),
      action: h(View, { style: { flexDirection: 'row', gap: 8 } }, h(Button, { label: tr('remotedesk.addRdp'), onClick: () => add('rdp') }), h(Button, { label: tr('remotedesk.addVnc'), onClick: () => add('vnc') })) })
    : !shown.length ? h(EmptyState, { icon: section === 'favorites' && !query ? '☆' : '⌕', title: section === 'favorites' && !query ? tr('remotedesk.favorites') : null,
      hint: query ? tr('remotedesk.noMatch', { q: query.trim() }) : section === 'favorites' ? tr('remotedesk.emptyFav') : tr('remotedesk.noKind') }) : null;
  return h(View, { style: { flex: 1, flexDirection: 'row', backgroundColor: t.windowBg } },
    side ? h(View, { style: { width: 200, backgroundColor: t.sidebarBg, paddingHorizontal: 10, paddingTop: 12, gap: 2 } },
      h(Hairline, { edge: 'end' }),
      h(SidebarItem, { label: tr('remotedesk.favorites'), icon: '★', selected: section === 'favorites', detail: r.devices.filter((d) => d.fav).length || null, onClick: () => putDesks({ section: 'favorites' }) }),
      h(SidebarItem, { label: tr('remotedesk.devices'), icon: h(View, { style: { width: 18, alignItems: 'center' } }, h(Monitor, { size: 15, color: section === 'devices' && t.selection !== 'indicator' ? '#fff' : t.accent })),
        selected: section === 'devices', detail: r.devices.length || null, onClick: () => putDesks({ section: 'devices' }) })) : null,
    h(View, { style: { flex: 1, minWidth: 0 } },
      h(Toolbar, null,
        h(ToolbarButton, { icon: '▤', title: tr('remotedesk.toggleSidebar'), active: side, onClick: () => putDesks({ sidebar: !side }) }),
        h(ToolbarSpacer),
        h(MenuButton, { icon: '+', title: tr('remotedesk.add'), items: addItems, minWidth: 300 }),
        h(ToolbarGroup, null,
          h(ToolbarButton, { small: true, icon: '▦', title: tr('remotedesk.viewGrid'), active: view === 'grid', onClick: () => putDesks({ view: 'grid' }) }),
          h(ToolbarButton, { small: true, icon: '≡', title: tr('remotedesk.viewList'), active: view === 'list', onClick: () => putDesks({ view: 'list' }) })),
        h(MenuButton, { icon: '⇅', title: tr('remotedesk.sort'), items: [
          { label: tr('remotedesk.sortName'), checked: sort === 'name', onClick: () => putDesks({ sort: 'name' }) },
          { label: tr('remotedesk.sortLast'), checked: sort === 'last', onClick: () => putDesks({ sort: 'last' }) }, '-',
          { label: tr('remotedesk.showGroups'), checked: grouped, onClick: () => putDesks({ sections: !grouped }) }] }),
        h(MenuButton, { icon: kind === 'all' ? '▽' : '▼', title: tr('remotedesk.filter'), items: [
          { label: tr('remotedesk.filterAll'), checked: kind === 'all', onClick: () => putDesks({ kind: 'all' }) },
          { label: tr('remotedesk.filterRdp'), checked: kind === 'rdp', onClick: () => putDesks({ kind: 'rdp' }) },
          { label: tr('remotedesk.filterVnc'), checked: kind === 'vnc', onClick: () => putDesks({ kind: 'vnc' }) }, '-',
          { label: tr('remotedesk.favorites'), checked: section === 'favorites', onClick: () => putDesks({ section: section === 'favorites' ? 'devices' : 'favorites' }) }] }),
        h(SearchField, { value: query, onChange: setQuery, placeholder: tr('remotedesk.search'), width: 180 })),
      empty || h(ScrollView, { style: { flex: 1 }, resetKey: section, contentStyle: { padding: 20, gap: 22 } },
        ...sections.map(([g, list]) => {
          const key = g || '-';
          const title = section === 'favorites' ? tr('remotedesk.favorites') : g ? g : tr('remotedesk.savedDevices');
          return h(Section, { key, title, open: !collapsed[key], onToggle: () => putDesks({ collapsed: { ...collapsed, [key]: !collapsed[key] } }) },
            list.length ? body(list) : h(Note, null, tr('remotedesk.groupEmpty')));
        }))),
    note ? h(View, { onClick: () => setNote(''), style: { position: 'absolute', left: '30%', right: '30%', bottom: 18, minHeight: 34, paddingHorizontal: 14, borderRadius: 17, justifyContent: 'center', alignItems: 'center',
      backgroundColor: popupBg(t), border: `1px solid ${t.separator}`, boxShadow: '0 6px 18px rgba(0,0,0,0.3)', zIndex: 15 } }, h(Text, { style: { fontSize: 12, color: t.text } }, note)) : null,
    picker ? h(FilePicker, { modal: true, mode: picker.mode, title: tr(picker.mode === 'save' ? 'remotedesk.export' : 'remotedesk.import'), filter: picker.mode === 'open' ? /\.rdp$/i : null,
      name: picker.mode === 'save' ? fileNameOf(picker.d) : '', onClose: () => setPicker(null),
      onPick: (p) => { const path = [].concat(p)[0]; if (picker.mode === 'save') exportFile(path, picker.d); else importFile(path); } }) : null,
    ask ? h(ConfirmDialog, { title: tr('remotedesk.removeTitle', { name: titleOf(ask) }), detail: tr('remotedesk.removeDetail'), okLabel: tr('remotedesk.remove'), cancelLabel: tr('remotedesk.cancel'),
      destructive: true, onOk: () => remove(ask), onCancel: () => setAsk(null) }) : null,
    sheet ? h(DeviceSheet, { start: sheet.start, onCancel: () => setSheet(null), onSaved: (d) => { setSheet(null); setSel(d.id); } }) : null);
}

// ---- a session window ----
const PICTURE_AT = [3000, 12000];          // the picture of the computer: once the first frames settled, again after a sign-in
const RETRIES = 3;

function SessionWindow({ winId, devId, focused }) {
  const t = useTheme();
  const [s] = useSettings();
  const r = desksOf(s);
  const d = r.devices.find((x) => x.id === devId);
  const cred = d ? r.creds.find((c) => c.id === d.cred) : null;
  const root = useRef(null);
  // going to full screen moves the app in the tree and remounts it: the session belongs to the window
  const [id, setId] = useWindowState('remote.id', 0);
  const [st, setSt] = useWindowState('remote.state', null);
  const [entered, setEntered] = useWindowState('remote.entered', false);
  const [error, setError] = useWindowState('remote.error', '');
  const [prompt, setPrompt] = useWindowState('remote.prompt', null);      // { note } : asks for what is missing
  const [retry, setRetry] = useWindowState('remote.retry', 0);
  // "Save to Passwords?" once the computer accepted a password typed in the sign-in: kept in memory only, until answered
  const [saveOffer, setSaveOffer] = useWindowState('remote.saveOffer', null);
  const [editing, setEditing] = useState(false);
  const secret = useRef(null);              // the password of this session, in memory while it lasts (a reconnection)
  const started = useRef(false);
  const name = d ? titleOf(d) : '';
  useEffect(() => { if (name) wm.setTitle(winId, name); }, [name]);
  const live = st?.state === 'connected' && !!id;
  const full = useStore(windows, (ws) => !!ws.find((w) => w.id === winId)?.fullscreen);
  useEffect(() => {
    wm.setMenus(winId, [
      { label: tr('remotedesk.menuFile'), items: [
        { label: tr('remotedesk.newWindow'), onClick: () => wm.open('remote-desk', { new: true }) },
        { label: tr('remotedesk.settingsMenu'), onClick: openPrefs },
      ] },
      { label: tr('remotedesk.menuConnections'), items: [
        { label: tr('remotedesk.disconnect'), disabled: !id, onClick: () => close() },
        { label: tr('remotedesk.sendCad'), disabled: !live || d?.proto !== 'rdp', onClick: () => sendCad(id) },
        { label: tr(full ? 'remotedesk.exitFullscreen' : 'remotedesk.fullscreen'), disabled: !live, onClick: () => wm.setFullscreen(winId, !full) },
        { label: tr('remotedesk.refreshPicture'), disabled: !live, onClick: () => { if (lord.rdesk.snapshot(id, d.id)) putDevice(d.id, { shot: Date.now() }); } },
        '-',
        { label: tr('remotedesk.editDevice'), disabled: !d, onClick: () => setEditing(true) },
      ] },
    ]);
  }, [winId, id, live, full, d?.proto]);
  useEffect(() => () => { if (id && !windows.get().some((w) => w.id === winId)) lord.rdesk.close(id); }, [id]);
  const stop = () => { if (id) lord.rdesk.close(id); setId(0); setSt(null); setEntered(false); wm.setFullscreen(winId, false); };
  const close = () => { stop(); secret.current = null; setSaveOffer(null); wm.close(winId); };
  const start = (sec) => {
    setError('');
    setPrompt(null);
    let [w, ht] = resSize(d.res);
    if (!w || d.proto === 'vnc') {
      const l = root.current ? lord.getLayout(root.current.id) : [0, 0, 1280, 800];
      w = Math.round(l[2]) & ~1; ht = Math.round(l[3] - 36) & ~1;
    }
    const n = lord.rdesk.open(d.proto, d.host, Number(d.port) || 0, sec.user || '', sec.domain || '', sec.password || '', Math.max(w, 640), Math.max(ht, 480),
      (d.folders || []).map((x) => [x.name, x.path, !!x.ro]), { admin: !!d.admin, scale: d.scale || 100, sound: SOUNDS.includes(d.sound) ? d.sound : 'here', depth: DEPTHS.includes(d.depth) ? d.depth : 32 });
    if (!n) { setError(tr('remotedesk.err.failed')); return; }
    secret.current = sec;
    setId(n);
  };
  const begin = async () => {
    if (!d) return;
    const sec = await secretOf(d, cred);
    if (sec) start(sec);
    else if (d.proto === 'vnc' && !cred) start({ user: '', domain: '', password: '' });
    else setPrompt({ note: '' });
  };
  useEffect(() => { if (!started.current && !id && !entered && !error && !prompt) { started.current = true; begin(); } }, []);
  useEffect(() => {
    if (!id) return undefined;
    const timers = [];
    const timer = setInterval(() => {
      const p = lord.rdesk.poll(id);
      if (!p) return;
      const trusted = desksOf(settings.get()).certs || {};
      if (p.cert && certCheck(trusted, keyOf(d), p.cert).kind === 'known') lord.rdesk.cert(id, 1);
      else setSt(p);
      if (p.state === 'connected') {
        setEntered((was) => {
          if (was) return was;
          setRetry(0);
          putDevice(d.id, { last: Date.now() });
          const sec = secret.current;
          if (sec?.saveTo && sec.password) { rememberCred(sec.saveTo, sec.password); secret.current = { ...sec, saveTo: null }; }
          if (sec?.offer) { setSaveOffer(sec.offer); secret.current = { ...secret.current, offer: null }; }
          if (d.full) wm.setFullscreen(winId, true);
          if (desksOf(settings.get()).pictures !== false) {
            for (const ms of PICTURE_AT) timers.push(setTimeout(() => { if (lord.rdesk.snapshot(id, d.id)) putDevice(d.id, { shot: Date.now() }); }, ms));
          }
          return true;
        });
      }
      if (['denied', 'failed', 'ended', 'unreachable', 'notfound', 'dropped'].includes(p.state)) {
        clearInterval(timer);
        stop();
        if (p.state === 'ended') { close(); return; }
        if (p.state === 'dropped' && d.reconnect && retry < RETRIES && secret.current) {
          setRetry(retry + 1);
          const sec = secret.current;
          timers.push(setTimeout(() => start(sec), 2000));
          return;
        }
        if (p.state === 'denied') { setPrompt({ note: tr('remotedesk.err.denied') }); return; }
        setError(tr(`remotedesk.err.${p.state}`, { host: d.host }));
      }
    }, 33);
    return () => { clearInterval(timer); timers.forEach(clearTimeout); };
  }, [id]);
  const acceptCert = (ok) => {
    if (ok) putDesks((x) => ({ certs: { ...(x.certs || {}), [keyOf(d)]: st.cert } }));
    lord.rdesk.cert(id, ok ? 1 : 0);
    setSt({ ...st, cert: '' });
  };
  if (!d) return h(EmptyState, { mascot: 'confused', title: tr('remotedesk.err.gone'), action: h(Button, { label: tr('remotedesk.close'), onClick: close }) });
  const offerBar = saveOffer ? h(View, { style: { position: 'absolute', right: 12, bottom: 12, width: 420, maxWidth: '90%', padding: 12, borderRadius: 10,
    backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 8px 24px rgba(0,0,0,0.45)', zIndex: 30 } },
  h(SaveLoginOffer, { login: saveOffer, hosts: [d.host], onDone: () => setSaveOffer(null) })) : null;
  if (st?.state === 'connected' && st.media && id) return h(Screen, { id, st, focused, onEnd: close, name, winId, folders: d.folders || [], swap: !!d.swap, extra: offerBar });
  const trusted = r.certs || {};
  const cert = st?.cert ? certCheck(trusted, keyOf(d), st.cert) : null;
  return h(View, { ref: root, style: { flex: 1, backgroundColor: t.windowBg } },
    h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, opacity: 0.35, background: `linear-gradient(150deg, ${t.accent}, ${t.mode === 'dark' ? '#17171c' : '#3a3f58'})` } }),
    error ? h(Sheet, { width: 440 },
      h(Title, null, name),
      h(Text, { style: { fontSize: 13, color: t.text, lineHeight: 1.4, textAlign: 'start' } }, error),
      h(Buttons, null,
        h(Button, { label: tr('remotedesk.close'), onClick: close }),
        h(Button, { label: tr('remotedesk.editDevice'), onClick: () => setEditing(true) }),
        h(Button, { primary: true, label: tr('remotedesk.tryAgain'), onClick: () => { setError(''); begin(); } }))) : null,
    prompt && !error ? h(SignIn, { d, cred, note: prompt.note, onCancel: close, onGo: start }) : null,
    id && !error && !prompt ? h(Sheet, { width: 440 },
      h(Title, null, retry ? tr('remotedesk.reconnecting', { n: retry, max: RETRIES }) : tr('remotedesk.connectingTo', { name })),
      cert ? h(Text, { style: { fontSize: 13, color: cert.kind === 'changed' ? RED : t.text, lineHeight: 1.4, textAlign: 'start' } },
        cert.kind === 'changed' ? tr('remotedesk.certChanged', { old: trusted[keyOf(d)], print: st.cert }) : tr('remotedesk.cert', { print: st.cert })) : null,
      h(Buttons, null,
        cert ? h(Button, { label: tr('remotedesk.certNo'), onClick: () => acceptCert(false) }) : h(Button, { label: tr('remotedesk.cancel'), onClick: close }),
        cert ? h(Button, { primary: true, label: tr('remotedesk.certYes'), onClick: () => acceptCert(true) }) : null)) : null,
    editing ? h(DeviceSheet, { start: { ...DEVICE, ...d }, onCancel: () => setEditing(false), onSaved: () => { setEditing(false); setError(''); setTimeout(begin, 0); } }) : null);
}

// What a connection needs and doesn't have: the password of its credential, or (ask when required) a user name too.
function SignIn({ d, cred, note, onCancel, onGo }) {
  const t = useTheme();
  const rdp = d.proto === 'rdp';
  const [user, setUser] = useState(cred ? credText(cred) : '');
  const [password, setPassword] = useState('');
  const [keep, setKeep] = useState(true);
  const filled = useRef('');                // the password as Passwords filled it: no "Save to Passwords?" for that one
  const split = splitUser(user, cred?.domain || '');
  const bad = rdp && (!split.user || !userOk(split.user) || !domainOk(split.domain));
  const go = async () => {
    if (bad) return;
    let c = cred;
    if (keep) {
      if (!c) {
        c = { id: newId('c'), user: rdp ? split.user : '', domain: rdp ? split.domain : '' };
        putDesks((x) => ({ creds: [...x.creds, c].slice(-MAX_CREDS), devices: x.devices.map((y) => (y.id === d.id ? { ...y, cred: c.id } : y)) }));
      }
    }
    // the password goes to the keyring once the computer accepted it (SessionWindow), never a wrong one; so does the
    // offer to save it in Passwords, for one the person typed
    const who = rdp ? (c ? { user: c.user, domain: c.domain } : split) : { user: '', domain: '' };
    const offer = typedSecret(password, filled.current) ? { kind: d.proto, host: d.host, port: d.port, user: credText(who), password } : null;
    onGo({ user: who.user, domain: who.domain, password, saveTo: keep ? c : null, offer });
  };
  return h(Sheet, { width: 420 },
    h(Title, null, tr('remotedesk.signIn', { name: titleOf(d) })),
    note ? h(Note, { color: RED }, note) : null,
    rdp ? h(Row, { label: tr('remotedesk.user') }, cred ? h(Text, { style: { fontSize: 13, color: t.text, textAlign: 'start' } }, credText(cred))
      : h(TextInput, { value: user, onChange: setUser, placeholder: tr('remotedesk.userHint'), autoFocus: true, onSubmit: go })) : null,
    h(Row, { label: tr('remotedesk.password') }, h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
      h(View, { style: { flex: 1, minWidth: 0 } }, h(TextInput, { value: password, onChange: setPassword, secure: true, autoFocus: !rdp || !!cred, onSubmit: go })),
      h(AutofillButton, { app: 'remote-desk', hosts: loginHosts(d.host), onFill: ({ username, password: pw }) => {
        if (rdp && !cred && username) setUser(username);
        setPassword(pw || '');
        filled.current = pw || '';
      } }))),
    !rdp ? h(Note, null, tr('remotedesk.vncPwHint')) : null,
    h(Check, { on: keep, onChange: setKeep, label: tr(cred ? 'remotedesk.rememberPw' : 'remotedesk.saveCred') }),
    h(Buttons, null,
      h(Button, { label: tr('remotedesk.cancel'), onClick: onCancel }),
      h(Button, { primary: true, label: tr('remotedesk.connect'), disabled: bad || (rdp && !password), onClick: go })));
}

// ---- settings window ----
function Person({ color }) {
  return h(View, { style: { width: 18, height: 16, alignItems: 'center' } },
    h(View, { style: { width: 7, height: 7, borderRadius: 4, border: `1.5px solid ${color}` } }),
    h(View, { style: { width: 12, height: 6, marginTop: 1.5, borderTopLeftRadius: 6, borderTopRightRadius: 6, border: `1.5px solid ${color}` } }));
}

function PrefsWindow({ winId }) {
  const t = useTheme();
  useEffect(() => { wm.setTitle(winId, tr('remotedesk.prefsTitle')); }, [winId]);
  const [s] = useSettings();
  const r = desksOf(s);
  const [page, setPage] = useWindowState('prefs.page', 'general');
  const [cur, setCur] = useState('');
  const [edit, setEdit] = useState(null);         // { cred } | { cred: null }
  const [group, setGroup] = useState('');
  const [cleared, setCleared] = useState(false);
  const used = (c) => r.devices.filter((d) => d.cred === c.id).length;
  const removeCred = (id) => {
    const c = r.creds.find((x) => x.id === id);
    if (!c) return;
    forgetCred(c);
    putDesks((x) => ({ creds: x.creds.filter((y) => y.id !== id), devices: x.devices.map((d) => (d.cred === id ? { ...d, cred: '' } : d)) }));
    setCur('');
  };
  const groups = r.groups || [];
  const addGroup = () => {
    const g = group.trim().slice(0, 40);
    if (!g || groups.includes(g) || g === groupName('')) return;
    putDesks({ groups: [...groups, g].slice(0, 30) });
    setGroup('');
  };
  const pages = [['general', tr('remotedesk.prefsGeneral'), '⚙'], ['res', tr('remotedesk.prefsRes'), '▭'], ['creds', tr('remotedesk.prefsCreds'), 'person'], ['groups', tr('remotedesk.prefsGroups'), '▤']];
  const [resText, setResText] = useState('');
  const customRes = (r.resolutions || []).filter(resOk);
  const addRes = () => {
    const v = resText.trim().toLowerCase().replace(/\s*[x×]\s*/, 'x');
    if (!resOk(v) || customRes.includes(v) || RESOLUTIONS.some((x) => x[0] === v)) return;
    putDesks({ resolutions: [...customRes, v].slice(0, 20) });
    setResText('');
  };
  const box = { borderRadius: t.groupRadius, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.02)' };
  const listRow = (key, on, onClick, children) => h(View, { key, onClick, style: { minHeight: 40, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, backgroundColor: on ? t.accent : 'transparent' } }, children(on));
  const plusMinus = (onPlus, onMinus, canMinus) => h(View, { style: { flexDirection: 'row', gap: 2, padding: 4, borderTop: `1px solid ${t.separator}` } },
    h(ToolbarButton, { small: true, icon: '+', title: tr('remotedesk.add'), onClick: onPlus }),
    h(ToolbarButton, { small: true, icon: '−', title: tr('remotedesk.remove'), disabled: !canMinus, onClick: onMinus }));
  return h(View, { style: { flex: 1, flexDirection: 'row', backgroundColor: t.windowBg } },
    h(View, { style: { width: 180, backgroundColor: t.sidebarBg, paddingHorizontal: 10, paddingTop: 12, gap: 2 } },
      h(Hairline, { edge: 'end' }),
      ...pages.map(([k, label, icon]) => h(SidebarItem, { key: k, label, selected: page === k, onClick: () => setPage(k),
        icon: icon === 'person' ? h(Person, { color: page === k && t.selection !== 'indicator' ? '#fff' : t.accent }) : icon }))),
    h(ScrollView, { style: { flex: 1 }, resetKey: page, contentStyle: { padding: 20, gap: 12 } },
      h(Text, { style: { fontSize: 17, fontWeight: 700, color: t.text, textAlign: 'start' } }, pages.find((p) => p[0] === page)[1]),
      page === 'general' ? [
        h(Check, { key: 'p', on: r.pictures !== false, onChange: (v) => putDesks({ pictures: v }), label: tr('remotedesk.pictures') }),
        h(Note, { key: 'ph' }, tr('remotedesk.picturesHint')),
        h(View, { key: 'pd', style: { flexDirection: 'row', alignItems: 'center', gap: 10 } },
          h(Button, { label: tr('remotedesk.picturesDelete'), disabled: !r.devices.some((d) => d.shot), onClick: () => {
            r.devices.forEach((d) => { if (d.shot) lord.rdesk?.forgetSnapshot?.(d.id); });
            putDesks((x) => ({ devices: x.devices.map((d) => ({ ...d, shot: 0 })) }));
            setCleared(true);
          } }),
          cleared ? h(Note, null, tr('remotedesk.picturesDeleted')) : null),
        h(View, { key: 's', style: { height: 1, backgroundColor: t.separator, marginVertical: 4 } }),
        h(Check, { key: 'rc', on: r.reconnectDefault !== false, onChange: (v) => putDesks({ reconnectDefault: v }), label: tr('remotedesk.reconnectDefault') }),
        h(View, { key: 's2', style: { height: 1, backgroundColor: t.separator, marginVertical: 4 } }),
        h(Note, { key: 'c' }, tr('remotedesk.certsCount', { count: Object.keys(r.certs || {}).length })),
        h(View, { key: 'cb', style: { flexDirection: 'row' } }, h(Button, { label: tr('remotedesk.certsForget'), disabled: !Object.keys(r.certs || {}).length, onClick: () => putDesks({ certs: {} }) }))] : null,
      page === 'res' ? [
        h(Note, { key: 'i' }, tr('remotedesk.resIntro')),
        h(View, { key: 'l', style: { ...box, overflow: 'hidden' } },
          ...RESOLUTIONS.filter(([k]) => k !== 'auto').map(([k]) => listRow(k, false, null, () => [h(Text, { key: 'n', style: { flex: 1, fontSize: 13, color: t.textDim, textAlign: 'start' } }, k.replace('x', ' × '))])),
          ...customRes.map((k) => listRow(k, cur === `r:${k}`, () => setCur(`r:${k}`), (on) => [h(Text, { key: 'n', style: { flex: 1, fontSize: 13, color: on ? '#fff' : t.text, textAlign: 'start' } }, k.replace('x', ' × '))])),
          plusMinus(addRes, () => { putDesks({ resolutions: customRes.filter((x) => `r:${x}` !== cur) }); setCur(''); }, cur.startsWith('r:'))),
        h(View, { key: 'n', style: { flexDirection: 'row', gap: 8 } },
          h(View, { style: { flex: 1 } }, h(TextInput, { value: resText, onChange: setResText, placeholder: tr('remotedesk.resNew'), onSubmit: addRes })),
          h(Button, { label: tr('remotedesk.groupAdd'), disabled: !resOk(resText.trim().toLowerCase().replace(/\s*[x×]\s*/, 'x')), onClick: addRes }))] : null,
      page === 'creds' ? [
        h(Note, { key: 'i' }, tr('remotedesk.credsIntro')),
        h(View, { key: 'l', style: { ...box, overflow: 'hidden' } },
          r.creds.length ? h(View, null, ...r.creds.map((c) => listRow(c.id, cur === c.id, () => setCur(c.id), (on) => [
            h(View, { key: 'a', style: { width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? 'rgba(255,255,255,0.25)' : t.control } },
              h(Text, { style: { fontSize: 13, fontWeight: 700, color: on ? '#fff' : t.text } }, (c.user || '?').slice(0, 1).toUpperCase())),
            h(View, { key: 'b', style: { flex: 1, minWidth: 0 } },
              h(Text, { style: { fontSize: 13, fontWeight: 600, color: on ? '#fff' : t.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } }, credName(c)),
              h(Text, { style: { fontSize: 11, color: on ? 'rgba(255,255,255,0.8)' : t.textDim, textAlign: 'start' } }, `${c.label ? `${credText(c)} · ` : ''}${tr('remotedesk.credUsedBy', { count: used(c) })}`)),
            h(ToolbarButton, { key: 'c', small: true, icon: 'ⓘ', color: on ? '#fff' : t.accent, title: tr('remotedesk.credEdit'), onClick: () => setEdit({ cred: c }) })])))
            : h(View, { style: { padding: 16 } }, h(Note, null, tr('remotedesk.credsEmpty'))),
          plusMinus(() => setEdit({ cred: null }), () => removeCred(cur), !!cur))] : null,
      page === 'groups' ? [
        h(Note, { key: 'i' }, tr('remotedesk.groupsIntro')),
        h(View, { key: 'l', style: { ...box, overflow: 'hidden' } },
          listRow('-', false, null, () => [h(Text, { key: 'n', style: { flex: 1, fontSize: 13, color: t.textDim, textAlign: 'start' } }, groupName('')),
            h(Text, { key: 'c', style: { fontSize: 11, color: t.textDim } }, tr('remotedesk.groupCount', { count: r.devices.filter((d) => !d.group).length }))]),
          ...groups.map((g) => listRow(g, cur === `g:${g}`, () => setCur(`g:${g}`), (on) => [
            h(Text, { key: 'n', style: { flex: 1, fontSize: 13, color: on ? '#fff' : t.text, textAlign: 'start' } }, g),
            h(Text, { key: 'c', style: { fontSize: 11, color: on ? 'rgba(255,255,255,0.8)' : t.textDim } }, tr('remotedesk.groupCount', { count: r.devices.filter((d) => d.group === g).length }))])),
          plusMinus(addGroup, () => {
            const g = cur.slice(2);
            putDesks((x) => ({ groups: (x.groups || []).filter((y) => y !== g), devices: x.devices.map((d) => (d.group === g ? { ...d, group: '' } : d)) }));
            setCur('');
          }, cur.startsWith('g:'))),
        h(View, { key: 'n', style: { flexDirection: 'row', gap: 8 } },
          h(View, { style: { flex: 1 } }, h(TextInput, { value: group, onChange: setGroup, placeholder: tr('remotedesk.groupNew'), onSubmit: addGroup })),
          h(Button, { label: tr('remotedesk.groupAdd'), disabled: !group.trim(), onClick: addGroup }))] : null),
    edit ? h(CredentialSheet, { cred: edit.cred, hosts: edit.cred ? r.devices.filter((d) => d.cred === edit.cred.id).map((d) => d.host) : [],
      onCancel: () => setEdit(null), onSave: (c) => { setEdit(null); setCur(c.id); } }) : null);
}

const openPrefs = () => wm.open('remote-desk', { instance: 'prefs', args: { prefs: true } });

export function RemoteDeskApp() {
  const { id: winId, args, focused } = useAppWindow();
  // settings from before 0.8.8.4 are rewritten once (sdk/remotedeskmodel.js); reading them migrates on the fly anyway
  useEffect(() => { const m = migrate(settings.get().remoteDesks); if (m.changed) settings.set({ remoteDesks: m.value }); }, []);
  useEffect(() => {
    if (!winId || !args?.prefs) return;       // the list and the sessions set their own menus
    wm.setMenus(winId, [{ label: tr('remotedesk.menuFile'), items: [
      { label: tr('remotedesk.newWindow'), hint: '❖N', onClick: () => wm.open('remote-desk', { new: true }) },
      { label: tr('remotedesk.settingsMenu'), hint: '❖,', onClick: openPrefs },
    ] }]);
  }, [winId]);
  useKeyboard((e) => {
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === ',' && !args?.connect) openPrefs();
  });
  if (args?.prefs) return h(PrefsWindow, { winId });
  if (args?.connect) return h(SessionWindow, { winId, devId: args.connect, focused });
  return h(ListWindow, { winId, args });
}
