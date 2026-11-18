// streammodel.js — network streams' plain logic (0.8.8.10; server/tests/stream_model_test.mjs): cameras, IPTV and the
// Player's "Open address…". lord-stream plays them (server/src/media/streamurl.h has the same address rules, checked
// again in C).
//
//   STREAM_SCHEMES            the schemes an address may use
//   streamUrlOk(url)          whether lord-stream would take it
//   splitCredentials(url)     { url (without user:password@), user, pass, key } — the password goes to the keyring
//   withCredentials(url, user, pass)   the address to open (user and password percent-encoded)
//   protocolOf(url)           'RTSP' | 'RTMP' | 'SRT' | 'RTP' | 'UDP' | 'HLS' | 'DASH' | 'MJPEG' | 'HTTP'
//   retryDelay(n)             the wait before the n-th reconnection (1 s, 2 s, 4 s … 30 s)
//   fatal(code)               an error no retry fixes (a wrong address or password)
//   lowQuality(url)           a camera's sub stream where the address says how (Dahua/Amcrest subtype, Hikvision 102)
export const STREAM_SCHEMES = ['rtsp', 'rtsps', 'rtmp', 'rtmps', 'rtmpt', 'srt', 'rtp', 'udp', 'http', 'https'];
const MAX = 2048;

export function streamUrlOk(url) {
  const u = String(url || '');
  if (u.length < 8 || u.length > MAX) return false;
  if (/[\x00-\x20\x7f-\uffff"'\\|`]/.test(u)) return false;
  const m = /^([a-z]+):\/\/(.)/.exec(u);
  return !!m && STREAM_SCHEMES.includes(m[1]) && m[2] !== '/';
}

export function splitCredentials(url) {
  const m = /^([a-z]+:\/\/)([^@/?#]*)@(.*)$/.exec(String(url || ''));
  if (!m) return { url: String(url || ''), user: '', pass: '', key: hostKey(url) };
  const [user, ...rest] = m[2].split(':');
  const dec = (s) => { try { return decodeURIComponent(s); } catch (e) { return s; } };
  const clean = `${m[1]}${m[3]}`;
  return { url: clean, user: dec(user || ''), pass: dec(rest.join(':')), key: hostKey(clean) };
}

// the keyring's service for a stream: its scheme, host and port (one password per camera)
export function hostKey(url) {
  const m = /^([a-z]+):\/\/(?:[^@/?#]*@)?([^/?#]+)/.exec(String(url || ''));
  return m ? `stream:${m[1]}://${m[2].toLowerCase()}` : '';
}

export function withCredentials(url, user, pass) {
  if (!user && !pass) return url;
  const m = /^([a-z]+:\/\/)(.*)$/.exec(String(url || ''));
  if (!m) return url;
  const enc = (s) => encodeURIComponent(s || '');
  return `${m[1]}${enc(user)}${pass ? `:${enc(pass)}` : ''}@${m[2]}`;
}

export function protocolOf(url) {
  const u = String(url || '').toLowerCase();
  const s = (/^([a-z]+):/.exec(u) || [])[1] || '';
  if (s.startsWith('rtsp')) return 'RTSP';
  if (s.startsWith('rtmp')) return 'RTMP';
  if (s === 'srt') return 'SRT';
  if (s === 'rtp') return 'RTP';
  if (s === 'udp') return 'UDP';
  if (/\.m3u8(\?|$)/.test(u)) return 'HLS';
  if (/\.mpd(\?|$)/.test(u)) return 'DASH';
  if (/mjpe?g|\.cgi|snapshot|video\.mjpg/.test(u)) return 'MJPEG';
  return 'HTTP';
}

export const retryDelay = (n) => Math.min(30000, 1000 * 2 ** Math.max(0, Math.min(5, n)));
export const fatal = (code) => code === 'media.streamAddress' || code === 'media.streamAuth' || code === 'media.noStreamHelper';

// The lighter stream of the same camera, for a small widget: only where the address itself says which one it is
export function lowQuality(url) {
  const u = String(url || '');
  if (/[?&]subtype=0\b/.test(u)) return u.replace(/([?&]subtype=)0\b/, '$11');
  if (/\/Streaming\/Channels\/(\d+)01\b/.test(u)) return u.replace(/\/Streaming\/Channels\/(\d+)01\b/, '/Streaming/Channels/$102');
  return u;
}

// A local .m3u file read in JS (a downloaded list): the same rules as lord-stream's parser for one over http(s)
export function parseM3U(text, max = 5000) {
  const out = [];
  let inf = null;
  for (const raw of String(text || '').replace(/^\uFEFF/, '').split(/\r?\n|\r/)) {
    const l = raw.trim();
    if (!l) continue;
    if (l.startsWith('#EXTINF:')) {
      let q = false, comma = -1;
      for (let i = 8; i < l.length; i++) { if (l[i] === '"') q = !q; else if (l[i] === ',' && !q) comma = i; }
      const attr = (k) => (new RegExp(`${k}="([^"]*)"`).exec(l) || [])[1] || '';
      const logo = attr('tvg-logo');
      inf = { name: comma >= 0 ? clean(l.slice(comma + 1)) : '', group: clean(attr('group-title')), logo: /^https?:\/\//.test(logo) && streamUrlOk(logo) ? logo : '' };
      continue;
    }
    if (l.startsWith('#')) continue;
    if (streamUrlOk(l)) {
      out.push({ name: (inf && inf.name) || clean(l), group: inf ? inf.group : '', logo: inf ? inf.logo : '', url: l });
      if (out.length >= max) break;
    }
    inf = null;
  }
  return out;
}
const clean = (s) => String(s || '').replace(/[\x00-\x1f\x7f]/g, '').trim().slice(0, 120);
