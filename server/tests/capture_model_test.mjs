// capture_model_test.mjs — the capture indicator by the clock (sdk/capturemodel.js): which glyph the pill shows for what
// is captured, the rows of its panel (one per app, whatever source said so) and what each row's buttons do.
import { captureRows, captureGlyph, captureSummary, rowStops } from '../../sdk/capturemodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const names = { 'desktop:com.obsproject.Studio': 'OBS Studio', 'flatpak:us.zoom.Zoom': 'Zoom', camera: 'Camera', 'lpk:meet': 'Meet', meet: 'Meet', recorder: 'Voice Memos' };
const opts = { name: (id) => names[id] || id, enforceable: (id) => /^(desktop|flatpak|lpk|lordos):/.test(id) };
const rows = (src) => captureRows(src, opts);
const glyph = (src) => captureGlyph(rows(src));

// ---- the glyph ----
eq('nothing: no pill', glyph({}), null);
eq('screen only', glyph({ casts: [{ id: 1, who: 'desktop:com.obsproject.Studio', screens: 1, windows: 0 }] }), 'screen');
eq('a window is the screen glyph too', glyph({ casts: [{ id: 1, who: 'desktop:com.obsproject.Studio', screens: 0, windows: 1 }] }), 'screen');
eq('the sharing API is the screen', glyph({ share: { app: 'lpk:meet', name: 'Meet', token: 3 } }), 'screen');
eq('a screenshot just taken is the screen', glyph({ shots: [{ key: 's1', name: 'GIMP', color: false }] }), 'screen');
eq('microphone only', glyph({ devices: [{ app: 'recorder', kind: 'microphone' }] }), 'mic');
eq('camera only', glyph({ devices: [{ app: 'camera', kind: 'camera' }] }), 'camera');
eq('camera and microphone', glyph({ devices: [{ app: 'camera', kind: 'camera' }, { app: 'recorder', kind: 'microphone' }] }), 'camera-mic');
eq('screen and microphone', glyph({ casts: [{ id: 1, who: 'desktop:com.obsproject.Studio', screens: 1, windows: 0 }],
  linux: [{ app: 'desktop:com.obsproject.Studio', mic: 1, camera: 0, screen: 1 }] }), 'screen-mic');
eq('screen and camera', glyph({ share: { app: 'lpk:meet', token: 1 }, devices: [{ app: 'camera', kind: 'camera' }] }), 'screen-camera');
eq('screen, camera and microphone: display with the camera', glyph({ share: { app: 'lpk:meet', token: 1 },
  linux: [{ app: 'flatpak:us.zoom.Zoom', mic: 1, camera: 1, screen: 0 }] }), 'screen-camera');
eq('a Linux app\'s microphone', glyph({ linux: [{ app: '', mic: 1, camera: 0, screen: 0 }] }), 'mic');
eq('a Linux app\'s camera', glyph({ linux: [{ app: 'flatpak:us.zoom.Zoom', mic: 0, camera: 1, screen: 0 }] }), 'camera');
eq('a Linux app linked to a shared screen', glyph({ linux: [{ app: 'desktop:x', mic: 0, camera: 0, screen: 1 }] }), 'screen');
eq('an entry with nothing in use is no pill', glyph({ linux: [{ app: 'desktop:x', mic: 0, camera: 0, screen: 0 }] }), null);
eq('summary', captureSummary(rows({ devices: [{ app: 'camera', kind: 'camera' }] })), { screen: false, camera: true, mic: false });

