// loginmodel.js — sign-in forms and Passwords, the pure part (sdk/vault.js: <AutofillButton hosts>, <SaveLoginOffer>;
// server/tests/login_model_test.mjs). A server reached by its address (a shared folder, sftp, a remote desktop) is
// matched only by that exact address: an entry's `host` field or its site's host, never a guess from the title.
//
//   hostOf(text)                     "smb://ana@NAS.local:445/x" → "nas.local" ("www." dropped, IPv6 brackets kept out)
//   loginHosts(...hosts)             the distinct non-empty hosts a form signs in to (the address, the name it was found under)
//   hostMatches(entry, host)         an entry of the Passwords list is for exactly that host
//   exactMatches(entries, hosts)     the entries to offer: [{ id, title, username, host }], not in the trash, each once
//   alreadySaved(entries, hosts, user)  Passwords already keeps a login for one of these hosts (and that user, when given)
//   typedSecret(password, filled)    the person typed the password in this form (not filled from Passwords or the keyring)
//   loginEntry(login)                the Passwords entry a sign-in is saved as: { cat, title, tags, f: [{ k, t, v }] }

export function hostOf(text) {
  let s = String(text || '').trim().toLowerCase();
  if (!s) return '';
  s = s.replace(/^[a-z][a-z0-9+.-]*:\/\//, '');          // scheme
  s = s.split(/[/?#]/)[0];                                 // path
  s = s.slice(s.lastIndexOf('@') + 1);                     // user@
  if (s.startsWith('[')) { const end = s.indexOf(']'); return end > 1 ? s.slice(1, end) : ''; }
  if ((s.match(/:/g) || []).length === 1) s = s.split(':')[0];   // host:port (a bare IPv6 address keeps its colons)
  s = s.replace(/^www\./, '').replace(/\.$/, '');
  // a host name or an address; a name a machine announces ("Ana's laptop") is not one
  return /^[a-z0-9_]([a-z0-9._-]*[a-z0-9_])?$/.test(s) || /^[0-9a-f:.]+$/.test(s) && s.includes(':') ? s : '';
}

export function loginHosts(...hosts) {
  const out = [];
  for (const x of hosts.flat()) { const h = hostOf(x); if (h && !out.includes(h)) out.push(h); }
  return out;
}

export function hostMatches(e, host) {
  const h = hostOf(host);
  return !!h && !!e && (hostOf(e.host) === h || hostOf(e.url) === h);
}

export function exactMatches(entries, hosts) {
  const hs = loginHosts(hosts);
  const out = [];
  for (const e of entries || []) {
    if (!e || e.trash) continue;
    const h = hs.find((x) => hostMatches(e, x));
    if (h && !out.some((o) => o.id === e.id)) out.push({ id: e.id, title: e.title || '', username: e.sub || '', host: hostOf(e.host) || hostOf(e.url) });
  }
  return out;
}

// the user part of "DOMAIN\ana" or "ana@corp" as it is compared: lowercase, without the domain
const bareUser = (u) => String(u || '').trim().toLowerCase().replace(/^[^\\]*\\/, '');

export function alreadySaved(entries, hosts, user) {
  const u = bareUser(user);
  return exactMatches(entries, hosts).some((e) => !u || bareUser(e.username) === u);
}

export const typedSecret = (password, filled) => !!password && password !== (filled || '');

// What each kind of sign-in is kept as in Passwords: the category whose template has host, port, user name and password
// (sftp is an ssh login; the rest a network device named by its protocol).
const KINDS = {
  smb: { cat: 'network', device: 'SMB' },
  sftp: { cat: 'ssh', device: 'SFTP' },
  ftp: { cat: 'network', device: 'FTP' },
  ftps: { cat: 'network', device: 'FTPS' },
  webdav: { cat: 'network', device: 'WebDAV' },
  davs: { cat: 'network', device: 'WebDAV' },
  rdp: { cat: 'network', device: 'RDP' },
  vnc: { cat: 'network', device: 'VNC' },
};
export const LOGIN_KINDS = Object.keys(KINDS);

// login: { kind, host, port, user, password }. null for an unknown kind, no host or no password.
export function loginEntry({ kind, host, port, user, password } = {}) {
  const k = KINDS[kind];
  const h = String(host || '').trim();
  if (!k || !hostOf(h) || !password) return null;
  const who = String(user || '').trim();
  const f = [];
  if (k.cat === 'network') f.push({ k: 'device', t: 'text', v: k.device });
  f.push({ k: 'host', t: 'text', v: h });
  const p = Number(port);
  if (Number.isInteger(p) && p > 0 && p < 65536) f.push({ k: 'port', t: 'number', v: String(p) });
  if (who) f.push({ k: 'username', t: 'text', v: who });
  f.push({ k: 'password', t: 'hidden', v: String(password) });
  return { cat: k.cat, title: `${k.device} ${who ? `${who}@` : ''}${h}`, tags: [], f };
}
