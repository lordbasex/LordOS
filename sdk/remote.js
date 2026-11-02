// remote.js — remote desktop: who is watching this session from the network, and the on-screen notice.
//
// axia-server notifies (__lordRemote) every time someone connects or disconnects through the
// remote desktop (lord-sessiond, docs/REMOTE.md). <RemoteBadge /> is mounted by startShell: a
// fixed notice at the top right while there are connections, in every theme.
//
//   useStore(remoteViewers) → [{ ip, user }]
//   remote.status()         → { enabled, installed, port, allow, net: 'lan'|'list'|'any', fingerprint, clients: [...],
//                               sessions: [...], monitor, blocked: [{ ip, until, strikes, manual }], allowed: ['cidr'] }
//   remoteVideo.setStun([...]) / setTurn(server, user, pass, 'udp'|'tcp'|'tls', fingerprint) / clearTurn()  (remote-ice)
//   remoteCidr(text)        → canonical-looking text if it's an address or network the admin actions accept, else ''
//                             (lord-sessiond's helper checks again, strictly: server/src/remote/ipnet.c)
// lord-sessiond's notice "this source was blocked after failed logins" arrives as __lordRemoteNotice and is
// shown as a notification that opens Settings → Sharing.
//   sshLogin.status()       → { installed, enabled, running, allow: 'all'|'admins', fingerprint } (remote login, SSH)
import { createElement as h } from 'react';
import { View, Text } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { notify } from './notify.js';
import { wm } from './windows.js';
import { admin } from './admin.js';
import { parseCode } from './errors.js';

export const remoteViewers = createStore([]);
globalThis.__lordRemote = (json) => {
  try { remoteViewers.set(JSON.parse(json)); } catch (e) { remoteViewers.set([]); }
};

// Mirrors rs_cidr_parse: IPv4 (no leading zeros) with /8../32, or IPv6 with /16../128.
export function remoteCidr(text) {
  const s = String(text || '').trim();
  const m = /^([0-9a-fA-F:.]+)(?:\/(\d{1,3}))?$/.exec(s);
  if (!m || (m[2] && m[2].length > 1 && m[2][0] === '0')) return '';
  const len = m[2] === undefined ? null : Number(m[2]);
  if (/^\d{1,3}(\.\d{1,3}){3}$/.test(m[1])) {
    if (m[1].split('.').some((o) => Number(o) > 255 || (o.length > 1 && o[0] === '0'))) return '';
    if (len !== null && (len < 8 || len > 32)) return '';
    return s;
  }
  if (!m[1].includes(':') || m[1].length > 45 || (m[1].match(/::/g) || []).length > 1) return '';
  const groups = m[1].split(':');
  if (groups.length > 8 || groups.some((g) => g.length > 4 && !/\./.test(g))) return '';
  if (len !== null && (len < 16 || len > 128)) return '';
  return s;
}

const minutesLeft = (until) => Math.max(1, Math.round((until - Date.now() / 1000) / 60));
globalThis.__lordRemoteNotice = (n) => {
  if (!n || n.t !== 'blocked' || typeof n.ip !== 'string' || !/^[0-9a-fA-F:./]{3,48}$/.test(n.ip)) return;
  const min = Math.max(1, Math.min(1440, Number(n.min) || 15));
  notify({
    app: 'settings',
    title: t('shell.remote.blockedTitle'),
    body: t('shell.remote.blockedBody', { ip: n.ip, minutes: min }),
    sticky: true,
    actions: [{ id: 'open', label: t('shell.remote.blockedOpen'), run: () => wm.open('settings', { args: { section: 'sharing' } }) }],
  });
};
export const remoteMinutesLeft = minutesLeft;

// The address lists and blocks, through the fixed admin actions (server/src/remote/remote_admin.c,
// validated again on the root side). Used by Settings → Sharing and Monitor → Network.
//   remoteAccess.failures() → [{ key, count, first, last, user }]  (administrators only; never a password)
export const remoteAccess = {
  block: (cidr) => admin.run('remote-block', cidr),
  unblock: (cidr) => admin.run('remote-unblock', cidr),
  allow: (cidr) => admin.run('remote-allow', `add ${cidr}`),
  disallow: (cidr) => admin.run('remote-allow', `del ${cidr}`),
  setNet: (net) => admin.run('remote-config', `net=${net}`),
  async failures() {
    const rows = [];
    await admin.run('remote-failures', '', (line) => {
      const c = parseCode(String(line));
      const p = c?.code === 'remote.failure' ? c.params : null;
      if (p && remoteCidr(p.key) && /^[\x20-\x7e]{1,32}$/.test(p.user || '')) {
        rows.push({ key: p.key, count: Number(p.count) || 0, first: Number(p.first) || 0, last: Number(p.last) || 0, user: p.user });
      }
    }, { quiet: true });
    return rows.sort((a, b) => b.last - a.last);
  },
};

