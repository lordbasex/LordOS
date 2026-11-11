// defaults.js — default applications (Settings → Default Apps, docs/SETTINGS.md). They are kept
// in ~/.config/mimeapps.list (freedesktop spec, so GNOME/KDE apps and xdg-open agree) through
// lord.mime; the terminal goes in ~/.config/xdg-terminals.list. LordOS's own apps are written as
// "lordos-<app>.desktop".
//
//   DEFAULT_CATEGORIES                  [{ id, mimes, natives, terminal?, family? }]
//   defaultFor(categoryId)              'lordos-photos.desktop' | 'org.gnome.Loupe.desktop' | null
//   defaultInfo(categoryId)             { id, auto } for Settings: auto when the user chose nothing
//   setDefaultFor(categoryId, id)       writes every MIME type of the category
//   resetDefaultFor(categoryId)         back to automatic (removes the user's lines)
//   candidatesFor(categoryId, linux)    [{ id, name, Icon, native }] that can be chosen
//   defaultOpenerId(mime)               the default for one type (Files: "Open With", double click)
import { getApp } from './windows.js';
import { appName } from './i18n.js';
import { isAppOff, appsOff } from './appsoff.js';
import { usableDefault } from './appsoffmodel.js';

// A LordOS app that can be a default: installed and not deactivated (Store → Installed, appsoffmodel.js).
const nativeOn = (n) => !!getApp(n) && !isAppOff(n);

const NATIVE = (id) => `lordos-${id}.desktop`;
export const nativeOf = (desktopId) => (desktopId && /^lordos-[a-z0-9_-]+\.desktop$/.test(desktopId) ? desktopId.slice(7, -8) : null);

// First MIME type is the one read back to show the current choice.
export const DEFAULT_CATEGORIES = [
  { id: 'browser', mimes: ['x-scheme-handler/https', 'x-scheme-handler/http', 'text/html', 'application/xhtml+xml'], natives: [] },
  { id: 'mail', mimes: ['x-scheme-handler/mailto', 'message/rfc822'], natives: ['mail'] },
  { id: 'music', mimes: ['audio/mpeg', 'audio/flac', 'audio/ogg', 'audio/x-vorbis+ogg', 'audio/x-opus+ogg', 'audio/mp4', 'audio/aac', 'audio/x-wav', 'audio/x-m4a'], natives: ['player'] },
  { id: 'video', mimes: ['video/mp4', 'video/x-matroska', 'video/webm', 'video/quicktime', 'video/x-msvideo', 'video/mpeg', 'video/ogg'], natives: ['player'] },
  { id: 'images', mimes: ['image/png', 'image/jpeg', 'image/gif', 'image/webp', 'image/bmp', 'image/tiff', 'image/svg+xml', 'image/heic'], natives: ['photos'] },
  { id: 'pdf', mimes: ['application/pdf'], natives: ['pdf'] },
  { id: 'text', mimes: ['text/plain', 'text/markdown', 'text/x-log'], natives: ['editor'] },
  { id: 'terminal', mimes: [], natives: ['terminal'], terminal: true },
  // the app the clock's calendar opens (issue #58): LordOS Calendar, or one such as Thunderbird. Only apps that declare
  // text/calendar itself: an editor that takes text/* is no calendar (family: false).
  { id: 'calendar', mimes: ['text/calendar'], natives: ['calendar'], family: false },
];

const cat = (id) => DEFAULT_CATEGORIES.find((c) => c.id === id);
const api = () => typeof lord !== 'undefined' && lord.mime;

export function defaultFor(id) {
  const c = cat(id);
  if (!c || !api()) return null;
  if (c.terminal) return lord.mime.terminal() || NATIVE('terminal');
  for (const m of c.mimes) { const d = usableDefault(lord.mime.get(m), appsOff()); if (d) return d; }
  return c.natives.find(nativeOn) ? NATIVE(c.natives.find(nativeOn)) : null;
}

