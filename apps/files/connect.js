// connect.js — what an address in Connect to Server means, no screen. Nothing here reaches a shell: an address is read
// strictly and each part goes on as data (docs/FILES.md, "Connect to Server").
const MOUNT = ['smb', 'nfs'];
const REMOTE = ['vnc', 'rdp'];
const FOLDER = ['ssh', 'sftp', 'ftp', 'ftps', 'webdav', 'davs'];    // a folder in Files, through rclone
const WEB = ['http', 'https'];
export const SCHEMES = [['smb', 'smb://server'], ['smb', 'smb://server/share'], ['nfs', 'nfs://server/path'], ['ssh', 'ssh://user@server:22'], ['sftp', 'sftp://user@server'],
  ['ftp', 'ftp://server'], ['ftps', 'ftps://server'], ['webdav', 'webdav://server/path'], ['davs', 'davs://server/path'], ['https', 'https://server'], ['vnc', 'vnc://server:5900'], ['rdp', 'rdp://server:3389']];
export const SUGGESTIONS = ['smb://', 'nfs://', 'ssh://', 'sftp://', 'ftp://', 'ftps://', 'webdav://', 'davs://', 'http://', 'https://', 'vnc://', 'rdp://'];

const HOST = /^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$/;
const USER = /^[A-Za-z0-9_][A-Za-z0-9._-]{0,63}$/;
const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;
export const isIPv4 = (h) => { const m = IPV4.exec(h); return !!m && m.slice(1).every((x) => Number(x) < 256); };

// { scheme, kind: 'mount'|'remote'|'folder'|'web', host, port, user, path, share, text } or { error }
export function parseAddress(raw) {
  const text = String(raw || '').trim();
  if (!text) return { error: 'empty' };
  if (/[\s"'`$;&|<>\\(){}*?!#\u0000-\u001f]/.test(text)) return { error: 'invalid' };
  const m = /^(?:([a-z][a-z0-9+.-]*):\/\/)?(?:([^@/]*)@)?([^/:]+)(?::(\d{1,5}))?(\/.*)?$/i.exec(text);
  if (!m) return { error: 'invalid' };
  const scheme = (m[1] || 'smb').toLowerCase();
  const kind = MOUNT.includes(scheme) ? 'mount' : REMOTE.includes(scheme) ? 'remote' : FOLDER.includes(scheme) ? 'folder' : WEB.includes(scheme) ? 'web' : null;
  if (!kind) return { error: 'scheme', scheme };
  const [, , user = '', host, port, path = ''] = m;
  if (!HOST.test(host) || (user && !USER.test(user))) return { error: 'invalid' };
  if (port && (Number(port) < 1 || Number(port) > 65535)) return { error: 'invalid' };
  if (/\/\.\.(\/|$)/.test(path)) return { error: 'invalid' };
  const out = { scheme, kind, host, port: port ? Number(port) : 0, user, path: path.replace(/\/+$/, ''), text };
  if (scheme === 'smb') out.share = out.path.split('/')[1] || '';
  return out;
}
// ssh: what is typed in Terminal, and left for the person to run
export function sshLine(a) { return `${a.scheme}${a.port ? ` ${a.scheme === 'ssh' ? '-p' : '-P'} ${a.port}` : ''} ${a.user ? `${a.user}@` : ''}${a.host}`; }
export const remoteUri = (a) => `${a.scheme}://${a.user ? `${a.user}@` : ''}${a.host}${a.port ? `:${a.port}` : ''}`;

// Names: only what Network already saw; a name is never resolved for the mount (share_admin.c).
export function hostAddress(hosts, name) {
  if (isIPv4(name)) return name;
  const n = name.toLowerCase().replace(/\.local$/, '');
  const hit = hosts.find((h) => (h.name || '').toLowerCase().replace(/\.local$/, '') === n || h.services?.some((s) => (s.name || '').toLowerCase() === n));
  return hit ? hit.addr : null;
}
