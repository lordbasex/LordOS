// share.js — the shell's side of the screen-sharing API (server/src/remote/share.c, docs/SCREEN-SHARING.md).
//
// The system draws both things here, never the app: the permission prompt, and the indicator with its Stop.
// lord-server sends no frame until <ShareBadge /> says it is on screen (lord.share.shown), and a share ends if
// it leaves. The decisions live in a root-owned file, written by the share-grant admin action; a Linux app's
// grant is tied to its program.
//
//   useStore(shareHolder)   → { app, name, input, token } | null
//   share.grants()          → [{ id, exe, perm, state }]
//   share.set(id, perm, state, exe) / share.forget(id) / share.stop()
import { createElement as h, useEffect } from 'react';
import { View, Text, screen } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { admin } from './admin.js';
import { appName } from './i18n.js';
import { getApp } from './windows.js';
import { shareAppId, shareAskList } from './sharemodel.js';

const api = () => typeof lord !== 'undefined' && lord.share;
export const shareHolder = createStore(null);
const shareAsk = createStore(null);       // { token, app, exe, name, perms: [...], step }
export const shareRev = createStore(0);   // goes up when the grants change (Settings → Privacy)

const nameOf = (id) => {
  const bare = shareAppId(id);
  return appName(getApp(bare)) || bare;
};

globalThis.__lordShare = (json) => {
  let e;
  try { e = JSON.parse(json); } catch (err) { return; }
  if (e.t === 'ask') {
    shareAsk.set({ token: e.token, app: e.app, exe: e.exe, name: nameOf(e.app), perms: shareAskList(e), step: 0 });
  } else if (e.t === 'start') {
    shareAsk.set(null);
    shareHolder.set({ app: e.app, name: nameOf(e.app), input: !!e.input, token: e.token });
  } else if (e.t === 'end') {
    shareAsk.set(null);
    shareHolder.set(null);
  } else if (e.t === 'input') {
    shareHolder.set((c) => (c ? { ...c, input: false } : c));
  }
};

export const share = {
  grants: () => { try { return api() ? lord.share.grants() : []; } catch (e) { return []; } },
  async set(id, perm, state, exe) {
    await admin.run('share-grant', `${id} ${perm} ${state}${exe ? ` ${exe}` : ''}`, undefined, { quiet: true });
    if (api()) lord.share.reload();
    shareRev.set((v) => v + 1);
  },
  async forget(id) {
    await admin.run('share-forget', id, undefined, { quiet: true });
    if (api()) lord.share.reload();
    shareRev.set((v) => v + 1);
  },
  stop: () => (api() ? lord.share.stop() : false),
};

// One pill per display, in the notification layer, above fullscreen windows. Unlike the remote desktop's
// notice this one takes clicks.
export function ShareBadge() {
  const c = useStore(shareHolder);
  const token = c ? c.token : 0;
  useEffect(() => {
    if (!token || !api()) return undefined;
    lord.share.shown(token);
    return () => { if (lord.share.status()?.token === token) lord.share.hidden(); };
  }, [token]);
  if (!c) return null;
  const list = screen.displays?.length ? screen.displays : [{ x: 0, y: 0, w: screen.width }];
  return h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } }, list.map((d, i) => h(View, {
    key: i,
    style: {
      position: 'absolute', left: (d.x || 0) + (d.w || screen.width) - 340, top: (d.y || 0) + 66, width: 328, flexDirection: 'row', alignItems: 'center', gap: 8,
      paddingHorizontal: 12, height: 32, borderRadius: 16, backgroundColor: 'rgba(255,69,58,0.95)', boxShadow: '0 6px 18px rgba(0,0,0,0.35)',
    },
  },
  h(View, { style: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' } }),
  h(Text, { style: { flex: 1, fontSize: 12, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap', overflow: 'hidden' } },
    t(c.input ? 'shell.share.sharingInput' : 'shell.share.sharing', { app: c.name })),
  h(View, { onClick: () => share.stop(), style: { paddingHorizontal: 10, height: 22, borderRadius: 11, justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.3)' } },
    h(Text, { style: { fontSize: 12, fontWeight: 700, color: '#fff' } }, t('shell.share.stop'))))));
}

async function answer(a, allow) {
  const perm = a.perms[a.step];
  await share.set(a.app, perm, allow ? 'allow' : 'deny', a.exe);
  // no to the picture ends it; the input is a question of its own
  if (allow && a.step + 1 < a.perms.length) { shareAsk.set({ ...a, step: a.step + 1 }); return; }
  shareAsk.set(null);
  if (api()) lord.share.answer(a.token);        // the root-owned file decides, not this call
}

export function SharePrompt() {
  const a = useStore(shareAsk);
  if (!a) return null;
  const perm = a.perms[a.step];
  const verified = a.app.startsWith('lpk:');
  return h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100000 } },
    h(View, { style: { marginTop: 80, width: 340, borderRadius: 14, padding: 20, gap: 12, backgroundColor: '#2b2b30', border: '1px solid rgba(255,255,255,0.12)',
      boxShadow: '0 20px 60px rgba(0,0,0,0.5)', alignItems: 'center' } },
      h(Text, { style: { fontSize: 14, fontWeight: 700, color: '#f5f5f7', textAlign: 'center', lineHeight: 1.4 } },
        t(perm === 'remote-input' ? 'shell.share.askInput' : 'shell.share.askScreen', { app: a.name })),
      !verified ? h(Text, { style: { fontSize: 11, color: '#ffd60a', textAlign: 'center' } }, t('shell.share.notVerified')) : null,
      !verified && a.exe ? h(Text, { style: { fontSize: 11, color: 'rgba(235,235,245,0.55)', textAlign: 'center' } }, a.exe) : null,
      h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 4 } },
        h(PromptBtn, { label: t('shell.share.deny'), onClick: () => answer(a, false) }),
        h(PromptBtn, { primary: true, label: t('shell.share.allow'), onClick: () => answer(a, true) }))));
}

function PromptBtn({ label, primary, onClick }) {
  return h(View, { onClick, style: { paddingVertical: 7, paddingHorizontal: 16, borderRadius: 8, backgroundColor: primary ? '#0a84ff' : 'rgba(255,255,255,0.1)' } },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: '#fff' } }, label));
}
