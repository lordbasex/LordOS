// share.js — the shell's side of the screen-sharing API (server/src/remote/share.c, docs/SCREEN-SHARING.md).
//
// The system draws both, never the app: the permission prompt here, and the indicator with its Stop by the clock
// (sdk/capturestatus.jsx). lord-server sends no frame until the indicator says it is on screen (lord.share.shown), and
// a share ends if it leaves. The decisions live in a root-owned file, written by the share-grant admin action; a Linux app's
// grant is tied to its program.
//
//   useStore(shareHolder)   → { app, name, input, token } | null
//   share.grants()          → [{ id, exe, perm, state }]
//   share.set(id, perm, state, exe) / share.forget(id) / share.stop()
import { createElement as h } from 'react';
import { View, Text } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { admin } from './admin.js';
import { appName } from './i18n.js';
import { getApp } from './windows.js';
import { shareAppId, shareAskList, staleGrants } from './sharemodel.js';

const api = () => typeof lord !== 'undefined' && lord.share;
export const shareHolder = createStore(null);
const shareAsk = createStore(null);       // { token, app, exe, name, perms: [...], step }
export const shareRev = createStore(0);   // goes up when the grants change (Settings → Privacy)

const nameOf = (id) => {
  const bare = shareAppId(id);
  return appName(getApp(bare)) || bare;
};

// Uninstalling forgets: a grant never outlives its app, even if another app later takes the id.
export function forgetShareGrants(bare) {
  const id = `lpk:${bare}`;
  if (share.grants().some((g) => g.id === id)) share.forget(id).catch(() => { /* shown as a stale grant at the next start */ });
}

// What was uninstalled behind the shell's back (dpkg, apt): at the session's start, against what is installed now.
export function sweepShareGrants(lpkIds, desktopIds) {
  for (const id of staleGrants(share.grants(), lpkIds, desktopIds)) share.forget(id).catch(() => { /* next session */ });
}

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

// The indicator with its Stop is the capture indicator by the clock (sdk/capturestatus.jsx): it shows the share, says
// lord.share.shown when it is on screen, and lord.share.hidden when every indicator has left.

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
