// cloudstatus.jsx — Cloud's item by the clock and its panel (docs/CLOUD.md section 11), and the notifications
// Cloud sends when its state changes (the States mock): an account needs a new sign-in, an account is full with
// changes waiting, a file changed on another device too.
//
// The item shows only while at least one account exists: a still cloud when idle, an arrow inside while files
// move, a badge for an account that needs the person or for changes waiting. Everything comes from the pushed
// state (cloud.js), so the item has no timer of its own. Its panel opens toward the middle of the screen (#29).
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme, popupBg } from './theme.js';
import { t } from './i18n.js';
import { formatBytes } from './system.js';
import { wm } from './windows.js';
import { Overlay } from './layers.js';
import { registerStatusItem, statusPanelAt } from './statusitems.js';
import { notify } from './notify.js';
import { openUrl } from './sysopen.js';
import { cloud, cloudState } from './cloud.js';
import { menuGlyph, stateLine, transfers, recentRows, allPaused, accountColor, usage, conflictName, transitions as transitionsOf } from './cloudmodel.js';
import { CloudGlyph, AccountBadge } from './cloudui.jsx';
import { installCloudMock } from './cloudmock.js';
import { askKeyringUnlock } from './vault.js';

const openSettings = (page) => wm.open('settings', { args: { section: 'cloud', page, nonce: Date.now() } });
const openFiles = (path, select) => wm.open('files', { new: true, args: { path, ...(select ? { select } : {}), nonce: Date.now() } });

const ToneColor = (th, tone) => (tone === 'warn' ? '#ffb340' : tone === 'busy' ? (th.mode === 'dark' ? '#64b5ff' : '#0a6cd6') : th.textDim);

function Btn({ label, onClick }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  return h(View, { onClick, onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { flex: 1, height: 28, borderRadius: th.controlRadius, alignItems: 'center', justifyContent: 'center', backgroundColor: th.control, opacity: hover ? 0.85 : 1 } },
  h(Text, { style: { fontSize: 12, fontWeight: 500, color: th.text, whiteSpace: 'nowrap' } }, label));
}

