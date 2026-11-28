// capturemodel.js — the capture indicator's plain logic (sdk/capturestatus.jsx; server/tests/capture_model_test.mjs).
//
// What is captured right now, from what LordOS itself knows, one row per app:
//   share    the screen-sharing API's holder (sdk/share.js: { app, name, input, token } | null)
//   casts    the ScreenCast portal's sessions (sdk/screencast.jsx castRuns: [{ id, who, seen, name, screens, windows }];
//            seen: the app lord-server told from the kernel when `who` is '', only to show it, server/src/sec/appid.h)
//   shots    a screenshot or a color an app just took (shotTaken: [{ key, name, color }]), a few seconds
//   devices  the camera and microphone LordOS hands out itself: native apps (lord.media) and isolated .lpk apps (the
//            broker): [{ app, kind: 'camera'|'microphone', media?, detail? }]; detail: what of the app (a machine's name)
//   linux    what Linux and Flatpak apps capture through PipeWire, as lord-server's PipeWire guard sees it
//            (server/src/sec/pwguard.c, hw event "capture-use"): [{ app, name, sys?, exe?, mic, camera, screen }], app ''
//            with sys 'sound' the sound system's own processes, with exe a program known only by its executable, with
//            neither an unknown program
//   studio   the Linux apps reading LordOS Studio's camera device (/dev/video50; server/src/media/mstudio.c, hw event
//            "studio-use", sdk/studiomodel.js studioUseOf): { apps: [id], camera, effects } | null; each is a camera row
//            whose via says the real camera behind it and whether effects are on
//   cameras  what opens a camera itself, not through LordOS or PipeWire: a web app's web process (WebKit opens the
//            video4linux device in its sandbox), a Linux app using V4L2; lord-server finds them from the kernel
//            (server/src/hw/camuse.c, hw event "camera-use", cameraUseOf): [id], '' a program LordOS can't name. Shown
//            only: there is no Turn off for them
//
//   cameraUseOf(data)    → [id] from the "camera-use" event ({ apps: [id] }): strings only, each once
//   captureRows(src, { name, enforceable })  → [{ key, app, name, screen, camera, mic, castIds, share, control, own, linux, media, via, sys, details }]
//     screen: null | 'screen' | 'window' | 'both' | 'shot' | 'color'; own: the camera/microphone is LordOS's (a native or
//     .lpk app: Turn off = its permission); linux: a Linux app's (Turn off = its firewall restriction), and only an app
//     the firewall can name; media: a native app's camera source, for the preview
//   captureGlyph(rows)   → null | 'screen' | 'mic' | 'camera' | 'screen-mic' | 'camera-mic' | 'screen-camera'
//   captureSummary(rows) → { screen, camera, mic } (for the item's accessible label)
//   rowStops(row)        → { screen: [{ kind: 'share' } | { kind: 'cast', id }],
//                            off: null | { own, perms: ['camera'|'microphone'], app, flags: ['no-camera'|'no-mic'|'no-screen'] } }
//     screen: what Stop sharing ends; off: what Turn off for this app turns off. own: the id LordOS's permissions know
//     the app by; app: the id the firewall knows it by

const SCREEN_RANK = { screen: 3, both: 4, window: 2, shot: 1, color: 1 };

// the wider of two "what of the screen" values (a share of the whole screen and a window's → both)
function widerScreen(a, b) {
  if (!a) return b || null;
  if (!b) return a;
  if ((a === 'screen' && b === 'window') || (a === 'window' && b === 'screen')) return 'both';
  return SCREEN_RANK[b] > SCREEN_RANK[a] ? b : a;
}

// One key per app, however each source spells it: "lpk:<id>" and the bare id of an .lpk app are one app
const keyOf = (app) => (typeof app === 'string' && app.startsWith('lpk:') ? app.slice(4) : String(app || ''));

export function cameraUseOf(data) {
  const d = data && typeof data === 'object' ? data : {};
  return [...new Set((Array.isArray(d.apps) ? d.apps : []).filter((a) => typeof a === 'string' && a.length < 200))];
}