// ---- the rows ----
{
  const r = rows({
    casts: [{ id: 7, who: 'desktop:com.obsproject.Studio', name: 'OBS Studio', screens: 1, windows: 1 }],
    linux: [{ app: 'desktop:com.obsproject.Studio', mic: 1, camera: 0, screen: 1 }],
  });
  eq('one row per app across sources', r.length, 1);
  eq('its name', r[0].name, 'OBS Studio');
  eq('a screen and a window: both', r[0].screen, 'both');
  eq('its microphone', r[0].mic, true);
  eq('Stop sharing ends its portal session', rowStops(r[0]).screen, [{ kind: 'cast', id: 7 }]);
  eq('Turn off: its microphone restriction, not the screen the portal stops', rowStops(r[0]).off,
    { own: 'desktop:com.obsproject.Studio', perms: [], app: 'desktop:com.obsproject.Studio', flags: ['no-mic'] });
}
{
  const r = rows({ casts: [{ id: 2, who: '', name: 'An unknown program', screens: 1, windows: 0 }, { id: 3, who: '', name: 'An unknown program', screens: 0, windows: 1 }] });
  eq('two unknown programs\' shares stay two rows', r.length, 2);
  eq('each with its Stop', r.map((x) => rowStops(x).screen), [[{ kind: 'cast', id: 2 }], [{ kind: 'cast', id: 3 }]]);
}
{
  const r = rows({ share: { app: 'lpk:meet', name: 'Meet', input: true, token: 9 }, devices: [{ app: 'meet', kind: 'microphone' }, { app: 'meet', kind: 'camera', media: 41 }] });
  eq('an .lpk app\'s two ids are one row', r.length, 1);
  eq('the API\'s share may control', r[0].control, true);
  eq('the camera\'s source for the preview', r[0].media, 41);
  eq('Stop sharing: the API\'s stop', rowStops(r[0]).screen, [{ kind: 'share' }]);
  eq('Turn off: its permissions, by the id the broker knows', rowStops(r[0]).off, { own: 'meet', perms: ['camera', 'microphone'], app: 'lpk:meet', flags: [] });
}
{
  const r = rows({ linux: [{ app: '', mic: 1, camera: 1, screen: 0 }] });
  eq('an unknown program\'s capture: one row, no name', [r.length, r[0].app, r[0].name], [1, '', '']);
  eq('nothing to turn off for an unknown program', rowStops(r[0]), { screen: [], off: null });
}
{
  const r = rows({ linux: [{ app: 'desktop:x', mic: 0, camera: 0, screen: 1 }] });
  eq('a screen the portal doesn\'t know: turned off with no-screen', rowStops(r[0]).off, { own: 'desktop:x', perms: [], app: 'desktop:x', flags: ['no-screen'] });
}
{
  const r = rows({ linux: [{ app: 'system:something', mic: 1, camera: 0, screen: 0 }] });
  eq('an app the firewall can\'t name: shown, nothing to turn off', [r[0].mic, rowStops(r[0]).off], [true, null]);
}
{
  const r = rows({ shots: [{ key: 's1', name: 'GIMP', color: false }, { key: 's2', name: 'Inkscape', color: true }] });
  eq('screenshots and colors: a row each, no Stop', r.map((x) => [x.name, x.screen, rowStops(x).screen.length]), [['GIMP', 'shot'], ['Inkscape', 'color']].map((a) => [...a, 0]));
}
{
  const r = rows({
    devices: [{ app: 'recorder', kind: 'microphone' }, { app: 'camera', kind: 'camera' }],
    casts: [{ id: 1, who: 'desktop:com.obsproject.Studio', name: 'OBS Studio', screens: 1, windows: 0 }],
  });
  eq('order: the screen, the camera, the microphone', r.map((x) => x.name), ['OBS Studio', 'Camera', 'Voice Memos']);
}
eq('junk is ignored', rows({ casts: [null, { id: 'x' }], devices: [{ app: 'a', kind: 'printer' }, null], linux: [{ app: 3 }, null], shots: [{}] }), []);

// ---- what used to be "an unknown program" (2026-11-22): what lord-server tells from the kernel, only to show it ----
// the indicator's own rule for Turn off (sdk/capturestatus.jsx restrictable): an app's firewall restriction, never a
// machine's scope, a LordOS component or LordOS itself
const shown = { ...names, 'desktop:google-chrome': 'Google Chrome', 'desktop:lordos-web-whatsapp': 'WhatsApp', 'lordos:terminal': 'Terminal',
  'desktop:lordos-vm-1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2': 'Machines — Windows 11 Pro (prueba)', machines: 'Machines' };