// lord.mime.resolve: { id, explicit, source: 'user'|'system'|'declared' } or null. The app
// launchWith('') opens a file of that type with: the user's choice, else the system's default
// (mimeapps.list/defaults.list), else the first installed app that declares the type.
const resolve = (mime) => (api() && lord.mime.resolve ? lord.mime.resolve(mime) : null);

// What Settings shows for a category: { id, auto } or null. auto: the user chose nothing, and this
// is what opens these files (the system's default, else LordOS's own app, else the first installed
// app that declares the type), so the order is the same as defaultOpenerId's.
export function defaultInfo(id) {
  const c = cat(id);
  if (!c || !api()) return null;
  if (c.terminal) return { id: defaultFor(id), auto: false };
  const rs = c.mimes.map(resolve);
  const mine = rs.find((r) => r && r.explicit);
  // a choice of a deactivated LordOS app still shows as the choice (Settings can change it); what
  // actually opens these files meanwhile is the next capable app (defaultOpenerId)
  if (mine) return { id: mine.id, auto: false };
  const sys = rs.find((r) => r && r.source === 'system' && usableDefault(r.id, appsOff()));
  if (sys) return { id: sys.id, auto: true };
  const native = c.natives.find(nativeOn);
  if (native) return { id: NATIVE(native), auto: true };
  const first = rs.find((r) => r && usableDefault(r.id, appsOff()));
  return first ? { id: first.id, auto: true } : null;
}

// Back to automatic: the user's lines for the category's types leave ~/.config/mimeapps.list.
export function resetDefaultFor(id) {
  const c = cat(id);
  if (!c || c.terminal || !api() || !lord.mime.unset) return false;
  let ok = true;
  for (const m of c.mimes) ok = lord.mime.unset(m) && ok;
  return ok;
}

export function setDefaultFor(id, desktopId) {
  const c = cat(id);
  if (!c || !api() || !desktopId) return false;
  if (c.terminal) return lord.mime.setTerminal(desktopId);
  let ok = true;
  for (const m of c.mimes) ok = lord.mime.set(m, desktopId) && ok;
  return ok;
}

// Does a Linux app's MimeType list cover the category? (its main type, or the whole family)
function covers(mimeTypes, c) {
  if (!mimeTypes) return false;
  const list = mimeTypes.split(';').filter(Boolean);
  const main = c.mimes[0], top = main.split('/')[0];
  return list.includes(main) || (c.family !== false && list.includes(`${top}/*`)) || (c.id === 'browser' && list.includes('text/html') && list.includes('x-scheme-handler/http'));
}

export function candidatesFor(id, linuxApps = []) {
  const c = cat(id);
  if (!c) return [];
  const out = c.natives.filter(nativeOn).map((n) => getApp(n)).map((a) => ({ id: NATIVE(a.id), name: appName(a), Icon: a.Icon, native: true }));
  for (const a of linuxApps) {
    if (a.kind !== 'linux') continue;
    const ok = c.terminal ? /(^|;)TerminalEmulator(;|$)/.test(a.categories || '') : covers(a.mimeTypes, c);
    if (ok) out.push({ id: `${a.desktopId}.desktop`, name: a.name, Icon: a.Icon, native: false });
  }
  return out;
}

export function defaultOpenerId(mime) {
  if (!api() || !mime) return null;
  // the user's choice or the system's default, if that app is installed
  // a deactivated LordOS app is skipped (appsoffmodel.js usableDefault): the next capable app opens it
  const r = resolve(mime);
  const off = appsOff();
  if (r && r.source !== 'declared' && usableDefault(r.id, off)) return r.id;
  // the category that lists the type itself first (text/calendar is Calendar's, not the text editor's), then the
  // family's (text/x-foo goes to the editor)
  const c = DEFAULT_CATEGORIES.find((x) => x.mimes.includes(mime))
    || DEFAULT_CATEGORIES.find((x) => x.mimes[0] && x.family !== false && mime.startsWith(`${x.mimes[0].split('/')[0]}/`) && x.id !== 'browser' && x.id !== 'mail');
  const d = c ? defaultFor(c.id) : null;
  // then the category's (LordOS's own app), then the first installed app that declares the type
  return d || (r ? usableDefault(r.id, off) : null);
}