function parseConfig(text) {
  const c = { port: 5673, allow: 'all', net: 'lan', stun: [], turn: '', turnUser: '', turnKey: false, turnTransport: 'udp', turnFp: '' };
  for (const line of String(text || '').split('\n')) {
    const m = /^(\w+)=(.*)$/.exec(line.trim());
    if (m && m[1] === 'turn_transport' && ['udp', 'tcp', 'tls'].includes(m[2])) c.turnTransport = m[2];
    if (m && m[1] === 'turn_fp') c.turnFp = remoteFingerprint(m[2]);
    if (m && m[1] === 'stun' && c.stun.length < 4) c.stun.push(m[2]);
    if (m && m[1] === 'turn') c.turn = m[2];
    if (m && m[1] === 'turn_user') c.turnUser = m[2];
    if (m && m[1] === 'turn_key') c.turnKey = m[2] === '1';
    if (m && m[1] === 'port') c.port = Number(m[2]) || 5673;
    if (m && m[1] === 'allow') c.allow = m[2] === 'group' ? 'group' : 'all';
    if (m && m[1] === 'net') c.net = ['lan', 'list', 'any'].includes(m[2]) ? m[2] : 'lan';
  }
  return c;
}

export const remote = {
  available: () => typeof lord.remote !== 'undefined',
  status() {
    if (typeof lord.remote === 'undefined') return { enabled: false, installed: false, net: 'lan', clients: [], sessions: [], monitor: '', blocked: [], allowed: [] };
    const s = lord.remote.status();
    const cfg = parseConfig(s.config);
    let st = null;
    try { st = s.status ? JSON.parse(s.status) : null; } catch (e) { st = null; }
    return {
      installed: !!s.installed, enabled: !!(st && st.running), port: st?.port || cfg.port, allow: st?.allow || cfg.allow,
      fingerprint: s.fingerprint || '', clients: st?.clients || [], sessions: st?.sessions || [], monitor: st?.monitor || '',
      net: st?.net || cfg.net, blocked: Array.isArray(st?.blocked) ? st.blocked : [], allowed: Array.isArray(st?.allowed) ? st.allowed : [],
      stun: cfg.stun, turn: cfg.turn, turnUser: cfg.turnUser, turnKey: cfg.turnKey, turnTransport: cfg.turnTransport, turnFp: cfg.turnFp,
    };
  },
};

// Mirrors rw_ice_server_valid (server/src/remote/rtcwire.c): host:port with a name, an IPv4 or [IPv6].
export function remoteIceServer(text) {
  const s = String(text || '').trim();
  const m = /^(\[[0-9a-fA-F:.]+\]|[A-Za-z0-9](?:[A-Za-z0-9.-]{0,61}[A-Za-z0-9])?):([1-9][0-9]{0,4})$/.exec(s);
  return m && Number(m[2]) <= 65535 && !m[1].includes('..') ? s : '';
}

// Mirrors rw_fp_parse (rtcwire.c): a SHA-256 certificate fingerprint, 64 hex digits, optionally in pairs
// separated by ':'. Returns the text (trimmed) or ''.
export function remoteFingerprint(text) {
  const s = String(text || '').trim();
  return /^[0-9a-fA-F]{64}$/.test(s) || /^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){31}$/.test(s) ? s : '';
}

// The WebRTC connection's settings (docs/REMOTE.md, "Video (WebRTC)"), through the remote-ice admin
// action; the TURN password goes quietly (never into the visible admin log) and is never read back.
export const remoteVideo = {
  setStun: (list) => admin.run('remote-ice', `stun=${list.join(',')}`),
  setTurn: (server, user, pass, transport = 'udp', fp = '') => admin.run('remote-ice',
    [`turn=${server}`, `turn_transport=${transport}`, `turn_fp=${transport === 'tls' ? fp : ''}`, `turn_user=${user}`, ...(pass ? [`turn_pass=${pass}`] : [])].join('\n'),
    undefined, { quiet: true }),
  clearTurn: () => admin.run('remote-ice', 'turn='),
};

// Remote login (SSH): read-only state; changes go through the ssh-set / ssh-allow admin actions.
export const sshLogin = {
  available: () => typeof lord.ssh !== 'undefined',
  status() {
    if (typeof lord.ssh === 'undefined') return { installed: false, enabled: false, running: false, allow: 'all', fingerprint: '' };
    const s = lord.ssh.status();
    return { installed: !!s.installed, enabled: !!s.enabled, running: !!s.running, allow: s.allow === 'admins' ? 'admins' : 'all', fingerprint: s.fingerprint || '' };
  },
};

export function RemoteBadge() {
  const list = useStore(remoteViewers);
  if (!list.length) return null;
  const who = list.map((c) => (c.user ? `${c.user} · ${c.ip}` : c.ip)).join(', ');
  return h(View, {
    style: {
      position: 'absolute', right: 12, top: 34, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, height: 28,
      borderRadius: 14, backgroundColor: 'rgba(48,209,88,0.92)', boxShadow: '0 6px 18px rgba(0,0,0,0.35)', pointerEvents: 'none',
    },
  },
  h(View, { style: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' } }),
  h(Text, { style: { fontSize: 12, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap' } }, t('shell.remote.watching', { who })));
}
