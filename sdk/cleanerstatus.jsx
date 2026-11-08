// cleanerstatus.jsx — Cleaner's item by the clock and its panel (docs/CLEANER.md). The item is a still glyph with
// no timer, so it costs nothing; it hides with Cleaner → Settings. Every number is read only while the panel
// is open, from what the system already reports. There is no monitoring claim here, no memory "free up" and no
// speed test. The status word has one written rule (statusWord) and appears only when it can be worked out.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image, addListener } from '@lordos/runtime';
import { useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { formatBytes, systemInfo } from './system.js';
import { wm } from './windows.js';
import { Overlay } from './layers.js';
import { registerStatusItem, statusPanelAt } from './statusitems.js';
import { cleaner } from './cleaner.js';
import { cleanerPalette } from './cleanertheme.js';
import { lordosUpdateWaiting } from './updates.js';
import { cleanerItemShown } from './menubarmodel.js';


// The rule, documented in docs/CLEANER.md: under 10 % of the disk free is "low"; with a LordOS update waiting,
// "update"; otherwise "good". No disk reading: no word.
export function statusWord(free, total, updateWaiting) {
  if (!(total > 0) || free < 0) return null;
  if (free / total < 0.1) return 'low';
  return updateWaiting ? 'update' : 'good';
}

const call = (fn, d) => { try { return fn(); } catch (e) { return d; } };

function read(prev) {
  const st = cleaner.status();
  const si = call(() => systemInfo(), null);
  const mem = si && si.memTotalKB ? { total: si.memTotalKB * 1024, available: (si.memAvailableKB) * 1024 } : null;
  const [t1, i1] = call(() => lord.system.cpuTimes(), [0, 0]);
  const net = call(() => lord.monitor.netTotals(), null);
  const fan = call(() => (lord.fan ? lord.fan.status() : null), null);
  const r = { st, mem, t1, i1, net, temp: fan && fan.temp != null ? fan.temp : null, disks: Math.max(0, cleaner.volumes().length - 2), at: Date.now() };
  r.cpu = prev && t1 > prev.t1 ? Math.max(0, Math.min(1, 1 - (i1 - prev.i1) / (t1 - prev.t1))) : null;
  if (prev && prev.net && net) { const dt = (net.t - prev.net.t) || (r.at - prev.at) / 1000; r.up = Math.max(0, (net.tx - prev.net.tx) / dt); r.down = Math.max(0, (net.rx - prev.net.rx) / dt); }
  return r;
}

function Card({ p, name, value, pct }) {
  return h(View, { style: { flex: 1, minHeight: 78, padding: 12, gap: 4, borderRadius: 14, backgroundColor: p.glass, border: `1px solid ${p.line}` } },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: p.text, textAlign: 'start' } }, name),
    h(Text, { style: { fontSize: 12, color: p.sub, textAlign: 'start' } }, value),
    pct != null ? h(View, { style: { height: 5, borderRadius: 3, backgroundColor: p.track, marginTop: 'auto', overflow: 'hidden', direction: 'ltr' } },
      h(View, { style: { width: `${Math.round(pct * 100)}%`, height: 5, background: `linear-gradient(90deg, ${p.barA}, ${p.barB})` } })) : null);
}