export function captureRows({ share = null, casts = [], shots = [], devices = [], linux = [], studio = null, cameras = [] } = {}, { name = (a) => a, enforceable = () => false } = {}) {
  const rows = new Map();
  const row = (app, label, unknownKey) => {
    const key = app ? keyOf(app) : unknownKey;
    if (!rows.has(key)) {
      rows.set(key, { key, app: app || '', name: label || '', screen: null, camera: false, mic: false, castIds: [], share: false, control: false, ownApp: '', own: { camera: false, mic: false }, linux: { camera: false, mic: false, screen: false }, media: null, via: null, sys: '', details: [] });
    }
    const r = rows.get(key);
    if (!r.name && label) r.name = label;
    return r;
  };
  if (share && share.app) {
    const r = row(share.app, share.name || name(share.app));
    r.share = true;
    if (share.input) r.control = true;      // the people it shares with may use the mouse, the keyboard and the clipboard
    r.screen = widerScreen(r.screen, 'screen');
  }
  for (const c of Array.isArray(casts) ? casts : []) {
    if (!c || typeof c.id !== 'number') continue;
    // an unknown program's session is its own row: two of them may be two programs. seen: the app lord-server told
    // from the kernel without a verified scope (Chrome's browser process leaves its own), so its share and its
    // microphone are one row
    const app = c.who || (typeof c.seen === 'string' ? c.seen : '');
    const r = row(app, c.name || (app ? name(app) : ''), `cast#${c.id}`);
    r.castIds.push(c.id);
    const what = c.windows > 0 && c.screens > 0 ? 'both' : c.windows > 0 ? 'window' : 'screen';
    r.screen = widerScreen(r.screen, what);
  }
  for (const s of Array.isArray(shots) ? shots : []) {
    if (!s || !s.key) continue;
    const r = row('', s.name || '', `shot#${s.key}`);
    r.screen = widerScreen(r.screen, s.color ? 'color' : 'shot');
  }
  for (const d of Array.isArray(devices) ? devices : []) {
    if (!d || !d.app || (d.kind !== 'camera' && d.kind !== 'microphone')) continue;
    const r = row(d.app, d.name || name(d.app));
    r.ownApp = d.app;
    // what of the app captures: Machines' machine ("Machines — Windows 11 Pro")
    if (typeof d.detail === 'string' && d.detail && !r.details.includes(d.detail)) r.details.push(d.detail);
    if (d.kind === 'camera') { r.camera = true; r.own.camera = true; if (d.media && !r.media) r.media = d.media; }
    else { r.mic = true; r.own.mic = true; }
  }
  for (const u of Array.isArray(linux) ? linux : []) {
    if (!u || typeof u.app !== 'string') continue;
    // with no app: the sound system's own processes are one row, each program lord-server knows only by its executable
    // another, and every other unknown program's capture one more (lord-server can't tell them apart either)
    const sys = !u.app && typeof u.sys === 'string' ? u.sys : '', exe = !u.app && !sys && typeof u.exe === 'string' ? u.exe : '';
    const r = row(u.app, u.app ? name(u.app) : (u.name || ''), sys ? `sys:${sys}` : exe ? `exe:${exe}` : 'unknown');
    if (sys) r.sys = sys;
    const named = !!u.app && enforceable(u.app);
    if (u.mic) { r.mic = true; if (named) r.linux.mic = true; }
    if (u.camera) { r.camera = true; if (named) r.linux.camera = true; }
    // a share the portal already shows (its Stop ends it) isn't said twice; one it doesn't know is the PipeWire guard's
    if (u.screen && !r.castIds.length && !r.share) { r.screen = widerScreen(r.screen, 'screen'); if (named) r.linux.screen = true; }
  }
  for (const a of studio && Array.isArray(studio.apps) ? studio.apps : []) {
    if (typeof a !== 'string') continue;
    // as above: every program LordOS can't name is one row
    const r = row(a, a ? name(a) : '', 'unknown');
    r.camera = true;
    r.via = { camera: typeof studio.camera === 'string' ? studio.camera : '', effects: studio.effects === true };
    if (a && enforceable(a)) r.linux.camera = true;
  }
  for (const a of Array.isArray(cameras) ? cameras : []) {
    if (typeof a !== 'string') continue;
    const r = row(a, a ? name(a) : '', 'unknown');
    r.camera = true;
  }
  // the screen first, then the camera, then the microphone; by name within each
  const weight = (r) => (r.screen && r.screen !== 'shot' && r.screen !== 'color' ? 0 : r.camera ? 1 : r.mic ? 2 : 3);
  return [...rows.values()].sort((a, b) => weight(a) - weight(b) || a.name.localeCompare(b.name));
}

export function captureSummary(rows) {
  const list = Array.isArray(rows) ? rows : [];
  return { screen: list.some((r) => !!r.screen), camera: list.some((r) => r.camera), mic: list.some((r) => r.mic) };
}

export function captureGlyph(rows) {
  const { screen, camera, mic } = captureSummary(rows);
  if (screen && camera) return 'screen-camera';
  if (screen && mic) return 'screen-mic';
  if (screen) return 'screen';
  if (camera && mic) return 'camera-mic';
  if (camera) return 'camera';
  if (mic) return 'mic';
  return null;
}

// What each of a row's buttons does: Stop for the screen (the share's own stop path), Turn off for the camera and the
// microphone (the app's permission for LordOS's own; the firewall's restriction for a Linux app's)
export function rowStops(row) {
  if (!row) return { screen: [], off: null };
  const screen = [];
  if (row.share) screen.push({ kind: 'share' });
  for (const id of row.castIds || []) screen.push({ kind: 'cast', id });
  const perms = [...(row.own?.camera ? ['camera'] : []), ...(row.own?.mic ? ['microphone'] : [])];
  const flags = [...(row.linux?.camera ? ['no-camera'] : []), ...(row.linux?.mic ? ['no-mic'] : []), ...(row.linux?.screen ? ['no-screen'] : [])];
  return { screen, off: perms.length || flags.length ? { own: row.ownApp || row.app, perms, app: row.app, flags } : null };
}
