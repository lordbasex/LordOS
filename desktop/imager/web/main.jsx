// main.jsx — the desktop app's window: the shared imager UI (apps/imager/index.jsx) in a browser view.
import { createRoot } from 'react-dom/client';
import { createElement as h } from 'react';
import Imager from '../../../apps/imager/index.jsx';
import { init, warm, platform, info } from './platform.js';
import { app, run, loadReleases } from '../../../apps/imager/state.js';
import { useTheme } from './sdk.jsx';

function Root() {
  const t = useTheme();
  document.body.style.background = t.windowBg;
  document.body.style.color = t.text;
  document.documentElement.style.colorScheme = t.mode;
  return h('div', { style: { display: 'flex', flex: 1, minWidth: 0 } }, h(Imager));
}

await init();
// state.js read the options before init() loaded them: take the saved ones (language, theme…)
app.set((s) => ({ ...s, options: { ...s.options, ...platform.loadOptions() } }));
await warm();
createRoot(document.getElementById('root')).render(h(Root));

// tests and screenshots (LORDOS_IMAGER_TEST_STATE, see src-tauri/src/main.rs): a starting state, and
// optionally the write itself, only ever to a listed test device
const ts = info().testState;
if (ts && typeof ts === 'object') {
  await loadReleases();
  const s = app.get();
  const patch = { ...(ts.patch || {}) };
  if (ts.cfg) patch.cfg = { ...s.cfg, ...ts.cfg, user: { ...s.cfg.user, ...(ts.cfg.user || {}) }, wifi: { ...s.cfg.wifi, ...(ts.cfg.wifi || {}) } };
  if (ts.secrets) patch.secrets = { ...s.secrets, ...ts.secrets };
  if (ts.drive) patch.drive = platform.drives().find((d) => d.id === ts.drive && d.raw.test_device) || null;
  if (ts.release === 'latest' && s.releases.list[0]) patch.image = { official: true, release: s.releases.list[0], name: s.releases.list[0].image.name, size: s.releases.list[0].image.size, raw: null, tag: s.releases.list[0].tag };
  app.set({ ...app.get(), ...patch });
  if (ts.run && app.get().drive) run();
}