function Panel({ st, onClose }) {
  const th = useTheme();
  const accs = st.accounts || [];
  const tr = transfers(accs);
  const head = [tr.up ? t('cloud.goingUp', { count: tr.up }) : '', tr.down ? t('cloud.comingDown', { count: tr.down }) : ''].filter(Boolean).join(' · ');
  const rows = recentRows(st.recent, accs);
  const paused = allPaused(st);
  const first = accs[0];
  return h(View, { style: { width: 380, borderRadius: 14, overflow: 'hidden', backgroundColor: popupBg(th), border: `1px solid ${th.separator}`, boxShadow: '0 24px 70px rgba(0,0,0,0.45)' } },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 16, paddingTop: 14, paddingBottom: 10 } },
      h(Text, { style: { flex: 1, fontSize: 14, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('cloud.title')),
      head ? h(Text, { style: { fontSize: 12, color: th.textDim } }, head) : null),
    h(View, { style: { paddingHorizontal: 10, paddingBottom: 8, gap: 2 } },
      ...accs.map((a) => {
        const line = stateLine(a, st.service);
        const u = usage(a.about);
        const text = line.key === 'cloud.state.ok' && u ? `${t(line.key)} · ${u.total != null ? t('cloud.space.usedOf', { used: formatBytes(u.used), total: formatBytes(u.total) }) : t('cloud.space.usedOnly', { used: formatBytes(u.used) })}` : t(line.key, line.vars);
        return h(View, { key: a.id, onClick: () => { onClose(); if (a.state === 'signin' || a.state === 'error') openSettings(a.id); else openFiles(a.mount); },
          style: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 6, paddingVertical: 7, borderRadius: 8 } },
        h(AccountBadge, { color: accountColor(accs, a.id), size: 30 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 1 } },
          h(Text, { style: { fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
          h(Text, { style: { fontSize: 12, color: ToneColor(th, line.tone), whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, text)),
        // a locked keyring: unlock it from here (the system's keyring prompt)
        a.error === 'cloud.keyringLocked' || a.state === 'locked' ? h(View, { onClick: (e) => { e.stopPropagation?.(); onClose(); askKeyringUnlock().then((ok) => { if (ok) cloud.retry(null); }); },
          style: { height: 24, paddingHorizontal: 10, borderRadius: th.controlRadiusSmall, justifyContent: 'center', backgroundColor: th.accent } },
        h(Text, { style: { fontSize: 12, fontWeight: 600, color: '#fff' } }, t('cloud.unlock'))) : null);
      })),
    rows.length ? h(View, { style: { borderTopWidth: 1, borderTopColor: th.separator, paddingHorizontal: 16, paddingTop: 8, paddingBottom: 4 } },
      h(Text, { style: { fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start' } }, t('cloud.recent'))) : null,
    rows.length ? h(View, { style: { paddingHorizontal: 10, paddingBottom: 8 } },
      ...rows.map((r, i) => h(View, { key: `${r.path}${i}`, onClick: () => { onClose(); openFiles(r.path.split('/').slice(0, -1).join('/'), r.name); },
        style: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 6, paddingVertical: 6 } },
      h(Text, { style: { width: 18, fontSize: 14, fontWeight: 700, color: r.dir === 'up' ? '#bf5af2' : '#0a84ff', textAlign: 'center' } }, r.dir === 'up' ? '↑' : '↓'),
      h(View, { style: { flex: 1, minWidth: 0 } },
        h(Text, { style: { fontSize: 12, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' } }, r.name),
        h(Text, { style: { fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, r.where)),
      h(Text, { style: { width: 54, fontSize: 12, color: th.textDim, textAlign: 'end' } }, t(r.ago.key, r.ago.vars))))) : null,
    h(View, { style: { flexDirection: 'row', gap: 8, paddingHorizontal: 14, paddingTop: 10, paddingBottom: 14, borderTopWidth: 1, borderTopColor: th.separator } },
      h(Btn, { label: t(paused ? 'cloud.resumeAll' : 'cloud.pauseAll'), onClick: () => (paused ? cloud.resume(null) : cloud.pause(null)) }),
      h(Btn, { label: t('cloud.showInFiles'), onClick: () => { onClose(); openFiles(first ? first.mount : `${(lord.fs && lord.fs.home()) || ''}/Cloud`); } }),
      h(Btn, { label: t('cloud.settingsMenu'), onClick: () => { onClose(); openSettings(); } })));
}

function CloudItem({ height, color }) {
  const th = useTheme();
  const st = useStore(cloudState);
  const [open, setOpen] = useState(false);
  const btn = useRef(null), panel = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, hh] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + hh; };
      if (!inside(panel.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  if (!st || !(st.accounts || []).length) return null;
  const g = menuGlyph(st);
  const label = g.badge === '!' ? t('cloud.menu.attention') : g.arrow ? t('cloud.menu.syncing') : t('cloud.menu.idle');
  return h(View, { ref: btn, title: label, onClick: () => setOpen((o) => !o), style: { height, width: 26, alignItems: 'center', justifyContent: 'center' } },
    h(CloudGlyph, { kind: 'menu', size: 18, color: color || th.text, arrow: g.arrow }),
    g.badge != null ? h(View, { style: { position: 'absolute', top: 1, insetInlineEnd: 0, minWidth: 12, height: 12, borderRadius: 6, paddingHorizontal: 3,
      backgroundColor: g.badge === '!' ? '#ff9f0a' : '#ff453a', alignItems: 'center', justifyContent: 'center' } },
    h(Text, { style: { fontSize: 8, fontWeight: 700, color: '#fff' } }, String(g.badge === '!' ? '!' : g.badge > 99 ? '99+' : g.badge))) : null,
    open ? h(Overlay, { anchor: btn }, h(View, { ref: panel, style: statusPanelAt(btn) }, h(Panel, { st, onClose: () => setOpen(false) }))) : null);
}

// ---- notifications ----

function announce(ev, st) {
  const label = t('cloud.title');
  const icon = () => h(CloudGlyph, { kind: 'cloud', size: 16, color: '#0a84ff' });
  const acc = (st.accounts || []).find((a) => a.id === ev.id);
  if (ev.type === 'signin') {
    notify({ app: 'settings', label, icon, sticky: true, title: t('cloud.notify.signinTitle', { name: ev.name }), body: t('cloud.notify.signinBody'),
      actions: [{ id: 'later', label: t('cloud.later'), run: () => {} }, { id: 'signin', label: t('cloud.signInAgain'), primary: true, run: () => openSettings(ev.id) }] });
  } else if (ev.type === 'full') {
    const u = usage(acc && acc.about);
    notify({ app: 'settings', label, icon, title: t('cloud.notify.fullTitle', { name: ev.name }),
      body: t('cloud.notify.fullBody', { used: u ? formatBytes(u.used) : '', total: u && u.total ? formatBytes(u.total) : '', count: ev.waiting }),
      actions: [
        { id: 'web', label: t('cloud.openWebsite'), run: () => { const url = cloud.websiteUrl(ev.id); if (url) openUrl(url); } },
        { id: 'files', label: t('cloud.notify.showWaiting'), primary: true, run: () => acc && openFiles(acc.mount) },
      ] });
  } else if (ev.type === 'conflict') {
    const name = String(ev.path).split('/').pop();
    const kept = ev.kept ? String(ev.kept).split('/').pop() : conflictName(name, t('cloud.cloudCopy'));   // the cloud's version, next to ours (17.2 item 23)
    notify({ app: 'files', label, icon, title: t('cloud.notify.conflictTitle'), body: t('cloud.notify.conflictBody', { file: name, account: ev.name, kept }),
      actions: [{ id: 'show', label: t('cloud.notify.showBoth'), primary: true, run: () => openFiles(String(ev.path).split('/').slice(0, -1).join('/'), name) }] });
  }
}

let started = false;
export function startCloud() {
  if (started) return;
  started = true;
  // development only: the stand-in for lord.cloud (cloudmock.js)
  try {
    const dev = settings.get().cloudDevMock;
    if (typeof lord !== 'undefined' && !lord.cloud && (dev === true || ['states', 'providersFail', 'locked', 'crashed'].includes(dev))) installCloudMock(lord.fs.home(), dev === true ? '' : dev);
  } catch (e) { console.log(`cloud mock: ${e}`); }
  let prev = cloudState.get();
  cloudState.subscribe(() => {
    const next = cloudState.get();
    try { for (const ev of transitionsOf(prev, next)) announce(ev, next); } catch (e) { console.log(`cloud: ${e}`); }
    prev = next;
  });
  registerStatusItem('cloud', CloudItem, 4.5);
}