const real = { name: (id) => shown[id] || id, enforceable: (id) => /^(desktop|flatpak|lpk):/.test(id) && !/^desktop:lordos-vm-/.test(id) };
const live = (src) => captureRows(src, real);
{
  // the maintainer's popover: Chrome sharing the screen (Meet) and recording the microphone; its browser process and
  // audio service had left Chrome's scope, so both were "an unknown program". Now: one row, both buttons
  const r = live({
    casts: [{ id: 4, who: '', seen: 'desktop:google-chrome', exe: '', name: 'Google Chrome', screens: 1, windows: 0 }],
    linux: [{ app: 'desktop:google-chrome', name: 'Google Chrome', mic: 1, camera: 0, screen: 0 }],
  });
  eq('Chrome: one row for its share and its microphone', r.length, 1);
  eq('Chrome: named', [r[0].app, r[0].name, r[0].screen, r[0].mic], ['desktop:google-chrome', 'Google Chrome', 'screen', true]);
  eq('Chrome: Stop sharing ends its session', rowStops(r[0]).screen, [{ kind: 'cast', id: 4 }]);
  eq('Chrome: Turn off is its no-mic', rowStops(r[0]).off, { own: 'desktop:google-chrome', perms: [], app: 'desktop:google-chrome', flags: ['no-mic'] });
}
{
  // a share whose asker lord-server can't name at all: still its own row with its Stop, named by castAppName
  const r = live({ casts: [{ id: 5, who: '', seen: '', exe: 'obs', name: 'An unknown program (obs)', screens: 1, windows: 0 }] });
  eq('unnamed share: its own row, its Stop', [r[0].key, r[0].app, r[0].name, rowStops(r[0]).screen], ['cast#5', '', 'An unknown program (obs)', [{ kind: 'cast', id: 5 }]]);
}
{
  // a LordOS web app's WebKit, a pulse client in its own sandbox: named by its app
  const r = live({ linux: [{ app: 'desktop:lordos-web-whatsapp', name: 'WhatsApp', mic: 1, camera: 1, screen: 0 }] });
  eq('web app: named, turned off by its restrictions', [r[0].name, rowStops(r[0]).off.flags], ['WhatsApp', ['no-camera', 'no-mic']]);
}
{
  // the sound system's own processes, and programs known only by their executable: a row each, nothing to turn off
  const r = live({ linux: [{ app: '', sys: 'sound', name: 'The sound system (PipeWire)', mic: 1 }, { app: '', exe: 'arecord', name: 'An unknown program (arecord)', mic: 1 },
    { app: '', exe: 'ffmpeg', name: 'An unknown program (ffmpeg)', camera: 1 }, { app: '', mic: 1 }] });
  eq('sound and executables: a row each', r.map((x) => x.key).sort(), ['exe:arecord', 'exe:ffmpeg', 'sys:sound', 'unknown']);
  const snd = r.find((x) => x.key === 'sys:sound');
  eq('the sound system: named, marked, nothing to turn off', [snd.name, snd.sys, rowStops(snd).off], ['The sound system (PipeWire)', 'sound', null]);
  eq('an executable: its name', r.find((x) => x.key === 'exe:arecord').name, 'An unknown program (arecord)');
  eq('nothing at all: still an unknown program', r.find((x) => x.key === 'unknown').name, '');
  eq('an app\'s entry ignores sys and exe', live({ linux: [{ app: 'desktop:google-chrome', sys: 'sound', exe: 'chrome', mic: 1 }] }).map((x) => [x.key, x.sys]), [['desktop:google-chrome', '']]);
}
{
  // started from the Terminal (a LordOS component): named, but the firewall's restrictions aren't the Terminal's to turn off
  const r = live({ linux: [{ app: 'lordos:terminal', mic: 1 }] });
  eq('Terminal: named, no Turn off', [r[0].name, rowStops(r[0]).off], ['Terminal', null]);
  const m = live({ linux: [{ app: 'desktop:lordos-vm-1b2b4eb6-77dd-42a8-933c-3cf7eac71aa2', mic: 1 }] });
  eq('a machine\'s QEMU: Machines and the machine, no firewall Turn off', [m[0].name, rowStops(m[0]).off], ['Machines — Windows 11 Pro (prueba)', null]);
}
{
  // Machines' own microphone (lord-server's stream): Machines and the machine, Turn off its permission; two machines,
  // one row with both, and one stopping leaves the other
  const r = live({ devices: [{ app: 'machines', kind: 'microphone', detail: 'Windows 11 Pro (prueba)' }, { app: 'machines', kind: 'microphone', detail: 'Ubuntu 24.04' }] });
  eq('Machines: one row, both machines', [r.length, r[0].name, r[0].details], [1, 'Machines', ['Windows 11 Pro (prueba)', 'Ubuntu 24.04']]);
  eq('Machines: Turn off its microphone permission', rowStops(r[0]).off, { own: 'machines', perms: ['microphone'], app: 'machines', flags: [] });
  eq('no detail: none', live({ devices: [{ app: 'recorder', kind: 'microphone' }] })[0].details, []);
}
eq('no row: no stops', rowStops(null), { screen: [], off: null });

console.log(`capture-model-test: ${ok}/${ok + bad}`);
process.exit(bad ? 1 : 0);
