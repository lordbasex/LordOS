// portalopenmodel.js — what the shell does when a Linux app asks the desktop to open something
// (org.freedesktop.portal.OpenURI, server/src/ui/portal.c). lord-server has already checked a local file
// (in the home folder or the user's disks, not a protected folder, never an executable or a launcher) and
// says whether the app that asks is the one in front (the bus's pid against the keyboard's).
//
//   openPlan(req)   { action, ask, target }: action 'url' (the default browser), 'file' (our apps or the
//                   default app for the type), 'folder' (Files there), 'reveal' (Files on its folder, the
//                   file selected) or 'refuse'. Only http and https leave as addresses: LordOS opens no
//                   other scheme for an app. ask: an app in the background makes the person confirm it.
//   urlHost(url)    the host the question shows first, so a long address can't hide where it goes
//   splitPath(p)    { name, dir } for the question about a file

const WEB = /^https?:\/\/[^\s/?#@]*@?[^\s/?#]+([/?#][^\s]*)?$/i;
const WEB_MAX = 4096;

export function urlHost(url) {
  const m = /^https?:\/\/(?:[^\s/?#@]*@)?(\[[0-9a-f:.]+\]|[^\s/?#:]+)/i.exec(String(url || ''));
  return m ? m[1].toLowerCase() : '';
}

export function splitPath(p) {
  const s = String(p || '').replace(/\/+$/, '');
  const i = s.lastIndexOf('/');
  return i < 0 ? { name: s, dir: '' } : { name: s.slice(i + 1), dir: s.slice(0, i) || '/' };
}

export function openPlan(req) {
  const refuse = { action: 'refuse', ask: false, target: '' };
  if (!req || typeof req.target !== 'string' || !req.target) return refuse;
  const ask = req.front !== true;
  const { kind, target } = req;
  if (kind === 'uri') {
    if (target.length > WEB_MAX || !WEB.test(target) || !urlHost(target)) return refuse;
    return { action: 'url', ask, target };
  }
  if (kind === 'file' || kind === 'folder' || kind === 'reveal') {
    if (target[0] !== '/') return refuse;
    return { action: kind, ask, target };
  }
  return refuse;
}
