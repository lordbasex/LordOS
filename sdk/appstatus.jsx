// appstatus.jsx — third-party items by the clock (docs/GO-LINK.md, section 7). An app runs isolated and has no code in the
// shell: it sends a description through the broker, lord-server keeps only the known fields (sec/statusitem.c), and
// this draws it. The icon is a file of the app's own package, the texts are plain text, and a choice goes back as an id.
// The item leaves with the app, or when the permission is revoked.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image, addListener } from '@lordos/runtime';
import { useStore, createStore } from './store.js';
import { useTheme } from './theme.js';
import { Overlay } from './layers.js';
import { registerStatusItem, statusPanelAt } from './statusitems.js';
import { packageList } from './packages.js';

const specs = createStore({});            // app id → { icon, title, alert, menu }
const off = new Map();                    // app id → unregister

function Menu({ app, spec, th, close }) {
  const pick = (id) => { close(); try { lord.broker.statusClick(app, id); } catch (e) { /* the app left */ } };
  return h(View, { style: { minWidth: 200, maxWidth: 320, padding: 6, borderRadius: 10, backgroundColor: th.menu || th.surface, border: `1px solid ${th.border}`, boxShadow: '0 12px 32px rgba(0,0,0,0.4)' } },
    spec.title ? h(Text, { style: { fontSize: 12, fontWeight: 600, color: th.textDim, padding: 6, textAlign: 'start' } }, spec.title) : null,
    ...spec.menu.map((m, i) => (m.line
      ? h(View, { key: `l${i}`, style: { height: 1, marginVertical: 4, backgroundColor: th.border } })
      : h(View, { key: m.id, onClick: m.off ? undefined : () => pick(m.id), style: { paddingVertical: 5, paddingHorizontal: 8, borderRadius: 6 } },
        h(Text, { style: { fontSize: 13, color: m.off ? th.textDim : th.text, textAlign: 'start' } }, m.label)))));
}

function AppItem({ app, height }) {
  const th = useTheme();
  const all = useStore(specs);
  const pkgs = useStore(packageList);
  const spec = all[app];
  const [open, setOpen] = useState(false);
  const btn = useRef(null), panel = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, hh] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + hh; };
      if (!inside(panel.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  const dir = pkgs.find((p) => p.id === app)?.dir;
  if (!spec || !dir) return null;
  return h(View, { ref: btn, onClick: () => setOpen((o) => !o), style: { height, width: 26, alignItems: 'center', justifyContent: 'center' } },
    h(Image, { src: `${dir}/${spec.icon}`, style: { width: 16, height: 16, objectFit: 'contain' } }),
    spec.alert ? h(View, { style: { position: 'absolute', right: 3, top: 3, width: 7, height: 7, borderRadius: 4, backgroundColor: '#ff453a' } }) : null,
    open ? h(Overlay, { anchor: btn }, h(View, { ref: panel, style: statusPanelAt(btn) }, h(Menu, { app, spec, th, close: () => setOpen(false) }))) : null);
}

export function appStatusEvent(d) {
  if (!d || !d.app) return;
  if (d.off) {
    if (off.has(d.app)) { off.get(d.app)(); off.delete(d.app); }
    specs.set((s) => { const n = { ...s }; delete n[d.app]; return n; });
    return;
  }
  specs.set((s) => ({ ...s, [d.app]: { icon: d.icon, title: d.title, alert: !!d.alert, menu: Array.isArray(d.menu) ? d.menu : [] } }));
  if (!off.has(d.app)) off.set(d.app, registerStatusItem(`app:${d.app}`, (p) => h(AppItem, { ...p, app: d.app }), 8));
}
