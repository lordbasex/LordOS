// powerbox.jsx — the open/save file dialog is drawn by axia-server (not the app), as
// a «powerbox»: the isolated app doesn't see the home folder; it only receives the fd
// of the file the user picks here. See docs/PRIVACY.md and server/src/sec/brokersrv.c.
//
// In the theme (compositor) process: axia-server notifies with __lordBrokerPick(reqid, app, save,
// suggest); the dialog is shown and, with the choice, lord.broker.pickResult(reqid,
// path) is called. In the app process: the SDK offers fs.openFile()/fs.saveFile().
import { createElement as h } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { useLocale } from './i18n.js';
import { TextInput, Button, ScrollView } from './components.jsx';
import * as fs from './fs.js';
import { useState, useEffect } from 'react';
import { appName } from './i18n.js';
import { getApp } from './windows.js';

const queue = createStore([]);           // [{ reqid, app, save, suggest }]
let seq = 1;

if (typeof globalThis !== 'undefined') {
  globalThis.__lordBrokerPick = (reqid, app, save, suggest) => {
    queue.set((q) => [...q, { key: seq++, reqid, app, save: !!save, suggest: suggest || '' }]);
  };
}

function finish(item, path) {
  queue.set((q) => q.filter((x) => x.key !== item.key));
  try { lord.broker.pickResult(item.reqid, path || ''); } catch (e) { /* server without broker */ }
}

function Picker({ item }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [dir, setDir] = useState('');            // relative to the home folder
  const [sel, setSel] = useState(null);
  const [name, setName] = useState(item.suggest || 'archivo.txt');
  const { entries, error } = fs.useDirectory(dir || '.');
  const shown = entries.filter((e) => e.dir || !item.save || true);
  const up = () => { setSel(null); setDir(fs.parent(dir)); };
  const open = () => {
    if (item.save) { if (!name) return; finish(item, fs.join(fs.home(), dir, name)); return; }
    if (sel && !sel.dir) finish(item, fs.join(fs.home(), dir, sel.name));
  };
  const app = appName(getApp(item.app)) || item.app;
  const title = item.save ? tr('powerbox.saveTitle', { app }) : tr('powerbox.openTitle', { app });
  return (
    h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'center',
      backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100000 } },
    h(View, { style: { width: 460, height: 380, borderRadius: 14, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
      boxShadow: '0 20px 60px rgba(0,0,0,0.5)', overflow: 'hidden' } },
      h(View, { style: { padding: 12, gap: 8, borderBottom: `1px solid ${t.separator}` } },
        h(Text, { style: { fontSize: 14, fontWeight: 700, color: t.text } }, title),
        h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
          h(Button, { label: tr('powerbox.up'), onClick: up }),
          h(Text, { style: { flex: 1, fontSize: 12, color: t.textDim, whiteSpace: 'nowrap' } }, `~/${dir}`))),
      h(ScrollView, { style: { flex: 1, padding: 6 } },
        error ? h(Text, { style: { fontSize: 12, color: '#ff453a', padding: 8 } }, String(error)) :
        shown.map((e) => h(View, { key: e.name,
          onClick: () => (e.dir ? (setSel(null), setDir(fs.join(dir, e.name))) : (setSel(e), item.save ? setName(e.name) : null)),
          style: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 26, paddingHorizontal: 8, borderRadius: 5,
            backgroundColor: sel && sel.name === e.name ? t.accent : 'transparent' } },
          h(Text, { style: { fontSize: 13 } }, e.dir ? '📁' : '📄'),
          h(Text, { style: { flex: 1, fontSize: 13, color: sel && sel.name === e.name ? '#fff' : t.text, whiteSpace: 'nowrap' } }, e.name)))),
      h(View, { style: { padding: 12, gap: 8, flexDirection: 'row', alignItems: 'center', borderTop: `1px solid ${t.separator}` } },
        item.save ? h(View, { style: { flex: 1 } }, h(TextInput, { value: name, onChange: setName, placeholder: tr('powerbox.name') })) : h(View, { style: { flex: 1 } }),
        h(Button, { label: tr('powerbox.cancel'), onClick: () => finish(item, '') }),
        h(Button, { primary: true, label: item.save ? tr('powerbox.save') : tr('powerbox.open'), onClick: open })))));
}

// Mounted in the shell (LocaleRoot). Shows one dialog at a time.
export function Powerbox() {
  const q = useStore(queue);
  if (!q.length) return null;
  return h(Picker, { item: q[0], key: q[0].key });
}

// ---- app side: fs.openFile() / fs.saveFile() ----
const inApp = () => typeof lord !== 'undefined' && lord.broker && typeof lord.broker.pick === 'function';

export async function openFile() {
  if (!inApp()) throw new Error('the file dialog only exists in an isolated app');
  const r = lord.broker.pick(false, '');          // blocks until the user chooses (system modal)
  if (!r) return null;
  const data = lord.broker.fdRead(r.fd);
  lord.broker.fdClose(r.fd);
  return { name: r.name, arrayBuffer: () => data, text: () => new TextDecoder().decode(data) };
}

export async function saveFile(suggestedName, data) {
  if (!inApp()) throw new Error('the file dialog only exists in an isolated app');
  const r = lord.broker.pick(true, suggestedName || 'archivo.txt');
  if (!r) return false;
  const buf = typeof data === 'string' ? new TextEncoder().encode(data).buffer : data;
  const ok = lord.broker.fdWrite(r.fd, buf);
  lord.broker.fdClose(r.fd);
  return { name: r.name, ok };
}