function Panel({ p, onClose }) {
  const [d, setD] = useState(() => read(null));
  const [waiting] = useState(() => call(lordosUpdateWaiting, false));   // read once, as the panel opens
  useEffect(() => {
    const id = setInterval(() => setD((o) => read(o)), 2000);
    return () => clearInterval(id);
  }, []);
  const c = settings.get().cleaner || {};
  const open = (section) => { onClose(); wm.open('cleaner', { args: { section } }); };
  const word = statusWord(d.st.free, d.st.total, waiting);
  const host = call(() => systemInfo().hostname, '') || '';
  const rate = (v) => (v == null ? '…' : `${formatBytes(v)}/s`);
  const memAvail = d.mem ? (d.mem.available != null ? d.mem.available : d.mem.free) : null;
  const memTotal = d.mem ? d.mem.total : 0;
  return h(View, { style: { width: 384, padding: 16, gap: 10, borderRadius: 20, backgroundColor: p.panel, border: `1px solid ${p.line}`, boxShadow: '0 24px 60px rgba(8,4,30,0.5)' } },
    h(View, null,
      h(Text, { style: { fontSize: 17, fontWeight: 600, color: p.text, textAlign: 'start' } }, host),
      word ? h(Text, { style: { fontSize: 12, color: word === 'good' ? p.good : p.warn, textAlign: 'start' } }, t(`cleaner.panel.word.${word}`)) : null),
    h(View, { style: { flexDirection: 'row', gap: 10 } },
      h(Card, { p, name: t('cleaner.panel.disk'), value: t('cleaner.disk.free', { size: formatBytes(d.st.free) }), pct: d.st.total > 0 ? 1 - d.st.free / d.st.total : null }),
      memTotal ? h(Card, { p, name: t('cleaner.panel.memory'), value: t('cleaner.panel.memoryValue', { size: formatBytes(memAvail) }), pct: 1 - memAvail / memTotal }) : null),
    h(View, { style: { flexDirection: 'row', gap: 10 } },
      h(Card, { p, name: t('cleaner.panel.cpu'), value: `${d.cpu == null ? '…' : `${Math.round(d.cpu * 100)} %`}${d.temp != null ? ` · ${Math.round(d.temp)} °C` : ''}`, pct: d.cpu }),
      h(Card, { p, name: t('cleaner.panel.network'), value: `↑ ${rate(d.up)}  ↓ ${rate(d.down)}` })),
    d.disks ? h(Text, { style: { fontSize: 12, color: p.sub, textAlign: 'start' } }, t('cleaner.panel.devices', { count: d.disks })) : null,
    h(View, { style: { padding: 12, gap: 12, flexDirection: 'row', alignItems: 'center', borderRadius: 14, backgroundColor: p.glass, border: `1px solid ${p.line}` } },
      h(View, { style: { flex: 1 } }, h(Text, { style: { fontSize: 12, color: p.sub, textAlign: 'start' } },
        c.lastScan && c.lastScan.allocated > 0 ? t('cleaner.panel.lastScan', { size: formatBytes(c.lastScan.allocated) }) : t('cleaner.panel.noScan'))),
      h(View, { onClick: () => open('cleanup'), style: { height: 36, paddingHorizontal: 14, borderRadius: 12, justifyContent: 'center', background: `linear-gradient(160deg, ${p.btnA}, ${p.btnB})` } },
        h(Text, { style: { fontSize: 13, fontWeight: 600, color: p.btnText } }, t('cleaner.panel.freeUp')))),
    h(View, { style: { flexDirection: 'row', justifyContent: 'space-between' } },
      h(Text, { onClick: () => open('scan'), style: { fontSize: 13, color: p.sub } }, t('cleaner.panel.open')),
      h(Text, { onClick: () => open('settings'), style: { fontSize: 13, color: p.sub } }, t('cleaner.panel.prefs'))));
}

function CleanerItem({ height }) {
  const th = useTheme();
  const all = useStore(settings);
  const on = cleanerItemShown(all);
  const [open, setOpen] = useState(false);
  const btn = useRef(null), panel = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, hh] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + hh; };
      if (!inside(panel.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  if (!on) return null;
  const p = cleanerPalette(all, th.mode);
  return h(View, { ref: btn, onClick: () => setOpen((o) => !o), style: { height, width: 26, alignItems: 'center', justifyContent: 'center' } },
    h(Image, { src: `asset:cleaner/status-${th.mode === 'light' ? 'dark' : 'light'}.png`, style: { width: 16, height: 16 } }),
    open ? h(Overlay, { anchor: btn }, h(View, { ref: panel, style: statusPanelAt(btn) }, h(Panel, { p, onClose: () => setOpen(false) }))) : null);
}

let started = false;
export function startCleanerStatus() {
  if (started) return;
  started = true;
  registerStatusItem('cleaner', CleanerItem, 4);
}
